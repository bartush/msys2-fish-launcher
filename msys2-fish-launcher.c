#define __USE_MINGW_ANSI_STDIO 1
#define _UNICODE
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 1

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>


int _CRT_glob = 0;

// if any of the properties change, it's best to use a brand new AppID
#define APPID_REVISION 13

#define WM_TRAYICON (WM_APP + 1)
#define TRAY_POLL_MS 500

static HWND g_hwnd = NULL;      // message-only window for tray callbacks
static HICON g_hicon = NULL;    // tray icon, loaded from resources
static HWND g_term = NULL;      // terminal (mintty) window
static HANDLE g_job = NULL;     // job object: kills entire process tree
static int g_tray_visible = 0;
static int g_closing = 0;
static DWORD g_menu_shown_tick = 0; // suppress spurious left-clicks after context menu
static NOTIFYICONDATAW g_nid;

static void ShowError(const wchar_t* desc, const wchar_t* err, const long code)
{
    wchar_t msg[1024];
    swprintf(msg, 1024, L"%ls. Reason: %ls (0x%lx)", desc, err, code);
    MessageBox(NULL, msg, L"Launcher error", MB_ICONEXCLAMATION | MB_OK);
}

static void ShowLastError(const wchar_t* desc)
{
    DWORD code;
    wchar_t* err;

    code = GetLastError();
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&err, 0, NULL);
    ShowError(desc, err, code);
    LocalFree(err);
}

static void ShowErrno(const wchar_t* desc)
{
    wchar_t* err;

    err = _wcserror(errno);
    ShowError(desc, err, errno);
}

// adapted from http://www.partow.net/programming/hashfunctions/index.html
// MIT licensed
static unsigned int APHash(const wchar_t* str, size_t length)
{
    unsigned int hash = 0xAAAAAAAA;
    size_t i = 0;

    for (i = 0; i < length; ++str, ++i)
    {
	hash ^= ((i & 1) == 0)
	    ? ((hash <<  7) ^ (*str) * (hash >> 3))
	    : (~((hash << 11) + ((*str) ^ (hash >> 5))));
    }

    return hash;
}

static PROCESS_INFORMATION StartChild(wchar_t* cmdline)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    SetLastError(0);
    code = CreateProcess(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (code == 0)
    {
	ShowLastError(L"Could not start the shell");
	ShowError(L"The command was", cmdline, 0);
	return pi;
    }

    return pi;
}

static void TrayUpdateTooltip(void)
{
    wchar_t title[256];

    if (g_term == NULL || !IsWindow(g_term))
        return;
    if (GetWindowTextW(g_term, title, 256) == 0)
        return;
    wcsncpy(g_nid.szTip, title, ARRAYSIZE(g_nid.szTip) - 1);
    g_nid.szTip[ARRAYSIZE(g_nid.szTip) - 1] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TrayShow(void)
{
    if (g_tray_visible)
        return;
    if (Shell_NotifyIconW(NIM_ADD, &g_nid) == 0)
    {
        ShowLastError(L"Could not add tray icon");
        return;
    }
    g_tray_visible = 1;
}

static void TrayHide(void)
{
    if (!g_tray_visible)
        return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_tray_visible = 0;
}

static void ShowTerminal(void)
{
    if (g_closing)
        return;
    if (g_term != NULL && IsWindow(g_term))
    {
        ShowWindow(g_term, SW_RESTORE);
        SetForegroundWindow(g_term);
    }
    TrayHide();
}

static void CloseTerminal(void)
{
    g_closing = 1;
    if (g_job != NULL)
        TerminateJobObject(g_job, 0);
    g_term = NULL;
}

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK)
        {
            // ignore spurious left-clicks within 1s after context menu
            if (GetTickCount() - g_menu_shown_tick < 1000)
                return 0;
            ShowTerminal();
        }
        else if (lParam == WM_RBUTTONUP)
        {
            HMENU menu;
            POINT pt;
            UINT cmd;

            g_menu_shown_tick = GetTickCount();
            menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Show");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, 2, L"Close terminal");
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(menu);
            // MSDN: must send WM_COMMAND after TrackPopupMenu
            SendMessageW(hwnd, WM_COMMAND, cmd, 0);
        }
        return 0;
    case WM_COMMAND:
        if (wParam == 1)
            ShowTerminal();
        else if (wParam == 2)
            CloseTerminal();
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

struct FindCtx
{
    DWORD pid;
    HWND hwnd;
};

static BOOL CALLBACK EnumFindProc(HWND hwnd, LPARAM lparam)
{
    struct FindCtx* ctx = (struct FindCtx*)lparam;
    DWORD out;

    if (GetWindowThreadProcessId(hwnd, &out) == 0)
        return TRUE;
    if (out != ctx->pid)
        return TRUE;
    if (!IsWindowVisible(hwnd))
        return TRUE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd)
        return TRUE;
    ctx->hwnd = hwnd;
    return FALSE;
}

static HWND FindTerminalWindow(DWORD pid)
{
    struct FindCtx ctx;

    ctx.pid = pid;
    ctx.hwnd = NULL;
    EnumWindows(EnumFindProc, (LPARAM)&ctx);
    return ctx.hwnd;
}

// a freshly created window is at the top of the Z-order, so the first
// visible mintty window in EnumWindows order is the newest one
static BOOL CALLBACK EnumFirstMinttyProc(HWND hwnd, LPARAM lparam)
{
    wchar_t cls[32];

    if (GetClassNameW(hwnd, cls, 32) == 0 || _wcsicmp(cls, L"mintty") != 0)
        return TRUE;
    if (!IsWindowVisible(hwnd))
        return TRUE;
    *(HWND*)lparam = hwnd;
    return FALSE;
}

static HWND FindNewestMinttyWindow(void)
{
    HWND hwnd = NULL;

    EnumWindows(EnumFirstMinttyProc, (LPARAM)&hwnd);
    return hwnd;
}

int wmain(int argc, wchar_t* argv[])
{
    (void)(argc);
    (void)(argv);

    // Set MSYS to persistent UCRT64
    const wchar_t msystem[] = L"UCRT64";
    const wchar_t start_app[] = L"fish";

    PROCESS_INFORMATION child;
    int code;
    size_t buflen;
    wchar_t* buf;
    wchar_t* tmp;
    wchar_t* msysdirhash;
    wchar_t msysdir[PATH_MAX];
    wchar_t exepath[PATH_MAX];

    code = GetModuleFileName(NULL, exepath, sizeof(exepath) / sizeof(*exepath));
    if (code == 0)
    {
	ShowLastError(L"Could not determine executable path");
	return __LINE__;
    }

    wcscpy(msysdir, exepath);
    tmp = wcsrchr(msysdir, L'\\');
    if (tmp == NULL)
    {
	ShowError(L"Could not find root directory", msysdir, 0);
	return __LINE__;
    }
    *tmp = L'\0';

    // the launcher may be run from any folder; fall back to the default
    // MSYS2 installation if mintty is not next to the executable
    {
	wchar_t probe[PATH_MAX];

	swprintf(probe, PATH_MAX, L"%ls\\usr\\bin\\mintty.exe", msysdir);
	if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES)
	{
		wcscpy(msysdir, L"C:\\msys64");
		swprintf(probe, PATH_MAX, L"%ls\\usr\\bin\\mintty.exe", msysdir);
		if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES)
		{
			ShowError(L"Could not find mintty.exe in the executable folder or in the default MSYS2 installation", L"C:\\msys64", 0);
			return __LINE__;
		}
	}
    }

    msysdirhash = (wchar_t*)alloca(10 * sizeof(wchar_t)); // dot + unsigned int as %x + null
    if (msysdirhash == NULL)
    {
	ShowError(L"Could not allocate memory", L"", 0);
	return __LINE__;
    }
    if (wcsicmp(msysdir, L"C:\\msys64") == 0)
    {
	code = swprintf(msysdirhash, 10, L""); // no change in AppID for default installations
    }
    else
    {
	code = swprintf(msysdirhash, 10, L".%8x", APHash(msysdir, wcslen(msysdir)));
    }
    if (code < 0)
    {
	ShowErrno(L"Could not write to buffer");
	return __LINE__;
    }

    code = SetEnvironmentVariable(L"CHERE_INVOKING", L"1");
    if (code == 0)
    {
	ShowLastError(L"Could not set environment variable");
    }

    code = SetEnvironmentVariable(L"MSYSTEM", msystem);
    if (code == 0)
    {
	ShowLastError(L"Could not set environment variable");
    }

    code = SetEnvironmentVariable(L"MSYSCON", L"mintty.exe");
    if (code == 0)
    {
	ShowLastError(L"Could not set environment variable");
    }

    code = -1;
    buf = NULL;
    buflen = 1024;
    while (code < 0 && buflen < 8192)
    {
	buf = (wchar_t*)realloc(buf, buflen * sizeof(wchar_t));
	if (buf == NULL)
	{
	    ShowError(L"Could not allocate memory", L"", 0);
	    return __LINE__;
	}

	code = swprintf(buf, buflen,
			L"%ls\\usr\\bin\\mintty.exe -i '%ls' -o 'AppLaunchCmd=%ls'"
			L" -o 'AppID=MSYS2.FishShell.%ls.%d%ls' -o 'AppName=MSYS2 %ls Fish Shell'"
			L" -t 'MSYS2 %ls Fish Shell' --store-taskbar-properties -- %ls %ls",
			msysdir, exepath, exepath,
			msystem, APPID_REVISION, msysdirhash, msystem,
			msystem, L"/usr/bin/sh -lc '\"$@\"' sh", start_app);
	buflen *= 2;
    }
    if (code < 0)
    {
	ShowErrno(L"Could not write to buffer");
	return __LINE__;
    }

    // create a job object so we can kill the entire process tree later
    g_job = CreateJobObjectW(NULL, NULL);
    if (g_job == NULL)
    {
        ShowLastError(L"Could not create job object");
        return __LINE__;
    }

    child = StartChild(buf);
    if (child.hProcess == NULL)
    {
	CloseHandle(g_job);
	g_job = NULL;
	return __LINE__;
    }

    // assign the child to the job object; all descendants inherit it
    if (!AssignProcessToJobObject(g_job, child.hProcess))
    {
        ShowLastError(L"Could not assign process to job object");
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
        CloseHandle(g_job);
        g_job = NULL;
	return __LINE__;
    }

    free(buf);
    buf = NULL;
    // tray setup: message-only window + icon from resources
    {
        WNDCLASSEXW wc;

        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TrayWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"MSYS2FishLauncherTray";
        if (RegisterClassExW(&wc) == 0)
        {
            ShowLastError(L"Could not register tray window class");
            return __LINE__;
        }
        // a real (hidden) top-level window is required for reliable tray
        // callbacks; message-only windows do not receive mouse messages
        g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
        if (g_hwnd == NULL)
        {
            ShowLastError(L"Could not create tray window");
            return __LINE__;
        }

        if (ExtractIconExW(exepath, 0, &g_hicon, NULL, 1) != 1 || g_hicon == NULL)
        {
            ShowLastError(L"Could not load tray icon");
            return __LINE__;
        }

        ZeroMemory(&g_nid, sizeof(g_nid));
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = g_hwnd;
        g_nid.uID = 1;
        // NIF_MESSAGE is required for uCallbackMessage to be delivered
        g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_MESSAGE;
        g_nid.hIcon = g_hicon;
        g_nid.uCallbackMessage = WM_TRAYICON;
    }

    // main loop: watch the terminal, move it to the tray when minimized.
    // mintty re-execs itself (new PID, same window), so once the window is
    // found we track the HWND, not the process.
    for (;;)
    {
        MSG msg;

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (g_closing)
            break;
        if (g_term == NULL)
        {
            HWND term = FindTerminalWindow(child.dwProcessId);
            if (term == NULL && WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0)
                term = FindNewestMinttyWindow(); // mintty re-exec'd: find its new window
            if (term != NULL)
                g_term = term;
            else if (WaitForSingleObject(child.hProcess, 0) == WAIT_OBJECT_0)
                break; // child exited and no window found
        }
        else
        {
            if (!IsWindow(g_term))
                break; // terminal window destroyed
            if (IsIconic(g_term))
            {
                ShowWindow(g_term, SW_HIDE);
                TrayUpdateTooltip();
                TrayShow();
            }
            else if (g_tray_visible)
                TrayHide();
        }
        Sleep(TRAY_POLL_MS);
    }

    TrayHide();
    DestroyWindow(g_hwnd);
    if (g_job != NULL)
        CloseHandle(g_job);
    CloseHandle(child.hProcess);
    CloseHandle(child.hThread);
    return 0;
}
