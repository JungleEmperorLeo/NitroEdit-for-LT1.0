//------------------------------------------------------------------
//
//	FILE	  : CrashHandler.cpp
//
//	PURPOSE	  : Writes DEdit_crash.txt and DEdit_crash.dmp next to DEdit.exe
//	            when the editor crashes, so the crash can be tracked down.
//	            The text file lists the exception, the faulting address as
//	            module+offset, and a stack walk (match the offsets against
//	            DEdit.map).
//
//------------------------------------------------------------------

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "dbghelp.lib")

static void CH_Describe(FILE *fp, DWORD addr)
{
	MEMORY_BASIC_INFORMATION mbi;
	char name[MAX_PATH] = "?";
	if(VirtualQuery((void*)addr, &mbi, sizeof(mbi)) && mbi.AllocationBase)
	{
		GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, sizeof(name));
		const char *slash = strrchr(name, '\\');
		fprintf(fp, "%08X  %s+0x%X\n", addr, slash ? slash + 1 : name, addr - (DWORD)mbi.AllocationBase);
	}
	else
		fprintf(fp, "%08X\n", addr);
}

static LONG WINAPI CH_Filter(EXCEPTION_POINTERS *pInfo)
{
	static bool bBusy = false;
	if(bBusy)
		return EXCEPTION_CONTINUE_SEARCH;
	bBusy = true;

	char dir[MAX_PATH];
	GetModuleFileNameA(NULL, dir, sizeof(dir));
	char *slash = strrchr(dir, '\\');
	if(slash)
		*slash = 0;

	char path[MAX_PATH];
	sprintf(path, "%s\\NitroEdit_crash.txt", dir);
	FILE *fp = fopen(path, "w");
	if(fp)
	{
		EXCEPTION_RECORD *er = pInfo->ExceptionRecord;
		CONTEXT *c = pInfo->ContextRecord;
		fprintf(fp, "Exception %08X at ", er->ExceptionCode);
		CH_Describe(fp, (DWORD)er->ExceptionAddress);
		if(er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
			fprintf(fp, "Access violation %s address %08X\n", er->ExceptionInformation[0] ? "writing" : "reading", (DWORD)er->ExceptionInformation[1]);
#ifdef _M_IX86
		fprintf(fp, "EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X\nEBP=%08X ESP=%08X EIP=%08X\n",
			c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip);

		fprintf(fp, "\nFrame chain:\n");
		DWORD ebp = c->Ebp;
		for(int i=0; i < 40 && ebp && !IsBadReadPtr((void*)ebp, 8); i++)
		{
			CH_Describe(fp, ((DWORD*)ebp)[1]);
			DWORD next = ((DWORD*)ebp)[0];
			if(next <= ebp)
				break;
			ebp = next;
		}

		fprintf(fp, "\nStack words that point into code:\n");
		DWORD *sp = (DWORD*)c->Esp;
		int n = 0;
		for(int i=0; i < 512 && n < 60 && !IsBadReadPtr(sp + i, 4); i++)
		{
			MEMORY_BASIC_INFORMATION mbi;
			if(VirtualQuery((void*)sp[i], &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) && mbi.State == MEM_COMMIT)
			{
				CH_Describe(fp, sp[i]);
				n++;
			}
		}
#endif
		fclose(fp);
	}

	sprintf(path, "%s\\NitroEdit_crash.dmp", dir);
	HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if(hFile != INVALID_HANDLE_VALUE)
	{
		MINIDUMP_EXCEPTION_INFORMATION mei;
		mei.ThreadId = GetCurrentThreadId();
		mei.ExceptionPointers = pInfo;
		mei.ClientPointers = FALSE;
		MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &mei, NULL, NULL);
		CloseHandle(hFile);
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

struct CCrashHandlerInstaller
{
	CCrashHandlerInstaller() { SetUnhandledExceptionFilter(CH_Filter); }
};

static CCrashHandlerInstaller g_CrashHandlerInstaller;
