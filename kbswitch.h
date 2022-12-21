#pragma once

#include <windows.h>
#include <windowsx.h>
#include <imm.h>
#include <shellapi.h>
#include <tchar.h>
#include <strsafe.h>
#include "resource.h"

#define KBSWITCH_CLASS TEXT("kbswitcher")

#define WM_LANGUAGE             (WM_USER + 100)
#define WM_WINDOWACTIVATED      (WM_USER + 101)
#define WM_WINDOWCREATED        (WM_USER + 102)
#define WM_WINDOWDESTROYED      (WM_USER + 103)
#define WM_WINDOWSETFOCUS       (WM_USER + 104)
