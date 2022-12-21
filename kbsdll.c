#include "kbswitch.h"

HINSTANCE g_hinstDLL = NULL;
HHOOK g_hShellHook = NULL;
HHOOK g_hCbtHook = NULL;
HWND g_hwnd = NULL;

static VOID
PostMessageToMainWnd(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (g_hwnd)
        PostMessage(g_hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK
ShellProc(
    INT nCode,
    WPARAM wParam,
    LPARAM lParam)
{
    if (nCode < 0)
        return CallNextHookEx(g_hShellHook, nCode, wParam, lParam);

    switch (nCode)
    {
    case HSHELL_LANGUAGE:
        PostMessageToMainWnd(WM_LANGUAGE, wParam, lParam);
        break;
    case HSHELL_WINDOWACTIVATED:
        PostMessageToMainWnd(WM_WINDOWACTIVATED, wParam, lParam);
        break;
    case HSHELL_WINDOWCREATED:
        PostMessageToMainWnd(WM_WINDOWCREATED, wParam, lParam);
        break;
    case HSHELL_WINDOWDESTROYED:
        PostMessageToMainWnd(WM_WINDOWDESTROYED, wParam, lParam);
        break;
    default:
        break;
    }

    return CallNextHookEx(g_hShellHook, nCode, wParam, lParam);
}

LRESULT CALLBACK
CbtProc(
    INT nCode,
    WPARAM wParam,
    LPARAM lParam)
{
    if (nCode < 0)
        return CallNextHookEx(g_hCbtHook, nCode, wParam, lParam);

    switch (nCode)
    {
    case HCBT_SETFOCUS:
        PostMessageToMainWnd(WM_WINDOWSETFOCUS, wParam, lParam);
        break;
    default:
        break;
    }

    return CallNextHookEx(g_hShellHook, nCode, wParam, lParam);
}

BOOL KbsHook(HWND hwnd)
{
    g_hwnd = hwnd;

    g_hShellHook = SetWindowsHookEx(WH_SHELL, ShellProc, g_hinstDLL, 0);
    g_hCbtHook = SetWindowsHookEx(WH_CBT, CbtProc, g_hinstDLL, 0);
    if (!g_hShellHook || !g_hCbtHook)
    {
        UnhookWindowsHookEx(g_hShellHook);
        UnhookWindowsHookEx(g_hCbtHook);
        g_hShellHook = g_hCbtHook = NULL;
        return FALSE;
    }

    return TRUE;
}

void KbsUnhook(void)
{
    UnhookWindowsHookEx(g_hShellHook);
    UnhookWindowsHookEx(g_hCbtHook);
    g_hShellHook = g_hCbtHook = NULL;
}

BOOL WINAPI
DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    HWND hwnd;

    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            g_hinstDLL = hinstDLL;
            break;
    }

    return TRUE;
}
