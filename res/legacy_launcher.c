// Ships in the release as melonDS.exe, purely to hand over to RyuE.exe.
//
// Installs made before the rename run melonDS.exe, and their updater -- already
// out in the wild, no longer fixable -- relaunches whatever it was started as
// after applying an update. Without a melonDS.exe in the release it would keep
// starting the old binary while version.txt already claimed to be current: up
// to date by its own reckoning, and never updating again. With this here, that
// updater overwrites melonDS.exe, relaunches it, and this hands straight over.
//
// Nothing else lives in here on purpose. It is ~50 KB next to a 100 MB
// emulator, and it can be dropped once nobody is running a pre-rename build.

#include <windows.h>

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    (void)inst; (void)prev; (void)cmdline; (void)show;

    // RyuE.exe sits next to us, not next to whatever the working directory is.
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return 1;

    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return 1;
    slash[1] = 0;

    wchar_t dir[MAX_PATH];
    wcscpy(dir, path);
    wcscat(path, L"RyuE.exe");

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(path, GetCommandLineW(), NULL, NULL, FALSE, 0, NULL, dir, &si, &pi))
    {
        MessageBoxW(NULL, L"RyuE.exe が見つかりません。\n"
                          L"同じフォルダに RyuE.exe があるか確認してください。",
                    L"RyuE", MB_ICONERROR);
        return 1;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}
