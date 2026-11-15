//------------------------------------------------------------------
//
//	FILE	  : LT1EdReader.h
//
//	PURPOSE	  : Reads a LithTech 1.0 (Blood 2 / Shogo) .ed level into a region.
//
//------------------------------------------------------------------

#ifndef __LT1EDREADER_H__
#define __LT1EDREADER_H__

class CEditRegion;
class CEditProjectMgr;

// Loads a version 1247 .ed (plain or zlib block compressed) into the (empty) region. Returns false
// and fills sError on failure; sWarnings lists things that couldn't be
// carried over (unknown object classes, properties that don't exist here, ...).
bool LT1Ed_ReadFile(CEditRegion *pRegion, CEditProjectMgr *pProject, const char *pFilename, CString &sError, CString &sWarnings);

#endif
