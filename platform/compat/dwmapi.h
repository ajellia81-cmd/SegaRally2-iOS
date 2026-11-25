// Win32 compatibility (iOS port): no compositor timing, so app.cpp's display
// clock uses its own estimate of the refresh (its fallback on Windows too)
#pragma once
#include "windows.h"
typedef struct _DWM_TIMING_INFO {
    UINT32 cbSize;
    ULONGLONG qpcRefreshPeriod;
    ULONGLONG qpcVBlank;
} DWM_TIMING_INFO;
inline HRESULT DwmGetCompositionTimingInfo(HWND, DWM_TIMING_INFO*) { return E_FAIL; }
