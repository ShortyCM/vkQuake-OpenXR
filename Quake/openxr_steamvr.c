/* SteamVR's installed utility can dismiss its dashboard without controllers. */
#include "quakedef.h"
#include "openxr_display.h"
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#endif

qboolean OXR_HideSteamVRDashboard (void)
{
#ifdef _WIN32
	WCHAR	path[4096], command[4200];
	HMODULE runtime = GetModuleHandleW (L"vrclient_x64.dll");
	if (!runtime)
		return false;
	DWORD length = GetModuleFileNameW (runtime, path, countof (path));
	if (!length || length >= countof (path))
		return false;
	WCHAR *leaf = wcsrchr (path, L'\\');
	if (!leaf || (size_t)(leaf - path) + 32 >= countof (path))
		return false;
	++leaf;
	wcscpy (leaf, L"vrcmd.exe");
	if (GetFileAttributesW (path) == INVALID_FILE_ATTRIBUTES)
		wcscpy (leaf, L"win64\\vrcmd.exe");
	if (GetFileAttributesW (path) == INVALID_FILE_ATTRIBUTES)
		return false;
	swprintf (command, countof (command), L"\"%ls\" --hidedashboard", path);
	STARTUPINFOW		startup = {0};
	PROCESS_INFORMATION process = {0};
	startup.cb = sizeof (startup);
	startup.dwFlags = STARTF_USESHOWWINDOW;
	startup.wShowWindow = SW_HIDE;
	if (!CreateProcessW (path, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process))
		return false;
	CloseHandle (process.hThread);
	CloseHandle (process.hProcess);
	return true;
#else
	return false;
#endif
}
