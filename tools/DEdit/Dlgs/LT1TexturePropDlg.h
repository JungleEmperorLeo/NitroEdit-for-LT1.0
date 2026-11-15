//------------------------------------------------------------------
//
//	FILE	  : LT1TexturePropDlg.h
//
//	PURPOSE	  : Texture Properties for LithTech 1.0 (Blood 2 / Shogo)
//	            DTX files, matching the original DEdit's dialog.
//
//------------------------------------------------------------------

#ifndef __LT1TEXTUREPROPDLG_H__
#define __LT1TEXTUREPROPDLG_H__

// Is this file a LithTech 1.0 DTX?
bool LT1Tex_IsLT1File(const char *pFullPath);

// Shows the dialog for these files (full paths) and writes the changes.
BOOL LT1Tex_DoProperties(const CStringArray &paths);

#endif
