/*
 * PROJECT:         Keyboard Layout Switcher
 * FILE:            base/applications/kbswitch/kbswitch.c
 * PURPOSE:         Switching Keyboard Layouts
 * PROGRAMMERS:     Dmitry Chapyshev (dmitry@reactos.org)
 *                  Colin Finck (mail@colinfinck.de)
 *                  Katayama Hirofumi MZ (katayama.hirofumi.mz@gmail.com)
 */

#include "kbswitch.h"
#include <shlobj.h>
#include <shobjidl.h>
#include "shlwapi_undoc.h"

/*
 * This program kbswitch is a mimic of Win2k's internat.exe.
 * However, there are some differences.
 *
 * Comparing with WinNT4 ActivateKeyboardLayout, WinXP ActivateKeyboardLayout has
 * process boundary, so we cannot activate the IME keyboard layout from the outer process.
 * It needs special care.
 *
 * We use global hook by our kbsdll.dll, to watch the shell and the windows.
 *
 * It might not work correctly on Vista+ because keyboard layout change notification
 * won't be generated in Vista+.
 */

#define TIMER_ID 999
#define TIMER_INTERVAL 1000

#define DEEP_DEBUG

#ifdef DEEP_DEBUG
#include <stdio.h>
void TRACE(const char *fmt, ...)
{
    va_list va;
    char szBuff[1024];
    va_start(va, fmt);
    StringCchVPrintfA(szBuff, _countof(szBuff), fmt, va);
    OutputDebugStringA(szBuff);
    fputs(szBuff, stdout);
    va_end(va);
}
#else
#define TRACE
#endif

typedef struct tagLAYOUT_ENTRY
{
    DWORD dwKLID;
    LPTSTR pszText; // malloc'ed
    DWORD dwVariant;
} LAYOUT_ENTRY, *PLAYOUT_ENTRY;

void DumpWndInfo(HWND hwnd)
{
    TCHAR szClass[64], szText[64];
    szClass[0] = szText[0] = 0;
    GetClassName(hwnd, szClass, _countof(szClass));
    GetWindowText(hwnd, szText, _countof(szText));
    TRACE("hwnd %p: %s: %s\n", hwnd, szClass, szText);
}

typedef BOOL (*FN_KBS_HOOK)(HWND hwnd);
typedef void (*FN_KBS_UNHOOK)(void);

HINSTANCE g_hInstance = NULL;
HINSTANCE g_hDLL = NULL;
HICON g_hTrayIcon = NULL;
UINT g_uTaskbarRestart = 0;
HWND g_hwndTrayWnd = NULL;
HMENU g_hMenu = NULL;
HMENU g_hRightPopupMenu = NULL;
DWORD g_dwCodePageBitField = 0;
FN_KBS_HOOK g_fnKbsHook = NULL;
FN_KBS_UNHOOK g_fnKbsUnhook = NULL;
HWND g_hwndLastActive = NULL;
HKL g_hKL = NULL;

// Shell_NotifyIcon's message ID
#define WM_NOTIFYICONMSG (WM_USER + 248)
// Character Count of a layout ID like "00000409"
#define CCH_LAYOUT_ID    8
// Maximum Character Count of a ULONG in decimal
#define CCH_ULONG_DEC    10
// Is hKL an IME HKL?
#define IS_IME_HKL(hKL) ((((ULONG_PTR)(hKL)) & 0xF0000000) == 0xE0000000)
// Is hKL a variant HKL?
#define IS_VARIANT_HKL(hKL) ((((ULONG_PTR)(hKL)) & 0xF0000000) == 0xF0000000)
// Get hKL's variant
#define GET_HKL_VARIANT(hKL) (HIWORD(hKL) & 0xFFF)

PLAYOUT_ENTRY g_pLayouts = NULL; // LocalAlloc'ed
UINT g_cLayouts = 0, g_cLayoutsCapacity = 0;

static VOID FreeKeyboardLayouts(VOID)
{
    UINT i;
    for (i = 0; i < g_cLayouts; ++i)
    {
        free(g_pLayouts[i].pszText);
    }

    g_cLayouts = g_cLayoutsCapacity = 0;
    LocalFree(g_pLayouts);
    g_pLayouts = NULL;
}

static BOOL LoadKeyboardLayouts(VOID)
{
    HKEY hLayoutsKey, hKey;
    LONG error;
    DWORD dwIndex, cb;
    TCHAR szKeyName[MAX_PATH], szText[MAX_PATH], szDispName[MAX_PATH], szVariant[MAX_PATH];
    PLAYOUT_ENTRY pLayout;
    DWORD dwKLID, dwNewCount = 256;
    HRESULT hr;

    FreeKeyboardLayouts();

    g_pLayouts = LocalAlloc(LPTR, dwNewCount * sizeof(LAYOUT_ENTRY));
    if (g_pLayouts == NULL)
    {
        return FALSE;
    }
    g_cLayoutsCapacity = dwNewCount;

    error = RegOpenKey(HKEY_LOCAL_MACHINE,
                       TEXT("SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts"),
                       &hLayoutsKey);
    if (error != ERROR_SUCCESS)
    {
        return FALSE;
    }

    for (dwIndex = 0; dwIndex < 256; ++dwIndex)
    {
        szKeyName[0] = UNICODE_NULL;
        error = RegEnumKey(hLayoutsKey, dwIndex, szKeyName, _countof(szKeyName));
        if (error != ERROR_SUCCESS)
            break;

        error = RegOpenKey(hLayoutsKey, szKeyName, &hKey);
        if (error != ERROR_SUCCESS)
            break;

        if (g_cLayoutsCapacity < g_cLayouts + 1)
        {
            DWORD dwNewCount = g_cLayouts + 16;
            cb = dwNewCount * sizeof(LAYOUT_ENTRY);
            g_pLayouts = LocalReAlloc(g_pLayouts, cb, LPTR);
            if (g_pLayouts == NULL)
                break;

            g_cLayoutsCapacity = dwNewCount;
        }

        pLayout = &g_pLayouts[g_cLayouts];

        // "Layout Text"
        szText[0] = 0;
        cb = sizeof(szText);
        error = RegQueryValueEx(hKey, TEXT("Layout Text"), NULL, NULL, (LPBYTE)szText, &cb);
        if (error == ERROR_SUCCESS && cb > sizeof(WCHAR))
        {
            // "Layout Id"
            pLayout->dwVariant = 0;
            cb = sizeof(szVariant);
            error = RegQueryValueEx(hKey, TEXT("Layout Id"), NULL, NULL, (LPBYTE)szVariant, &cb);
            if (error == ERROR_SUCCESS && cb > sizeof(WCHAR))
            {
                pLayout->dwVariant = _tcstoul(szVariant, NULL, 16);
            }

            pLayout->pszText = _tcsdup(szText);

            // dwKLID
            pLayout->dwKLID = dwKLID = _tcstoul(szKeyName, NULL, 16);

            g_cLayouts++;
        }

        RegCloseKey(hKey);
    }

    RegCloseKey(hLayoutsKey);
    return g_cLayouts > 0;
}

INT FindLayoutEntry(HKL hKL)
{
    PLAYOUT_ENTRY pEntry;
    UINT i;

    if (IS_IME_HKL(hKL))
    {
        for (i = 0; i < g_cLayouts; ++i)
        {
            if (hKL == (HKL)(DWORD_PTR)g_pLayouts[i].dwKLID)
                return i;
        }
    }
    else if (IS_VARIANT_HKL(hKL))
    {
        DWORD dwVariant = GET_HKL_VARIANT(hKL);
        for (i = 0; i < g_cLayouts; ++i)
        {
            if (LOWORD(hKL) == LOWORD(g_pLayouts[i].dwKLID) && g_pLayouts[i].dwVariant == dwVariant)
                return i;
        }
    }
    else
    {
        LANGID LangID = LOWORD(hKL);
        for (i = 0; i < g_cLayouts; ++i)
        {
            if (LangID == LOWORD(g_pLayouts[i].dwKLID))
                return i;
        }
        LangID = HIWORD(hKL);
        for (i = 0; i < g_cLayouts; ++i)
        {
            if (LangID == LOWORD(g_pLayouts[i].dwKLID))
                return i;
        }
    }

    return -1;
}

static HBITMAP BitmapFromIcon(HICON hIcon)
{
    HDC hdcScreen = GetDC(NULL);
    HDC hdc = CreateCompatibleDC(hdcScreen);
    INT cxIcon = GetSystemMetrics(SM_CXSMICON);
    INT cyIcon = GetSystemMetrics(SM_CYSMICON);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, cxIcon, cyIcon);
    HGDIOBJ hbmOld;

    if (hbm != NULL)
    {
        hbmOld = SelectObject(hdc, hbm);
        DrawIconEx(hdc, 0, 0, hIcon, cxIcon, cyIcon, 0, GetSysColorBrush(COLOR_MENU), DI_NORMAL);
        SelectObject(hdc, hbmOld);
    }

    DeleteDC(hdc);
    ReleaseDC(NULL, hdcScreen);
    return hbm;
}

static BOOL
GetSystemLibraryPath(LPTSTR szPath, SIZE_T cchPath, LPCTSTR FileName)
{
    if (!GetSystemDirectory(szPath, cchPath))
        return FALSE;

    StringCchCat(szPath, cchPath, TEXT("\\"));
    StringCchCat(szPath, cchPath, FileName);
    return TRUE;
}

static HICON
CreateTrayIcon(HKL hKL, LPCTSTR szImeFile OPTIONAL)
{
    LANGID LangID;
    TCHAR szBuf[4];
    HDC hdcScreen, hdc;
    HBITMAP hbmColor, hbmMono, hBmpOld;
    HFONT hFont, hFontOld;
    LOGFONT lf;
    RECT rect;
    ICONINFO IconInfo;
    HICON hIcon;
    INT cxIcon = GetSystemMetrics(SM_CXSMICON);
    INT cyIcon = GetSystemMetrics(SM_CYSMICON);
    TCHAR szPath[MAX_PATH];

    if (szImeFile && szImeFile[0])
    {
        if (GetSystemLibraryPath(szPath, _countof(szPath), szImeFile))
        {
            ExtractIconEx(szPath, 0, NULL, &hIcon, 1);
            if (hIcon)
                return hIcon;
        }
    }

    /* Getting "EN", "FR", etc. from English, French, ... */
    LangID = LOWORD(hKL);
    if (GetLocaleInfo(LangID,
                      LOCALE_SABBREVLANGNAME | LOCALE_NOUSEROVERRIDE,
                      szBuf,
                      _countof(szBuf)) == 0)
    {
        szBuf[0] = szBuf[1] = _T('?');
    }
    szBuf[2] = 0; /* Truncate the identifier to two characters: "ENG" --> "EN" etc. */

    /* Create hdc, hbmColor and hbmMono */
    hdcScreen = GetDC(NULL);
    hdc = CreateCompatibleDC(hdcScreen);
    hbmColor = CreateCompatibleBitmap(hdcScreen, cxIcon, cyIcon);
    ReleaseDC(NULL, hdcScreen);
    hbmMono = CreateBitmap(cxIcon, cyIcon, 1, 1, NULL);

    /* Checking NULL */
    if (!hdc || !hbmColor || !hbmMono)
    {
        if (hbmMono)
            DeleteObject(hbmMono);
        if (hbmColor)
            DeleteObject(hbmColor);
        if (hdc)
            DeleteDC(hdc);
        return NULL;
    }

    /* Create a font */
    hFont = NULL;
    if (SystemParametersInfo(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0))
    {
        /* Override the current size with something manageable */
        lf.lfHeight = -11;
        lf.lfWidth = 0;
        hFont = CreateFontIndirect(&lf);
    }
    if (!hFont)
        hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    SetRect(&rect, 0, 0, cxIcon, cyIcon);

    /* Draw hbmColor */
    hBmpOld = SelectObject(hdc, hbmColor);
    SetDCBrushColor(hdc, GetSysColor(COLOR_HIGHLIGHT));
    FillRect(hdc, &rect, (HBRUSH)GetStockObject(DC_BRUSH));
    hFontOld = SelectObject(hdc, hFont);
    SetTextColor(hdc, GetSysColor(COLOR_HIGHLIGHTTEXT));
    SetBkMode(hdc, TRANSPARENT);
    DrawText(hdc, szBuf, 2, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    SelectObject(hdc, hFontOld);

    /* Fill hbmMono with black */
    SelectObject(hdc, hbmMono);
    PatBlt(hdc, 0, 0, cxIcon, cyIcon, BLACKNESS);
    SelectObject(hdc, hBmpOld);

    /* Create an icon from hbmColor and hbmMono */
    IconInfo.fIcon = TRUE;
    IconInfo.xHotspot = IconInfo.yHotspot = 0;
    IconInfo.hbmColor = hbmColor;
    IconInfo.hbmMask = hbmMono;
    hIcon = CreateIconIndirect(&IconInfo);

    /* Clean up */
    DeleteObject(hFont);
    DeleteObject(hbmMono);
    DeleteObject(hbmColor);
    DeleteDC(hdc);

    return hIcon;
}

HKL ShowKeyboardMenu(HWND hwnd, HKL hCheckKL, POINT pt)
{
    HKL hKL, ahKLs[256];
    UINT iKL, cKLs;
    HMENU hMenu = CreatePopupMenu();
    MENUITEMINFO mii = { sizeof(mii) };
    TCHAR szText[MAX_PATH], szImeFile[MAX_PATH];
    PLAYOUT_ENTRY pEntry;
    INT iEntry;
    HICON hIcon;

    cKLs = GetKeyboardLayoutList(_countof(ahKLs), ahKLs);
    for (iKL = 0; iKL < cKLs; ++iKL)
    {
        hKL = ahKLs[iKL];

        iEntry = FindLayoutEntry(hKL);
        if (iEntry == -1)
            continue;

        pEntry = &g_pLayouts[iEntry];

        szText[0] = 0;
        szImeFile[0] = 0;

        if (IS_IME_HKL(hKL))
        {
            ImmGetDescription(hKL, szText, _countof(szText));
            ImmGetIMEFileName(hKL, szImeFile, _countof(szImeFile));
        }
        else
        {
            GetLocaleInfo(LOWORD(hKL), LOCALE_SLANGUAGE, szText, _countof(szText));
            if (LOWORD(hKL) != HIWORD(hKL))
            {
                if (pEntry->pszText)
                {
                    StringCchCat(szText, _countof(szText), TEXT(" - "));
                    StringCchCat(szText, _countof(szText), pEntry->pszText);
                }
            }
        }

        if (szText[0] == 0)
        {
            StringCchCopy(szText, _countof(szText), TEXT("(Unknown)"));
        }

        mii.fMask       = MIIM_ID | MIIM_STRING;
        mii.wID         = 300 + iKL;
        mii.dwTypeData  = szText;

        hIcon = CreateTrayIcon(hKL, szImeFile);
        if (hIcon)
        {
            mii.hbmpItem = BitmapFromIcon(hIcon);
            if (mii.hbmpItem)
                mii.fMask |= MIIM_BITMAP;
        }

        if (hKL == hCheckKL)
        {
            mii.fMask |= MIIM_STATE;
            mii.fState = MFS_CHECKED;
        }

        InsertMenuItem(hMenu, -1, TRUE, &mii);
        if (hIcon)
            DestroyIcon(hIcon);
    }

    hKL = NULL;
    INT nID = TrackPopupMenu(hMenu, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, NULL);
    if (nID != 0)
    {
        hKL = ahKLs[nID - 300];
    }
    DestroyMenu(hMenu);

    return hKL;
}

static HWND GetTrayWnd(VOID)
{
    return FindWindow(TEXT("Shell_TrayWnd"), NULL);
}

static BOOL IsWndClass(HWND hwnd, LPCTSTR pszClass)
{
    TCHAR szClass[128];
    return (GetClassName(hwnd, szClass, _countof(szClass)) && _tcsicmp(szClass, pszClass) == 0);
}

static BOOL IsKbswitchWindow(HWND hwnd)
{
    return IsWndClass(hwnd, KBSWITCH_CLASS);
}

// NOTE: GetWindowThreadProcessId function doesn't return the correct value on
//       console window.
static BOOL IsConsoleWnd(HWND hwnd)
{
    return IsWndClass(hwnd, TEXT("ConsoleWindowClass"));
}

static BOOL IsTrayWnd(HWND hwnd)
{
    return g_hwndTrayWnd == hwnd;
}

static HWND GetTopLevelOwner(HWND hwndTarget)
{
    HWND hwndDesktop = GetDesktopWindow();
    HWND hTopWnd = hwndTarget;

    for (;;)
    {
        if (hwndTarget == NULL || hwndTarget == hwndDesktop)
            break;
        hTopWnd = hwndTarget;
        if ((GetWindowLongPtrW(hwndTarget, GWL_STYLE) & WS_CHILD) == 0)
            hwndTarget = GetWindow(hwndTarget, GW_OWNER);
        else
            hwndTarget = GetParent(hwndTarget);
    }

    return hTopWnd;
}

static HWND RealGetTopLevelOwner(HWND hwndTarget)
{
    DWORD dwTID1, dwTID2;
    HWND hwndTopLevel = GetTopLevelOwner(hwndTarget);

    dwTID1 = GetWindowThreadProcessId(hwndTopLevel, NULL);
    dwTID2 = GetWindowThreadProcessId(hwndTarget, NULL);
    if (dwTID1 != dwTID2)
        hwndTopLevel = hwndTarget;

    return hwndTopLevel;
}

static BOOL IsWndIgnored(HWND hwndTarget)
{
    HWND hwndTopLevel = RealGetTopLevelOwner(hwndTarget);

    return !IsWindowVisible(hwndTopLevel) ||
           IsTrayWnd(hwndTopLevel) ||
           IsKbswitchWindow(hwndTopLevel);
}

static void SetLastActive(HWND hwndTarget, INT line)
{
    if (g_hwndLastActive != hwndTarget)
    {
        TRACE("SetLastActive: %d (%d)\n", hwndTarget, line);
        DumpWndInfo(hwndTarget);
        g_hwndLastActive = hwndTarget;
    }
}

static DWORD GetCodePageBitField(HWND hwnd)
{
    CHARSETINFO CharSet;
    HDC hDC = GetDC(hwnd);
    UINT uCharSet = GetTextCharset(hDC);
    TranslateCharsetInfo((LPDWORD)(UINT_PTR)uCharSet, &CharSet, TCI_SRCCHARSET);
    ReleaseDC(hwnd, hDC);
    return CharSet.fs.fsCsb[0];
}

static BOOL IsHKLCharSetSupported(HKL hKL)
{
    LOCALESIGNATURE Signature;
    if (!GetLocaleInfoW(LOWORD(hKL), LOCALE_FONTSIGNATURE, (LPWSTR)&Signature, sizeof(Signature)))
        return FALSE;
    return !!(Signature.lsCsbSupported[0] & g_dwCodePageBitField);
}

static BOOL GetImeFile(LPTSTR szImeFile, SIZE_T cchImeFile, HKL hKL)
{
    szImeFile[0] = UNICODE_NULL;

    if (hKL == NULL || !IS_IME_HKL(hKL))
        return FALSE;

    return ImmGetIMEFileName(hKL, szImeFile, cchImeFile);
}

static VOID
AddTrayIcon(HWND hwnd, HKL hKL)
{
    NOTIFYICONDATA tnid = { sizeof(tnid), hwnd, 1, NIF_ICON | NIF_MESSAGE | NIF_TIP };
    TCHAR szImeFile[80];
    INT iEntry;

    iEntry = FindLayoutEntry(hKL);
    if (iEntry == -1)
        return;

    GetImeFile(szImeFile, _countof(szImeFile), hKL);

    tnid.uCallbackMessage = WM_NOTIFYICONMSG;
    tnid.hIcon = CreateTrayIcon(hKL, szImeFile);
    StringCchCopy(tnid.szTip, _countof(tnid.szTip), g_pLayouts[iEntry].pszText);

    Shell_NotifyIcon(NIM_ADD, &tnid);

    if (g_hTrayIcon)
        DestroyIcon(g_hTrayIcon);
    g_hTrayIcon = tnid.hIcon;
}

static VOID
DeleteTrayIcon(HWND hwnd)
{
    NOTIFYICONDATA tnid = { sizeof(tnid), hwnd, 1 };
    Shell_NotifyIcon(NIM_DELETE, &tnid);

    if (g_hTrayIcon)
    {
        DestroyIcon(g_hTrayIcon);
        g_hTrayIcon = NULL;
    }
}

static VOID
UpdateTrayIcon(HWND hwnd, HKL hKL)
{
    NOTIFYICONDATA tnid = { sizeof(tnid), hwnd, 1, NIF_ICON | NIF_MESSAGE | NIF_TIP };
    TCHAR szImeFile[80];
    INT iEntry;

    iEntry = FindLayoutEntry(hKL);
    if (iEntry == -1)
        return;

    GetImeFile(szImeFile, _countof(szImeFile), hKL);

    tnid.uCallbackMessage = WM_NOTIFYICONMSG;
    tnid.hIcon = CreateTrayIcon(hKL, szImeFile);
    StringCchCopy(tnid.szTip, _countof(tnid.szTip), g_pLayouts[iEntry].pszText);

    Shell_NotifyIcon(NIM_MODIFY, &tnid);

    if (g_hTrayIcon)
        DestroyIcon(g_hTrayIcon);
    g_hTrayIcon = tnid.hIcon;
}

static void
RememberWindowHKL(HWND hwnd, HWND hwndTarget, HKL hKL)
{
    TCHAR szHWND[32];

    StringCchPrintf(szHWND, _countof(szHWND), TEXT("%p"), hwndTarget);

    SetProp(hwnd, szHWND, hKL);
}

static HKL
RecallWindowHKL(HWND hwnd, HWND hwndTarget)
{
    TCHAR szHWND[32];
    DWORD dwThreadId;
    HKL hKL;

    StringCchPrintf(szHWND, _countof(szHWND), TEXT("%p"), hwndTarget);

    hKL = (HKL)GetProp(hwnd, szHWND);
    if (hKL == NULL)
    {
        dwThreadId = GetWindowThreadProcessId(hwndTarget, NULL);
        hKL = GetKeyboardLayout(dwThreadId);
        if (hKL == NULL)
            hKL = GetKeyboardLayout(0);
    }

    return hKL;
}

static void
ForgetWindowHKL(HWND hwnd, HWND hwndTarget)
{
    TCHAR szHWND[32];

    StringCchPrintf(szHWND, _countof(szHWND), TEXT("%p"), hwndTarget);
    RemoveProp(hwnd, szHWND);
}

static BOOL OnCreate(HWND hwnd, LPCREATESTRUCT lpCreateStruct)
{
    if (!LoadKeyboardLayouts())
        return FALSE;

    g_hDLL = LoadLibrary(TEXT("kbsdll.dll"));
    if (!g_hDLL)
        return TRUE;

    g_fnKbsHook = (FN_KBS_HOOK)GetProcAddress(g_hDLL, "KbsHook");
    g_fnKbsUnhook = (FN_KBS_UNHOOK)GetProcAddress(g_hDLL, "KbsUnhook");
    if (!g_fnKbsHook || !g_fnKbsUnhook)
    {
        g_fnKbsHook = NULL;
        g_fnKbsUnhook = NULL;
        FreeLibrary(g_hDLL);
        g_hDLL = NULL;
        return FALSE;
    }

    g_hKL = GetKeyboardLayout(0);
    AddTrayIcon(hwnd, g_hKL);
    g_hwndTrayWnd = GetTrayWnd();
    g_uTaskbarRestart = RegisterWindowMessage(TEXT("TaskbarCreated"));

    g_dwCodePageBitField = GetCodePageBitField(hwnd);

    g_fnKbsHook(hwnd);
    SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
    return TRUE;
}

static void OnTimer(HWND hwnd, UINT id)
{
    if (id != TIMER_ID)
        return;

    HWND hwndTarget = GetForegroundWindow();
    if (IsWndIgnored(hwndTarget))
        return;

    SetLastActive(hwndTarget, __LINE__);

    DWORD dwThreadId = GetWindowThreadProcessId(hwndTarget, NULL);
    HKL hKL = GetKeyboardLayout(dwThreadId);
    if (hKL == NULL)
    {
        hKL = RecallWindowHKL(hwnd, hwndTarget);
    }

    TRACE("hKL++: %p\n", hKL);
    UpdateTrayIcon(hwnd, hKL);
    g_hKL = hKL;
}

static BOOL CALLBACK
RemovePropProc(HWND hwnd, LPCTSTR lpszString, HANDLE hData)
{
    RemoveProp(hwnd, lpszString);
    return TRUE;
}

static void OnDestroy(HWND hwnd)
{
    KillTimer(hwnd, TIMER_ID);

    if (g_hMenu)
    {
        DestroyMenu(g_hMenu);
        g_hMenu = NULL;
    }

    DeleteTrayIcon(hwnd);

    if (g_fnKbsUnhook)
    {
        g_fnKbsUnhook();
    }

    if (g_hDLL)
    {
        FreeLibrary(g_hDLL);
        g_hDLL = NULL;
    }

    g_fnKbsHook = NULL;
    g_fnKbsUnhook = NULL;

    EnumProps(hwnd, RemovePropProc);

    FreeKeyboardLayouts();

    PostQuitMessage(0);
}

static void ChooseLayout(HWND hwnd, HKL hKL)
{
    HWND hwndTarget = g_hwndLastActive;
    if (hwndTarget == NULL)
        return;

    HWND hwndTopLevel = GetTopLevelOwner(hwndTarget);
    DWORD dwTID1 = GetWindowThreadProcessId(hwndTopLevel, NULL);
    DWORD dwTID2 = GetWindowThreadProcessId(hwndTarget, NULL);
    if (dwTID1 != dwTID2)
    {
        hwndTopLevel = hwndTarget;
    }

    HWND hwndLastActive = GetLastActivePopup(hwndTopLevel);
    SetForegroundWindow(hwndLastActive);

    BOOL bSupported = IsHKLCharSetSupported(hKL);
    PostMessage(hwndLastActive, WM_INPUTLANGCHANGEREQUEST, bSupported, (LPARAM)hKL);

    TRACE("hKL--: %p\n", hKL);
}

static void OnNotifyIcon(HWND hwnd, LPARAM lParam)
{
    POINT pt;

    switch (lParam)
    {
        case WM_RBUTTONUP:
        case WM_LBUTTONUP:
        {
            KillTimer(hwnd, TIMER_ID);

            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);

            if (lParam == WM_LBUTTONUP)
            {
                /* Rebuild the left popup menu on every click to take care of keyboard layout changes */
                HKL hKL = ShowKeyboardMenu(hwnd, g_hKL, pt);
                if (hKL)
                {
                    ChooseLayout(hwnd, hKL);
                    g_hKL = hKL;
                }
            }
            else
            {
                if (!g_hRightPopupMenu)
                {
                    g_hMenu = LoadMenu(g_hInstance, MAKEINTRESOURCE(IDR_POPUP));
                    g_hRightPopupMenu = GetSubMenu(g_hMenu, 0);
                }
                TrackPopupMenu(g_hRightPopupMenu, 0, pt.x, pt.y, 0, hwnd, NULL);
            }

            PostMessage(hwnd, WM_NULL, 0, 0);
            PostMessage(g_hwndTrayWnd, WM_NULL, 0, 0);

            SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
            break;
        }
    }
}

static VOID AttachWindowInput(HWND hwndAttach)
{
    DWORD dwTID1 = GetWindowThreadProcessId(g_hwndTrayWnd, NULL);
    DWORD dwTID2 = GetWindowThreadProcessId(hwndAttach, NULL);
    if (!AttachThreadInput(dwTID2, dwTID1, TRUE))
        MessageBeep(0);
}

static HKL GetNextLayout(void)
{
    HKL ahKLs[256];
    UINT iKL, cKLs;

    cKLs = GetKeyboardLayoutList(_countof(ahKLs), ahKLs);
    for (iKL = 0; iKL < cKLs; ++iKL)
    {
        if (g_hKL == ahKLs[iKL])
        {
            return ahKLs[(iKL + 1) % cKLs];
        }
    }

    return NULL;
}

static void OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify)
{
    switch (id)
    {
        case ID_EXIT:
        {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            break;
        }

        case ID_PREFERENCES:
        {
            INT_PTR ret = (INT_PTR)ShellExecute(hwnd, NULL,
                                                TEXT("control.exe"), TEXT("input.dll"),
                                                NULL, SW_SHOWNORMAL);
            if (ret <= 32)
                MessageBox(hwnd, _T("Can't start input.dll"), NULL, MB_ICONERROR);
            break;
        }

        case ID_NEXTLAYOUT:
        {
            HKL hKL = GetNextLayout();
            ChooseLayout(hwnd, hKL);
            break;
        }

        default:
            break;
    }
}

LRESULT CALLBACK
WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        HANDLE_MSG(hwnd, WM_CREATE, OnCreate);
        HANDLE_MSG(hwnd, WM_TIMER, OnTimer);
        HANDLE_MSG(hwnd, WM_COMMAND, OnCommand);
        HANDLE_MSG(hwnd, WM_DESTROY, OnDestroy);
        case WM_NOTIFYICONMSG:
        {
            OnNotifyIcon(hwnd, lParam);
            break;
        }
        case WM_LANGUAGE: // HSHELL_LANGUAGE
        {
            HWND hwndTarget = (HWND)wParam;
            HKL hKL = (HKL)lParam;
            TRACE("WM_LANGUAGE: %p, %p\n", wParam, lParam);
            if (hKL == NULL || hwndTarget == NULL)
                break;
            DumpWndInfo(hwndTarget);
            if (IsWndIgnored(hwndTarget))
                break;
            if (IsConsoleWnd(hwndTarget) && hKL)
                RememberWindowHKL(hwnd, hwndTarget, hKL);
            g_hKL = hKL;
            UpdateTrayIcon(hwnd, g_hKL);
            break;
        }
        case WM_WINDOWACTIVATED: // HSHELL_WINDOWACTIVATED
        {
            HWND hwndTarget = (HWND)wParam;
            DWORD dwThreadId = 0;
            HKL hKL = NULL;
            TRACE("WM_WINDOWACTIVATED: %p, %p\n", wParam, lParam);

            if (IsWndIgnored(hwndTarget))
                break;

            DumpWndInfo(hwndTarget);

            if (IsConsoleWnd(hwndTarget))
            {
                hKL = RecallWindowHKL(hwnd, hwndTarget);
            }
            else
            {
                dwThreadId = GetWindowThreadProcessId(hwndTarget, NULL);
                hKL = GetKeyboardLayout(dwThreadId);
            }

            g_hKL = hKL;
            UpdateTrayIcon(hwnd, g_hKL);
            break;
        }
        case WM_WINDOWCREATED: // HSHELL_WINDOWCREATED
        {
            HWND hwndTarget = (HWND)wParam;
            TRACE("WM_WINDOWCREATED: %p, %p\n", wParam, lParam);
            DumpWndInfo(hwndTarget);
            break;
        }
        case WM_WINDOWDESTROYED: // HSHELL_WINDOWCREATED
        {
            HWND hwndTarget = (HWND)wParam;
            TRACE("WM_WINDOWDESTROYED: %p, %p\n", wParam, lParam);
            DumpWndInfo(hwndTarget);
            if (IsConsoleWnd(hwndTarget))
                ForgetWindowHKL(hwnd, hwndTarget);
            break;
        }
        case WM_WINDOWSETFOCUS: // HCBT_SETFOCUS
        {
            HWND hwndGaining = (HWND)wParam;
            HWND hwndLosing = (HWND)lParam;
            TRACE("WM_WINDOWSETFOCUS: %p, %p\n", wParam, lParam);
            DumpWndInfo(hwndGaining);
            break;
        }
        default:
        {
            if (uMsg == g_uTaskbarRestart)
            {
                AddTrayIcon(hwnd, g_hKL);
                g_hwndTrayWnd = GetTrayWnd();
                break;
            }
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
    }
    return 0;
}

int main(void)
{
    WNDCLASS WndClass;
    MSG msg;
    HANDLE hMutex;
    HWND hwnd;
    HINSTANCE hInstance = GetModuleHandle(NULL);

    switch (GetUserDefaultUILanguage())
    {
        case MAKELANGID(LANG_HEBREW, SUBLANG_DEFAULT):
            SetProcessDefaultLayout(LAYOUT_RTL);
            break;
        default:
            break;
    }

    hMutex = CreateMutex(NULL, TRUE, KBSWITCH_CLASS);
    if (!hMutex)
        return 1;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(hMutex);
        return 1;
    }

    g_hInstance = hInstance;

    ZeroMemory(&WndClass, sizeof(WndClass));
    WndClass.lpfnWndProc   = WindowProc;
    WndClass.hInstance     = hInstance;
    WndClass.hCursor       = LoadCursor(NULL, IDC_ARROW);
    WndClass.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    WndClass.lpszClassName = KBSWITCH_CLASS;
    if (!RegisterClass(&WndClass))
    {
        CloseHandle(hMutex);
        return 1;
    }

    hwnd = CreateWindow(KBSWITCH_CLASS, NULL, 0, 0, 0, 320, 200, NULL, NULL,
                        hInstance, NULL);
    if (hwnd == NULL)
    {
        MessageBox(NULL, TEXT("CreateWindow failed"), KBSWITCH_CLASS, MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return (INT)msg.wParam;
}
