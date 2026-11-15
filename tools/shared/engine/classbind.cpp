
#include "bdefs.h"
#include "classbind.h"
#include "bindmgr.h"
#include "ltobjectcreate.h"


struct ClassBindModule
{
	HBINDMODULE	m_hModule;
	ClassDef	**m_pClassDefs;
	int			m_nClassDefs;
};


typedef ClassDef** (*ObjectDLLSetupFn)(int *nDefs, void *pServer, int *version);


static void cb_VerifyClassDefProperties(ClassBindModule *pModule)
{
	int i, j;
	PropDef *pProp;

	for(i=0; i < pModule->m_nClassDefs; i++)
	{
		for(j=0; j < pModule->m_pClassDefs[i]->m_nProps; j++)
		{
			pProp = &pModule->m_pClassDefs[i]->m_Props[j];

			if(pProp->m_PropType >= NUM_PROPERTYTYPES || pProp->m_PropType < 0)
				pProp->m_PropType = NUM_PROPERTYTYPES;
		}
	}
}



// ------------------------------------------------------------------
// LithTech 1.0 (Blood 2 / Shogo) object modules.
//
// Their ClassDef and PropDef structures are laid out differently from the
// ones this code base uses (an extra message function pair instead of the
// plugin function, 16 bit property flags, bit mask property groups). When
// a module turns out to use that layout it is converted here to our layout,
// so the rest of DEdit doesn't have to know about it.
// ------------------------------------------------------------------

struct LT1PropDef
{
	char	*m_PropName;
	short	m_PropType;
	float	m_DefaultValueVector[3];
	float	m_DefaultValueFloat;
	char	*m_DefaultValueString;
	short	m_PropFlags;
	void	*m_pInternal;
};

struct LT1ClassDef
{
	char			*m_ClassName;
	LT1ClassDef		*m_ParentClass;
	uint32			m_ClassFlags;
	void			*m_ConstructFn;
	void			*m_DestructFn;
	void			*m_EngineMessageFn;
	void			*m_ObjectMessageFn;
	short			m_nProps;
	LT1PropDef		*m_Props;
	long			m_ClassObjectSize;
	void			*m_pInternal[2];
};

static bool cb_ReadableString(const char *p)
{
	return p && !IsBadStringPtrA(p, 512);
}

// Does this list of class definitions look like our layout?
static bool cb_LooksLikeCurrentLayout(ClassDef **pDefs, int nDefs)
{
	for(int i=0; i < nDefs; i++)
	{
		ClassDef *pDef = pDefs[i];
		if(!pDef || IsBadReadPtr(pDef, sizeof(ClassDef)) || !cb_ReadableString(pDef->m_ClassName))
			return false;

		if(pDef->m_nProps < 0 || pDef->m_nProps > 2000)
			return false;

		if(pDef->m_nProps == 0)
			continue;

		if(!pDef->m_Props || IsBadReadPtr(pDef->m_Props, sizeof(PropDef) * pDef->m_nProps))
			return false;

		if(!cb_ReadableString(pDef->m_Props[0].m_PropName))
			return false;
	}
	return true;
}

static uint32 cb_ConvertLT1PropFlags(uint32 flags)
{
	// Bits 0-4 and 5 (group owner) are the same. Bits 6-11 were one bit per
	// group; the current layout stores the group number in those bits.
	uint32 out = flags & 0x3F;
	for(int g=1; g <= 6; g++)
	{
		if(flags & (1 << (5 + g)))
		{
			out |= PF_GROUP(g);
			break;
		}
	}
	return out;
}

static ClassDef** cb_ConvertLT1Classes(ClassDef **pOldDefs, int nDefs)
{
	LT1ClassDef **pOld = (LT1ClassDef**)pOldDefs;
	ClassDef **pNewList = (ClassDef**)malloc(sizeof(ClassDef*) * (nDefs ? nDefs : 1));
	ClassDef *pNew = (ClassDef*)calloc(nDefs ? nDefs : 1, sizeof(ClassDef));
	int i, j;

	for(i=0; i < nDefs; i++)
	{
		LT1ClassDef *pIn = pOld[i];
		ClassDef *pOut = &pNew[i];

		pNewList[i] = pOut;

		pOut->m_ClassName		= pIn->m_ClassName;
		pOut->m_ClassFlags		= pIn->m_ClassFlags;
		pOut->m_ConstructFn		= (ConstructObjectFn)pIn->m_ConstructFn;
		pOut->m_DestructFn		= (DestructObjectFn)pIn->m_DestructFn;
		pOut->m_PluginFn		= NULL;
		pOut->m_ClassObjectSize	= pIn->m_ClassObjectSize;
		pOut->m_nProps			= pIn->m_nProps;
		pOut->m_Props			= NULL;

		if(pIn->m_nProps > 0)
		{
			pOut->m_Props = (PropDef*)calloc(pIn->m_nProps, sizeof(PropDef));
			for(j=0; j < pIn->m_nProps; j++)
			{
				LT1PropDef *pInProp = &pIn->m_Props[j];
				PropDef *pOutProp = &pOut->m_Props[j];

				pOutProp->m_PropName			= pInProp->m_PropName;
				pOutProp->m_PropType			= pInProp->m_PropType;
				pOutProp->m_DefaultValueVector	= LTVector(pInProp->m_DefaultValueVector[0], pInProp->m_DefaultValueVector[1], pInProp->m_DefaultValueVector[2]);
				pOutProp->m_DefaultValueFloat	= pInProp->m_DefaultValueFloat;
				pOutProp->m_DefaultValueString	= pInProp->m_DefaultValueString;
				pOutProp->m_PropFlags			= cb_ConvertLT1PropFlags((uint16)pInProp->m_PropFlags);
				pOutProp->m_pDEditInternal		= NULL;
				pOutProp->m_pInternal			= NULL;
			}
		}
	}

	// Parent pointers.
	for(i=0; i < nDefs; i++)
	{
		pNew[i].m_ParentClass = NULL;
		if(pOld[i]->m_ParentClass)
		{
			for(j=0; j < nDefs; j++)
			{
				if(pOld[j] == pOld[i]->m_ParentClass)
				{
					pNew[i].m_ParentClass = &pNew[j];
					break;
				}
			}
		}
	}

	return pNewList;
}

#ifdef __PS2
extern ClassDef** ObjectDLLSetup(int *nDefs, ILTServer *pServer, int *version);
#endif

int cb_LoadModule(const char *pModuleName, ILTServer *pServer, HCLASSMODULE *phModule, int *version)
{
	ClassBindModule *pModule;
	// this is alloced here and then returned :(
	pModule = (ClassBindModule*)malloc(sizeof(ClassBindModule));
	HBINDMODULE hModule;

	// platforms that use static binding should just call the funciton.
	// Otherwise figure out which function and call that

	#ifdef __PS2
	pModule->m_hModule = NULL;
	// FIX THIS MJS - should not hard code to a name with "DLL" in it
	pModule->m_pClassDefs = ObjectDLLSetup(&pModule->m_nClassDefs, 
	                                       pServer, version);

	#else
	ObjectDLLSetupFn theFunction;
	int status;

	
	status = bm_BindModule(pModuleName, &hModule);
	if(status == BIND_CANTFINDMODULE)
		return CB_CANTFINDMODULE;

	// Get the function.
	theFunction = (ObjectDLLSetupFn)bm_GetFunctionPointer(hModule, "ObjectDLLSetup");
	if(!theFunction)
	{
		bm_UnbindModule(hModule);
		return CB_NOTCLASSMODULE;
	}

	// Ok.. setup the HCLASSMODULE.
	pModule->m_hModule = hModule;
	pModule->m_pClassDefs = theFunction(&pModule->m_nClassDefs, pServer, version);

	if(*version != SERVEROBJ_VERSION)
	{
		free(pModule);
		bm_UnbindModule(hModule);
		return CB_VERSIONMISMATCH;
	}
	#endif

	// LithTech 1.0 modules use a different structure layout.
	if(!cb_LooksLikeCurrentLayout(pModule->m_pClassDefs, pModule->m_nClassDefs))
		pModule->m_pClassDefs = cb_ConvertLT1Classes(pModule->m_pClassDefs, pModule->m_nClassDefs);

	// Verify all properties!
	cb_VerifyClassDefProperties(pModule);

	*phModule = pModule;
	return CB_NOERROR;
}


void cb_UnloadModule(HCLASSMODULE hModule)
{
	ClassBindModule *pModule;

	pModule = (ClassBindModule*)hModule;
	bm_UnbindModule(pModule->m_hModule);

	free(pModule);
}


int cb_GetNumClassDefs(HCLASSMODULE hModule)
{
	return ((ClassBindModule*)hModule)->m_nClassDefs;
}


ClassDef** cb_GetClassDefs(HCLASSMODULE hModule)
{
	return ((ClassBindModule*)hModule)->m_pClassDefs;
}


ClassDef* cb_FindClass(HCLASSMODULE hModule, const char *pClassName)
{
	ClassBindModule *pModule;
	int i;

	pModule = (ClassBindModule*)hModule;
	for(i=0; i < pModule->m_nClassDefs; i++)
		if(strcmp(pModule->m_pClassDefs[i]->m_ClassName, pClassName) == 0)
			return pModule->m_pClassDefs[i];

	return LTNULL;
}


PropDef* cb_FindVarFull(HCLASSMODULE hModule, ClassDef *pClass, const char *pVarName)
{
	int i;

	while(pClass)
	{
		for(i=0; i < pClass->m_nProps; i++)
			if(strcmp(pClass->m_Props[i].m_PropName, pVarName) == 0)
				return &pClass->m_Props[i];
	
		pClass = pClass->m_ParentClass;
	}

	return LTNULL;
}


// IsClassFlagSet
//
// Finds if class flags are set in this or any parent class...
ClassDef *cb_IsClassFlagSet( HCLASSMODULE hModule, ClassDef *pClass, const uint32 dwClassFlag )
{
	while(pClass)
	{
		if( pClass->m_ClassFlags & dwClassFlag )
			return pClass;
	
		pClass = pClass->m_ParentClass;
	}

	return LTNULL;
}


bool cb_IsChildClass( HCLASSMODULE hModule, const char *pChildName, const char *pParentName )
{
	ClassDef *pClassDef = cb_FindClass(hModule, pChildName);

	// Walk up the heirarchy looking for the parent name
	while (pClassDef)
	{
		// Did we find the parent?
		if (strcmp(pClassDef->m_ClassName, pParentName) == 0)
			return true;

		// Go up the heirarchy
		pClassDef = pClassDef->m_ParentClass;
	}

	// We didn't find the parent...
	return false;
}