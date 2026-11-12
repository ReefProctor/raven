#include <windows.h>

__declspec(dllexport) DWORD WINAPI RavenLocalDsoundMarker(void)
{
	return 0x5241564eu;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
	return TRUE;
}
