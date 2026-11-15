//------------------------------------------------------------------
//
//	FILE	  : LightPreview.h
//
//	PURPOSE	  : In-editor lighting preview (Ctrl+L). Bakes lightmaps for the
//	            region's polies on the CPU and hands them to the D3D view.
//
//------------------------------------------------------------------

#ifndef __LIGHTPREVIEW_H__
#define __LIGHTPREVIEW_H__

class CEditRegion;
class CEditPoly;
class D3DRender;
struct IDirect3DTexture2;

namespace LightPreview
{
	// Bakes the lighting for this region and turns the preview on.
	void Compute(CEditRegion *pRegion);

	// Is the preview showing for this region?
	bool IsEnabled(CEditRegion *pRegion);

	// Turns the preview on (baking it) or off.
	void Toggle(CEditRegion *pRegion);

	// Turns the preview off and frees its data.
	void Clear();

	// Lightmap texture for a poly and per-vertex lightmap UVs (2 floats per
	// vertex), or NULL when the poly has none.
	IDirect3DTexture2* GetPolyLightmap(D3DRender *pRender, CEditRegion *pRegion, CEditPoly *pPoly, float *pUV);

	// Frees the textures created for a renderer.
	void ReleaseRender(D3DRender *pRender);
}

#endif
