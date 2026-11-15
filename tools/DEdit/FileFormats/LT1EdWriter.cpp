//------------------------------------------------------------------
//
//	FILE	  : LT1EdWriter.cpp
//
//	PURPOSE	  : See LT1EdWriter.h.
//
//	The .ed layout (version 1247, uncompressed) is:
//
//	  uint32  version (1247)
//	  uint8   compressed (0)
//	  uint32  info string length, chars
//	  uint32  dummy[8]
//	  uint32  brush count, brushes
//	  node tree (root first)
//
//	Brush:   uint8 color[3], uint32 nPoints, float pts[nPoints][3],
//	         uint32 nPolies, polies
//	Poly:    uint32 nVerts, uint16 indices[nVerts], float normal[3], float dist,
//	         float xScale, yScale, xOffset, yOffset, rotation, uint32 0,
//	         uint16 len + texture name, uint32 0, uint8 shade[3]
//	Node:    (not for the root) uint32 type (0 node, 1 brush, 2 object),
//	         (brush only) uint32 brush index,
//	         uint16 child count, children,
//	         uint16 data length, uint16 len + class, uint32 prop count, props,
//	         uint32 id, uint32 flags, uint16 len + display name
//	Prop:    uint16 len + name, uint8 type, uint32 flags, uint16 data length, data
//
//	LithTech 1.0 does not store explicit texture vectors. Processor.exe rebuilds
//	them from a planar projection (picked from the poly normal) plus scale,
//	offset and rotation. Those five values are recovered here from the
//	O, P, Q vectors this editor stores; skew can't be represented and is dropped.
//
//------------------------------------------------------------------

#include "bdefs.h"
#include "LT1EdWriter.h"
#include "editregion.h"
#include "editbrush.h"
#include "editpoly.h"
#include "worldnode.h"
#include "proplist.h"
#include "geomroutines.h"

#include <vector>
#include <map>
#include <math.h>


#define LT1ED_VERSION		1247

#define LT1ED_NODE_NULL		0
#define LT1ED_NODE_BRUSH	1
#define LT1ED_NODE_OBJECT	2

#define LT1ED_NODEFLAG_ROOT	8


class CLT1EdBuffer
{
public:
	std::vector<unsigned char>	m_Data;

	void Bytes(const void *p, uint32 n)
	{
		const unsigned char *pc = (const unsigned char*)p;
		m_Data.insert(m_Data.end(), pc, pc + n);
	}
	void U8(uint8 v)		{ Bytes(&v, 1); }
	void U16(uint16 v)		{ Bytes(&v, 2); }
	void U32(uint32 v)		{ Bytes(&v, 4); }
	void F32(float v)		{ Bytes(&v, 4); }
	void Vec(const LTVector &v)	{ F32(v.x); F32(v.y); F32(v.z); }
	void Str16(const char *p)
	{
		uint32 len = p ? (uint32)strlen(p) : 0;
		if(len > 0xFFFF)
			len = 0xFFFF;
		U16((uint16)len);
		if(len)
			Bytes(p, len);
	}

	uint32 Pos() const		{ return (uint32)m_Data.size(); }
	void PatchU16(uint32 pos, uint16 v)	{ memcpy(&m_Data[pos], &v, 2); }
	void PatchU32(uint32 pos, uint32 v)	{ memcpy(&m_Data[pos], &v, 4); }
};


struct LT1EdContext
{
	CLT1EdBuffer					m_Buf;
	std::map<CEditBrush*, uint32>	m_BrushIndex;
	uint32							m_NextID;
	uint32							m_nSkippedPrefabs;
	uint32							m_nSkippedPolies;
};


// ------------------------------------------------------------------
// Texture mapping
// ------------------------------------------------------------------

// The planar axes Processor.exe uses, picked by the largest normal component
// (ties go to Y, then X). Worked out by comparing Blood 2's shipped .ed files
// with the .dat files built from them.
void LT1Ed_GetBaseAxes(const LTVector &n, LTVector &U, LTVector &V)
{
	float ax = (float)fabs(n.x), ay = (float)fabs(n.y), az = (float)fabs(n.z);

	if(ay >= ax && ay >= az)
	{
		U.Init(n.y > 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);
		V.Init(0.0f, 0.0f, -1.0f);
	}
	else if(ax >= az)
	{
		U.Init(0.0f, 0.0f, n.x > 0.0f ? 1.0f : -1.0f);
		V.Init(0.0f, -1.0f, 0.0f);
	}
	else
	{
		U.Init(n.z > 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f);
		V.Init(0.0f, -1.0f, 0.0f);
	}
}

// Processor.exe builds:
//   P = ( cos(r) U - sin(r) V ) / xScale
//   Q = ( sin(r) U + cos(r) V ) / yScale
//   O = (xOffset / xScale) U + (yOffset / yScale) V
// This inverts that for the given O, P, Q.
static void LT1Ed_OPQToMapping(const LTVector &normal, float dist, const LTVector &O, const LTVector &Pin, const LTVector &Qin, float map[5])
{
	LTVector U, V;
	LT1Ed_GetBaseAxes(normal, U, V);

	// Processor.exe only uses the parts of P and Q in its base plane (U, V).
	// DEdit's P and Q can point anywhere (eg. a planar projection on a slope),
	// so fold the part along the dominant axis W into U and V: on the poly's
	// plane, W is a linear function of the U and V coordinates.
	LTVector W = U.Cross(V);
	float nW = normal.Dot(W);
	if((float)fabs(nW) < 1e-6f)
		nW = 1e-6f;
	float kU = normal.Dot(U) / nW, kV = normal.Dot(V) / nW;

	float pW = Pin.Dot(W), qW = Qin.Dot(W);
	LTVector P = U * (Pin.Dot(U) - pW * kU) + V * (Pin.Dot(V) - pW * kV);
	LTVector Q = U * (Qin.Dot(U) - qW * kU) + V * (Qin.Dot(V) - qW * kV);
	float cP = pW * dist / nW;	// constant part of pt.P on the plane
	float cQ = qW * dist / nW;

	float a = P.Dot(U);
	float b = P.Dot(V);
	float pLen = (float)sqrt(a*a + b*b);

	float rot = 0.0f;
	float xScale = 1.0f;
	if(pLen > 1e-6f)
	{
		rot = (float)atan2(-b, a);
		xScale = 1.0f / pLen;
	}

	float c = (float)cos(rot);
	float s = (float)sin(rot);

	LTVector qDir = U * s + V * c;
	float qd = Q.Dot(qDir);
	float yScale = ((float)fabs(qd) > 1e-6f) ? (1.0f / qd) : 1.0f;

	LTVector newP = (U * c - V * s) * (1.0f / xScale);
	LTVector newQ = qDir * (1.0f / yScale);

	// Find x', y' so that (x'U + y'V) projects onto P and Q the same way O does.
	float tp = O.Dot(Pin) - cP;
	float tq = O.Dot(Qin) - cQ;
	float m00 = U.Dot(newP), m01 = V.Dot(newP);
	float m10 = U.Dot(newQ), m11 = V.Dot(newQ);
	float det = m00*m11 - m01*m10;

	float xo = 0.0f, yo = 0.0f;
	if((float)fabs(det) > 1e-9f)
	{
		xo = ( tp*m11 - m01*tq) / det;
		yo = (m00*tq  - tp*m10) / det;
	}

	map[0] = xScale;
	map[1] = yScale;
	map[2] = xo * xScale;
	map[3] = yo * yScale;
	map[4] = rot;
}


// ------------------------------------------------------------------
// Brushes
// ------------------------------------------------------------------

static void LT1Ed_WriteBrush(LT1EdContext &ctx, CEditBrush *pBrush)
{
	CLT1EdBuffer &buf = ctx.m_Buf;
	uint32 i, j;

	buf.U8(255); buf.U8(255); buf.U8(255);	// brush color

	buf.U32(pBrush->m_Points.GetSize());
	for(i=0; i < pBrush->m_Points.GetSize(); i++)
	{
		const LTVector &pt = pBrush->m_Points[i];
		buf.Vec(pt);
	}

	// Count the polies that will actually be written.
	uint32 nPolies = 0;
	for(i=0; i < pBrush->m_Polies.GetSize(); i++)
	{
		if(pBrush->m_Polies[i] && pBrush->m_Polies[i]->NumVerts() >= 3)
			nPolies++;
		else
			ctx.m_nSkippedPolies++;
	}

	buf.U32(nPolies);
	for(i=0; i < pBrush->m_Polies.GetSize(); i++)
	{
		CEditPoly *pPoly = pBrush->m_Polies[i];
		if(!pPoly || pPoly->NumVerts() < 3)
			continue;

		buf.U32(pPoly->NumVerts());
		for(j=0; j < pPoly->NumVerts(); j++)
			buf.U16((uint16)pPoly->Index(j));

		LTVector normal = pPoly->Normal();
		buf.Vec(normal);
		buf.F32(pPoly->Dist());

		CTexturedPlane &tex = pPoly->GetTexture(0);
		float map[5];
		LT1Ed_OPQToMapping(normal, pPoly->Dist(), tex.GetO(), tex.GetP(), tex.GetQ(), map);
		for(j=0; j < 5; j++)
			buf.F32(map[j]);
		buf.U32(0);

		buf.Str16(tex.m_pTextureName ? tex.m_pTextureName : "");
		buf.U32(0);
		buf.U8(0); buf.U8(0); buf.U8(0);	// shade
	}
}


// ------------------------------------------------------------------
// Properties and nodes
// ------------------------------------------------------------------

static void LT1Ed_WriteProp(CLT1EdBuffer &buf, CBaseProp *pProp)
{
	buf.Str16(pProp->GetName());
	buf.U8((uint8)pProp->GetType());
	buf.U32((uint32)pProp->GetFlags());

	switch(pProp->GetType())
	{
		case LT_PT_STRING:
		{
			const char *pStr = ((CStringProp*)pProp)->GetString();
			uint32 len = pStr ? (uint32)strlen(pStr) : 0;
			buf.U16((uint16)(2 + len));
			buf.Str16(pStr);
		}
		break;

		case LT_PT_VECTOR:
		case LT_PT_COLOR:
			buf.U16(12);
			buf.Vec(((CVectorProp*)pProp)->m_Vector);
		break;

		case LT_PT_REAL:
			buf.U16(4);
			buf.F32(((CRealProp*)pProp)->m_Value);
		break;

		// LithTech 1.0 stores flags and long ints as floats too.
		case LT_PT_FLAGS:
		case LT_PT_LONGINT:
			buf.U16(4);
			buf.F32(((CRealProp*)pProp)->m_Value);
		break;

		case LT_PT_BOOL:
			buf.U16(1);
			buf.U8(((CBoolProp*)pProp)->m_Value ? 1 : 0);
		break;

		case LT_PT_ROTATION:
		{
			// LithTech 1.0 .ed files store Euler angles plus a trailing 1.0.
			const LTVector &euler = ((CRotationProp*)pProp)->GetEulerAngles();
			buf.U16(16);
			buf.F32(euler.x);
			buf.F32(euler.y);
			buf.F32(euler.z);
			buf.F32(1.0f);
		}
		break;

		default:
			buf.U16(0);
		break;
	}
}

static void LT1Ed_WriteNodeData(LT1EdContext &ctx, const char *pClassName, CPropList *pProps,
	uint32 flags, const char *pDisplayName)
{
	CLT1EdBuffer &buf = ctx.m_Buf;

	uint32 lenPos = buf.Pos();
	buf.U16(0);

	buf.Str16(pClassName);
	uint32 nProps = pProps ? pProps->GetSize() : 0;
	buf.U32(nProps);
	for(uint32 i=0; i < nProps; i++)
		LT1Ed_WriteProp(buf, pProps->GetAt(i));

	buf.PatchU16(lenPos, (uint16)(buf.Pos() - lenPos - 2));

	buf.U32(ctx.m_NextID++);
	buf.U32(flags);
	buf.Str16(pDisplayName);
}

static bool LT1Ed_IsWritable(CWorldNode *pNode)
{
	return pNode->GetType() != Node_PrefabRef;
}

static void LT1Ed_WriteNode(LT1EdContext &ctx, CWorldNode *pNode, bool bRoot)
{
	CLT1EdBuffer &buf = ctx.m_Buf;

	if(!bRoot)
	{
		switch(pNode->GetType())
		{
			case Node_Brush:
			{
				buf.U32(LT1ED_NODE_BRUSH);
				std::map<CEditBrush*, uint32>::iterator it = ctx.m_BrushIndex.find(pNode->AsBrush());
				buf.U32(it != ctx.m_BrushIndex.end() ? it->second : 0);
			}
			break;

			case Node_Object:
				buf.U32(LT1ED_NODE_OBJECT);
			break;

			default:
				buf.U32(LT1ED_NODE_NULL);
			break;
		}
	}

	// Children.
	uint32 countPos = buf.Pos();
	buf.U16(0);
	uint16 nChildren = 0;
	for(GPOS pos=pNode->m_Children; pos; )
	{
		CWorldNode *pChild = pNode->m_Children.GetNext(pos);
		if(!LT1Ed_IsWritable(pChild))
		{
			ctx.m_nSkippedPrefabs++;
			continue;
		}

		LT1Ed_WriteNode(ctx, pChild, false);
		nChildren++;
	}
	buf.PatchU16(countPos, nChildren);

	// Node data.
	if(bRoot)
	{
		LT1Ed_WriteNodeData(ctx, "", NULL, LT1ED_NODEFLAG_ROOT, "");
	}
	else if(pNode->GetType() == Node_Null)
	{
		LT1Ed_WriteNodeData(ctx, "", NULL, 0, pNode->GetNodeLabel() ? pNode->GetNodeLabel() : "");
	}
	else
	{
		const char *pClass = (pNode->GetType() == Node_Brush) ? "Brush" : pNode->GetClassName();
		LT1Ed_WriteNodeData(ctx, pClass, &pNode->m_PropList, 0, pNode->GetName());
	}
}


// ------------------------------------------------------------------
// Entry point
// ------------------------------------------------------------------

bool LT1Ed_WriteFile(CEditRegion *pRegion, const char *pFilename, CString &sError, CString &sWarnings)
{
	LT1EdContext ctx;
	ctx.m_NextID = 1;
	ctx.m_nSkippedPrefabs = 0;
	ctx.m_nSkippedPolies = 0;

	CLT1EdBuffer &buf = ctx.m_Buf;

	// Header.
	buf.U32(LT1ED_VERSION);
	buf.U8(0);	// not compressed

	const char *pInfo = pRegion->m_pInfoString ? pRegion->m_pInfoString : "";
	buf.U32((uint32)strlen(pInfo));
	buf.Bytes(pInfo, (uint32)strlen(pInfo));
	for(uint32 i=0; i < 8; i++)
		buf.U32(0);

	// Brushes, in region order.
	uint32 nBrushes = 0;
	buf.U32(0);
	uint32 brushCountPos = buf.Pos() - 4;
	for(LPOS pos=pRegion->m_Brushes; pos; )
	{
		CEditBrush *pBrush = pRegion->m_Brushes.GetNext(pos);
		ctx.m_BrushIndex[pBrush] = nBrushes++;
		LT1Ed_WriteBrush(ctx, pBrush);
	}
	buf.PatchU32(brushCountPos, nBrushes);

	// Node tree.
	LT1Ed_WriteNode(ctx, pRegion->GetRootNode(), true);

	// Save.
	FILE *fp = fopen(pFilename, "wb");
	if(!fp)
	{
		sError.Format("Unable to open %s for writing.", pFilename);
		return false;
	}

	size_t nWritten = fwrite(&buf.m_Data[0], 1, buf.m_Data.size(), fp);
	fclose(fp);

	if(nWritten != buf.m_Data.size())
	{
		sError.Format("Unable to write %s.", pFilename);
		return false;
	}

	sWarnings.Empty();
	if(ctx.m_nSkippedPrefabs)
	{
		CString s;
		s.Format("%u prefab reference(s) were skipped (LithTech 1.0 has no prefabs).\n", ctx.m_nSkippedPrefabs);
		sWarnings += s;
	}
	if(ctx.m_nSkippedPolies)
	{
		CString s;
		s.Format("%u degenerate poly(s) were skipped.\n", ctx.m_nSkippedPolies);
		sWarnings += s;
	}

	return true;
}
