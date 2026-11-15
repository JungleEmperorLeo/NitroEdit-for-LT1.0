//------------------------------------------------------------------
//
//	FILE	  : LT1TexturePropDlg.cpp
//
//	PURPOSE	  : Texture Properties for LithTech 1.0 (Blood 2 / Shogo)
//	            8 bit DTX files, laid out like the original DEdit's dialog.
//	            Only the header fields and the LIGHTDEFS section are
//	            written; the palette and mipmaps are left untouched.
//
//	            Header (44 bytes): 0 ResType, 4 version (-2), 8 width,
//	            10 height, 12 mipmaps, 14 sections, 16 flags (bit 0 full
//	            brights, bit 1 alpha masks, bit 5 don't map to master
//	            palette), 20 user flags, 24 extra[12]: [0] group,
//	            [1] mipmaps to use, [2] alpha cutoff | 0x80, [3] average
//	            alpha. Sections follow the mipmaps: type[15], name[10],
//	            3 pad bytes, uint32 data length, data.
//
//------------------------------------------------------------------

#include "bdefs.h"
#include "dedit.h"
#include "resource.h"
#include "LT1TexturePropDlg.h"

#include <vector>
#include <string>
#include <stdio.h>

#define LT1TEX_HEADER_SIZE		44
#define LT1TEX_PALETTE_SIZE		1024
#define LT1TEX_SECTION_HEADER	32

#define LT1TEX_FULLBRITE		(1 << 0)
#define LT1TEX_ALPHAMASKS		(1 << 1)
#define LT1TEX_NOMASTERPALETTE	(1 << 5)


struct LT1TexFile
{
	std::vector<uint8>	m_Data;
	uint32				m_SectionsStart;	// end of the mipmaps

	uint32&	Flags()			{ return *(uint32*)&m_Data[16]; }
	uint32&	UserFlags()		{ return *(uint32*)&m_Data[20]; }
	uint8&	Extra(int i)	{ return m_Data[24 + i]; }

	bool Load(const char *pPath)
	{
		FILE *fp = fopen(pPath, "rb");
		if(!fp)
			return false;
		fseek(fp, 0, SEEK_END);
		long size = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		if(size < LT1TEX_HEADER_SIZE + LT1TEX_PALETTE_SIZE)
		{
			fclose(fp);
			return false;
		}
		m_Data.resize(size);
		bool bOK = fread(&m_Data[0], 1, size, fp) == (size_t)size;
		fclose(fp);
		if(!bOK || *(int32*)&m_Data[4] != -2)	// only the layout the Blood 2 files use
			return false;

		uint32 w = *(uint16*)&m_Data[8], h = *(uint16*)&m_Data[10], nMips = *(uint16*)&m_Data[12];
		uint32 pos = LT1TEX_HEADER_SIZE + LT1TEX_PALETTE_SIZE;
		for(uint32 i=0; i < nMips && i < 8; i++)
			pos += (w >> i) * (h >> i);
		if(pos > m_Data.size())
			return false;
		m_SectionsStart = pos;
		return true;
	}

	bool Save(const char *pPath)
	{
		FILE *fp = fopen(pPath, "wb");
		if(!fp)
			return false;
		bool bOK = fwrite(&m_Data[0], 1, m_Data.size(), fp) == m_Data.size();
		fclose(fp);
		return bOK;
	}

	// Finds a section by type. Returns the offset of its header or 0.
	uint32 FindSection(const char *pType, uint32 *pLen)
	{
		uint32 pos = m_SectionsStart;
		while(pos + LT1TEX_SECTION_HEADER <= m_Data.size())
		{
			char type[16];
			memcpy(type, &m_Data[pos], 15);
			type[15] = 0;
			uint32 len = *(uint32*)&m_Data[pos + 28];
			if(pos + LT1TEX_SECTION_HEADER + len > m_Data.size())
				break;
			if(_stricmp(type, pType) == 0)
			{
				*pLen = len;
				return pos;
			}
			pos += LT1TEX_SECTION_HEADER + len;
		}
		return 0;
	}

	CString GetLights()
	{
		uint32 len;
		uint32 pos = FindSection("LIGHTDEFS", &len);
		if(!pos || !len)
			return CString();
		std::string s((const char*)&m_Data[pos + LT1TEX_SECTION_HEADER], len);
		size_t z = s.find('\0');
		if(z != std::string::npos)
			s.resize(z);
		return CString(s.c_str());
	}

	void SetLights(const CString &lights)
	{
		uint32 len;
		uint32 pos = FindSection("LIGHTDEFS", &len);
		if(pos)
		{
			m_Data.erase(m_Data.begin() + pos, m_Data.begin() + pos + LT1TEX_SECTION_HEADER + len);
			*(uint16*)&m_Data[14] = (uint16)(*(uint16*)&m_Data[14] - 1);
		}

		if(lights.IsEmpty())
			return;

		uint8 hdr[LT1TEX_SECTION_HEADER];
		memset(hdr, 0, sizeof(hdr));
		strcpy((char*)hdr, "LIGHTDEFS");
		uint32 dataLen = (uint32)lights.GetLength() + 1;
		*(uint32*)&hdr[28] = dataLen;
		m_Data.insert(m_Data.end(), hdr, hdr + sizeof(hdr));
		const char *p = lights;
		m_Data.insert(m_Data.end(), (const uint8*)p, (const uint8*)p + dataLen);
		*(uint16*)&m_Data[14] = (uint16)(*(uint16*)&m_Data[14] + 1);
	}
};


bool LT1Tex_IsLT1File(const char *pFullPath)
{
	FILE *fp = fopen(pFullPath, "rb");
	if(!fp)
		return false;
	int32 hdr[2] = { 0, 0 };
	size_t got = fread(hdr, 1, sizeof(hdr), fp);
	fclose(fp);
	if(got == sizeof(hdr) && (hdr[0] == -2 || hdr[1] == -2))
		return true;
	// Legacy model skin (CRATE1.DTX etc.): claim it so the generic loader
	// doesn't report an error; DoProperties explains it.
	return got == sizeof(hdr) && (hdr[0] & 0xFFFF) == 4;
}


class CLT1TexturePropDlg : public CDialog
{
public:
	CLT1TexturePropDlg() : CDialog(IDD_LT1_TEXTUREPROP) {}

	uint32		m_Flags;
	CString		m_Lights;
	int			m_Group;
	int			m_AlphaCutoff;
	int			m_AverageAlpha;
	int			m_nMipmaps;
	BOOL		m_bNoMasterPalette;
	BOOL		m_bFullBrights;
	BOOL		m_bAlphaMasks;
	bool		m_bMulti;

protected:
	virtual BOOL OnInitDialog()
	{
		CDialog::OnInitDialog();
		SetDlgItemInt(IDC_LT1TEX_FLAGS, m_Flags, FALSE);
		SetDlgItemText(IDC_LT1TEX_LIGHTS, m_Lights);
		SetDlgItemInt(IDC_LT1TEX_GROUP, m_Group, FALSE);
		SetDlgItemInt(IDC_LT1TEX_ALPHACUTOFF, m_AlphaCutoff, FALSE);
		SetDlgItemInt(IDC_LT1TEX_AVERAGEALPHA, m_AverageAlpha, FALSE);
		SetDlgItemInt(IDC_LT1TEX_MIPMAPS, m_nMipmaps, FALSE);
		CheckDlgButton(IDC_LT1TEX_NOMASTERPAL, m_bNoMasterPalette ? BST_CHECKED : BST_UNCHECKED);
		SetDlgItemText(IDC_LT1TEX_FULLBRIGHTS, m_bFullBrights ? "TRUE" : "FALSE");
		SetDlgItemText(IDC_LT1TEX_ALPHAMASKS, m_bAlphaMasks ? "TRUE" : "FALSE");
		if(m_bMulti)
			SetWindowText("Texture Properites (multiple textures)");
		((CEdit*)GetDlgItem(IDC_LT1TEX_FLAGS))->SetSel(0, -1);
		GetDlgItem(IDC_LT1TEX_FLAGS)->SetFocus();
		return FALSE;
	}

	virtual void OnOK()
	{
		BOOL bOK;
		m_Flags = GetDlgItemInt(IDC_LT1TEX_FLAGS, &bOK, FALSE);
		GetDlgItemText(IDC_LT1TEX_LIGHTS, m_Lights);
		m_Group = GetDlgItemInt(IDC_LT1TEX_GROUP, &bOK, FALSE);
		m_AlphaCutoff = GetDlgItemInt(IDC_LT1TEX_ALPHACUTOFF, &bOK, FALSE);
		m_AverageAlpha = GetDlgItemInt(IDC_LT1TEX_AVERAGEALPHA, &bOK, FALSE);
		m_nMipmaps = GetDlgItemInt(IDC_LT1TEX_MIPMAPS, &bOK, FALSE);
		m_bNoMasterPalette = IsDlgButtonChecked(IDC_LT1TEX_NOMASTERPAL) == BST_CHECKED;

		if(m_Group > 255) m_Group = 255;
		if(m_AlphaCutoff > 127) m_AlphaCutoff = 127;
		if(m_AverageAlpha > 255) m_AverageAlpha = 255;
		if(m_nMipmaps < 1) m_nMipmaps = 1;
		if(m_nMipmaps > 4) m_nMipmaps = 4;
		CDialog::OnOK();
	}
};


static void LT1Tex_ReadFields(LT1TexFile &f, CLT1TexturePropDlg &dlg)
{
	dlg.m_Flags				= f.UserFlags();
	dlg.m_Lights			= f.GetLights();
	dlg.m_Group				= f.Extra(0);
	dlg.m_nMipmaps			= f.Extra(1) ? f.Extra(1) : 4;
	dlg.m_AlphaCutoff		= (f.Extra(2) & 0x80) ? (f.Extra(2) & 0x7F) : 8;
	dlg.m_AverageAlpha		= f.Extra(3);
	dlg.m_bNoMasterPalette	= (f.Flags() & LT1TEX_NOMASTERPALETTE) != 0;
	dlg.m_bFullBrights		= (f.Flags() & LT1TEX_FULLBRITE) != 0;
	dlg.m_bAlphaMasks		= (f.Flags() & LT1TEX_ALPHAMASKS) != 0;
}

BOOL LT1Tex_DoProperties(const CStringArray &paths)
{
	if(paths.GetSize() == 0)
		return FALSE;

	LT1TexFile first;
	if(!first.Load(paths[0]))
	{
		// Old-style model skin: 34 byte header, 8 bit pixels, master palette.
		FILE *fp = fopen(paths[0], "rb");
		if(fp)
		{
			uint8 h[34];
			size_t got = fread(h, 1, sizeof(h), fp);
			fseek(fp, 0, SEEK_END);
			long size = ftell(fp);
			fclose(fp);
			if(got == sizeof(h))
			{
				uint32 w = *(uint16*)&h[18], ht = *(uint16*)&h[20], tot = 34;
				for(int m=0; m < 4; m++)
					tot += (w >> m) * (ht >> m);
				if(w && ht && tot == (uint32)size)
				{
					CString s;
					s.Format("%s is a %ux%u model skin (master palette, no texture properties).",
						(LPCTSTR)paths[0], w, ht);
					AfxMessageBox(s);
					return FALSE;
				}
			}
		}

		CString s;
		s.Format("Couldn't read %s as a LithTech 1.0 texture.", (LPCTSTR)paths[0]);
		AfxMessageBox(s);
		return FALSE;
	}

	CLT1TexturePropDlg dlg, orig;
	LT1Tex_ReadFields(first, dlg);
	LT1Tex_ReadFields(first, orig);
	dlg.m_bMulti = paths.GetSize() > 1;

	if(dlg.DoModal() != IDOK)
		return FALSE;

	// Only the fields that were changed are written, so a multiple
	// selection keeps each texture's other settings.
	int nFailed = 0;
	for(int i=0; i < paths.GetSize(); i++)
	{
		LT1TexFile f;
		if(!f.Load(paths[i]))
		{
			nFailed++;
			continue;
		}

		if(dlg.m_Flags != orig.m_Flags)
			f.UserFlags() = dlg.m_Flags;
		if(dlg.m_Lights != orig.m_Lights)
			f.SetLights(dlg.m_Lights);
		if(dlg.m_Group != orig.m_Group)
			f.Extra(0) = (uint8)dlg.m_Group;
		if(dlg.m_nMipmaps != orig.m_nMipmaps)
			f.Extra(1) = (uint8)dlg.m_nMipmaps;
		if(dlg.m_AlphaCutoff != orig.m_AlphaCutoff)
			f.Extra(2) = (uint8)(0x80 | dlg.m_AlphaCutoff);
		if(dlg.m_AverageAlpha != orig.m_AverageAlpha)
			f.Extra(3) = (uint8)dlg.m_AverageAlpha;
		if(dlg.m_bNoMasterPalette != orig.m_bNoMasterPalette)
		{
			f.Flags() &= ~LT1TEX_NOMASTERPALETTE;
			if(dlg.m_bNoMasterPalette)
				f.Flags() |= LT1TEX_NOMASTERPALETTE;
		}

		if(!f.Save(paths[i]))
			nFailed++;
	}

	if(nFailed)
	{
		CString s;
		s.Format("%d texture(s) couldn't be updated (read-only or not LithTech 1.0 textures).", nFailed);
		AfxMessageBox(s);
	}
	return TRUE;
}
