//------------------------------------------------------------------
//
//	FILE	  : LightPreview.cpp
//
//	PURPOSE	  : See LightPreview.h.
//
//	Lighting model (an approximation of the LithTech 1.0 Processor.exe):
//	  - Light:    colour fades linearly from LightColor at the light to
//	              OuterColor at LightRadius, times BrightScale.
//	  - DirLight: same with InnerColor/OuterColor, limited to a cone of
//	              FOV degrees around the light's forward vector.
//	  - Lambert (N.L) on the receiving surface.
//	  - ClipLight = true lights are blocked by brushes (shadows).
//	  - The world's "AmbientLight r g b" (info string) is added everywhere.
//
//------------------------------------------------------------------

#include "bdefs.h"
#include "LightPreview.h"
#include "editregion.h"
#include "editbrush.h"
#include "editpoly.h"
#include "editobjects.h"
#include "worldnode.h"
#include "proplist.h"
#include "geomroutines.h"
#include "LT1EdWriter.h"
#include "draw_d3d.h"
#include "d3d_texturemgr.h"

#include <vector>
#include <map>
#include <math.h>


#define LP_PAGE_SIZE		256
#define LP_DEFAULT_GRID		16.0f
#define LP_MAX_LM_SIZE		62		// texels per side, so a block plus border fits easily
#define LP_SURFACE_OFFSET	1.0f	// samples are lifted off the surface by this much


// ------------------------------------------------------------------
// Data
// ------------------------------------------------------------------

struct LPLight
{
	LTVector	m_vPos;
	float		m_fRadius;
	LTVector	m_vInner;
	LTVector	m_vOuter;
	float		m_fBright;
	bool		m_bShadows;
	bool		m_bDirectional;
	LTVector	m_vForward;
	float		m_fCosHalfFOV;
};

struct LPPoly
{
	LTVector	m_vNormal;
	float		m_fDist;
	uint32		m_nFirstVert;
	uint32		m_nNumVerts;
	LTVector	m_vMin, m_vMax;
	bool		m_bOccluder;
};

struct LPLightmap
{
	uint32		m_nPage;
	uint32		m_nX, m_nY;		// top-left of the lightmap inside the page
	LTVector	m_vU, m_vV;		// in-plane axes
	float		m_fSMin, m_fTMin;
	float		m_fStep;
};

struct LPRenderTextures
{
	D3DRender							*m_pRender;
	std::vector<LPDIRECTDRAWSURFACE4>	m_Surfaces;
	std::vector<IDirect3DTexture2*>		m_Textures;
};

static CEditRegion							*g_pRegion = NULL;
static std::map<CEditPoly*, uint32>			g_PolyMap;
static std::vector<LPLightmap>				g_Lightmaps;
static std::vector< std::vector<uint8> >	g_Pages;		// RGB, LP_PAGE_SIZE^2 * 3
static std::vector<LPRenderTextures*>		g_Renders;


// ------------------------------------------------------------------
// Property helpers
// ------------------------------------------------------------------

static CBaseProp* LP_FindProp(CWorldNode *pNode, const char *pName, int type1, int type2 = -1)
{
	CBaseProp *pProp = pNode->m_PropList.GetProp(pName);
	if(pProp && (pProp->GetType() == type1 || pProp->GetType() == type2))
		return pProp;
	return NULL;
}

static float LP_GetReal(CWorldNode *pNode, const char *pName, float fDefault)
{
	CBaseProp *pProp = LP_FindProp(pNode, pName, LT_PT_REAL, LT_PT_LONGINT);
	return pProp ? ((CRealProp*)pProp)->m_Value : fDefault;
}

static LTVector LP_GetVec(CWorldNode *pNode, const char *pName, const LTVector &vDefault)
{
	CBaseProp *pProp = LP_FindProp(pNode, pName, LT_PT_VECTOR, LT_PT_COLOR);
	return pProp ? ((CVectorProp*)pProp)->m_Vector : vDefault;
}

static bool LP_GetBool(CWorldNode *pNode, const char *pName, bool bDefault)
{
	CBaseProp *pProp = LP_FindProp(pNode, pName, LT_PT_BOOL);
	return pProp ? (((CBoolProp*)pProp)->m_Value != 0) : bDefault;
}


// ------------------------------------------------------------------
// Ray casting against the level's polies through a uniform grid
// ------------------------------------------------------------------

class CLPRayGrid
{
public:
	std::vector<LPPoly>				*m_pPolies;
	std::vector<LTVector>			*m_pVerts;

	LTVector						m_vMin;
	float							m_fCell;
	int								m_nDim[3];
	std::vector< std::vector<uint32> >	m_Cells;
	std::vector<uint32>				m_Stamps;
	uint32							m_nStamp;

	void Build(std::vector<LPPoly> &polies, std::vector<LTVector> &verts)
	{
		m_pPolies = &polies;
		m_pVerts = &verts;
		m_nStamp = 0;
		m_Stamps.assign(polies.size(), 0);

		LTVector vMin(1e30f, 1e30f, 1e30f), vMax(-1e30f, -1e30f, -1e30f);
		uint32 i;
		for(i=0; i < polies.size(); i++)
		{
			if(!polies[i].m_bOccluder)
				continue;
			for(int k=0; k < 3; k++)
			{
				if(polies[i].m_vMin[k] < vMin[k]) vMin[k] = polies[i].m_vMin[k];
				if(polies[i].m_vMax[k] > vMax[k]) vMax[k] = polies[i].m_vMax[k];
			}
		}
		if(vMin.x > vMax.x)
		{
			vMin.Init(0, 0, 0);
			vMax.Init(1, 1, 1);
		}

		vMin -= LTVector(1, 1, 1);
		vMax += LTVector(1, 1, 1);
		m_vMin = vMin;

		float fLargest = vMax.x - vMin.x;
		if(vMax.y - vMin.y > fLargest) fLargest = vMax.y - vMin.y;
		if(vMax.z - vMin.z > fLargest) fLargest = vMax.z - vMin.z;

		m_fCell = 128.0f;
		if(fLargest / m_fCell > 96.0f)
			m_fCell = fLargest / 96.0f;

		for(int k=0; k < 3; k++)
		{
			m_nDim[k] = (int)((vMax[k] - vMin[k]) / m_fCell) + 1;
			if(m_nDim[k] < 1) m_nDim[k] = 1;
		}

		m_Cells.clear();
		m_Cells.resize((size_t)m_nDim[0] * m_nDim[1] * m_nDim[2]);

		for(i=0; i < polies.size(); i++)
		{
			const LPPoly &poly = polies[i];
			if(!poly.m_bOccluder)
				continue;

			int c0[3], c1[3];
			for(int k=0; k < 3; k++)
			{
				c0[k] = Clamp((int)((poly.m_vMin[k] - m_vMin[k]) / m_fCell), m_nDim[k]);
				c1[k] = Clamp((int)((poly.m_vMax[k] - m_vMin[k]) / m_fCell), m_nDim[k]);
			}

			for(int z=c0[2]; z <= c1[2]; z++)
				for(int y=c0[1]; y <= c1[1]; y++)
					for(int x=c0[0]; x <= c1[0]; x++)
						m_Cells[CellIndex(x, y, z)].push_back(i);
		}
	}

	// True if anything (other than skipPoly) is between a and b.
	bool Blocked(const LTVector &a, const LTVector &b, uint32 skipPoly)
	{
		if(++m_nStamp == 0)
		{
			m_Stamps.assign(m_Stamps.size(), 0);
			m_nStamp = 1;
		}

		LTVector dir = b - a;

		// 3D DDA from a to b.
		float pos[3], d[3];
		int cell[3], step[3];
		float tMax[3], tDelta[3];
		for(int k=0; k < 3; k++)
		{
			pos[k] = (a[k] - m_vMin[k]) / m_fCell;
			d[k] = dir[k] / m_fCell;
			cell[k] = (int)floor(pos[k]);
			if(d[k] > 0.0f)
			{
				step[k] = 1;
				tDelta[k] = 1.0f / d[k];
				tMax[k] = ((float)(cell[k] + 1) - pos[k]) / d[k];
			}
			else if(d[k] < 0.0f)
			{
				step[k] = -1;
				tDelta[k] = -1.0f / d[k];
				tMax[k] = (pos[k] - (float)cell[k]) / -d[k];
			}
			else
			{
				step[k] = 0;
				tDelta[k] = 1e30f;
				tMax[k] = 1e30f;
			}
		}

		for(int nIter=0; nIter < 4096; nIter++)
		{
			if(cell[0] >= 0 && cell[1] >= 0 && cell[2] >= 0 &&
				cell[0] < m_nDim[0] && cell[1] < m_nDim[1] && cell[2] < m_nDim[2])
			{
				std::vector<uint32> &list = m_Cells[CellIndex(cell[0], cell[1], cell[2])];
				for(uint32 i=0; i < list.size(); i++)
				{
					uint32 iPoly = list[i];
					if(iPoly == skipPoly || m_Stamps[iPoly] == m_nStamp)
						continue;
					m_Stamps[iPoly] = m_nStamp;

					if(SegmentHitsPoly(a, b, (*m_pPolies)[iPoly]))
						return true;
				}
			}

			// Next cell.
			int axis = 0;
			if(tMax[1] < tMax[axis]) axis = 1;
			if(tMax[2] < tMax[axis]) axis = 2;
			if(tMax[axis] > 1.0f)
				break;
			cell[axis] += step[axis];
			tMax[axis] += tDelta[axis];
		}

		return false;
	}

private:

	int CellIndex(int x, int y, int z) const
	{
		return (z * m_nDim[1] + y) * m_nDim[0] + x;
	}

	static int Clamp(int v, int dim)
	{
		if(v < 0) return 0;
		if(v >= dim) return dim - 1;
		return v;
	}

	bool SegmentHitsPoly(const LTVector &a, const LTVector &b, const LPPoly &poly) const
	{
		float da = poly.m_vNormal.Dot(a) - poly.m_fDist;
		float db = poly.m_vNormal.Dot(b) - poly.m_fDist;
		if((da > 0.0f && db > 0.0f) || (da < 0.0f && db < 0.0f))
			return false;

		float denom = da - db;
		if((float)fabs(denom) < 1e-6f)
			return false;

		float t = da / denom;
		if(t <= 0.001f || t >= 0.999f)
			return false;

		LTVector hit = a + (b - a) * t;

		// Inside test that doesn't care about winding direction.
		const std::vector<LTVector> &verts = *m_pVerts;
		bool bPos = false, bNeg = false;
		for(uint32 i=0; i < poly.m_nNumVerts; i++)
		{
			const LTVector &v0 = verts[poly.m_nFirstVert + i];
			const LTVector &v1 = verts[poly.m_nFirstVert + ((i + 1) % poly.m_nNumVerts)];
			float side = (v1 - v0).Cross(hit - v0).Dot(poly.m_vNormal);
			if(side > 0.01f) bPos = true;
			else if(side < -0.01f) bNeg = true;
			if(bPos && bNeg)
				return false;
		}
		return true;
	}
};


// ------------------------------------------------------------------
// Render textures
// ------------------------------------------------------------------

static void LP_ReleaseRenderTextures(LPRenderTextures *pRT)
{
	for(uint32 i=0; i < pRT->m_Textures.size(); i++)
		if(pRT->m_Textures[i])
			pRT->m_Textures[i]->Release();
	for(uint32 j=0; j < pRT->m_Surfaces.size(); j++)
		if(pRT->m_Surfaces[j])
			pRT->m_Surfaces[j]->Release();
	pRT->m_Textures.clear();
	pRT->m_Surfaces.clear();
}

static void LP_ReleaseAllRenders()
{
	for(uint32 i=0; i < g_Renders.size(); i++)
	{
		LP_ReleaseRenderTextures(g_Renders[i]);
		delete g_Renders[i];
	}
	g_Renders.clear();
}

static bool LP_UploadPage(D3DRender *pRender, const std::vector<uint8> &page, LPDIRECTDRAWSURFACE4 pSurface)
{
	DDSURFACEDESC2 desc;
	memset(&desc, 0, sizeof(desc));
	desc.dwSize = sizeof(desc);

	if(pSurface->Lock(NULL, &desc, DDLOCK_WAIT, NULL) != DD_OK)
		return false;

	TextureFormat *pFmt = pRender->m_pTextureFormat;
	uint16 *pLine = (uint16*)desc.lpSurface;
	uint32 pitch = desc.lPitch >> 1;

	for(uint32 y=0; y < LP_PAGE_SIZE; y++)
	{
		const uint8 *pIn = &page[y * LP_PAGE_SIZE * 3];
		for(uint32 x=0; x < LP_PAGE_SIZE; x++)
		{
			uint32 r = pIn[0], g = pIn[1], b = pIn[2];
			pLine[x] = (uint16)(((r >> pFmt->m_RRightShift) << pFmt->m_RLeftShift) |
								((g >> pFmt->m_GRightShift) << pFmt->m_GLeftShift) |
								((b >> pFmt->m_BRightShift) << pFmt->m_BLeftShift));
			pIn += 3;
		}
		pLine += pitch;
	}

	pSurface->Unlock(NULL);
	return true;
}

static LPRenderTextures* LP_GetRenderTextures(D3DRender *pRender)
{
	for(uint32 i=0; i < g_Renders.size(); i++)
		if(g_Renders[i]->m_pRender == pRender)
			return g_Renders[i];

	LPRenderTextures *pRT = new LPRenderTextures;
	pRT->m_pRender = pRender;

	if(pRender->m_pTextureFormat)
	{
		for(uint32 i=0; i < g_Pages.size(); i++)
		{
			IDirect3DTexture2 *pTexture = NULL;
			LPDIRECTDRAWSURFACE4 pSurface = d3d_CreateTextureSurface(pRender, LP_PAGE_SIZE, LP_PAGE_SIZE, 0);
			if(pSurface)
			{
				pSurface->QueryInterface(IID_IDirect3DTexture2, (void**)&pTexture);
				if(!pTexture || !LP_UploadPage(pRender, g_Pages[i], pSurface))
				{
					if(pTexture)
						pTexture->Release();
					pTexture = NULL;
					pSurface->Release();
					pSurface = NULL;
				}
			}
			pRT->m_Surfaces.push_back(pSurface);
			pRT->m_Textures.push_back(pTexture);
		}
	}

	g_Renders.push_back(pRT);
	return pRT;
}


// ------------------------------------------------------------------
// Lighting
// ------------------------------------------------------------------

static void LP_GatherLights(CEditRegion *pRegion, std::vector<LPLight> &lights)
{
	for(uint32 i=0; i < pRegion->m_Objects.GetSize(); i++)
	{
		CBaseEditObj *pObj = pRegion->m_Objects[i];
		const char *pClass = pObj->GetClassName();
		bool bDir = (stricmp(pClass, "DirLight") == 0);
		if(!bDir && stricmp(pClass, "Light") != 0)
			continue;

		LPLight light;
		light.m_vPos = pObj->GetPos();
		light.m_fRadius = LP_GetReal(pObj, "LightRadius", 300.0f);
		light.m_vInner = LP_GetVec(pObj, bDir ? "InnerColor" : "LightColor", LTVector(255.0f, 255.0f, 255.0f));
		light.m_vOuter = LP_GetVec(pObj, "OuterColor", LTVector(0.0f, 0.0f, 0.0f));
		light.m_fBright = LP_GetReal(pObj, "BrightScale", 1.0f);
		light.m_bShadows = LP_GetBool(pObj, "ClipLight", true);
		light.m_bDirectional = bDir;
		light.m_vForward.Init(0.0f, 0.0f, 1.0f);
		light.m_fCosHalfFOV = -1.0f;

		if(bDir)
		{
			CBaseProp *pRot = LP_FindProp(pObj, "Rotation", LT_PT_ROTATION);
			if(pRot)
			{
				LTVector vRight, vUp;
				gr_GetEulerVectors(((CRotationProp*)pRot)->GetEulerAngles(), vRight, vUp, light.m_vForward);
			}
			float fFOV = LP_GetReal(pObj, "FOV", 90.0f);
			light.m_fCosHalfFOV = (float)cos(MATH_DEGREES_TO_RADIANS(fFOV * 0.5f));
		}

		if(light.m_fRadius > 0.0f)
			lights.push_back(light);
	}
}

static LTVector LP_GetAmbient(CEditRegion *pRegion)
{
	LTVector vAmbient(0.0f, 0.0f, 0.0f);
	const char *pInfo = pRegion->m_pInfoString;
	if(pInfo)
	{
		const char *pKey = strstr(pInfo, "AmbientLight");
		if(pKey)
		{
			float r = 0.0f, g = 0.0f, b = 0.0f;
			if(sscanf(pKey + strlen("AmbientLight"), "%f %f %f", &r, &g, &b) == 3)
				vAmbient.Init(r, g, b);
		}
	}
	return vAmbient;
}

// Shelf packer for the pages.
struct LPPacker
{
	uint32	m_nX, m_nY, m_nRowHeight;

	void Alloc(uint32 w, uint32 h, uint32 &page, uint32 &x, uint32 &y)
	{
		// Blocks get a one texel border on every side.
		w += 2;
		h += 2;

		if(g_Pages.empty())
			NewPage();

		if(m_nX + w > LP_PAGE_SIZE)
		{
			m_nX = 0;
			m_nY += m_nRowHeight;
			m_nRowHeight = 0;
		}
		if(m_nY + h > LP_PAGE_SIZE)
			NewPage();

		page = (uint32)g_Pages.size() - 1;
		x = m_nX + 1;
		y = m_nY + 1;

		m_nX += w;
		if(h > m_nRowHeight)
			m_nRowHeight = h;
	}

	void NewPage()
	{
		g_Pages.push_back(std::vector<uint8>(LP_PAGE_SIZE * LP_PAGE_SIZE * 3, 255));
		m_nX = m_nY = m_nRowHeight = 0;
	}
};


void LightPreview::Compute(CEditRegion *pRegion)
{
	Clear();
	if(!pRegion)
		return;

	// Collect the geometry.
	std::vector<LPPoly>		polies;
	std::vector<LTVector>	verts;
	std::vector<CEditPoly*>	editPolies;
	std::vector<bool>		wantsLightmap;

	for(LPOS pos=pRegion->m_Brushes; pos; )
	{
		CEditBrush *pBrush = pRegion->m_Brushes.GetNext(pos);

		bool bNonexistant	= LP_GetBool(pBrush, "Nonexistant", false);
		bool bInvisible		= LP_GetBool(pBrush, "Invisible", false);
		bool bFullBright	= LP_GetBool(pBrush, "FullyBright", false);
		bool bSky			= LP_GetBool(pBrush, "SkyPortal", false);

		for(uint32 i=0; i < pBrush->m_Polies.GetSize(); i++)
		{
			CEditPoly *pPoly = pBrush->m_Polies[i];
			if(!pPoly || pPoly->NumVerts() < 3)
				continue;

			LPPoly poly;
			poly.m_vNormal = pPoly->Normal();
			poly.m_fDist = pPoly->Dist();
			poly.m_nFirstVert = (uint32)verts.size();
			poly.m_nNumVerts = pPoly->NumVerts();
			poly.m_vMin.Init(1e30f, 1e30f, 1e30f);
			poly.m_vMax.Init(-1e30f, -1e30f, -1e30f);
			for(uint32 v=0; v < pPoly->NumVerts(); v++)
			{
				LTVector pt = pPoly->Pt(v);
				verts.push_back(pt);
				for(int k=0; k < 3; k++)
				{
					if(pt[k] < poly.m_vMin[k]) poly.m_vMin[k] = pt[k];
					if(pt[k] > poly.m_vMax[k]) poly.m_vMax[k] = pt[k];
				}
			}
			poly.m_bOccluder = !bNonexistant && !bInvisible && !bSky;

			polies.push_back(poly);
			editPolies.push_back(pPoly);
			wantsLightmap.push_back(!bNonexistant && !bInvisible && !bFullBright && !bSky);
		}
	}

	std::vector<LPLight> lights;
	LP_GatherLights(pRegion, lights);
	LTVector vAmbient = LP_GetAmbient(pRegion);

	CLPRayGrid grid;
	grid.Build(polies, verts);

	LPPacker packer;
	packer.m_nX = packer.m_nY = packer.m_nRowHeight = 0;

	std::vector<uint32> nearLights;
	std::vector<LTVector> texels;

	for(uint32 iPoly=0; iPoly < polies.size(); iPoly++)
	{
		if(!wantsLightmap[iPoly])
			continue;

		const LPPoly &poly = polies[iPoly];
		const LTVector &n = poly.m_vNormal;

		// In-plane axes based on the texture projection axes.
		LTVector U, V;
		LT1Ed_GetBaseAxes(n, U, V);
		U = U - n * n.Dot(U);
		if(U.Mag() < 0.01f)
			U = V - n * n.Dot(V);
		U.Norm();
		V = n.Cross(U);
		V.Norm();

		float sMin = 1e30f, sMax = -1e30f, tMin = 1e30f, tMax = -1e30f;
		for(uint32 v=0; v < poly.m_nNumVerts; v++)
		{
			const LTVector &pt = verts[poly.m_nFirstVert + v];
			float s = pt.Dot(U), t = pt.Dot(V);
			if(s < sMin) sMin = s;
			if(s > sMax) sMax = s;
			if(t < tMin) tMin = t;
			if(t > tMax) tMax = t;
		}

		float fStep = LP_DEFAULT_GRID;
		float fExtent = (sMax - sMin) > (tMax - tMin) ? (sMax - sMin) : (tMax - tMin);
		if(fExtent / fStep > (float)(LP_MAX_LM_SIZE - 1))
			fStep = fExtent / (float)(LP_MAX_LM_SIZE - 1);

		uint32 w = (uint32)ceil((sMax - sMin) / fStep) + 1;
		uint32 h = (uint32)ceil((tMax - tMin) / fStep) + 1;
		if(w < 2) w = 2;
		if(h < 2) h = 2;
		if(w > LP_MAX_LM_SIZE) w = LP_MAX_LM_SIZE;
		if(h > LP_MAX_LM_SIZE) h = LP_MAX_LM_SIZE;

		// Lights that can reach this poly.
		nearLights.clear();
		LTVector vCenter = (poly.m_vMin + poly.m_vMax) * 0.5f;
		float fPolyRadius = (poly.m_vMax - poly.m_vMin).Mag() * 0.5f;
		for(uint32 l=0; l < lights.size(); l++)
		{
			const LPLight &light = lights[l];
			float fPlaneDist = n.Dot(light.m_vPos) - poly.m_fDist;
			if(fPlaneDist <= 0.0f)
				continue;	// behind the poly
			if((light.m_vPos - vCenter).Mag() > light.m_fRadius + fPolyRadius)
				continue;
			nearLights.push_back(l);
		}

		// Light every texel.
		texels.resize(w * h);
		LTVector vPlaneBase = n * poly.m_fDist;
		for(uint32 y=0; y < h; y++)
		{
			for(uint32 x=0; x < w; x++)
			{
				LTVector vSample = vPlaneBase + U * (sMin + x * fStep) + V * (tMin + y * fStep) + n * LP_SURFACE_OFFSET;
				LTVector vColor = vAmbient;

				for(uint32 l=0; l < nearLights.size(); l++)
				{
					const LPLight &light = lights[nearLights[l]];
					LTVector vToLight = light.m_vPos - vSample;
					float fDist = vToLight.Mag();
					if(fDist >= light.m_fRadius || fDist < 0.001f)
						continue;

					LTVector vDir = vToLight / fDist;
					float fNDotL = n.Dot(vDir);
					if(fNDotL <= 0.0f)
						continue;

					// LithTech 1.0 doesn't attenuate by angle, only by distance.
					float fScale = light.m_fBright;

					if(light.m_bDirectional)
					{
						float fCos = light.m_vForward.Dot(-vDir);
						if(fCos < light.m_fCosHalfFOV)
							continue;
						// Soften the last part of the cone edge.
						float fEdge = (fCos - light.m_fCosHalfFOV) / ((1.0f - light.m_fCosHalfFOV) * 0.25f + 1e-4f);
						if(fEdge < 1.0f)
							fScale *= fEdge;
					}

					if(light.m_bShadows && grid.Blocked(vSample, light.m_vPos, iPoly))
						continue;

					float fT = fDist / light.m_fRadius;
					vColor += (light.m_vInner * (1.0f - fT) + light.m_vOuter * fT) * fScale;
				}

				texels[y * w + x] = vColor;
			}
		}

		// Pack it.
		LPLightmap lm;
		packer.Alloc(w, h, lm.m_nPage, lm.m_nX, lm.m_nY);
		lm.m_vU = U;
		lm.m_vV = V;
		lm.m_fSMin = sMin;
		lm.m_fTMin = tMin;
		lm.m_fStep = fStep;

		std::vector<uint8> &page = g_Pages[lm.m_nPage];
		for(int by=-1; by <= (int)h; by++)
		{
			for(int bx=-1; bx <= (int)w; bx++)
			{
				// The border repeats the edge texels so filtering doesn't bleed.
				int sx = bx < 0 ? 0 : (bx >= (int)w ? (int)w - 1 : bx);
				int sy = by < 0 ? 0 : (by >= (int)h ? (int)h - 1 : by);
				const LTVector &c = texels[sy * w + sx];

				uint8 *pOut = &page[((lm.m_nY + by) * LP_PAGE_SIZE + (lm.m_nX + bx)) * 3];
				pOut[0] = (uint8)(c.x < 0.0f ? 0 : (c.x > 255.0f ? 255 : (int)c.x));
				pOut[1] = (uint8)(c.y < 0.0f ? 0 : (c.y > 255.0f ? 255 : (int)c.y));
				pOut[2] = (uint8)(c.z < 0.0f ? 0 : (c.z > 255.0f ? 255 : (int)c.z));
			}
		}

		g_PolyMap[editPolies[iPoly]] = (uint32)g_Lightmaps.size();
		g_Lightmaps.push_back(lm);
	}

	g_pRegion = pRegion;
}


// ------------------------------------------------------------------
// Interface
// ------------------------------------------------------------------

bool LightPreview::IsEnabled(CEditRegion *pRegion)
{
	return pRegion && pRegion == g_pRegion;
}

void LightPreview::Toggle(CEditRegion *pRegion)
{
	if(IsEnabled(pRegion))
		Clear();
	else
		Compute(pRegion);
}

void LightPreview::Clear()
{
	LP_ReleaseAllRenders();
	g_PolyMap.clear();
	g_Lightmaps.clear();
	g_Pages.clear();
	g_pRegion = NULL;
}

IDirect3DTexture2* LightPreview::GetPolyLightmap(D3DRender *pRender, CEditRegion *pRegion, CEditPoly *pPoly, float *pUV)
{
	if(!g_pRegion || pRegion != g_pRegion)
		return NULL;

	std::map<CEditPoly*, uint32>::iterator it = g_PolyMap.find(pPoly);
	if(it == g_PolyMap.end())
		return NULL;

	const LPLightmap &lm = g_Lightmaps[it->second];

	LPRenderTextures *pRT = LP_GetRenderTextures(pRender);
	if(lm.m_nPage >= pRT->m_Textures.size() || !pRT->m_Textures[lm.m_nPage])
		return NULL;

	const float fInvPage = 1.0f / (float)LP_PAGE_SIZE;
	for(uint32 i=0; i < pPoly->NumVerts(); i++)
	{
		const LTVector &pt = pPoly->Pt(i);
		float s = (pt.Dot(lm.m_vU) - lm.m_fSMin) / lm.m_fStep;
		float t = (pt.Dot(lm.m_vV) - lm.m_fTMin) / lm.m_fStep;
		pUV[i*2 + 0] = ((float)lm.m_nX + s + 0.5f) * fInvPage;
		pUV[i*2 + 1] = ((float)lm.m_nY + t + 0.5f) * fInvPage;
	}

	return pRT->m_Textures[lm.m_nPage];
}

void LightPreview::ReleaseRender(D3DRender *pRender)
{
	for(uint32 i=0; i < g_Renders.size(); i++)
	{
		if(g_Renders[i]->m_pRender == pRender)
		{
			LP_ReleaseRenderTextures(g_Renders[i]);
			delete g_Renders[i];
			g_Renders.erase(g_Renders.begin() + i);
			return;
		}
	}
}
