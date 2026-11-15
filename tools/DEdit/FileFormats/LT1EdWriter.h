//------------------------------------------------------------------
//
//	FILE	  : LT1EdWriter.h
//
//	PURPOSE	  : Writes a region as a LithTech 1.0 (Blood 2 / Shogo) .ed file.
//
//------------------------------------------------------------------

#ifndef __LT1EDWRITER_H__
#define __LT1EDWRITER_H__

#include "ltvector.h"

class CEditRegion;

// Saves the region as a version 1247 .ed. Returns false and fills sError on
// failure; sWarnings lists things that couldn't be represented.
bool LT1Ed_WriteFile(CEditRegion *pRegion, const char *pFilename, CString &sError, CString &sWarnings);

// The two base texture axes LithTech 1.0 picks for a surface normal.
void LT1Ed_GetBaseAxes(const LTVector &n, LTVector &U, LTVector &V);

#endif
