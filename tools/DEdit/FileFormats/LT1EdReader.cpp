//------------------------------------------------------------------
//
//	FILE	  : LT1EdReader.cpp
//
//	PURPOSE	  : See LT1EdReader.h. The layout is described in LT1EdWriter.cpp.
//
//	Texture mapping is the inverse of the writer's: Processor.exe builds
//	  P = ( cos(r) U - sin(r) V ) / xScale
//	  Q = ( sin(r) U + cos(r) V ) / yScale
//	  O = (xOffset / xScale) U + (yOffset / yScale) V
//	from the planar axes (U, V) chosen by the poly normal.
//
//------------------------------------------------------------------

#include "bdefs.h"
#include "LT1EdReader.h"
#include "LT1EdWriter.h"
#include "editregion.h"
#include "editbrush.h"
#include "editpoly.h"
#include "worldnode.h"
#include "proplist.h"
#include "editobjects.h"
#include "node_ops.h"
#include "editprojectmgr.h"

#include <vector>
#include <string>
#include <map>
#include <math.h>

#define LT1ED_VERSION		1247

#define LT1ED_NODE_NULL		0
#define LT1ED_NODE_BRUSH	1
#define LT1ED_NODE_OBJECT	2

#define LT1ED_NODEFLAG_ROOT	8



// ------------------------------------------------------------------
// Inflate (RFC 1950/1951), after zlib's puff.c. Used for compressed .ed files.
// ------------------------------------------------------------------

namespace
{
	struct InfState
	{
		const unsigned char	*m_pIn;
		size_t				m_nIn, m_Pos;
		unsigned			m_BitBuf;
		int					m_BitCnt;
		std::vector<unsigned char> *m_pOut;
		bool				m_bErr;
	};

	struct InfHuff
	{
		short	m_Count[16];
		short	m_Symbol[288];
	};

	static int InfBits(InfState *s, int need)
	{
		long val = s->m_BitBuf;
		while(s->m_BitCnt < need)
		{
			if(s->m_Pos >= s->m_nIn)
			{
				s->m_bErr = true;
				return 0;
			}
			val |= (long)s->m_pIn[s->m_Pos++] << s->m_BitCnt;
			s->m_BitCnt += 8;
		}
		s->m_BitBuf = (unsigned)(val >> need);
		s->m_BitCnt -= need;
		return (int)(val & ((1L << need) - 1));
	}

	static int InfDecode(InfState *s, const InfHuff *h)
	{
		int code = 0, first = 0, index = 0;
		for(int len = 1; len <= 15; len++)
		{
			code |= InfBits(s, 1);
			if(s->m_bErr)
				return -1;
			int count = h->m_Count[len];
			if(code - count < first)
				return h->m_Symbol[index + (code - first)];
			index += count;
			first += count;
			first <<= 1;
			code <<= 1;
		}
		s->m_bErr = true;
		return -1;
	}

	static int InfConstruct(InfHuff *h, const short *length, int n)
	{
		int symbol, len, left;
		short offs[16];
		for(len = 0; len <= 15; len++)
			h->m_Count[len] = 0;
		for(symbol = 0; symbol < n; symbol++)
			h->m_Count[length[symbol]]++;
		if(h->m_Count[0] == n)
			return 0;
		left = 1;
		for(len = 1; len <= 15; len++)
		{
			left <<= 1;
			left -= h->m_Count[len];
			if(left < 0)
				return left;
		}
		offs[1] = 0;
		for(len = 1; len < 15; len++)
			offs[len + 1] = offs[len] + h->m_Count[len];
		for(symbol = 0; symbol < n; symbol++)
			if(length[symbol] != 0)
				h->m_Symbol[offs[length[symbol]]++] = (short)symbol;
		return left;
	}

	static bool InfCodes(InfState *s, const InfHuff *lencode, const InfHuff *distcode)
	{
		static const short lens[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
		static const short lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
		static const short dists[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
		static const short dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

		for(;;)
		{
			int symbol = InfDecode(s, lencode);
			if(s->m_bErr || symbol < 0)
				return false;
			if(symbol < 256)
				s->m_pOut->push_back((unsigned char)symbol);
			else if(symbol == 256)
				return true;
			else
			{
				symbol -= 257;
				if(symbol >= 29)
					return false;
				int len = lens[symbol] + InfBits(s, lext[symbol]);
				symbol = InfDecode(s, distcode);
				if(s->m_bErr || symbol < 0 || symbol >= 30)
					return false;
				size_t dist = (size_t)dists[symbol] + InfBits(s, dext[symbol]);
				if(s->m_bErr || dist > s->m_pOut->size())
					return false;
				size_t from = s->m_pOut->size() - dist;
				for(int i=0; i < len; i++)
					s->m_pOut->push_back((*s->m_pOut)[from + i]);
			}
		}
	}

	// Inflates a zlib stream and appends the result to out.
	static bool InflateZlib(const unsigned char *pIn, size_t nIn, std::vector<unsigned char> &out)
	{
		if(nIn < 2)
			return false;
		InfState s;
		s.m_pIn = pIn; s.m_nIn = nIn; s.m_Pos = 2;	// skip the zlib header
		s.m_BitBuf = 0; s.m_BitCnt = 0; s.m_pOut = &out; s.m_bErr = false;

		static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
		static bool s_bFixed = false;
		static InfHuff s_FixLen, s_FixDist;
		if(!s_bFixed)
		{
			short lengths[288];
			int sym;
			for(sym = 0; sym < 144; sym++)	lengths[sym] = 8;
			for(; sym < 256; sym++)			lengths[sym] = 9;
			for(; sym < 280; sym++)			lengths[sym] = 7;
			for(; sym < 288; sym++)			lengths[sym] = 8;
			InfConstruct(&s_FixLen, lengths, 288);
			for(sym = 0; sym < 30; sym++)	lengths[sym] = 5;
			InfConstruct(&s_FixDist, lengths, 30);
			s_bFixed = true;
		}

		int last;
		do
		{
			last = InfBits(&s, 1);
			int type = InfBits(&s, 2);
			if(s.m_bErr)
				return false;

			if(type == 0)
			{
				s.m_BitBuf = 0; s.m_BitCnt = 0;
				if(s.m_Pos + 4 > s.m_nIn)
					return false;
				unsigned len = s.m_pIn[s.m_Pos] | (s.m_pIn[s.m_Pos + 1] << 8);
				unsigned nlen = s.m_pIn[s.m_Pos + 2] | (s.m_pIn[s.m_Pos + 3] << 8);
				s.m_Pos += 4;
				if(len != (~nlen & 0xFFFF) || s.m_Pos + len > s.m_nIn)
					return false;
				out.insert(out.end(), s.m_pIn + s.m_Pos, s.m_pIn + s.m_Pos + len);
				s.m_Pos += len;
			}
			else if(type == 1)
			{
				if(!InfCodes(&s, &s_FixLen, &s_FixDist))
					return false;
			}
			else if(type == 2)
			{
				int nlen = InfBits(&s, 5) + 257;
				int ndist = InfBits(&s, 5) + 1;
				int ncode = InfBits(&s, 4) + 4;
				if(s.m_bErr || nlen > 286 || ndist > 30)
					return false;
				short lengths[320];
				int index;
				for(index = 0; index < ncode; index++)
					lengths[order[index]] = (short)InfBits(&s, 3);
				for(; index < 19; index++)
					lengths[order[index]] = 0;
				InfHuff lencode, distcode;
				if(InfConstruct(&lencode, lengths, 19) != 0)
					return false;
				index = 0;
				while(index < nlen + ndist)
				{
					int symbol = InfDecode(&s, &lencode);
					if(s.m_bErr || symbol < 0)
						return false;
					if(symbol < 16)
						lengths[index++] = (short)symbol;
					else
					{
						int len = 0, rep;
						if(symbol == 16)
						{
							if(index == 0)
								return false;
							len = lengths[index - 1];
							rep = 3 + InfBits(&s, 2);
						}
						else if(symbol == 17)
							rep = 3 + InfBits(&s, 3);
						else
							rep = 11 + InfBits(&s, 7);
						if(index + rep > nlen + ndist)
							return false;
						while(rep--)
							lengths[index++] = (short)len;
					}
				}
				int err = InfConstruct(&lencode, lengths, nlen);
				if(err < 0 || (err > 0 && nlen - lencode.m_Count[0] != 1))
					return false;
				err = InfConstruct(&distcode, lengths + nlen, ndist);
				if(err < 0 || (err > 0 && ndist - distcode.m_Count[0] != 1))
					return false;
				if(!InfCodes(&s, &lencode, &distcode))
					return false;
			}
			else
				return false;
		} while(!last);
		return true;
	}
}

// ------------------------------------------------------------------
// Raw data
// ------------------------------------------------------------------

struct LT1RawPoly
{
	std::vector<uint16>	m_Indices;
	float				m_Map[5];	// xScale, yScale, xOffset, yOffset, rotation
	std::string			m_Texture;
	uint32				m_Flag;		// 1 = texture origin is anchored to the poly's corner
};

struct LT1RawBrush
{
	std::vector<LTVector>		m_Points;
	std::vector<LT1RawPoly>		m_Polies;
};

struct LT1RawProp
{
	std::string					m_Name;
	uint8						m_Type;
	std::vector<unsigned char>	m_Data;
};

struct LT1RawNode
{
	uint32						m_Type;
	uint32						m_BrushIndex;
	std::string					m_Class;
	std::string					m_DisplayName;
	uint32						m_Flags;
	std::vector<LT1RawProp>		m_Props;
	std::vector<LT1RawNode*>	m_Children;

	LT1RawNode() : m_Type(0), m_BrushIndex(0), m_Flags(0) {}
	~LT1RawNode()
	{
		for(size_t i=0; i < m_Children.size(); i++)
			delete m_Children[i];
	}
};


class CLT1Reader
{
public:
	std::vector<unsigned char>	m_Data;
	size_t						m_Pos;
	bool						m_bFail;

	CLT1Reader() : m_Pos(0), m_bFail(false) {}

	bool Have(size_t n)
	{
		if(m_bFail || m_Pos + n > m_Data.size())
		{
			m_bFail = true;
			return false;
		}
		return true;
	}
	uint8 U8()		{ uint8 v = 0; if(Have(1)) { v = m_Data[m_Pos]; m_Pos += 1; } return v; }
	uint16 U16()	{ uint16 v = 0; if(Have(2)) { memcpy(&v, &m_Data[m_Pos], 2); m_Pos += 2; } return v; }
	uint32 U32()	{ uint32 v = 0; if(Have(4)) { memcpy(&v, &m_Data[m_Pos], 4); m_Pos += 4; } return v; }
	float F32()		{ float v = 0.0f; if(Have(4)) { memcpy(&v, &m_Data[m_Pos], 4); m_Pos += 4; } return v; }
	void Skip(size_t n)	{ if(Have(n)) m_Pos += n; }
	std::string Str16()
	{
		std::string s;
		uint32 len = U16();
		if(Have(len))
		{
			s.assign((const char*)&m_Data[m_Pos], len);
			m_Pos += len;
		}
		return s;
	}
};


static bool LT1Ed_ReadBrush(CLT1Reader &r, LT1RawBrush &brush)
{
	r.Skip(3);	// color

	uint32 nPoints = r.U32();
	if(r.m_bFail || nPoints > 65535 || !r.Have((size_t)nPoints * 12))
		return false;
	brush.m_Points.resize(nPoints);
	uint32 i, j;
	for(i=0; i < nPoints; i++)
	{
		brush.m_Points[i].x = r.F32();
		brush.m_Points[i].y = r.F32();
		brush.m_Points[i].z = r.F32();
	}

	uint32 nPolies = r.U32();
	if(r.m_bFail || nPolies > 65535)
		return false;
	brush.m_Polies.resize(nPolies);
	for(i=0; i < nPolies; i++)
	{
		LT1RawPoly &poly = brush.m_Polies[i];
		uint32 nVerts = r.U32();
		if(r.m_bFail || nVerts > 256)
			return false;
		poly.m_Indices.resize(nVerts);
		for(j=0; j < nVerts; j++)
			poly.m_Indices[j] = r.U16();

		r.Skip(16);	// normal + distance (recomputed)
		for(j=0; j < 5; j++)
			poly.m_Map[j] = r.F32();
		poly.m_Flag = r.U32();
		poly.m_Texture = r.Str16();
		r.U32();
		r.Skip(3);	// shade
		if(r.m_bFail)
			return false;
	}
	return !r.m_bFail;
}

static LT1RawNode* LT1Ed_ReadNode(CLT1Reader &r, bool bRoot, int depth)
{
	if(depth > 256 || r.m_bFail)
		return NULL;

	LT1RawNode *pNode = new LT1RawNode;
	if(!bRoot)
	{
		pNode->m_Type = r.U32();
		if(pNode->m_Type == LT1ED_NODE_BRUSH)
			pNode->m_BrushIndex = r.U32();
		else if(pNode->m_Type > LT1ED_NODE_OBJECT)
			r.m_bFail = true;
	}

	uint32 nChildren = r.U16();
	for(uint32 i=0; i < nChildren && !r.m_bFail; i++)
	{
		LT1RawNode *pChild = LT1Ed_ReadNode(r, false, depth + 1);
		if(!pChild)
		{
			r.m_bFail = true;
			break;
		}
		pNode->m_Children.push_back(pChild);
	}

	uint32 dataLen = r.U16();
	size_t dataEnd = r.m_Pos + dataLen;
	pNode->m_Class = r.Str16();
	uint32 nProps = r.U32();
	if(nProps > 4096)
		r.m_bFail = true;
	for(uint32 p=0; p < nProps && !r.m_bFail; p++)
	{
		LT1RawProp prop;
		prop.m_Name = r.Str16();
		prop.m_Type = r.U8();
		r.U32();	// flags
		uint32 len = r.U16();
		if(r.Have(len))
		{
			prop.m_Data.assign(&r.m_Data[r.m_Pos], &r.m_Data[r.m_Pos] + len);
			r.m_Pos += len;
		}
		pNode->m_Props.push_back(prop);
	}
	if(!r.m_bFail)
		r.m_Pos = dataEnd;	// trust the stored length

	r.U32();	// id
	pNode->m_Flags = r.U32();
	pNode->m_DisplayName = r.Str16();

	if(r.m_bFail)
	{
		delete pNode;
		return NULL;
	}
	return pNode;
}


// ------------------------------------------------------------------
// Building the region
// ------------------------------------------------------------------

struct LT1BuildContext
{
	CEditRegion						*m_pRegion;
	CEditProjectMgr					*m_pProject;
	std::vector<LT1RawBrush>		*m_pBrushes;
	std::map<std::string, int>		m_Unknown;		// unknown class -> count
	std::map<std::string, int>		m_UnknownProps;	// property name -> count
	uint32							m_nBrushes, m_nObjects, m_nNulls, m_nPolies;
};

static float LT1Ed_Float(const LT1RawProp &p, size_t idx)
{
	float v = 0.0f;
	if(p.m_Data.size() >= (idx + 1) * 4)
		memcpy(&v, &p.m_Data[idx * 4], 4);
	return v;
}

static void LT1Ed_ApplyProp(LT1BuildContext &ctx, CWorldNode *pNode, const LT1RawProp &raw)
{
	CBaseProp *pProp = pNode->m_PropList.GetProp(raw.m_Name.c_str());
	if(!pProp)
	{
		// Not defined by the class here: keep it as a generic property so it
		// is written back out instead of being lost.
		pProp = CreatePropFromCode(raw.m_Type, (char*)raw.m_Name.c_str());
		if(!pProp)
		{
			ctx.m_UnknownProps[raw.m_Name]++;
			return;
		}
		pNode->m_PropList.m_Props.Append(pProp);
	}

	switch(pProp->GetType())
	{
		case LT_PT_STRING:
		{
			std::string s;
			if(raw.m_Data.size() >= 2)
			{
				uint16 len;
				memcpy(&len, &raw.m_Data[0], 2);
				if((size_t)len + 2 <= raw.m_Data.size())
					s.assign((const char*)&raw.m_Data[2], len);
			}
			((CStringProp*)pProp)->SetString(s.c_str());
		}
		break;

		case LT_PT_VECTOR:
		case LT_PT_COLOR:
			((CVectorProp*)pProp)->SetVector(LTVector(LT1Ed_Float(raw, 0), LT1Ed_Float(raw, 1), LT1Ed_Float(raw, 2)));
		break;

		case LT_PT_REAL:
		case LT_PT_FLAGS:
		case LT_PT_LONGINT:
			((CRealProp*)pProp)->SetValue(LT1Ed_Float(raw, 0));
		break;

		case LT_PT_BOOL:
			((CBoolProp*)pProp)->SetValue(raw.m_Data.size() >= 1 && raw.m_Data[0] != 0);
		break;

		case LT_PT_ROTATION:
			((CRotationProp*)pProp)->SetEulerAngles(LTVector(LT1Ed_Float(raw, 0), LT1Ed_Float(raw, 1), LT1Ed_Float(raw, 2)));
		break;

		default:
		break;
	}
}

static void LT1Ed_FillBrush(LT1BuildContext &ctx, CEditBrush *pBrush, const LT1RawBrush &src)
{
	uint32 i, j;
	for(i=0; i < src.m_Points.size(); i++)
		pBrush->m_Points.Append(CEditVert(src.m_Points[i]));

	for(i=0; i < src.m_Polies.size(); i++)
	{
		const LT1RawPoly &sp = src.m_Polies[i];
		bool bOK = sp.m_Indices.size() >= 3;
		for(j=0; j < sp.m_Indices.size() && bOK; j++)
			if(sp.m_Indices[j] >= src.m_Points.size())
				bOK = false;
		if(!bOK)
			continue;

		CEditPoly *pPoly = new CEditPoly(pBrush);
		for(j=0; j < sp.m_Indices.size(); j++)
			pPoly->m_Indices.Append((uint32)sp.m_Indices[j]);
		pPoly->UpdatePlane();

		CTexturedPlane &tex = pPoly->GetTexture(0);
		tex.m_pTextureName = ctx.m_pRegion->m_pStringHolder->AddString((char*)sp.m_Texture.c_str());

		LTVector U, V;
		LT1Ed_GetBaseAxes(pPoly->Normal(), U, V);
		float xs = (float)fabs(sp.m_Map[0]) > 1e-6f ? sp.m_Map[0] : 1.0f;
		float ys = (float)fabs(sp.m_Map[1]) > 1e-6f ? sp.m_Map[1] : 1.0f;
		float c = (float)cos(sp.m_Map[4]), s = (float)sin(sp.m_Map[4]);
		tex.SetP((U * c - V * s) * (1.0f / xs));
		tex.SetQ((U * s + V * c) * (1.0f / ys));
		LTVector O = U * (sp.m_Map[2] / xs) + V * (sp.m_Map[3] / ys);

		// Polys flagged 1 anchor the texture origin at a corner of the poly
		// (the far corner along the base axes, found by comparing Blood 2's .ed
		// files with the .dat files Processor.exe built from them). The
		// anchor is folded into O, so exporting writes it back as an offset.
		if(sp.m_Flag == 1)
		{
			float maxU = -1e30f, maxV = -1e30f;
			for(j=0; j < sp.m_Indices.size(); j++)
			{
				const LTVector &pt = src.m_Points[sp.m_Indices[j]];
				float pu = pt.Dot(U), pv = pt.Dot(V);
				if(pu > maxU) maxU = pu;
				if(pv > maxV) maxV = pv;
			}
			O += U * maxU + V * maxV;
		}
		tex.SetO(O);

		pBrush->m_Polies.Append(pPoly);
		ctx.m_nPolies++;
	}

	pBrush->UpdatePlanes();
	pBrush->UpdateBoundingInfo();
}

static void LT1Ed_BuildNode(LT1BuildContext &ctx, const LT1RawNode *pRaw, CWorldNode *pParent)
{
	CEditRegion *pRegion = ctx.m_pRegion;
	CWorldNode *pNode = NULL;

	switch(pRaw->m_Type)
	{
		case LT1ED_NODE_BRUSH:
		{
			if(pRaw->m_BrushIndex >= ctx.m_pBrushes->size())
				break;
			CEditBrush *pBrush = no_CreateNewBrush(pRegion, pParent);
			LT1Ed_FillBrush(ctx, pBrush, (*ctx.m_pBrushes)[pRaw->m_BrushIndex]);
			// The node's display name goes in first; the brush's own Name
			// property (e.g. "cdmladder2") then overrides it.
			if(!pRaw->m_DisplayName.empty())
				pBrush->SetName(pRaw->m_DisplayName.c_str());
			for(size_t p=0; p < pRaw->m_Props.size(); p++)
				LT1Ed_ApplyProp(ctx, pBrush, pRaw->m_Props[p]);
			pNode = pBrush;
			ctx.m_nBrushes++;
		}
		break;

		case LT1ED_NODE_OBJECT:
		{
			ClassDef *pClass = ctx.m_pProject ? ctx.m_pProject->FindClassDef(pRaw->m_Class.c_str()) : NULL;
			if(pClass)
			{
				CBaseEditObj *pObj = new CBaseEditObj;
				no_InitializeNewNode(pRegion, pObj, pParent);
				SetupWorldNodeFromClass(pClass, NULL, pObj, pRegion,
					pRaw->m_DisplayName.empty() ? NULL : (char*)pRaw->m_DisplayName.c_str());
				for(size_t p=0; p < pRaw->m_Props.size(); p++)
					LT1Ed_ApplyProp(ctx, pObj, pRaw->m_Props[p]);
				pNode = pObj;
				ctx.m_nObjects++;
			}
			else
			{
				ctx.m_Unknown[pRaw->m_Class]++;
				// Keep the hierarchy: an empty node stands in for the object.
				pNode = pRegion->AddNullNode(pParent);
				pNode->SetNodeLabel(pRaw->m_DisplayName.empty() ? pRaw->m_Class.c_str() : pRaw->m_DisplayName.c_str());
			}
		}
		break;

		default:
		{
			pNode = pRegion->AddNullNode(pParent);
			pNode->SetNodeLabel(pRaw->m_DisplayName.c_str());
			ctx.m_nNulls++;
		}
		break;
	}

	if(!pNode)
		return;
	for(size_t i=0; i < pRaw->m_Children.size(); i++)
		LT1Ed_BuildNode(ctx, pRaw->m_Children[i], pNode);
}


// ------------------------------------------------------------------
// Entry point
// ------------------------------------------------------------------

bool LT1Ed_ReadFile(CEditRegion *pRegion, CEditProjectMgr *pProject, const char *pFilename, CString &sError, CString &sWarnings)
{
	CLT1Reader r;

	FILE *fp = fopen(pFilename, "rb");
	if(!fp)
	{
		sError.Format("Unable to open %s.", pFilename);
		return false;
	}
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if(size < 48)
	{
		fclose(fp);
		sError = "This isn't a LithTech 1.0 .ed file (too small).";
		return false;
	}
	r.m_Data.resize(size);
	size_t got = fread(&r.m_Data[0], 1, size, fp);
	fclose(fp);
	if(got != (size_t)size)
	{
		sError.Format("Unable to read %s.", pFilename);
		return false;
	}

	uint32 version = r.U32();
	uint8 bCompressed = r.U8();
	if(version != LT1ED_VERSION)
	{
		sError.Format("Unsupported .ed version %u (only %d is supported).", version, LT1ED_VERSION);
		return false;
	}

	uint32 infoLen = r.U32();
	std::string info;
	if(r.Have(infoLen))
	{
		info.assign((const char*)&r.m_Data[r.m_Pos], infoLen);
		r.m_Pos += infoLen;
	}
	r.Skip(32);

	if(bCompressed)
	{
		// n, block size, n compressed sizes, ... and the zlib blocks at the end of the file.
		uint32 nBlocks = r.U32();
		r.U32();
		if(r.m_bFail || nBlocks == 0 || nBlocks > 1000000 || !r.Have((size_t)nBlocks * 4))
		{
			sError = "The .ed file is damaged (compression header).";
			return false;
		}
		std::vector<uint32> sizes(nBlocks);
		size_t total = 0;
		for(uint32 b=0; b < nBlocks; b++)
		{
			sizes[b] = r.U32();
			total += sizes[b];
		}
		if(total > r.m_Data.size() - r.m_Pos)
		{
			sError = "The .ed file is damaged (block sizes).";
			return false;
		}

		std::vector<unsigned char> inflated;
		size_t blockPos = r.m_Data.size() - total;
		for(uint32 b=0; b < nBlocks; b++)
		{
			if(!InflateZlib(&r.m_Data[blockPos], sizes[b], inflated))
			{
				sError.Format("The .ed file is damaged (block %u wouldn't decompress).", b);
				return false;
			}
			blockPos += sizes[b];
		}
		r.m_Data.swap(inflated);
		r.m_Pos = 0;
	}

	uint32 nBrushes = r.U32();
	if(r.m_bFail || nBrushes > 200000)
	{
		sError = "The .ed file is damaged (bad brush count).";
		return false;
	}
	std::vector<LT1RawBrush> brushes(nBrushes);
	for(uint32 i=0; i < nBrushes; i++)
	{
		if(!LT1Ed_ReadBrush(r, brushes[i]))
		{
			sError.Format("The .ed file is damaged (brush %u).", i);
			return false;
		}
	}

	LT1RawNode *pRoot = LT1Ed_ReadNode(r, true, 0);
	if(!pRoot)
	{
		sError = "The .ed file is damaged (node tree).";
		return false;
	}

	pRegion->m_pInfoString = pRegion->m_pStringHolder->AddString((char*)info.c_str());

	LT1BuildContext ctx;
	ctx.m_pRegion = pRegion;
	ctx.m_pProject = pProject;
	ctx.m_pBrushes = &brushes;
	ctx.m_nBrushes = ctx.m_nObjects = ctx.m_nNulls = ctx.m_nPolies = 0;

	for(size_t c=0; c < pRoot->m_Children.size(); c++)
		LT1Ed_BuildNode(ctx, pRoot->m_Children[c], pRegion->GetRootNode());
	delete pRoot;

	sWarnings.Empty();
	CString s;
	std::map<std::string, int>::iterator it;
	for(it = ctx.m_Unknown.begin(); it != ctx.m_Unknown.end(); ++it)
	{
		s.Format("%d object(s) of class \"%s\" aren't in this project and became empty nodes.\n", it->second, it->first.c_str());
		sWarnings += s;
	}
	for(it = ctx.m_UnknownProps.begin(); it != ctx.m_UnknownProps.end(); ++it)
	{
		s.Format("Property \"%s\" doesn't exist here (%d time(s)) and was dropped.\n", it->first.c_str(), it->second);
		sWarnings += s;
	}
	return true;
}
