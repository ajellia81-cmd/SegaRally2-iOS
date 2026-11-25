// Win32 compatibility layer for the iOS port: see windows.h next to this file.
#include "windows.h"

#include <dirent.h>
#include <fnmatch.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <SDL3/SDL.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

// ---------------------------------------------------------------- strings

static std::string to_utf8(const wchar_t* s, int n = -1) {
    std::string out;
    if (!s) return out;
    size_t len = n < 0 ? wcslen(s) : size_t(n);
    for (size_t i = 0; i < len; i++) {
        uint32_t c = uint32_t(s[i]);
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
        else if (c < 0x10000) out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
        else out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3F)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    }
    return out;
}

static std::wstring from_utf8(const char* s, int n = -1) {
    std::wstring out;
    if (!s) return out;
    size_t len = n < 0 ? strlen(s) : size_t(n);
    for (size_t i = 0; i < len;) {
        uint8_t b = uint8_t(s[i]);
        uint32_t c;
        int extra;
        if (b < 0x80) c = b, extra = 0;
        else if ((b >> 5) == 6) c = b & 0x1F, extra = 1;
        else if ((b >> 4) == 14) c = b & 0x0F, extra = 2;
        else if ((b >> 3) == 30) c = b & 0x07, extra = 3;
        else { i++; out += L'?'; continue; }
        i++;
        for (int k = 0; k < extra && i < len; k++, i++) c = (c << 6) | (uint8_t(s[i]) & 0x3F);
        out += wchar_t(c);
    }
    return out;
}

int MultiByteToWideChar(UINT, DWORD, const char* src, int n, wchar_t* dst, int cap) {
    // n == -1: up to and including the terminating zero (counted)
    std::wstring w = from_utf8(src, n);
    int need = int(w.size()) + (n < 0 ? 1 : 0);
    if (!dst || cap == 0) return need;
    if (cap < need) return 0;
    memcpy(dst, w.data(), w.size() * sizeof(wchar_t));
    if (n < 0) dst[w.size()] = 0;
    return need;
}

int WideCharToMultiByte(UINT, DWORD, const wchar_t* src, int n, char* dst, int cap, const char*, BOOL* used_def) {
    if (used_def) *used_def = FALSE;
    std::string s = to_utf8(src, n);
    int need = int(s.size()) + (n < 0 ? 1 : 0);
    if (!dst || cap == 0) return need;
    if (cap < need) return 0;
    memcpy(dst, s.data(), s.size());
    if (n < 0) dst[s.size()] = 0;
    return need;
}

FILE* _wfopen(const wchar_t* path, const wchar_t* mode) { return fopen(to_utf8(path).c_str(), to_utf8(mode).c_str()); }

// ---------------------------------------------------------------- files

BOOL CreateDirectoryA(const char* path, void*) { return mkdir(path, 0755) == 0; }
BOOL CreateDirectoryW(const wchar_t* path, void*) { return CreateDirectoryA(to_utf8(path).c_str(), nullptr); }

DWORD GetFileAttributesA(const char* path) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
DWORD GetFileAttributesW(const wchar_t* path) { return GetFileAttributesA(to_utf8(path).c_str()); }
BOOL DeleteFileA(const char* path) { return unlink(path) == 0; }
BOOL MoveFileExA(const char* from, const char* to, DWORD) { return rename(from, to) == 0; }

DWORD GetFullPathNameA(const char* path, DWORD cap, char* out, char** file_part) {
    std::string full = path;
    if (full.empty() || full[0] != '/') {
        char cwd[4096];
        if (getcwd(cwd, sizeof cwd)) full = std::string(cwd) + "/" + full;
    }
    if (full.size() + 1 > cap) return DWORD(full.size() + 1);
    memcpy(out, full.c_str(), full.size() + 1);
    if (file_part) {
        char* slash = strrchr(out, '/');
        *file_part = slash ? slash + 1 : out;
    }
    return DWORD(full.size());
}
DWORD GetFullPathNameW(const wchar_t* path, DWORD cap, wchar_t* out, wchar_t** file_part) {
    char buf[4096];
    if (!GetFullPathNameA(to_utf8(path).c_str(), sizeof buf, buf, nullptr)) return 0;
    std::wstring w = from_utf8(buf);
    if (w.size() + 1 > cap) return DWORD(w.size() + 1);
    memcpy(out, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    if (file_part) {
        wchar_t* slash = wcsrchr(out, L'/');
        *file_part = slash ? slash + 1 : out;
    }
    return DWORD(w.size());
}

static std::string executable_path() {
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return buf;
    return {};
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = 0;
    return buf;
#endif
}
DWORD GetModuleFileNameA(HMODULE, char* out, DWORD cap) {
    std::string p = executable_path();
    if (!cap) return 0;
    size_t n = std::min<size_t>(p.size(), cap - 1);
    memcpy(out, p.data(), n);
    out[n] = 0;
    return DWORD(n);
}
DWORD GetModuleFileNameW(HMODULE, wchar_t* out, DWORD cap) {
    std::wstring p = from_utf8(executable_path().c_str());
    if (!cap) return 0;
    size_t n = std::min<size_t>(p.size(), cap - 1);
    memcpy(out, p.data(), n * sizeof(wchar_t));
    out[n] = 0;
    return DWORD(n);
}
HMODULE GetModuleHandleA(const char*) { return (HMODULE)1; }

namespace {
struct FindState {
    std::vector<std::string> names;
    std::vector<bool> dirs;
    size_t pos = 0;
};
FindState* find_open(const std::string& pattern) {
    std::string dir = ".", glob = pattern;
    size_t slash = pattern.find_last_of("/\\");
    if (slash != std::string::npos) dir = pattern.substr(0, slash), glob = pattern.substr(slash + 1);
    if (dir.empty()) dir = "/";
    DIR* d = opendir(dir.c_str());
    if (!d) return nullptr;
    auto* f = new FindState;
    while (dirent* e = readdir(d)) {
        if (fnmatch(glob.c_str(), e->d_name, FNM_CASEFOLD) != 0) continue;
        f->names.push_back(e->d_name);
        f->dirs.push_back(GetFileAttributesA((dir + "/" + e->d_name).c_str()) == FILE_ATTRIBUTE_DIRECTORY);
    }
    closedir(d);
    if (f->names.empty()) {
        delete f;
        return nullptr;
    }
    return f;
}
}  // namespace

HANDLE FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* out) {
    FindState* f = find_open(pattern);
    if (!f) return INVALID_HANDLE_VALUE;
    if (!FindNextFileA(f, out)) {
        delete f;
        return INVALID_HANDLE_VALUE;
    }
    return f;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA* out) {
    auto* f = static_cast<FindState*>(h);
    if (!f || f->pos >= f->names.size()) return FALSE;
    memset(out, 0, sizeof *out);
    snprintf(out->cFileName, sizeof out->cFileName, "%s", f->names[f->pos].c_str());
    out->dwFileAttributes = f->dirs[f->pos] ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    f->pos++;
    return TRUE;
}
HANDLE FindFirstFileW(const wchar_t* pattern, WIN32_FIND_DATAW* out) {
    FindState* f = find_open(to_utf8(pattern));
    if (!f) return INVALID_HANDLE_VALUE;
    if (!FindNextFileW(f, out)) {
        delete f;
        return INVALID_HANDLE_VALUE;
    }
    return f;
}
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* out) {
    auto* f = static_cast<FindState*>(h);
    if (!f || f->pos >= f->names.size()) return FALSE;
    memset(out, 0, sizeof *out);
    std::wstring w = from_utf8(f->names[f->pos].c_str());
    wcsncpy(out->cFileName, w.c_str(), MAX_PATH - 1);
    out->dwFileAttributes = f->dirs[f->pos] ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    f->pos++;
    return TRUE;
}
BOOL FindClose(HANDLE h) {
    delete static_cast<FindState*>(h);
    return TRUE;
}

// ---------------------------------------------------------------- time

static int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
BOOL QueryPerformanceCounter(LARGE_INTEGER* out) {
    out->QuadPart = now_ns();
    return TRUE;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER* out) {
    out->QuadPart = 1000000000LL;
    return TRUE;
}
ULONGLONG GetTickCount64() { return ULONGLONG(now_ns() / 1000000); }
void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---------------------------------------------------------------- threads

namespace {
struct ThreadHandle {
    pthread_t thread{};
    LPTHREAD_START_ROUTINE fn = nullptr;
    LPVOID arg = nullptr;
    std::atomic<bool> done{false};
};
void* thread_main(void* p) {
    auto* t = static_cast<ThreadHandle*>(p);
    t->fn(t->arg);
    t->done = true;
    return nullptr;
}
}  // namespace

HANDLE CreateThread(void*, size_t stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD, DWORD* id) {
    auto* t = new ThreadHandle;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack) {
        // A large stack is only reserved address space until it is touched;
        // if the system refuses it, try smaller ones
        size_t page = size_t(getpagesize());
        size_t size = (stack + page - 1) / page * page;
        while (size >= (8u << 20) && pthread_attr_setstacksize(&attr, size) != 0) size /= 2;
    }
    int r = pthread_create(&t->thread, &attr, thread_main, t);
    if (r != 0 && stack) {   // the reservation was refused at creation: 256 MB, then 64
        for (size_t size : {size_t(256) << 20, size_t(64) << 20}) {
            pthread_attr_setstacksize(&attr, size);
            if ((r = pthread_create(&t->thread, &attr, thread_main, t)) == 0) {
                fprintf(stderr, "[compat] game thread stack reduced to %zu MB\n", size >> 20);
                break;
            }
        }
    }
    pthread_attr_destroy(&attr);
    if (r != 0) {
        delete t;
        return nullptr;
    }
    if (id) *id = 1;
    return t;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    auto* t = static_cast<ThreadHandle*>(h);
    if (!t) return WAIT_OBJECT_0;
    if (ms == INFINITE) {
        pthread_join(t->thread, nullptr);
        return WAIT_OBJECT_0;
    }
    int64_t end = now_ns() + int64_t(ms) * 1000000;
    while (!t->done && now_ns() < end) Sleep(1);
    return t->done ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
BOOL CloseHandle(HANDLE) { return TRUE; }   // threads are left detached in effect
void ExitProcess(UINT code) {
    fflush(stdout);
    fflush(stderr);
    _exit(int(code));
}

// ---------------------------------------------------------------- window and keys

namespace {
std::atomic<HWND> g_hwnd{nullptr};
std::atomic<int> g_win_w{0}, g_win_h{0};
std::atomic<bool> g_focused{true};
std::atomic<uint8_t> g_keys[256];
}  // namespace

void compat_set_window(HWND hwnd, int width, int height) {
    g_hwnd = hwnd;
    g_win_w = width;
    g_win_h = height;
}
void compat_set_focus(bool focused) { g_focused = focused; }
HWND GetForegroundWindow() { return g_focused ? g_hwnd.load() : nullptr; }
BOOL GetClientRect(HWND, RECT* rc) {
    rc->left = rc->top = 0;
    rc->right = g_win_w;
    rc->bottom = g_win_h;
    return TRUE;
}
BOOL ScreenToClient(HWND, POINT*) { return TRUE; }   // the window fills the screen
BOOL ClientToScreen(HWND, POINT*) { return TRUE; }
BOOL GetCursorPos(POINT* p) {
    p->x = p->y = 0;
    return TRUE;
}
BOOL SetWindowTextA(HWND, const char*) { return TRUE; }   // no title bar on iOS

int MessageBoxA(HWND, const char* text, const char* caption, UINT type) {
    fprintf(stderr, "[%s] %s\n", caption ? caption : "", text ? text : "");
    SDL_ShowSimpleMessageBox((type & MB_ICONERROR) ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION,
                             caption ? caption : "", text ? text : "", nullptr);
    return 1;
}
int MessageBoxW(HWND hwnd, const wchar_t* text, const wchar_t* caption, UINT type) {
    return MessageBoxA(hwnd, to_utf8(text).c_str(), to_utf8(caption).c_str(), type);
}

void compat_set_key(int vk, bool down) {
    if (vk >= 0 && vk < 256) g_keys[vk] = down;
}
void compat_release_keys() {
    for (auto& k : g_keys) k = 0;
}
short GetAsyncKeyState(int vk) { return vk >= 0 && vk < 256 && g_keys[vk] ? short(0x8000) : 0; }
short GetKeyState(int vk) { return GetAsyncKeyState(vk); }

// ---------------------------------------------------------------- GDI text

namespace {

enum class Kind { Font, Bitmap, Dc };
struct GdiObject {
    Kind kind;
    explicit GdiObject(Kind k) : kind(k) {}
    virtual ~GdiObject() = default;
};

struct FontFile {
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    std::vector<std::wstring> families;
    int win_ascent = 0, win_descent = 0;   // OS/2 usWinAscent / usWinDescent (font units), what GDI measures with
};
std::mutex g_font_mutex;
std::vector<std::unique_ptr<FontFile>> g_fonts;

struct GdiFont : GdiObject {
    GdiFont() : GdiObject(Kind::Font) {}
    FontFile* file = nullptr;
    float scale = 1;
    int ascent = 0, descent = 0, em = 0;
};
struct GdiBitmap : GdiObject {
    GdiBitmap() : GdiObject(Kind::Bitmap) {}
    int w = 0, h = 0;
    bool top_down = true;
    std::vector<uint32_t> px;
};
struct GdiDc : GdiObject {
    GdiDc() : GdiObject(Kind::Dc) {}
    GdiFont* font = nullptr;
    GdiBitmap* bitmap = nullptr;
    COLORREF color = 0;
};

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

std::wstring name_string(const stbtt_fontinfo& info, int name_id) {
    // Windows platform, Unicode BMP, any English language; UTF-16BE
    for (int lang : {0x409, 0x809, 0}) {
        int len = 0;
        const char* s = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                                lang, name_id);
        if (!s || len <= 0) continue;
        std::wstring w;
        for (int i = 0; i + 1 < len; i += 2) w += wchar_t(be16(reinterpret_cast<const uint8_t*>(s) + i));
        return w;
    }
    return {};
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(towlower(wint_t(c)));
    return s;
}

FontFile* match_font(const wchar_t* face) {
    std::lock_guard<std::mutex> lock(g_font_mutex);
    if (g_fonts.empty()) return nullptr;
    std::wstring want = lower(face ? face : L"");
    for (auto& f : g_fonts)
        for (auto& fam : f->families)
            if (lower(fam) == want) return f.get();
    // The first word ("Saira", "Barlow", "Share", "Jurassic")
    std::wstring first = want.substr(0, want.find(L' '));
    for (auto& f : g_fonts)
        for (auto& fam : f->families)
            if (!first.empty() && lower(fam).rfind(first, 0) == 0) return f.get();
    return g_fonts.front().get();
}

// The advance of each character with kerning, in pixels (unrounded)
float text_width(const GdiFont& f, const wchar_t* s, int n) {
    float x = 0;
    for (int i = 0; i < n; i++) {
        int adv = 0, lsb = 0;
        int g = stbtt_FindGlyphIndex(&f.file->info, int(s[i]));
        stbtt_GetGlyphHMetrics(&f.file->info, g, &adv, &lsb);
        x += float(adv) * f.scale;
        if (i + 1 < n) {
            int g2 = stbtt_FindGlyphIndex(&f.file->info, int(s[i + 1]));
            x += float(stbtt_GetGlyphKernAdvance(&f.file->info, g, g2)) * f.scale;
        }
    }
    return x;
}

}  // namespace

HANDLE AddFontMemResourceEx(void* data, DWORD size, void*, DWORD* count) {
    auto f = std::make_unique<FontFile>();
    f->data.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    int offset = stbtt_GetFontOffsetForIndex(f->data.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&f->info, f->data.data(), offset)) return nullptr;
    for (int id : {1, 16, 4}) {
        std::wstring n = name_string(f->info, id);
        if (!n.empty()) f->families.push_back(n);
    }
    uint32_t os2 = stbtt__find_table(f->data.data(), uint32_t(offset), "OS/2");
    if (os2 && os2 + 78 <= f->data.size()) {
        f->win_ascent = be16(f->data.data() + os2 + 74);
        f->win_descent = be16(f->data.data() + os2 + 76);
    }
    if (!f->win_ascent) {
        int a, d, g;
        stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
        f->win_ascent = a, f->win_descent = -d;
    }
    if (count) *count = 1;
    std::lock_guard<std::mutex> lock(g_font_mutex);
    g_fonts.push_back(std::move(f));
    return g_fonts.back().get();
}
BOOL RemoveFontMemResourceEx(HANDLE) { return TRUE; }   // kept for the run: fonts made from it may still be in use

HFONT CreateFontW(int height, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                  const wchar_t* face) {
    FontFile* file = match_font(face);
    if (!file) return nullptr;
    auto* f = new GdiFont;
    f->file = file;
    int units_em = 0;
    {
        // unitsPerEm from the head table
        uint32_t head = stbtt__find_table(file->data.data(), uint32_t(file->info.fontstart), "head");
        units_em = head ? be16(file->data.data() + head + 18) : 2048;
    }
    float cell_units = float(file->win_ascent + file->win_descent);
    if (height < 0) f->scale = float(-height) / float(units_em);   // character height: the em
    else if (height > 0) f->scale = float(height) / cell_units;   // cell height
    else f->scale = 16.0f / float(units_em);
    f->ascent = int(std::ceil(float(file->win_ascent) * f->scale));
    f->descent = int(std::ceil(float(file->win_descent) * f->scale));
    f->em = int(std::lround(float(units_em) * f->scale));
    return f;
}

HDC CreateCompatibleDC(HDC) { return new GdiDc; }
BOOL DeleteDC(HDC dc) {
    delete static_cast<GdiDc*>(dc);
    return TRUE;
}
HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj) {
    auto* d = static_cast<GdiDc*>(dc);
    auto* o = static_cast<GdiObject*>(obj);
    if (!d || !o) return nullptr;
    if (o->kind == Kind::Font) {
        GdiFont* old = d->font;
        d->font = static_cast<GdiFont*>(o);
        return old;
    }
    if (o->kind == Kind::Bitmap) {
        GdiBitmap* old = d->bitmap;
        d->bitmap = static_cast<GdiBitmap*>(o);
        return old;
    }
    return nullptr;
}
BOOL DeleteObject(HGDIOBJ obj) {
    delete static_cast<GdiObject*>(obj);
    return TRUE;
}
HBITMAP CreateDIBSection(HDC, const BITMAPINFO* bi, UINT, void** bits, HANDLE, DWORD) {
    if (!bi || bi->bmiHeader.biBitCount != 32 || bi->bmiHeader.biWidth <= 0 || bi->bmiHeader.biHeight == 0) return nullptr;
    auto* b = new GdiBitmap;
    b->w = bi->bmiHeader.biWidth;
    b->h = std::abs(bi->bmiHeader.biHeight);
    b->top_down = bi->bmiHeader.biHeight < 0;
    b->px.assign(size_t(b->w) * size_t(b->h), 0);
    if (bits) *bits = b->px.data();
    return b;
}
BOOL GetTextExtentPoint32W(HDC dc, const wchar_t* s, int n, SIZE* out) {
    auto* d = static_cast<GdiDc*>(dc);
    if (!d || !d->font) {
        out->cx = out->cy = 0;
        return FALSE;
    }
    out->cx = LONG(std::ceil(text_width(*d->font, s, n)));
    out->cy = d->font->ascent + d->font->descent;
    return TRUE;
}
BOOL GetTextMetricsW(HDC dc, TEXTMETRICW* out) {
    auto* d = static_cast<GdiDc*>(dc);
    memset(out, 0, sizeof *out);
    if (!d || !d->font) return FALSE;
    const GdiFont& f = *d->font;
    out->tmAscent = f.ascent;
    out->tmDescent = f.descent;
    out->tmHeight = f.ascent + f.descent;
    out->tmInternalLeading = std::max(0, out->tmHeight - f.em);
    out->tmAveCharWidth = LONG(text_width(f, L"x", 1));
    out->tmMaxCharWidth = LONG(text_width(f, L"W", 1));
    out->tmWeight = FW_BOLD;
    return TRUE;
}
int SetBkMode(HDC, int) { return TRANSPARENT; }
COLORREF SetTextColor(HDC dc, COLORREF c) {
    auto* d = static_cast<GdiDc*>(dc);
    if (!d) return 0;
    COLORREF old = d->color;
    d->color = c;
    return old;
}
BOOL TextOutW(HDC dc, int x0, int y0, const wchar_t* s, int n) {
    auto* d = static_cast<GdiDc*>(dc);
    if (!d || !d->font || !d->bitmap) return FALSE;
    const GdiFont& f = *d->font;
    GdiBitmap& b = *d->bitmap;
    const stbtt_fontinfo& info = f.file->info;
    const uint32_t r = d->color & 0xFF, g = (d->color >> 8) & 0xFF, bl = (d->color >> 16) & 0xFF;
    float pen = float(x0);
    const int baseline = y0 + f.ascent;
    std::vector<uint8_t> glyph;
    for (int i = 0; i < n; i++) {
        int gi = stbtt_FindGlyphIndex(&info, int(s[i]));
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&info, gi, &adv, &lsb);
        float shift = pen - std::floor(pen);
        int ix0, iy0, ix1, iy1;
        stbtt_GetGlyphBitmapBoxSubpixel(&info, gi, f.scale, f.scale, shift, 0, &ix0, &iy0, &ix1, &iy1);
        int gw = ix1 - ix0, gh = iy1 - iy0;
        if (gw > 0 && gh > 0) {
            glyph.assign(size_t(gw) * size_t(gh), 0);
            stbtt_MakeGlyphBitmapSubpixel(&info, glyph.data(), gw, gh, gw, f.scale, f.scale, shift, 0, gi);
            int ox = int(std::floor(pen)) + ix0, oy = baseline + iy0;
            for (int y = 0; y < gh; y++) {
                int py = oy + y;
                if (py < 0 || py >= b.h) continue;
                int row = b.top_down ? py : b.h - 1 - py;
                for (int x = 0; x < gw; x++) {
                    int px = ox + x;
                    if (px < 0 || px >= b.w) continue;
                    uint32_t a = glyph[size_t(y) * size_t(gw) + size_t(x)];
                    if (!a) continue;
                    uint32_t& dst = b.px[size_t(row) * size_t(b.w) + size_t(px)];
                    // BGRA in memory (as a DIB): the coverage times the text colour,
                    // the stronger of the overlapping glyphs
                    uint32_t cb = bl * a / 255, cg = g * a / 255, cr = r * a / 255;
                    uint32_t ob = dst & 0xFF, og = (dst >> 8) & 0xFF, orr = (dst >> 16) & 0xFF, oa = dst >> 24;
                    dst = std::max(ob, cb) | std::max(og, cg) << 8 | std::max(orr, cr) << 16 | std::max(oa, a) << 24;
                }
            }
        }
        pen += float(adv) * f.scale;
        if (i + 1 < n) pen += float(stbtt_GetGlyphKernAdvance(&info, gi, stbtt_FindGlyphIndex(&info, int(s[i + 1])))) * f.scale;
    }
    return TRUE;
}

// ---------------------------------------------------------------- PlaySound (SDL3 audio)

namespace {
std::mutex g_sound_mutex;
SDL_AudioStream* g_sound_stream = nullptr;
}  // namespace

BOOL PlaySoundW(const wchar_t* sound, HMODULE, DWORD flags) {
    std::lock_guard<std::mutex> lock(g_sound_mutex);
    if (g_sound_stream) SDL_ClearAudioStream(g_sound_stream);
    if (!sound || !(flags & SND_MEMORY)) return TRUE;   // stop (or a file: not used)
    const auto* wav = reinterpret_cast<const uint8_t*>(sound);
    uint32_t riff_size = 0;
    memcpy(&riff_size, wav + 4, 4);
    if (memcmp(wav, "RIFF", 4) != 0) return FALSE;
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return FALSE;
    SDL_AudioSpec spec;
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV_IO(SDL_IOFromConstMem(wav, size_t(riff_size) + 8), true, &spec, &buf, &len)) return FALSE;
    if (!g_sound_stream) {
        g_sound_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (g_sound_stream) SDL_ResumeAudioStreamDevice(g_sound_stream);
    } else {
        SDL_SetAudioStreamFormat(g_sound_stream, &spec, nullptr);
    }
    bool ok = g_sound_stream && SDL_PutAudioStreamData(g_sound_stream, buf, int(len));
    SDL_free(buf);
    return ok;
}

// ---------------------------------------------------------------- shell

HINSTANCE ShellExecuteW(HWND, const wchar_t*, const wchar_t* file, const wchar_t*, const wchar_t*, int) {
    std::string f = to_utf8(file);
    if (f.rfind("http://", 0) == 0 || f.rfind("https://", 0) == 0)
        return SDL_OpenURL(f.c_str()) ? (HINSTANCE)(intptr_t)33 : (HINSTANCE)(intptr_t)2;
    return (HINSTANCE)(intptr_t)2;   // no programs to start on iOS
}
HINSTANCE ShellExecuteA(HWND, const char*, const char* file, const char*, const char*, int) {
    std::string f = file ? file : "";
    if (f.rfind("http://", 0) == 0 || f.rfind("https://", 0) == 0)
        return SDL_OpenURL(f.c_str()) ? (HINSTANCE)(intptr_t)33 : (HINSTANCE)(intptr_t)2;
    return (HINSTANCE)(intptr_t)2;
}
