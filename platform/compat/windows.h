// Minimal Win32 compatibility layer for the iOS port (and a Linux build used
// to check it compiles). Only what the game's sources use is here: basic
// types, files and folders, time, threads, the window queries app.cpp makes
// (answered by the SDL window in platform/vk_presenter_sdl.cpp), the
// keyboard state (fed by SDL and the touch controls) and a small GDI text
// renderer on stb_truetype for the menus' and notices' fonts.
// Implemented in platform/compat/win_compat.cpp.
#pragma once
#ifndef _WIN32

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <string>

// ---- Calling conventions and basic types ----
#define WINAPI
#define CALLBACK
#define APIENTRY
#define STDMETHODCALLTYPE
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

typedef int BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef uint32_t UINT32;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint64_t DWORD64;
typedef intptr_t INT_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t UINT_PTR;
typedef uintptr_t ULONG_PTR;
typedef intptr_t LRESULT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef int32_t HRESULT;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef char* LPSTR;
typedef const char* LPCSTR;
typedef wchar_t* LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef DWORD COLORREF;

typedef void* HANDLE;
struct HWND__;
typedef HWND__* HWND;
typedef void* HINSTANCE;
typedef void* HMODULE;
typedef void* HDC;
typedef void* HGDIOBJ;
typedef void* HFONT;
typedef void* HBITMAP;
typedef void* HICON;
typedef void* HCURSOR;

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef struct tagRECT {
    LONG left, top, right, bottom;
} RECT;
typedef struct tagPOINT {
    LONG x, y;
} POINT;
typedef struct tagSIZE {
    LONG cx, cy;
} SIZE;
typedef struct tagWINDOWPLACEMENT {
    UINT length;
    UINT flags;
    UINT showCmd;
    POINT ptMinPosition, ptMaxPosition;
    RECT rcNormalPosition;
} WINDOWPLACEMENT;

#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define CP_UTF8 65001
#define CP_ACP 0

#define LOWORD(l) ((WORD)(((DWORD_PTR_T)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR_T)(l)) >> 16) & 0xffff))
typedef uintptr_t DWORD_PTR_T;
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)(((WORD)(l)) | (((DWORD)(WORD)(h)) << 16)))
#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)(((WORD)(l)) | (((DWORD)(WORD)(h)) << 16)))
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))

// ---- Keys (virtual-key codes) ----
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_DELETE 0x2E
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B

// ---- Window messages the input hook receives ----
#define WM_SIZE 0x0005
#define WM_CLOSE 0x0010
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_MOUSEWHEEL 0x020A
#define WHEEL_DELTA 120
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#define GET_WHEEL_DELTA_WPARAM(wp) ((short)HIWORD(wp))

#define MB_OK 0x0
#define MB_ICONERROR 0x10
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
#define SW_SHOWNORMAL 1
#define SW_SHOW 5

// ---- Strings ----
int MultiByteToWideChar(UINT cp, DWORD flags, const char* src, int n, wchar_t* dst, int cap);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t* src, int n, char* dst, int cap, const char* def,
                        BOOL* used_def);
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
inline int _putenv(const char* s) {
    // "NAME=value" (empty value: unset)
    const char* eq = strchr(s, '=');
    if (!eq) return -1;
    std::string name(s, size_t(eq - s));
    if (!eq[1]) return unsetenv(name.c_str());
    return setenv(name.c_str(), eq + 1, 1);
}
inline int localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : -1; }
inline int gmtime_s(struct tm* out, const time_t* t) { return gmtime_r(t, out) ? 0 : -1; }
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

inline BOOL PtInRect(const RECT* r, POINT p) {
    return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom;
}
inline BOOL SetRect(RECT* r, int l, int t, int rr, int b) {
    r->left = l, r->top = t, r->right = rr, r->bottom = b;
    return TRUE;
}
extern "C" char** environ;
#define _environ environ

// ---- Files and folders ----
BOOL CreateDirectoryA(const char* path, void* security);
BOOL CreateDirectoryW(const wchar_t* path, void* security);
DWORD GetFileAttributesA(const char* path);
DWORD GetFileAttributesW(const wchar_t* path);
BOOL DeleteFileA(const char* path);
BOOL MoveFileExA(const char* from, const char* to, DWORD flags);
#define MOVEFILE_REPLACE_EXISTING 0x1
DWORD GetFullPathNameA(const char* path, DWORD cap, char* out, char** file_part);
DWORD GetFullPathNameW(const wchar_t* path, DWORD cap, wchar_t* out, wchar_t** file_part);
DWORD GetModuleFileNameA(HMODULE module, char* out, DWORD cap);
DWORD GetModuleFileNameW(HMODULE module, wchar_t* out, DWORD cap);
HMODULE GetModuleHandleA(const char* name);

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    DWORD nFileSizeHigh, nFileSizeLow;
    char cFileName[MAX_PATH];
} WIN32_FIND_DATAA;
typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    DWORD nFileSizeHigh, nFileSizeLow;
    wchar_t cFileName[MAX_PATH];
} WIN32_FIND_DATAW;
HANDLE FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* out);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA* out);
HANDLE FindFirstFileW(const wchar_t* pattern, WIN32_FIND_DATAW* out);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* out);
BOOL FindClose(HANDLE h);

// _wfopen: wide path to UTF-8
FILE* _wfopen(const wchar_t* path, const wchar_t* mode);

// ---- Time ----
BOOL QueryPerformanceCounter(LARGE_INTEGER* out);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* out);
ULONGLONG GetTickCount64();
inline DWORD GetTickCount() { return DWORD(GetTickCount64()); }
void Sleep(DWORD ms);

// ---- Threads and processes ----
typedef DWORD(WINAPI* LPTHREAD_START_ROUTINE)(LPVOID);
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
HANDLE CreateThread(void* security, size_t stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD* id);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL CloseHandle(HANDLE h);
[[noreturn]] void ExitProcess(UINT code);
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258

// Console (the game attaches to a parent console on Windows; nothing here)
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define FILE_TYPE_UNKNOWN 0x0000
#define FILE_TYPE_CHAR 0x0002
#define ATTACH_PARENT_PROCESS ((DWORD)-1)
inline HANDLE GetStdHandle(DWORD) { return (HANDLE)(intptr_t)1; }
inline DWORD GetFileType(HANDLE) { return FILE_TYPE_CHAR; }
inline BOOL AttachConsole(DWORD) { return FALSE; }

// ---- The window (one: the game's) ----
// The presenter registers its window here; the queries below answer from it
void compat_set_window(HWND hwnd, int width, int height);
void compat_set_focus(bool focused);
HWND GetForegroundWindow();
BOOL GetClientRect(HWND hwnd, RECT* rc);
BOOL ScreenToClient(HWND hwnd, POINT* p);
BOOL ClientToScreen(HWND hwnd, POINT* p);
BOOL GetCursorPos(POINT* p);
BOOL SetWindowTextA(HWND hwnd, const char* text);
int MessageBoxA(HWND hwnd, const char* text, const char* caption, UINT type);
int MessageBoxW(HWND hwnd, const wchar_t* text, const wchar_t* caption, UINT type);

// The keyboard: what SDL and the touch controls hold down (virtual-key codes)
void compat_set_key(int vk, bool down);
void compat_release_keys();
short GetAsyncKeyState(int vk);
short GetKeyState(int vk);

// ---- GDI text (stb_truetype) ----
#define FW_NORMAL 400
#define FW_BOLD 700
#define DEFAULT_CHARSET 1
#define OUT_DEFAULT_PRECIS 0
#define OUT_TT_ONLY_PRECIS 7
#define CLIP_DEFAULT_PRECIS 0
#define ANTIALIASED_QUALITY 4
#define CLEARTYPE_QUALITY 5
#define DEFAULT_QUALITY 0
#define DEFAULT_PITCH 0
#define FF_DONTCARE 0
#define TRANSPARENT 1
#define OPAQUE 2
#define DIB_RGB_COLORS 0
#define BI_RGB 0

#pragma pack(push, 2)
typedef struct tagBITMAPFILEHEADER {
    WORD bfType;
    DWORD bfSize;
    WORD bfReserved1;
    WORD bfReserved2;
    DWORD bfOffBits;
} BITMAPFILEHEADER;
#pragma pack(pop)
typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER;
typedef struct tagRGBQUAD {
    BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved;
} RGBQUAD;
typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
} BITMAPINFO;
static_assert(sizeof(BITMAPFILEHEADER) == 14, "BMP file header");
static_assert(sizeof(BITMAPINFOHEADER) == 40, "BMP info header");

typedef struct tagTEXTMETRICW {
    LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth,
        tmWeight, tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY;
    WCHAR tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    BYTE tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
} TEXTMETRICW;

HANDLE AddFontMemResourceEx(void* data, DWORD size, void* reserved, DWORD* count);
BOOL RemoveFontMemResourceEx(HANDLE h);
HFONT CreateFontW(int height, int width, int escapement, int orientation, int weight, DWORD italic,
                  DWORD underline, DWORD strike, DWORD charset, DWORD out_precision, DWORD clip_precision,
                  DWORD quality, DWORD pitch, const wchar_t* face);
HDC CreateCompatibleDC(HDC dc);
BOOL DeleteDC(HDC dc);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj);
BOOL DeleteObject(HGDIOBJ obj);
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO* bi, UINT usage, void** bits, HANDLE section, DWORD offset);
BOOL GetTextExtentPoint32W(HDC dc, const wchar_t* s, int n, SIZE* out);
BOOL GetTextMetricsW(HDC dc, TEXTMETRICW* out);
int SetBkMode(HDC dc, int mode);
COLORREF SetTextColor(HDC dc, COLORREF c);
BOOL TextOutW(HDC dc, int x, int y, const wchar_t* s, int n);
inline BOOL GdiFlush() { return TRUE; }

// ---- Sound (mmsystem) ----
#define SND_SYNC 0x0000
#define SND_ASYNC 0x0001
#define SND_NODEFAULT 0x0002
#define SND_MEMORY 0x0004
#define SND_LOOP 0x0008
#define SND_FILENAME 0x00020000
BOOL PlaySoundW(const wchar_t* sound, HMODULE module, DWORD flags);
inline UINT timeBeginPeriod(UINT) { return 0; }
inline UINT timeEndPeriod(UINT) { return 0; }

// ---- Shell ----
HINSTANCE ShellExecuteW(HWND hwnd, const wchar_t* op, const wchar_t* file, const wchar_t* params,
                        const wchar_t* dir, int show);
HINSTANCE ShellExecuteA(HWND hwnd, const char* op, const char* file, const char* params, const char* dir, int show);

// ---- COM (only CoInitialize, nothing else is used without _WIN32) ----
#define COINIT_MULTITHREADED 0x0
#define COINIT_APARTMENTTHREADED 0x2
inline HRESULT CoInitializeEx(void*, DWORD) { return S_OK; }
inline void CoUninitialize() {}

#endif  // !_WIN32
