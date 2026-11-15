//------------------------------------------------------------------
//
//	FILE	  : Terrain.cpp
//
//	PURPOSE	  : Paintable terrain (see Terrain.h).
//
//	Layout of a terrain with N cells per side over the footprint
//	(x0, z0) - (x0 + sx, z0 + sz):
//	  - grid vertex (i, j), 0 <= i, j <= N, sits at
//	        x = x0 + sx * i / N,  z = z0 + sz * j / N
//	  - cell (i, j) holds two prisms, (i,j)(i+1,j)(i+1,j+1) and
//	    (i,j)(i+1,j+1)(i,j+1). Each prism brush has 6 points: 0-2 are
//	    the top (painted heights), 3-5 the flat base at baseY.
//	  - the brushes are found again by their top points, so undo and
//	    reordering in the node tree don't matter.
//
//------------------------------------------------------------------

#include "bdefs.h"
#include "dedit.h"
#include "resource.h"
#include "Terrain.h"
#include "regiondoc.h"
#include "regionview.h"
#include "projectbar.h"
#include "edithelpers.h"
#include "editregion.h"
#include "editbrush.h"
#include "editpoly.h"
#include "worldnode.h"
#include "node_ops.h"
#include "edit_actions.h"
#include "nodeview.h"
#include "texture.h"
#include "d_filemgr.h"
#include "editprojectmgr.h"
#include "drawbase.h"

// hover reticle state (used by the picking code below)
static bool			g_bHover = false;
static LTVector		g_vHover(0.0f, 0.0f, 0.0f);
static CWorldNode	*g_pHoverNode = NULL;

#include <vector>
#include <string>
#include <algorithm>
#include <math.h>
#include <stdio.h>

#define TERRAIN_TEX_SIZE	256
#define TERRAIN_NUM_LAYERS	4
#define TERRAIN_MIN_THICK	1.0f


//------------------------------------------------------------------
// Settings shared by the tool window and the painting code.
//------------------------------------------------------------------

static int		g_Mode		= Terrain::MODE_RAISE;
static float	g_Radius	= 128.0f;
static int		g_Strength	= 25;		// 1 - 100
static bool		g_bPaint	= true;
static CString	g_Layers[TERRAIN_NUM_LAYERS];


//------------------------------------------------------------------
// Terrain data
//------------------------------------------------------------------

struct TerrainInfo
{
	int					m_N;
	float				m_x0, m_z0, m_sx, m_sz, m_BaseY;
	std::string			m_Layers[TERRAIN_NUM_LAYERS];
	std::vector<float>	m_Weights;	// (N+1)^2 * 4

	int		NumVerts() const	{ return (m_N + 1) * (m_N + 1); }
	float	GX(int i) const		{ return m_x0 + m_sx * (float)i / (float)m_N; }
	float	GZ(int j) const		{ return m_z0 + m_sz * (float)j / (float)m_N; }

	// Grid index of a world position (rounded), -1 if it's off the grid.
	int GridIndex(float x, float z) const
	{
		int i = (int)floor((x - m_x0) / m_sx * m_N + 0.5f);
		int j = (int)floor((z - m_z0) / m_sz * m_N + 0.5f);
		if(i < 0 || j < 0 || i > m_N || j > m_N)
			return -1;
		return j * (m_N + 1) + i;
	}
};

struct TerrainRef
{
	CWorldNode					*m_pNode;
	int							m_ID;
	TerrainInfo					m_Info;
	std::vector<CEditBrush*>	m_Brushes;
	std::vector<float>			m_Heights;
};


static CString Terrain_ProjectDir()
{
	CString dir = GetProject()->m_BaseProjectDir;
	if(dir.GetLength() > 0 && dir[dir.GetLength()-1] != '\\' && dir[dir.GetLength()-1] != '/')
		dir += "\\";
	return dir;
}

static CString Terrain_TextureName(int id)
{
	CString s;
	s.Format("Textures\\Terrainmesh\\%04d.dtx", id);
	return s;
}

static CString Terrain_SidecarPath(int id)
{
	CString s;
	s.Format("%sTextures\\Terrainmesh\\%04d.trn", (LPCTSTR)Terrain_ProjectDir(), id);
	return s;
}

static void Terrain_MakeDirs()
{
	CString dir = Terrain_ProjectDir() + "Textures";
	CreateDirectory(dir, NULL);
	dir += "\\Terrainmesh";
	CreateDirectory(dir, NULL);
}

static bool Terrain_SaveInfo(int id, const TerrainInfo &info)
{
	Terrain_MakeDirs();
	FILE *fp = fopen(Terrain_SidecarPath(id), "wt");
	if(!fp)
		return false;

	fprintf(fp, "LT1TERRAIN 1\n");
	fprintf(fp, "%d %f %f %f %f %f\n", info.m_N, info.m_x0, info.m_z0, info.m_sx, info.m_sz, info.m_BaseY);
	for(int l=0; l < TERRAIN_NUM_LAYERS; l++)
		fprintf(fp, "%s\n", info.m_Layers[l].empty() ? "-" : info.m_Layers[l].c_str());
	for(int v=0; v < info.NumVerts(); v++)
	{
		const float *w = &info.m_Weights[v * TERRAIN_NUM_LAYERS];
		fprintf(fp, "%.3f %.3f %.3f %.3f\n", w[0], w[1], w[2], w[3]);
	}
	fclose(fp);
	return true;
}

static bool Terrain_LoadInfo(int id, TerrainInfo &info)
{
	FILE *fp = fopen(Terrain_SidecarPath(id), "rt");
	if(!fp)
		return false;

	char line[512];
	bool bOK = false;
	if(fgets(line, sizeof(line), fp) && strncmp(line, "LT1TERRAIN", 10) == 0 &&
		fgets(line, sizeof(line), fp) &&
		sscanf(line, "%d %f %f %f %f %f", &info.m_N, &info.m_x0, &info.m_z0, &info.m_sx, &info.m_sz, &info.m_BaseY) == 6 &&
		info.m_N >= 1 && info.m_N <= 64 && info.m_sx > 0.0f && info.m_sz > 0.0f)
	{
		bOK = true;
		for(int l=0; l < TERRAIN_NUM_LAYERS && bOK; l++)
		{
			if(!fgets(line, sizeof(line), fp))
			{
				bOK = false;
				break;
			}
			char *p = line + strlen(line);
			while(p > line && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' '))
				*--p = 0;
			info.m_Layers[l] = strcmp(line, "-") == 0 ? "" : line;
		}

		info.m_Weights.assign(info.NumVerts() * TERRAIN_NUM_LAYERS, 0.0f);
		for(int v=0; v < info.NumVerts() && bOK; v++)
		{
			float *w = &info.m_Weights[v * TERRAIN_NUM_LAYERS];
			if(!fgets(line, sizeof(line), fp) || sscanf(line, "%f %f %f %f", &w[0], &w[1], &w[2], &w[3]) != 4)
			{
				// Missing weights: fall back to layer 1.
				for(; v < info.NumVerts(); v++)
					info.m_Weights[v * TERRAIN_NUM_LAYERS] = 1.0f;
				break;
			}
		}
	}

	fclose(fp);
	return bOK;
}


// Terrain nodes are labelled "TerrainNNNN".
static int Terrain_NodeID(CWorldNode *pNode)
{
	const char *pLabel = pNode->GetNodeLabel();
	if(!pLabel || _strnicmp(pLabel, "Terrain", 7) != 0)
		return -1;
	const char *p = pLabel + 7;
	if(strlen(p) != 4)
		return -1;
	for(int i=0; i < 4; i++)
		if(p[i] < '0' || p[i] > '9')
			return -1;
	return atoi(p);
}

static void Terrain_FindNodes(CWorldNode *pNode, std::vector<CWorldNode*> &out)
{
	if(Terrain_NodeID(pNode) >= 0)
	{
		out.push_back(pNode);
		return;
	}
	for(GPOS pos = pNode->m_Children.GetHeadPosition(); pos; )
		Terrain_FindNodes(pNode->m_Children.GetNext(pos), out);
}


// Fills in the brushes and heights of a terrain from its node.
static bool Terrain_Gather(CWorldNode *pNode, TerrainRef &ref)
{
	ref.m_pNode = pNode;
	ref.m_ID = Terrain_NodeID(pNode);
	if(ref.m_ID < 0 || !Terrain_LoadInfo(ref.m_ID, ref.m_Info))
		return false;

	const TerrainInfo &info = ref.m_Info;
	ref.m_Brushes.clear();
	ref.m_Heights.assign(info.NumVerts(), info.m_BaseY + TERRAIN_MIN_THICK);

	for(GPOS pos = pNode->m_Children.GetHeadPosition(); pos; )
	{
		CWorldNode *pChild = pNode->m_Children.GetNext(pos);
		if(pChild->GetType() != Node_Brush)
			continue;
		CEditBrush *pBrush = pChild->AsBrush();
		if(pBrush->m_Points.GetSize() != 6)
			continue;

		ref.m_Brushes.push_back(pBrush);
		for(uint32 k=0; k < 3; k++)
		{
			const CEditVert &pt = pBrush->m_Points[k];
			int g = info.GridIndex(pt.x, pt.z);
			if(g >= 0)
				ref.m_Heights[g] = pt.y;
		}
	}
	return ref.m_Brushes.size() > 0;
}


// Writes the heights back into the prisms.
static void Terrain_Apply(TerrainRef &ref)
{
	for(size_t b=0; b < ref.m_Brushes.size(); b++)
	{
		CEditBrush *pBrush = ref.m_Brushes[b];
		for(uint32 k=0; k < 3; k++)
		{
			CEditVert &pt = pBrush->m_Points[k];
			int g = ref.m_Info.GridIndex(pt.x, pt.z);
			if(g >= 0)
				pt.y = ref.m_Heights[g];
		}
		pBrush->UpdatePlanes();
		pBrush->UpdateBoundingInfo();
	}
}


//------------------------------------------------------------------
// Texture baking
//------------------------------------------------------------------

struct LayerImage
{
	int						m_W, m_H;
	std::vector<uint32>		m_Pixels;	// 0x00RRGGBB
};

// Loads mip 0 of a LithTech 1.0 DTX (both header layouts).
static bool Terrain_LoadLT1Dtx(const char *pName, LayerImage &img)
{
	if(!pName || !pName[0])
		return false;

	CString path = Terrain_ProjectDir() + pName;
	FILE *fp = fopen(path, "rb");
	if(!fp)
		return false;

	uint8 hdr[44], pal[1024];
	bool bOK = false;
	if(fread(hdr, 1, 44, fp) == 44 && fread(pal, 1, 1024, fp) == 1024)
	{
		int w, h;
		if(*(int32*)&hdr[0] == -2)
		{
			w = *(uint16*)&hdr[4];
			h = *(uint16*)&hdr[6];
		}
		else if(*(int32*)&hdr[4] == -2)
		{
			w = *(uint16*)&hdr[8];
			h = *(uint16*)&hdr[10];
		}
		else
		{
			w = h = 0;
		}

		if(w >= 1 && h >= 1 && w <= 4096 && h <= 4096)
		{
			std::vector<uint8> idx(w * h);
			if(fread(&idx[0], 1, idx.size(), fp) == idx.size())
			{
				img.m_W = w;
				img.m_H = h;
				img.m_Pixels.resize(w * h);
				for(int i=0; i < w * h; i++)
				{
					const uint8 *e = &pal[idx[i] * 4];
					img.m_Pixels[i] = ((uint32)e[1] << 16) | ((uint32)e[2] << 8) | e[3];
				}
				bOK = true;
			}
		}
	}
	fclose(fp);
	return bOK;
}

// Median cut down to 256 colours.
struct ColorBucket
{
	uint32	m_Key;		// 5:5:5
	uint32	m_Count;
	float	m_Sum[3];
};

static int g_SortChannel;
static bool Terrain_BucketLess(const ColorBucket &a, const ColorBucket &b)
{
	uint32 ca = (a.m_Key >> (10 - g_SortChannel * 5)) & 31;
	uint32 cb = (b.m_Key >> (10 - g_SortChannel * 5)) & 31;
	return ca < cb;
}

static void Terrain_Quantize(const std::vector<uint32> &pixels, uint8 palette[256][3], uint8 lut[32768])
{
	std::vector<ColorBucket> hist(32768);
	uint32 i;
	for(i=0; i < 32768; i++)
	{
		hist[i].m_Key = i;
		hist[i].m_Count = 0;
		hist[i].m_Sum[0] = hist[i].m_Sum[1] = hist[i].m_Sum[2] = 0.0f;
	}
	for(i=0; i < pixels.size(); i++)
	{
		uint32 c = pixels[i];
		uint32 r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
		ColorBucket &bk = hist[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
		bk.m_Count++;
		bk.m_Sum[0] += r;
		bk.m_Sum[1] += g;
		bk.m_Sum[2] += b;
	}

	std::vector<ColorBucket> used;
	for(i=0; i < 32768; i++)
		if(hist[i].m_Count)
			used.push_back(hist[i]);

	struct Box { size_t m_Begin, m_End; };
	std::vector<Box> boxes;
	Box all = { 0, used.size() };
	boxes.push_back(all);

	while(boxes.size() < 256)
	{
		// Split the box with the widest channel range (ties: more colours).
		int best = -1, bestRange = 0, bestChan = 0;
		for(size_t bx=0; bx < boxes.size(); bx++)
		{
			if(boxes[bx].m_End - boxes[bx].m_Begin < 2)
				continue;
			int lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 };
			for(size_t k = boxes[bx].m_Begin; k < boxes[bx].m_End; k++)
			{
				for(int ch=0; ch < 3; ch++)
				{
					int v = (used[k].m_Key >> (10 - ch * 5)) & 31;
					if(v < lo[ch]) lo[ch] = v;
					if(v > hi[ch]) hi[ch] = v;
				}
			}
			for(int ch=0; ch < 3; ch++)
			{
				if(hi[ch] - lo[ch] > bestRange)
				{
					bestRange = hi[ch] - lo[ch];
					best = (int)bx;
					bestChan = ch;
				}
			}
		}
		if(best < 0)
			break;

		Box box = boxes[best];
		g_SortChannel = bestChan;
		std::sort(used.begin() + box.m_Begin, used.begin() + box.m_End, Terrain_BucketLess);

		uint32 total = 0, half = 0;
		size_t k;
		for(k = box.m_Begin; k < box.m_End; k++)
			total += used[k].m_Count;
		size_t split = box.m_Begin + 1;
		for(k = box.m_Begin; k < box.m_End - 1; k++)
		{
			half += used[k].m_Count;
			split = k + 1;
			if(half * 2 >= total)
				break;
		}

		Box a = { box.m_Begin, split }, b = { split, box.m_End };
		boxes[best] = a;
		boxes.push_back(b);
	}

	memset(palette, 0, 256 * 3);
	for(size_t bx=0; bx < boxes.size(); bx++)
	{
		float sum[3] = { 0, 0, 0 };
		float count = 0;
		for(size_t k = boxes[bx].m_Begin; k < boxes[bx].m_End; k++)
		{
			sum[0] += used[k].m_Sum[0];
			sum[1] += used[k].m_Sum[1];
			sum[2] += used[k].m_Sum[2];
			count += (float)used[k].m_Count;
		}
		if(count > 0)
			for(int ch=0; ch < 3; ch++)
				palette[bx][ch] = (uint8)(sum[ch] / count + 0.5f);
	}
	int nColors = (int)boxes.size();
	if(nColors < 1)
		nColors = 1;

	for(i=0; i < 32768; i++)
	{
		int r = ((i >> 10) & 31) * 8 + 4, g = ((i >> 5) & 31) * 8 + 4, b = (i & 31) * 8 + 4;
		int bestIdx = 0, bestDist = 0x7FFFFFFF;
		for(int p=0; p < nColors; p++)
		{
			int dr = r - palette[p][0], dg = g - palette[p][1], db = b - palette[p][2];
			int d = dr*dr*3 + dg*dg*4 + db*db*2;
			if(d < bestDist)
			{
				bestDist = d;
				bestIdx = p;
			}
		}
		lut[i] = (uint8)bestIdx;
	}
}

static inline uint8 Terrain_Lookup(const uint8 lut[32768], uint32 c)
{
	return lut[(((c >> 19) & 31) << 10) | (((c >> 11) & 31) << 5) | ((c >> 3) & 31)];
}

// The surface type flags of a LithTech 1.0 DTX (what decides the bullet hole,
// footstep sounds and so on). Both header layouts.
static uint32 Terrain_ReadUserFlags(const char *pName)
{
	if(!pName || !pName[0])
		return 0;

	CString path = Terrain_ProjectDir() + pName;
	FILE *fp = fopen(path, "rb");
	if(!fp)
		return 0;

	uint8 hdr[44];
	uint32 flags = 0;
	if(fread(hdr, 1, 44, fp) == 44)
	{
		if(*(int32*)&hdr[4] == -2)
			flags = *(uint32*)&hdr[20];
		else if(*(int32*)&hdr[0] == -2)
			flags = *(uint32*)&hdr[16];
	}
	fclose(fp);
	return flags;
}

// Writes a 256x256, 4 mip, 8 bit LithTech 1.0 DTX (the header layout
// the shipping Blood 2 textures use).
static bool Terrain_WriteDtx(const char *pPath, const std::vector<uint32> &pixels, int size, uint32 userFlags)
{
	uint8 palette[256][3];
	static uint8 lut[32768];
	Terrain_Quantize(pixels, palette, lut);

	FILE *fp = fopen(pPath, "wb");
	if(!fp)
		return false;

	uint8 hdr[44];
	memset(hdr, 0, sizeof(hdr));
	*(int32*)&hdr[0]	= 0;			// resource type
	*(int32*)&hdr[4]	= -2;			// version
	*(uint16*)&hdr[8]	= (uint16)size;
	*(uint16*)&hdr[10]	= (uint16)size;
	*(uint16*)&hdr[12]	= 4;			// mips
	*(uint32*)&hdr[20]	= userFlags;	// surface type
	fwrite(hdr, 1, sizeof(hdr), fp);

	for(int p=0; p < 256; p++)
	{
		uint8 e[4] = { 0, palette[p][0], palette[p][1], palette[p][2] };
		fwrite(e, 1, 4, fp);
	}

	std::vector<uint32> mip = pixels;
	int w = size;
	std::vector<uint8> idx;
	for(int m=0; m < 4; m++)
	{
		idx.resize(w * w);
		for(int i=0; i < w * w; i++)
			idx[i] = Terrain_Lookup(lut, mip[i]);
		fwrite(&idx[0], 1, idx.size(), fp);

		if(m < 3)
		{
			int nw = w / 2;
			std::vector<uint32> next(nw * nw);
			for(int y=0; y < nw; y++)
			{
				for(int x=0; x < nw; x++)
				{
					uint32 c[4] = { mip[(y*2)*w + x*2], mip[(y*2)*w + x*2+1], mip[(y*2+1)*w + x*2], mip[(y*2+1)*w + x*2+1] };
					uint32 r = 0, g = 0, b = 0;
					for(int k=0; k < 4; k++)
					{
						r += (c[k] >> 16) & 255;
						g += (c[k] >> 8) & 255;
						b += c[k] & 255;
					}
					next[y*nw + x] = ((r / 4) << 16) | ((g / 4) << 8) | (b / 4);
				}
			}
			mip.swap(next);
			w = nw;
		}
	}

	bool bOK = ferror(fp) == 0;
	fclose(fp);
	return bOK;
}

// Fallback colours for layers without a loadable texture.
static const uint32 g_LayerFallback[TERRAIN_NUM_LAYERS] = { 0x5A7A3A, 0x7A6648, 0x8A8A88, 0xC8C090 };

// Composites the terrain's layers for the pixels u0..u1 x v0..v1 of a size x size image.
static void Terrain_BakePixels(const TerrainInfo &info, const LayerImage *layers, const bool *bHave,
	int size, int u0, int u1, int v0, int v1, std::vector<uint32> &pixels, float *layerWeight)
{
	const int stride = info.m_N + 1;
	int l;
	for(int v=v0; v < v1; v++)
	{
		// v runs from the far (max z) edge towards min z.
		float z = info.m_z0 + info.m_sz - ((float)v + 0.5f) * info.m_sz / (float)size;
		float gz = (z - info.m_z0) / info.m_sz * info.m_N;
		int j = (int)floor(gz);
		if(j < 0) j = 0;
		if(j > info.m_N - 1) j = info.m_N - 1;
		float fz = gz - (float)j;

		for(int u=u0; u < u1; u++)
		{
			float x = info.m_x0 + ((float)u + 0.5f) * info.m_sx / (float)size;
			float gx = (x - info.m_x0) / info.m_sx * info.m_N;
			int i = (int)floor(gx);
			if(i < 0) i = 0;
			if(i > info.m_N - 1) i = info.m_N - 1;
			float fx = gx - (float)i;

			const float *w00 = &info.m_Weights[(j * stride + i) * TERRAIN_NUM_LAYERS];
			const float *w10 = &info.m_Weights[(j * stride + i + 1) * TERRAIN_NUM_LAYERS];
			const float *w01 = &info.m_Weights[((j+1) * stride + i) * TERRAIN_NUM_LAYERS];
			const float *w11 = &info.m_Weights[((j+1) * stride + i + 1) * TERRAIN_NUM_LAYERS];

			float rgb[3] = { 0, 0, 0 }, total = 0.0f;
			for(l=0; l < TERRAIN_NUM_LAYERS; l++)
			{
				float w = (w00[l] * (1 - fx) + w10[l] * fx) * (1 - fz) + (w01[l] * (1 - fx) + w11[l] * fx) * fz;
				if(w <= 0.001f)
					continue;
				if(layerWeight) layerWeight[l] += w;

				uint32 c;
				if(bHave[l])
				{
					// One texel per world unit, like a default-scaled face.
					int tx = ((int)floor(x)) % layers[l].m_W;
					int ty = ((int)floor(-z)) % layers[l].m_H;
					if(tx < 0) tx += layers[l].m_W;
					if(ty < 0) ty += layers[l].m_H;
					c = layers[l].m_Pixels[ty * layers[l].m_W + tx];
				}
				else
				{
					c = g_LayerFallback[l];
				}
				rgb[0] += w * ((c >> 16) & 255);
				rgb[1] += w * ((c >> 8) & 255);
				rgb[2] += w * (c & 255);
				total += w;
			}
			if(total <= 0.0f)
			{
				total = 1.0f;
				rgb[0] = rgb[1] = rgb[2] = 128.0f;
			}

			uint32 r = (uint32)(rgb[0] / total + 0.5f), g = (uint32)(rgb[1] / total + 0.5f), b = (uint32)(rgb[2] / total + 0.5f);
			if(r > 255) r = 255;
			if(g > 255) g = 255;
			if(b > 255) b = 255;
			pixels[v * size + u] = (r << 16) | (g << 8) | b;
		}
	}

}

static bool Terrain_Bake(const TerrainRef &ref)
{
	const TerrainInfo &info = ref.m_Info;
	LayerImage layers[TERRAIN_NUM_LAYERS];
	bool bHave[TERRAIN_NUM_LAYERS];
	int l;
	for(l=0; l < TERRAIN_NUM_LAYERS; l++)
		bHave[l] = Terrain_LoadLT1Dtx(info.m_Layers[l].c_str(), layers[l]);

	const int size = TERRAIN_TEX_SIZE;
	std::vector<uint32> pixels(size * size);
	const int stride = info.m_N + 1;
	float layerWeight[TERRAIN_NUM_LAYERS] = { 0, 0, 0, 0 };

	Terrain_BakePixels(info, layers, bHave, size, 0, size, 0, size, pixels, layerWeight);
	Terrain_MakeDirs();
	CString path = Terrain_ProjectDir() + Terrain_TextureName(ref.m_ID);
	// The surface type comes from the layer that covers the most area, so
	// bullet holes and footsteps match what the terrain looks like.
	int best = 0;
	for(l=1; l < TERRAIN_NUM_LAYERS; l++)
	{
		if(layerWeight[l] > layerWeight[best])
			best = l;
	}
	uint32 userFlags = Terrain_ReadUserFlags(info.m_Layers[best].c_str());

	return Terrain_WriteDtx(path, pixels, size, userFlags);
}

// Live painting: bakes only the painted rectangle straight into the editor's in-memory copy
// of the terrain texture and has Direct3D re-upload it. No file writes, no reload.
void d3d_DropBindings(CTexture *pSystemTexture);

static bool g_bDirty = false;
static float g_Dirty[4];		// minx, maxx, minz, maxz (world)

static void Terrain_AddDirty(const LTVector &hit, float pad)
{
	if(!g_bDirty)
	{
		g_Dirty[0] = hit.x - pad; g_Dirty[1] = hit.x + pad;
		g_Dirty[2] = hit.z - pad; g_Dirty[3] = hit.z + pad;
		g_bDirty = true;
		return;
	}
	if(hit.x - pad < g_Dirty[0]) g_Dirty[0] = hit.x - pad;
	if(hit.x + pad > g_Dirty[1]) g_Dirty[1] = hit.x + pad;
	if(hit.z - pad < g_Dirty[2]) g_Dirty[2] = hit.z - pad;
	if(hit.z + pad > g_Dirty[3]) g_Dirty[3] = hit.z + pad;
}

static bool Terrain_LiveUpdate(const TerrainRef &ref, const float *pDirty)
{
	const TerrainInfo &info = ref.m_Info;
	DFileIdent *pIdent = NULL;
	dfm_GetFileIdentifier(GetFileMgr(), Terrain_TextureName(ref.m_ID), &pIdent);
	if(!pIdent)
		return false;
	CTexture *pTex = dib_GetDibTexture(pIdent);
	if(!pTex || !pTex->m_pDib)
		return false;
	CDib *pDib = pTex->m_pDib;
	const int W = pDib->GetWidth(), H = pDib->GetHeight();
	if(W <= 0 || H <= 0 || W != H)
		return false;

	// Layer images are kept while the layer names stay the same.
	static LayerImage s_L[TERRAIN_NUM_LAYERS];
	static bool s_Have[TERRAIN_NUM_LAYERS];
	static std::string s_LSig = "\x01";
	std::string sig;
	int l;
	for(l=0; l < TERRAIN_NUM_LAYERS; l++) { sig += info.m_Layers[l]; sig += '|'; }
	if(sig != s_LSig)
	{
		s_LSig = sig;
		for(l=0; l < TERRAIN_NUM_LAYERS; l++)
			s_Have[l] = Terrain_LoadLT1Dtx(info.m_Layers[l].c_str(), s_L[l]);
	}

	static int s_ID = -1, s_Size = 0;
	static std::string s_Sig;
	static std::vector<uint32> s_Px;
	int u0 = 0, u1 = W, v0 = 0, v1 = H;
	if(s_ID != ref.m_ID || s_Size != W || s_Sig != sig || s_Px.size() != (size_t)W * H || !pDirty)
	{
		s_ID = ref.m_ID; s_Size = W; s_Sig = sig;
		s_Px.assign((size_t)W * H, 0);
	}
	else
	{
		u0 = (int)floor((pDirty[0] - info.m_x0) / info.m_sx * W) - 1;
		u1 = (int)ceil((pDirty[1] - info.m_x0) / info.m_sx * W) + 1;
		v0 = (int)floor((info.m_z0 + info.m_sz - pDirty[3]) / info.m_sz * H) - 1;
		v1 = (int)ceil((info.m_z0 + info.m_sz - pDirty[2]) / info.m_sz * H) + 1;
		if(u0 < 0) u0 = 0;
		if(v0 < 0) v0 = 0;
		if(u1 > W) u1 = W;
		if(v1 > H) v1 = H;
		if(u1 <= u0 || v1 <= v0)
			return true;
	}
	Terrain_BakePixels(info, s_L, s_Have, W, u0, u1, v0, v1, s_Px, NULL);

	WORD *pBuf = pDib->GetBuf16();
	const int pitch = pDib->GetPitch();
	for(int v=v0; v < v1; v++)
	{
		WORD *pOut = pBuf + (size_t)v * pitch;
		const uint32 *pIn = &s_Px[(size_t)v * W];
		for(int u=u0; u < u1; u++)
		{
			uint32 c = pIn[u];
			pOut[u] = (WORD)((((c >> 19) & 31) << 10) | (((c >> 11) & 31) << 5) | ((c >> 3) & 31));
		}
	}
	d3d_DropBindings(pTex);
	return true;
}

// Makes the editor drop its cached copy of the baked texture and re-point
// the terrain's polies at it.
static void Terrain_ReloadTexture(TerrainRef &ref)
{
	DFileIdent *pIdent = NULL;
	dfm_GetFileIdentifier(GetFileMgr(), Terrain_TextureName(ref.m_ID), &pIdent);
	if(pIdent && pIdent->m_pUser && pIdent->m_UserType == 0)
		dib_DestroyDibTexture((CTexture*)pIdent->m_pUser);

	for(size_t b=0; b < ref.m_Brushes.size(); b++)
	{
		CEditBrush *pBrush = ref.m_Brushes[b];
		for(uint32 p=0; p < pBrush->m_Polies.GetSize(); p++)
			pBrush->m_Polies[p]->GetTexture(0).UpdateTextureID();
	}
}


//------------------------------------------------------------------
// Conversion
//------------------------------------------------------------------

static void Terrain_SetupPolyTexture(CEditPoly *pPoly, const TerrainInfo &info, const char *pTexName, bool bTop)
{
	CTexturedPlane &tex = pPoly->GetTexture(0);
	tex.m_pTextureName = (char*)pTexName;

	if(bTop)
	{
		// Planar from above: the whole footprint maps onto the baked texture.
		tex.SetO(LTVector(info.m_x0, 0.0f, info.m_z0 + info.m_sz));
		tex.SetP(LTVector((float)TERRAIN_TEX_SIZE / info.m_sx, 0.0f, 0.0f));
		tex.SetQ(LTVector(0.0f, 0.0f, -(float)TERRAIN_TEX_SIZE / info.m_sz));
	}
	else
	{
		tex.SetO(LTVector(0.0f, 0.0f, 0.0f));
		pPoly->SetupBaseTextureSpace(0);
	}

	for(uint32 nTex = 1; nTex < CEditPoly::NUM_TEXTURES; nTex++)
	{
		pPoly->GetTexture(nTex).SetO(LTVector(0.0f, 0.0f, 0.0f));
		pPoly->SetupBaseTextureSpace(nTex);
	}
}

static void Terrain_AddPoly(CEditBrush *pBrush, const int *pIdx, int nIdx, const LTVector &centroid,
	const TerrainInfo &info, const char *pTexName, bool bTop)
{
	CEditPoly *pPoly = new CEditPoly(pBrush);
	LTVector center(0.0f, 0.0f, 0.0f);
	for(int i=0; i < nIdx; i++)
	{
		pPoly->m_Indices.Append((uint32)pIdx[i]);
		center += pBrush->m_Points[pIdx[i]];
	}
	center *= 1.0f / (float)nIdx;

	pPoly->UpdatePlane();
	if(pPoly->Normal().Dot(center - centroid) < 0.0f)
	{
		pPoly->Flip();
		pPoly->UpdatePlane();
	}

	Terrain_SetupPolyTexture(pPoly, info, pTexName, bTop);
	pBrush->m_Polies.Append(pPoly);
}

static CEditBrush* Terrain_MakePrism(CEditRegion *pRegion, CWorldNode *pParent, const TerrainInfo &info,
	const LTVector top[3], const char *pTexName)
{
	CEditBrush *pBrush = no_CreateNewBrush(pRegion, pParent);

	LTVector centroid(0.0f, 0.0f, 0.0f);
	int k;
	for(k=0; k < 3; k++)
		pBrush->m_Points.Append(CEditVert(top[k]));
	for(k=0; k < 3; k++)
		pBrush->m_Points.Append(CEditVert(LTVector(top[k].x, info.m_BaseY, top[k].z)));
	for(k=0; k < 6; k++)
		centroid += pBrush->m_Points[k];
	centroid *= 1.0f / 6.0f;

	int topIdx[3] = { 0, 1, 2 };
	int botIdx[3] = { 3, 4, 5 };
	Terrain_AddPoly(pBrush, topIdx, 3, centroid, info, pTexName, true);
	Terrain_AddPoly(pBrush, botIdx, 3, centroid, info, pTexName, false);
	for(k=0; k < 3; k++)
	{
		int a = k, b = (k + 1) % 3;
		int side[4] = { a, b, b + 3, a + 3 };
		Terrain_AddPoly(pBrush, side, 4, centroid, info, pTexName, false);
	}

	pBrush->UpdatePlanes();
	pBrush->UpdateBoundingInfo();
	return pBrush;
}

static int Terrain_FreeID(CEditRegion *pRegion)
{
	std::vector<CWorldNode*> nodes;
	Terrain_FindNodes(pRegion->GetRootNode(), nodes);

	for(int id=1; id < 10000; id++)
	{
		bool bUsed = false;
		for(size_t n=0; n < nodes.size(); n++)
			if(Terrain_NodeID(nodes[n]) == id)
				bUsed = true;
		if(!bUsed && GetFileAttributes(Terrain_SidecarPath(id)) == INVALID_FILE_ATTRIBUTES)
			return id;
	}
	return -1;
}


class CTerrainConvertDlg : public CDialog
{
public:
	CTerrainConvertDlg() : CDialog(IDD_TERRAIN_CONVERT), m_N(8) {}
	int m_N;

protected:
	virtual BOOL OnInitDialog()
	{
		CDialog::OnInitDialog();
		CheckRadioButton(IDC_TERRAIN_RES8, IDC_TERRAIN_RES32, IDC_TERRAIN_RES32);
		return TRUE;
	}
	virtual void OnOK()
	{
		if(IsDlgButtonChecked(IDC_TERRAIN_RES8))		m_N = 2;
		else if(IsDlgButtonChecked(IDC_TERRAIN_RES16))	m_N = 4;
		else											m_N = 8;
		CDialog::OnOK();
	}
};


void Terrain::ConvertSelection(CRegionView *pView)
{
	CEditRegion *pRegion = pView->GetRegion();
	if(pRegion->GetNumSelections() != 1 || pRegion->GetSelection(0)->GetType() != Node_Brush)
	{
		AfxMessageBox("Select one brush to turn into terrain.");
		return;
	}

	CEditBrush *pSource = pRegion->GetSelection(0)->AsBrush();
	if(pSource->m_Points.GetSize() < 4)
		return;

	CTerrainConvertDlg dlg;
	if(dlg.DoModal() != IDOK)
		return;

	int id = Terrain_FreeID(pRegion);
	if(id < 0)
		return;

	// Footprint from the brush's bounds. The top of the brush becomes the
	// starting height and the bottom the flat base of every prism.
	LTVector vMin = pSource->m_Points[0], vMax = pSource->m_Points[0];
	for(uint32 i=1; i < pSource->m_Points.GetSize(); i++)
	{
		const CEditVert &pt = pSource->m_Points[i];
		if(pt.x < vMin.x) vMin.x = pt.x;
		if(pt.y < vMin.y) vMin.y = pt.y;
		if(pt.z < vMin.z) vMin.z = pt.z;
		if(pt.x > vMax.x) vMax.x = pt.x;
		if(pt.y > vMax.y) vMax.y = pt.y;
		if(pt.z > vMax.z) vMax.z = pt.z;
	}
	if(vMax.x - vMin.x < 1.0f || vMax.z - vMin.z < 1.0f)
	{
		AfxMessageBox("The brush needs some width and depth to become terrain.");
		return;
	}

	TerrainInfo info;
	info.m_N = dlg.m_N;
	info.m_x0 = vMin.x;
	info.m_z0 = vMin.z;
	info.m_sx = vMax.x - vMin.x;
	info.m_sz = vMax.z - vMin.z;
	info.m_BaseY = vMin.y;
	float topY = vMax.y;
	if(topY < info.m_BaseY + TERRAIN_MIN_THICK)
		topY = info.m_BaseY + TERRAIN_MIN_THICK;

	// Layer 1 is the brush's own texture unless the tool window says otherwise.
	const char *pSourceTex = pSource->m_Polies.GetSize() ? pSource->m_Polies[0]->GetTexture(0).m_pTextureName : NULL;
	for(int l=0; l < TERRAIN_NUM_LAYERS; l++)
		info.m_Layers[l] = (LPCTSTR)g_Layers[l];
	if(info.m_Layers[0].empty() && pSourceTex)
		info.m_Layers[0] = pSourceTex;

	info.m_Weights.assign(info.NumVerts() * TERRAIN_NUM_LAYERS, 0.0f);
	for(int v=0; v < info.NumVerts(); v++)
		info.m_Weights[v * TERRAIN_NUM_LAYERS] = 1.0f;

	if(!Terrain_SaveInfo(id, info))
	{
		AfxMessageBox("Couldn't write to Textures\\Terrainmesh in the project folder.");
		return;
	}

	CString texName = Terrain_TextureName(id);
	const char *pTexName = pRegion->m_pStringHolder->AddString((char*)(LPCTSTR)texName);

	CWorldNode *pParent = pSource->GetParent();
	CWorldNode *pGroup = pRegion->AddNullNode(pParent);
	CString label;
	label.Format("Terrain%04d", id);
	pGroup->SetNodeLabel(label);

	for(int j=0; j < info.m_N; j++)
	{
		for(int i=0; i < info.m_N; i++)
		{
			LTVector p00(info.GX(i), topY, info.GZ(j));
			LTVector p10(info.GX(i+1), topY, info.GZ(j));
			LTVector p11(info.GX(i+1), topY, info.GZ(j+1));
			LTVector p01(info.GX(i), topY, info.GZ(j+1));

			LTVector t0[3] = { p00, p10, p11 };
			LTVector t1[3] = { p00, p11, p01 };
			Terrain_MakePrism(pRegion, pGroup, info, t0, pTexName);
			Terrain_MakePrism(pRegion, pGroup, info, t1, pTexName);
		}
	}

	TerrainRef ref;
	if(Terrain_Gather(pGroup, ref))
	{
		Terrain_Bake(ref);
		Terrain_ReloadTexture(ref);
	}

	PreActionList actionList;
	actionList.AddTail(new CPreAction(ACTION_MODIFYNODE, pSource));
	actionList.AddTail(new CPreAction(ACTION_ADDEDNODE, pGroup));
	pView->GetRegionDoc()->Modify(&actionList, TRUE);

	pRegion->ClearSelections();
	GetNodeView()->DeleteNode(pSource);
	no_DestroyNode(pRegion, pSource, FALSE);

	GetNodeView()->UpdateTreeItemLabel(pGroup);
	GetNodeView()->UpdateNodeImage(pRegion->GetRootNode());

	// Select the whole terrain group.
	pRegion->RecurseAndSelect(pGroup);
	pView->GetRegionDoc()->NotifySelectionChange();
	pView->GetRegionDoc()->RedrawAllViews();
}


//------------------------------------------------------------------
// Painting
//------------------------------------------------------------------

static bool			g_bStroking = false;
static TerrainRef	g_Stroke;
static float		g_FlattenHeight = 0.0f;
static bool			g_bWeightsChanged = false;

static bool Terrain_RayTri(const LTVector &orig, const LTVector &dir, const LTVector &a, const LTVector &b, const LTVector &c, float &t)
{
	LTVector e1 = b - a, e2 = c - a;
	LTVector p = dir.Cross(e2);
	float det = e1.Dot(p);
	if((float)fabs(det) < 1e-8f)
		return false;
	float inv = 1.0f / det;
	LTVector s = orig - a;
	float u = s.Dot(p) * inv;
	if(u < 0.0f || u > 1.0f)
		return false;
	LTVector q = s.Cross(e1);
	float v = dir.Dot(q) * inv;
	if(v < 0.0f || u + v > 1.0f)
		return false;
	t = e2.Dot(q) * inv;
	return t > 0.0f;
}

// Finds the terrain under the mouse. Returns the hit point in pHit.
static bool Terrain_Pick(CRegionView *pView, CPoint point, CWorldNode **ppNode, LTVector *pHit)
{
	CEditRay ray = pView->ViewDef()->MakeRayFromScreenPoint(point);
	LTVector orig = ray.m_Pos, dir = ray.m_Dir;

	std::vector<CWorldNode*> nodes;
	Terrain_FindNodes(pView->GetRegion()->GetRootNode(), nodes);

	float bestT = 1e30f;
	CWorldNode *pBest = NULL;
	for(size_t n=0; n < nodes.size(); n++)
	{
		for(GPOS pos = nodes[n]->m_Children.GetHeadPosition(); pos; )
		{
			CWorldNode *pChild = nodes[n]->m_Children.GetNext(pos);
			if(pChild->GetType() != Node_Brush)
				continue;
			CEditBrush *pBrush = pChild->AsBrush();
			if(pBrush->m_Points.GetSize() != 6)
				continue;

			float t;
			if(Terrain_RayTri(orig, dir, pBrush->m_Points[0], pBrush->m_Points[1], pBrush->m_Points[2], t) && t < bestT)
			{
				bestT = t;
				pBest = nodes[n];
			}
		}
	}

	if(!pBest)
		return false;
	*ppNode = pBest;
	*pHit = orig + dir * bestT;
	return true;
}

static int g_DabMode = Terrain::MODE_RAISE;
static bool g_bStrokeRight = false;

// The mode a stroke actually uses: Shift smooths (height modes only) and the
// right button swaps Raise and Lower.
static int Terrain_EffectiveMode(bool bRight)
{
	int mode = g_Mode;
	if(mode >= Terrain::MODE_PAINT1)
		return mode;
	if(GetKeyState(VK_SHIFT) & 0x8000)
		return Terrain::MODE_SMOOTH;
	if(bRight)
	{
		if(mode == Terrain::MODE_RAISE)	return Terrain::MODE_LOWER;
		if(mode == Terrain::MODE_LOWER)	return Terrain::MODE_RAISE;
	}
	return mode;
}

static void Terrain_Dab(TerrainRef &ref, const LTVector &hit)
{
	TerrainInfo &info = ref.m_Info;
	const int stride = info.m_N + 1;
	const float s = (float)g_Strength / 100.0f;
	float radius = g_Radius;
	if(radius < 1.0f)
		radius = 1.0f;

	std::vector<float> src = ref.m_Heights;

	if(g_DabMode >= Terrain::MODE_PAINT1)
		Terrain_AddDirty(hit, radius + 8.0f + info.m_sx / (float)TERRAIN_TEX_SIZE * 2.0f);

	for(int j=0; j <= info.m_N; j++)
	{
		for(int i=0; i <= info.m_N; i++)
		{
			float dx = info.GX(i) - hit.x, dz = info.GZ(j) - hit.z;
			float d = (float)sqrt(dx*dx + dz*dz);
			if(d >= radius)
				continue;
			float f = 1.0f - d / radius;
			f = f * f * (3.0f - 2.0f * f);

			int g = j * stride + i;
			float &h = ref.m_Heights[g];

			switch(g_DabMode)
			{
				case Terrain::MODE_RAISE:	h += s * f * 8.0f; break;
				case Terrain::MODE_LOWER:	h -= s * f * 8.0f; break;

				case Terrain::MODE_SMOOTH:
				{
					float sum = 0.0f;
					int count = 0;
					for(int oj=-1; oj <= 1; oj++)
						for(int oi=-1; oi <= 1; oi++)
						{
							int ni = i + oi, nj = j + oj;
							if(ni < 0 || nj < 0 || ni > info.m_N || nj > info.m_N)
								continue;
							sum += src[nj * stride + ni];
							count++;
						}
					h += (sum / (float)count - h) * s * f;
					break;
				}

				case Terrain::MODE_FLATTEN:	h += (g_FlattenHeight - h) * s * f; break;

				default:
				{
					int layer = g_DabMode - Terrain::MODE_PAINT1;
					float *w = &info.m_Weights[g * TERRAIN_NUM_LAYERS];
					float add = s * f * 0.5f;
					w[layer] += add;
					if(w[layer] > 1.0f)
						w[layer] = 1.0f;
					float others = 0.0f;
					int l;
					for(l=0; l < TERRAIN_NUM_LAYERS; l++)
						if(l != layer)
							others += w[l];
					if(others > 0.0f)
					{
						float scale = (1.0f - w[layer]) / others;
						for(l=0; l < TERRAIN_NUM_LAYERS; l++)
							if(l != layer)
								w[l] *= scale;
					}
					else
					{
						w[layer] = 1.0f;
					}
					g_bWeightsChanged = true;
					break;
				}
			}

			if(h < info.m_BaseY + TERRAIN_MIN_THICK)
				h = info.m_BaseY + TERRAIN_MIN_THICK;
		}
	}

	if(g_DabMode < Terrain::MODE_PAINT1)
		Terrain_Apply(ref);
}


bool Terrain::IsStroking()
{
	return g_bStroking;
}

bool Terrain::IsStrokeRightButton()
{
	return g_bStroking && g_bStrokeRight;
}

bool Terrain::BeginStroke(CRegionView *pView, CPoint point, bool bRightButton)
{
	if(!IsActive() || (GetKeyState(VK_MENU) & 0x8000) || (GetKeyState(VK_CONTROL) & 0x8000))
		return false;

	// Shift smooths in the height modes; in the paint modes it keeps its usual meaning.
	if((GetKeyState(VK_SHIFT) & 0x8000) && g_Mode >= MODE_PAINT1)
		return false;

	// The right button only does something in Raise / Lower.
	if(bRightButton && g_Mode != MODE_RAISE && g_Mode != MODE_LOWER)
		return false;

	CWorldNode *pNode;
	LTVector hit;
	if(!Terrain_Pick(pView, point, &pNode, &hit))
		return false;
	if(!Terrain_Gather(pNode, g_Stroke))
		return false;

	g_bStrokeRight = bRightButton;
	g_DabMode = Terrain_EffectiveMode(bRightButton);

	if(g_Mode < MODE_PAINT1)
	{
		PreActionList actionList;
		for(size_t b=0; b < g_Stroke.m_Brushes.size(); b++)
			actionList.AddTail(new CPreAction(ACTION_MODIFYNODE, g_Stroke.m_Brushes[b]));
		pView->GetRegionDoc()->Modify(&actionList, TRUE);
	}

	g_FlattenHeight = hit.y;
	g_bWeightsChanged = false;
	g_bDirty = false;
	g_bStroking = true;

	Terrain_Dab(g_Stroke, hit);
	pView->GetRegionDoc()->RedrawAllViews();
	return true;
}

void Terrain::ContinueStroke(CRegionView *pView, CPoint point)
{
	if(!g_bStroking)
		return;

	CWorldNode *pNode;
	LTVector hit;
	if(!Terrain_Pick(pView, point, &pNode, &hit) || pNode != g_Stroke.m_pNode)
		return;

	g_bHover = true;
	g_pHoverNode = pNode;
	g_vHover = hit;

	g_DabMode = Terrain_EffectiveMode(g_bStrokeRight);
	Terrain_Dab(g_Stroke, hit);

	// Live paint: update the texture in memory (the file is written when the stroke ends).
	static DWORD s_LastLive = 0;
	DWORD now = GetTickCount();
	if(g_DabMode >= Terrain::MODE_PAINT1 && g_bDirty && g_bWeightsChanged && now - s_LastLive >= 30)
	{
		Terrain_LiveUpdate(g_Stroke, g_Dirty);
		g_bDirty = false;
		s_LastLive = GetTickCount();
	}
	pView->GetRegionDoc()->RedrawAllViews();
}

void Terrain::EndStroke(CRegionView *pView)
{
	if(!g_bStroking)
		return;
	g_bStroking = false;

	if(g_bWeightsChanged)
	{
		Terrain_SaveInfo(g_Stroke.m_ID, g_Stroke.m_Info);
		if(Terrain_Bake(g_Stroke))
			Terrain_ReloadTexture(g_Stroke);
	}
	g_bDirty = false;

	pView->GetRegionDoc()->Modify();
	pView->GetRegionDoc()->RedrawAllViews();
}


//------------------------------------------------------------------
// Tool window
//------------------------------------------------------------------

static const char *g_ModeNames[Terrain::NUM_MODES] =
{
	"Raise", "Lower", "Smooth", "Flatten",
	"Paint layer 1", "Paint layer 2", "Paint layer 3", "Paint layer 4"
};

class CTerrainToolsDlg : public CDialog
{
public:
	CTerrainToolsDlg() : m_bInit(false) {}

	BOOL Create(CWnd *pParent)	{ return CDialog::Create(IDD_TERRAIN_TOOLS, pParent); }

protected:
	bool m_bInit;

	virtual BOOL OnInitDialog()
	{
		CDialog::OnInitDialog();

		CComboBox *pCombo = (CComboBox*)GetDlgItem(IDC_TERRAIN_MODE);
		for(int i=0; i < Terrain::NUM_MODES; i++)
			pCombo->AddString(g_ModeNames[i]);
		pCombo->SetCurSel(g_Mode);

		SetDlgItemInt(IDC_TERRAIN_RADIUS, (UINT)g_Radius);
		SetDlgItemInt(IDC_TERRAIN_STRENGTH, (UINT)g_Strength);
		// The layer textures from the last session.
		static bool s_bLayersLoaded = false;
		if(!s_bLayersLoaded)
		{
			s_bLayersLoaded = true;
			for(int k=0; k < TERRAIN_NUM_LAYERS; k++)
			{
				CString key;
				key.Format("TerrainLayer%d", k + 1);
				if(g_Layers[k].IsEmpty())
					g_Layers[k] = AfxGetApp()->GetProfileString("Settings", key, "");
			}
		}
		for(int l=0; l < TERRAIN_NUM_LAYERS; l++)
			SetDlgItemText(IDC_TERRAIN_LAYER1 + l, g_Layers[l]);
		CheckDlgButton(IDC_TERRAIN_ACTIVE, g_bPaint ? BST_CHECKED : BST_UNCHECKED);

		m_bInit = true;
		return TRUE;
	}

	virtual void OnOK()		{}
	virtual void OnCancel()	{ ShowWindow(SW_HIDE); }

	virtual BOOL OnCommand(WPARAM wParam, LPARAM lParam)
	{
		UINT id = LOWORD(wParam), code = HIWORD(wParam);
		if(m_bInit)
		{
			if(id == IDC_TERRAIN_MODE && code == CBN_SELCHANGE)
			{
				int sel = ((CComboBox*)GetDlgItem(IDC_TERRAIN_MODE))->GetCurSel();
				if(sel >= 0)
					g_Mode = sel;
				return TRUE;
			}
			if(id == IDC_TERRAIN_RADIUS && code == EN_CHANGE)
			{
				BOOL bOK;
				UINT v = GetDlgItemInt(IDC_TERRAIN_RADIUS, &bOK, FALSE);
				if(bOK && v > 0)
					g_Radius = (float)v;
				return TRUE;
			}
			if(id == IDC_TERRAIN_STRENGTH && code == EN_CHANGE)
			{
				BOOL bOK;
				UINT v = GetDlgItemInt(IDC_TERRAIN_STRENGTH, &bOK, FALSE);
				if(bOK)
					g_Strength = v < 1 ? 1 : (v > 100 ? 100 : (int)v);
				return TRUE;
			}
			if(id >= IDC_TERRAIN_LAYER1 && id < IDC_TERRAIN_LAYER1 + TERRAIN_NUM_LAYERS && code == EN_CHANGE)
			{
				GetDlgItemText(id, g_Layers[id - IDC_TERRAIN_LAYER1]);
				if(m_bInit)
				{
					CString key;
					key.Format("TerrainLayer%d", id - IDC_TERRAIN_LAYER1 + 1);
					AfxGetApp()->WriteProfileString("Settings", key, g_Layers[id - IDC_TERRAIN_LAYER1]);
				}
				return TRUE;
			}
			if(id >= IDC_TERRAIN_USE1 && id < IDC_TERRAIN_USE1 + TERRAIN_NUM_LAYERS && code == BN_CLICKED)
			{
				char szName[MAX_PATH];
				szName[0] = 0;
				GetProjectBar()->GetCurrentTextureName(szName, FALSE);
				SetDlgItemText(IDC_TERRAIN_LAYER1 + (id - IDC_TERRAIN_USE1), szName);
				return TRUE;
			}
			if(id == IDC_TERRAIN_ACTIVE && code == BN_CLICKED)
			{
				g_bPaint = IsDlgButtonChecked(IDC_TERRAIN_ACTIVE) == BST_CHECKED;
				return TRUE;
			}
			if(id == IDC_TERRAIN_REBAKE && code == BN_CLICKED)
			{
				Rebake();
				return TRUE;
			}
		}
		return CDialog::OnCommand(wParam, lParam);
	}

	// Applies the layer textures to the selected terrains (or all of them
	// when none are selected) and bakes them again.
	void Rebake()
	{
		CRegionDoc *pDoc = GetActiveRegionDoc();
		if(!pDoc)
			return;
		CEditRegion *pRegion = pDoc->GetRegion();

		std::vector<CWorldNode*> nodes, picked;
		Terrain_FindNodes(pRegion->GetRootNode(), nodes);
		for(size_t n=0; n < nodes.size(); n++)
		{
			bool bSel = nodes[n]->IsFlagSet(NODEFLAG_SELECTED) != 0;
			for(GPOS pos = nodes[n]->m_Children.GetHeadPosition(); pos && !bSel; )
				if(nodes[n]->m_Children.GetNext(pos)->IsFlagSet(NODEFLAG_SELECTED))
					bSel = true;
			if(bSel)
				picked.push_back(nodes[n]);
		}
		if(picked.empty())
			picked = nodes;
		if(picked.empty())
		{
			AfxMessageBox("There are no terrains in this level. Use Brush > Convert to Terrain first.");
			return;
		}

		CWaitCursor wait;
		for(size_t n=0; n < picked.size(); n++)
		{
			TerrainRef ref;
			if(!Terrain_Gather(picked[n], ref))
				continue;
			for(int l=0; l < TERRAIN_NUM_LAYERS; l++)
				if(!g_Layers[l].IsEmpty())
					ref.m_Info.m_Layers[l] = (LPCTSTR)g_Layers[l];
			Terrain_SaveInfo(ref.m_ID, ref.m_Info);
			if(Terrain_Bake(ref))
				Terrain_ReloadTexture(ref);
		}
		pDoc->RedrawAllViews();
	}
};

static CTerrainToolsDlg *g_pToolsDlg = NULL;

void Terrain::ToggleToolsWindow()
{
	if(!g_pToolsDlg)
	{
		g_pToolsDlg = new CTerrainToolsDlg;
		if(!g_pToolsDlg->Create(AfxGetMainWnd()))
		{
			delete g_pToolsDlg;
			g_pToolsDlg = NULL;
			return;
		}
		g_pToolsDlg->ShowWindow(SW_SHOW);
		return;
	}

	g_pToolsDlg->ShowWindow(g_pToolsDlg->IsWindowVisible() ? SW_HIDE : SW_SHOW);
}

bool Terrain::IsToolsWindowVisible()
{
	return g_pToolsDlg && g_pToolsDlg->GetSafeHwnd() && g_pToolsDlg->IsWindowVisible();
}

bool Terrain::IsActive()
{
	return g_bPaint && IsToolsWindowVisible();
}

bool Terrain::BlocksSelection()
{
	if(!IsToolsWindowVisible())
		return false;

	// Let the navigation hotkeys (move / rotate / orbit / draw poly) through.
	static const int navKeys[] = { 'I', 'O', 'L', VK_SPACE };
	for(int i=0; i < (int)(sizeof(navKeys) / sizeof(navKeys[0])); i++)
		if(GetKeyState(navKeys[i]) & 0x8000)
			return false;
	return true;
}

bool Terrain::HandleMouseWheel(short zDelta)
{
	if(!IsToolsWindowVisible() || !(GetKeyState(VK_CONTROL) & 0x8000) || zDelta == 0)
		return false;

	int nSteps = zDelta / WHEEL_DELTA;
	if(nSteps == 0)
		nSteps = (zDelta > 0) ? 1 : -1;

	if(GetKeyState(VK_SHIFT) & 0x8000)
	{
		int s = g_Strength + nSteps * 5;
		g_Strength = s < 1 ? 1 : (s > 100 ? 100 : s);
		g_pToolsDlg->SetDlgItemInt(IDC_TERRAIN_STRENGTH, (UINT)g_Strength);
	}
	else
	{
		float r = g_Radius;
		for(int i=0; i < abs(nSteps); i++)
		{
			float n = (nSteps > 0) ? r * 1.1f : r / 1.1f;
			if(nSteps > 0 && n < r + 1.0f) n = r + 1.0f;
			if(nSteps < 0 && n > r - 1.0f) n = r - 1.0f;
			r = n;
		}
		int ri = (int)(r + 0.5f);
		if(ri < 1) ri = 1;
		if(ri > 4096) ri = 4096;
		g_Radius = (float)ri;
		g_pToolsDlg->SetDlgItemInt(IDC_TERRAIN_RADIUS, (UINT)ri);
	}

	CRegionDoc *pDoc = GetActiveRegionDoc();
	if(pDoc)
		pDoc->RedrawAllViews();
	return true;
}


//------------------------------------------------------------------
// Reticle
//------------------------------------------------------------------


// Height of the terrain's surface at (x, z), from its prism tops.
static bool Terrain_HeightAt(CWorldNode *pNode, float x, float z, float &y)
{
	LTVector orig(x, 1e6f, z), dir(0.0f, -1.0f, 0.0f);
	float bestT = 1e30f;
	for(GPOS pos = pNode->m_Children.GetHeadPosition(); pos; )
	{
		CWorldNode *pChild = pNode->m_Children.GetNext(pos);
		if(pChild->GetType() != Node_Brush)
			continue;
		CEditBrush *pBrush = pChild->AsBrush();
		if(pBrush->m_Points.GetSize() != 6)
			continue;
		float t;
		if(Terrain_RayTri(orig, dir, pBrush->m_Points[0], pBrush->m_Points[1], pBrush->m_Points[2], t) && t < bestT)
			bestT = t;
	}
	if(bestT > 1e29f)
		return false;
	y = orig.y - bestT;
	return true;
}

static bool Terrain_NodeInRegion(CEditRegion *pRegion, CWorldNode *pNode)
{
	std::vector<CWorldNode*> nodes;
	Terrain_FindNodes(pRegion->GetRootNode(), nodes);
	for(size_t n=0; n < nodes.size(); n++)
		if(nodes[n] == pNode)
			return true;
	return false;
}

void Terrain::UpdateHover(CRegionView *pView, CPoint point)
{
	if(!IsActive())
	{
		if(g_bHover)
		{
			g_bHover = false;
			pView->GetRegionDoc()->RedrawAllViews();
		}
		return;
	}

	CWorldNode *pNode;
	LTVector hit;
	bool bHit = Terrain_Pick(pView, point, &pNode, &hit);
	if(!bHit && !g_bHover)
		return;

	g_bHover = bHit;
	if(bHit)
	{
		g_pHoverNode = pNode;
		g_vHover = hit;
	}
	pView->GetRegionDoc()->RedrawAllViews();
}

void Terrain::DrawReticle(DrawBase *pDraw)
{
	if(!g_bHover || !IsActive() || !g_pHoverNode)
		return;
	if(!Terrain_NodeInRegion(pDraw->m_pView->GetRegion(), g_pHoverNode))
	{
		g_bHover = false;
		return;
	}

	const DWORD color = 0xFF30FF30;
	const DWORD centerColor = 0xFFFFFF30;
	const int nSegs = 40;
	const float lift = 1.0f;

	pDraw->ZEnable(FALSE);

	// Brush circle draped over the surface.
	LTVector prev;
	for(int i=0; i <= nSegs; i++)
	{
		float a = (float)i / (float)nSegs * 6.2831853f;
		float x = g_vHover.x + (float)cos(a) * g_Radius;
		float z = g_vHover.z + (float)sin(a) * g_Radius;
		float y = g_vHover.y;
		Terrain_HeightAt(g_pHoverNode, x, z, y);
		LTVector cur(x, y + lift, z);
		if(i > 0)
			pDraw->TransformAndDrawLine(prev, cur, color);
		prev = cur;
	}

	// Inner ring at half radius and a centre spike scaled by strength.
	for(int i=0; i <= nSegs / 2; i++)
	{
		float a = (float)i / (float)(nSegs / 2) * 6.2831853f;
		float x = g_vHover.x + (float)cos(a) * g_Radius * 0.5f;
		float z = g_vHover.z + (float)sin(a) * g_Radius * 0.5f;
		float y = g_vHover.y;
		Terrain_HeightAt(g_pHoverNode, x, z, y);
		LTVector cur(x, y + lift, z);
		if(i > 0)
			pDraw->TransformAndDrawLine(prev, cur, color);
		prev = cur;
	}

	float spike = 8.0f + g_Radius * 0.25f * (float)g_Strength / 100.0f;
	if(g_Mode == MODE_LOWER)
		spike = -spike;
	LTVector top(g_vHover.x, g_vHover.y + spike, g_vHover.z);
	pDraw->TransformAndDrawLine(g_vHover, top, centerColor);
	float c = g_Radius * 0.08f + 2.0f;
	pDraw->TransformAndDrawLine(LTVector(g_vHover.x - c, g_vHover.y + lift, g_vHover.z), LTVector(g_vHover.x + c, g_vHover.y + lift, g_vHover.z), centerColor);
	pDraw->TransformAndDrawLine(LTVector(g_vHover.x, g_vHover.y + lift, g_vHover.z - c), LTVector(g_vHover.x, g_vHover.y + lift, g_vHover.z + c), centerColor);

	pDraw->ZEnable(pDraw->m_pView->IsPerspectiveViewType());
}
