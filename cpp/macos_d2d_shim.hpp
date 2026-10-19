#pragma once
#ifdef __APPLE__

#include <mach-o/dyld.h>

extern "C" {
// Objective-C Runtime ABI
typedef struct objc_object* id;
typedef struct objc_selector* SEL;
typedef struct objc_class* Class;
typedef void (*IMP)(void);

#if defined(__aarch64__)
using ObjcBool = bool;
#else
using ObjcBool = signed char;
#endif

Class objc_getClass(const char* name);
SEL sel_registerName(const char* str);
void objc_msgSend(void);
#if defined(__x86_64__)
void objc_msgSend_stret(void);
#endif
id objc_retain(id obj);
void objc_release(id obj);
Class objc_allocateClassPair(Class superclass, const char* name, size_t extraBytes);
signed char class_addMethod(Class cls, SEL name, IMP imp, const char* types);
void objc_registerClassPair(Class cls);

// Dynamic loader & GCD ABI
void* dlopen(const char* path, int mode);
void* dlsym(void* handle, const char* symbol);
typedef struct dispatch_queue_s* dispatch_queue_t;
typedef struct dispatch_queue_attr_s* dispatch_queue_attr_t;
dispatch_queue_t dispatch_queue_create(const char* label, dispatch_queue_attr_t attr);

// CoreFoundation ABI
typedef double CGFloat;
typedef signed long CFIndex;
typedef unsigned char UInt8;
typedef unsigned int CFStringEncoding;
constexpr CFStringEncoding kCFStringEncodingUTF8 = 0x08000100U;

typedef const void* CFTypeRef;
typedef const struct __CFAllocator* CFAllocatorRef;
typedef const struct __CFString* CFStringRef;
typedef const struct __CFData* CFDataRef;
typedef const struct __CFDictionary* CFDictionaryRef;
typedef const struct __CFAttributedString* CFAttributedStringRef;

extern const void* kCFTypeDictionaryKeyCallBacks[];
extern const void* kCFTypeDictionaryValueCallBacks[];

CFTypeRef CFRetain(CFTypeRef cf);
void CFRelease(CFTypeRef cf);
CFStringRef CFStringCreateWithCString(CFAllocatorRef alloc, const char* cStr, CFStringEncoding encoding);
CFStringRef CFStringCreateWithBytes(CFAllocatorRef alloc, const UInt8* bytes, CFIndex numBytes, CFStringEncoding encoding, unsigned char isExternalRepresentation);
CFDataRef CFDataCreate(CFAllocatorRef allocator, const UInt8* bytes, CFIndex length);
CFDictionaryRef CFDictionaryCreate(CFAllocatorRef allocator, const void** keys, const void** values, CFIndex numValues, const void* keyCallBacks, const void* valueCallBacks);
CFAttributedStringRef CFAttributedStringCreate(CFAllocatorRef alloc, CFStringRef str, CFDictionaryRef attributes);

// CoreGraphics (Quartz 2D) ABI
struct CGPoint { CGFloat x; CGFloat y; };
struct CGSize  { CGFloat width; CGFloat height; };
struct CGRect  { CGPoint origin; CGSize size; };
struct CGAffineTransform { CGFloat a, b, c, d, tx, ty; };

inline CGPoint CGPointMake(CGFloat x, CGFloat y) { return {x, y}; }
inline CGSize CGSizeMake(CGFloat w, CGFloat h) { return {w, h}; }
inline CGRect CGRectMake(CGFloat x, CGFloat y, CGFloat w, CGFloat h) { return {{x, y}, {w, h}}; }
inline CGAffineTransform CGAffineTransformMakeScale(CGFloat sx, CGFloat sy) { return {sx, 0.0, 0.0, sy, 0.0, 0.0}; }

typedef struct CGContext* CGContextRef;
typedef struct CGImage* CGImageRef;
typedef const struct CGPath* CGPathRef;
typedef struct CGColorSpace* CGColorSpaceRef;
typedef struct CGColor* CGColorRef;

void CGContextSaveGState(CGContextRef c);
void CGContextRestoreGState(CGContextRef c);
void CGContextSetRGBFillColor(CGContextRef c, CGFloat r, CGFloat g, CGFloat b, CGFloat a);
void CGContextSetRGBStrokeColor(CGContextRef c, CGFloat r, CGFloat g, CGFloat b, CGFloat a);
void CGContextSetLineWidth(CGContextRef c, CGFloat width);
void CGContextFillRect(CGContextRef c, CGRect rect);
void CGContextStrokeRect(CGContextRef c, CGRect rect);
void CGContextFillEllipseInRect(CGContextRef c, CGRect rect);
void CGContextStrokeEllipseInRect(CGContextRef c, CGRect rect);
void CGContextStrokeLineSegments(CGContextRef c, const CGPoint* points, size_t count);
void CGContextAddPath(CGContextRef c, CGPathRef path);
void CGContextFillPath(CGContextRef c);
void CGContextStrokePath(CGContextRef c);
void CGContextClipToRect(CGContextRef c, CGRect rect);
void CGContextSetAlpha(CGContextRef c, CGFloat alpha);
void CGContextTranslateCTM(CGContextRef c, CGFloat tx, CGFloat ty);
void CGContextScaleCTM(CGContextRef c, CGFloat sx, CGFloat sy);
void CGContextDrawImage(CGContextRef c, CGRect rect, CGImageRef image);
void CGContextSetTextMatrix(CGContextRef c, CGAffineTransform t);
void CGContextSetTextPosition(CGContextRef c, CGFloat x, CGFloat y);

CGPathRef CGPathCreateWithRoundedRect(CGRect rect, CGFloat cornerWidth, CGFloat cornerHeight, const CGAffineTransform* transform);
void CGPathRelease(CGPathRef path);
CGColorSpaceRef CGColorSpaceCreateDeviceRGB(void);
void CGColorSpaceRelease(CGColorSpaceRef cs);
CGColorRef CGColorCreate(CGColorSpaceRef space, const CGFloat* components);
void CGColorRelease(CGColorRef color);
CGImageRef CGImageCreateWithImageInRect(CGImageRef image, CGRect rect);
void CGImageRelease(CGImageRef image);
size_t CGImageGetWidth(CGImageRef image);
size_t CGImageGetHeight(CGImageRef image);
CGContextRef CGBitmapContextCreate(void* data, size_t width, size_t height, size_t bitsPerComponent, size_t bytesPerRow, CGColorSpaceRef space, uint32_t bitmapInfo);
CGImageRef CGBitmapContextCreateImage(CGContextRef context);
void CGContextRelease(CGContextRef c);
typedef int32_t CGInterpolationQuality;
constexpr CGInterpolationQuality kCGInterpolationNone = 1;
constexpr CGInterpolationQuality kCGInterpolationHigh = 3;
void CGContextSetInterpolationQuality(CGContextRef c, CGInterpolationQuality quality);

// CoreText ABI
typedef const struct __CTFont* CTFontRef;
typedef const struct __CTLine* CTLineRef;
extern const CFStringRef kCTFontAttributeName;
extern const CFStringRef kCTForegroundColorAttributeName;

CTFontRef CTFontCreateWithName(CFStringRef name, CGFloat size, const CGAffineTransform* matrix);
CTLineRef CTLineCreateWithAttributedString(CFAttributedStringRef attrString);
double CTLineGetTypographicBounds(CTLineRef line, CGFloat* ascent, CGFloat* descent, CGFloat* leading);
void CTLineDraw(CTLineRef line, CGContextRef context);

// ImageIO ABI
typedef struct CGImageSource* CGImageSourceRef;
CGImageSourceRef CGImageSourceCreateWithData(CFDataRef data, CFDictionaryRef options);
CGImageRef CGImageSourceCreateImageAtIndex(CGImageSourceRef isrc, size_t index, CFDictionaryRef options);

#ifdef MESHCORE_IOS
int UIApplicationMain(int argc, char* argv[], id principalClassName, id delegateClassName);
CGContextRef UIGraphicsGetCurrentContext(void);
id UIImagePNGRepresentation(id image);
#endif
} // extern "C"
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <dirent.h>
#include <strings.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#ifndef _stricmp
#define _stricmp strcasecmp
#endif
#ifndef _strnicmp
#define _strnicmp strncasecmp
#endif

// ============================================================================
// Win32 / Direct2D / DirectWrite Compatibility Layer for macOS Sonoma+ & iOS
// ============================================================================
using UINT = unsigned int;
using UINT32 = uint32_t;
using DWORD = uint32_t;
using LONG = int32_t;
using BOOL = int;
using BYTE = uint8_t;
using ULONGLONG = uint64_t;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using LRESULT = intptr_t;
using HRESULT = int32_t;
using ATOM = uint16_t;
using HINSTANCE = void*;
using HICON = void*;
using HCURSOR = void*;
using HBRUSH = void*;
using HGDIOBJ = void*;
using HBITMAP = void*;
using HGLOBAL = void*;
using HANDLE = void*;
using LPSTR = char*;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 1024
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef WINAPI
#define WINAPI
#endif
#define SUCCEEDED(hr) ((hr) >= 0)

struct POINT { LONG x = 0; LONG y = 0; };
struct RECT  { LONG left = 0; LONG top = 0; LONG right = 0; LONG bottom = 0; };

struct MacWindowState {
    int width = 1460;
    int height = 880;
    id ns_window = nullptr;
    id ns_view = nullptr;
    id ios_vc = nullptr;
    CGContextRef active_cg = nullptr;
    bool needs_display = false;
};

inline void (*g_ios_on_image_picked)(const std::wstring&) = nullptr;

using HWND = MacWindowState*;
using HDC  = CGContextRef;

struct PAINTSTRUCT {
    HDC hdc = nullptr;
};

// Window messages
constexpr UINT WM_SIZE          = 0x0005;
constexpr UINT WM_PAINT         = 0x000F;
constexpr UINT WM_DESTROY       = 0x0002;
constexpr UINT WM_ERASEBKGND    = 0x0014;
constexpr UINT WM_SETCURSOR     = 0x0020;
constexpr UINT WM_KEYDOWN       = 0x0100;
constexpr UINT WM_CHAR          = 0x0102;
constexpr UINT WM_TIMER         = 0x0113;
constexpr UINT WM_MOUSEMOVE     = 0x0200;
constexpr UINT WM_LBUTTONDOWN   = 0x0201;
constexpr UINT WM_LBUTTONUP     = 0x0202;
constexpr UINT WM_LBUTTONDBLCLK = 0x0203;
constexpr UINT WM_MOUSEWHEEL    = 0x020A;
constexpr UINT WM_PRINTCLIENT   = 0x0318;
constexpr UINT WM_SETICON       = 0x0080;

constexpr UINT HTCLIENT         = 1;
constexpr UINT ICON_BIG         = 1;
constexpr UINT ICON_SMALL       = 0;

// Virtual key codes
constexpr WPARAM VK_TAB     = 0x09;
constexpr WPARAM VK_RETURN  = 0x0D;
constexpr WPARAM VK_CONTROL = 0x11;
constexpr WPARAM VK_ESCAPE  = 0x1B;
constexpr WPARAM VK_PRIOR   = 0x21; // PageUp
constexpr WPARAM VK_NEXT    = 0x22; // PageDown

// Cursor IDs
#define IDC_ARROW   reinterpret_cast<const wchar_t*>(1)
#define IDC_HAND    reinterpret_cast<const wchar_t*>(2)
#define IDC_SIZEWE  reinterpret_cast<const wchar_t*>(3)
#define IDC_SIZENS  reinterpret_cast<const wchar_t*>(4)
#define IDC_SIZEALL reinterpret_cast<const wchar_t*>(5)

inline uint16_t LOWORD(uintptr_t l) { return static_cast<uint16_t>(l & 0xFFFF); }
inline uint16_t HIWORD(uintptr_t l) { return static_cast<uint16_t>((l >> 16) & 0xFFFF); }
inline int16_t GET_X_LPARAM(intptr_t l) { return static_cast<int16_t>(static_cast<uint32_t>(l) & 0xFFFF); }
inline int16_t GET_Y_LPARAM(intptr_t l) { return static_cast<int16_t>((static_cast<uint32_t>(l) >> 16) & 0xFFFF); }
inline int16_t GET_WHEEL_DELTA_WPARAM(uintptr_t w) { return static_cast<int16_t>((w >> 16) & 0xFFFF); }
inline LPARAM MAKELPARAM_MAC(int x, int y) {
    return static_cast<LPARAM>((static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16) |
                               static_cast<uint32_t>(static_cast<uint16_t>(x)));
}

inline ULONGLONG GetTickCount64() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct SYSTEMTIME {
    uint16_t wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};

inline void GetLocalTime(SYSTEMTIME* st) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm_val{};
    localtime_r(&t, &tm_val);
    st->wYear = static_cast<uint16_t>(tm_val.tm_year + 1900);
    st->wMonth = static_cast<uint16_t>(tm_val.tm_mon + 1);
    st->wDayOfWeek = static_cast<uint16_t>(tm_val.tm_wday);
    st->wDay = static_cast<uint16_t>(tm_val.tm_mday);
    st->wHour = static_cast<uint16_t>(tm_val.tm_hour);
    st->wMinute = static_cast<uint16_t>(tm_val.tm_min);
    st->wSecond = static_cast<uint16_t>(tm_val.tm_sec);
    st->wMilliseconds = static_cast<uint16_t>(ms.count());
}

inline int localtime_s(std::tm* out, const std::time_t* t) {
    return localtime_r(t, out) ? 0 : -1;
}

template <typename Ret, typename... Args>
inline Ret mac_msg_send(id obj, SEL sel, Args... args) {
    if (!obj) {
        if constexpr (std::is_void_v<Ret>) return;
        else return Ret{};
    }
    if constexpr (std::is_void_v<Ret>) {
        reinterpret_cast<void (*)(id, SEL, Args...)>(objc_msgSend)(obj, sel, args...);
    } else {
#if defined(__x86_64__)
        if constexpr (sizeof(Ret) > 16) {
            return reinterpret_cast<Ret (*)(id, SEL, Args...)>(objc_msgSend_stret)(obj, sel, args...);
        } else
#endif
        {
            return reinterpret_cast<Ret (*)(id, SEL, Args...)>(objc_msgSend)(obj, sel, args...);
        }
    }
}

template <typename Ret, typename... Args>
inline Ret mac_cls_send(const char* cls_name, SEL sel, Args... args) {
    id cls_obj = reinterpret_cast<id>(objc_getClass(cls_name));
    return mac_msg_send<Ret>(cls_obj, sel, args...);
}

inline id mac_ns_str(const char* utf8) {
    return mac_cls_send<id>("NSString", sel_registerName("stringWithUTF8String:"), utf8 ? utf8 : "");
}

inline std::string mac_ns_to_std_str(id ns_str_obj) {
    if (!ns_str_obj) return "";
    const char* c = mac_msg_send<const char*>(ns_str_obj, sel_registerName("UTF8String"));
    return c ? std::string(c) : "";
}

inline std::string mac_wide_to_utf8(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t wc : w) {
        uint32_t cp = static_cast<uint32_t>(wc);
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = '?';
        if (cp > 0x10FFFF) cp = '?';
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

inline std::wstring mac_utf8_to_wide(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<wchar_t>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            uint32_t cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
            out.push_back(static_cast<wchar_t>(cp));
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            uint32_t cp = ((c & 0x0F) << 12) |
                          ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 2]) & 0x3F);
            out.push_back(static_cast<wchar_t>(cp));
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            uint32_t cp = ((c & 0x07) << 18) |
                          ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                          ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 3]) & 0x3F);
            out.push_back(static_cast<wchar_t>(cp));
            i += 4;
        } else {
            ++i;
        }
    }
    return out;
}

// Returns writable ~/Documents/MeshCore++ (iOS) or ~/Library/Application Support/MeshCore++ (macOS) path
inline DWORD GetModuleFileNameW(void*, wchar_t* buf, DWORD cap) {
    std::string app_dir;
    if (const char* home = std::getenv("HOME")) {
#ifdef MESHCORE_IOS
        std::string doc_dir = std::string(home) + "/Documents";
        app_dir = doc_dir + "/MeshCore++";
        mkdir(doc_dir.c_str(), 0755);
        mkdir(app_dir.c_str(), 0755);
#else
        std::string lib_dir = std::string(home) + "/Library";
        std::string sup_dir = lib_dir + "/Application Support";
        app_dir = sup_dir + "/MeshCore++";
        mkdir(lib_dir.c_str(), 0755);
        mkdir(sup_dir.c_str(), 0755);
        mkdir(app_dir.c_str(), 0755);
#endif
    } else {
        app_dir = "/tmp/MeshCore++";
        mkdir(app_dir.c_str(), 0755);
    }
    std::string fake_exe = app_dir + "/MeshCore++";
    std::wstring w = mac_utf8_to_wide(fake_exe);
    size_t n = std::min<size_t>(w.size(), cap > 0 ? cap - 1 : 0);
    for (size_t i = 0; i < n; ++i) buf[i] = w[i];
    if (cap > 0) buf[n] = L'\0';
    return static_cast<DWORD>(n);
}

inline BOOL CreateDirectoryW(const wchar_t* path, void*) {
    std::string p = mac_wide_to_utf8(path ? path : L"");
    for (char& c : p) if (c == '\\') c = '/';
    return mkdir(p.c_str(), 0755) == 0 ? TRUE : FALSE;
}

inline void GetClientRect(HWND hwnd, RECT* rc) {
    if (!rc) return;
    rc->left = 0;
    rc->top = 0;
    rc->right = hwnd ? hwnd->width : 1460;
    rc->bottom = hwnd ? hwnd->height : 880;
}

inline void InvalidateRect(HWND hwnd, const RECT*, BOOL) {
    if (hwnd) hwnd->needs_display = true;
}

inline void SetCapture(HWND) {}
inline void ReleaseCapture() {}
inline HCURSOR LoadCursor(void*, const wchar_t* id) { return const_cast<wchar_t*>(id); }
inline void SetCursor(HCURSOR) {}
inline HDC CreateCompatibleDC(HDC hdc) { return hdc; }
inline HBITMAP CreateCompatibleBitmap(HDC, int, int) { return nullptr; }
inline HGDIOBJ SelectObject(HDC, HGDIOBJ) { return nullptr; }
inline void BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD) {}
inline void DeleteObject(HGDIOBJ) {}
inline void DeleteDC(HDC) {}
constexpr DWORD SRCCOPY = 0;

inline HDC BeginPaint(HWND hwnd, PAINTSTRUCT* ps) {
    if (ps) ps->hdc = hwnd ? hwnd->active_cg : nullptr;
    return ps ? ps->hdc : nullptr;
}
inline void EndPaint(HWND, const PAINTSTRUCT*) {}
inline LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return 0; }
inline void PostQuitMessage(int) {}

extern bool g_mac_cmd_down;
inline short GetKeyState(int vk) {
    if (vk == VK_CONTROL && g_mac_cmd_down) return static_cast<short>(0x8000);
    return 0;
}

// ============================================================================
// Direct2D & DirectWrite Types + CoreGraphics / CoreText Implementation
// ============================================================================
struct D2D1_COLOR_F { float r = 0, g = 0, b = 0, a = 1; };
struct D2D1_POINT_2F { float x = 0, y = 0; };
struct D2D1_RECT_F { float left = 0, top = 0, right = 0, bottom = 0; };
struct D2D1_ROUNDED_RECT { D2D1_RECT_F rect{}; float radiusX = 0, radiusY = 0; };
struct D2D1_ELLIPSE { D2D1_POINT_2F point{}; float radiusX = 0, radiusY = 0; };

enum DWRITE_TEXT_ALIGNMENT {
    DWRITE_TEXT_ALIGNMENT_LEADING = 0,
    DWRITE_TEXT_ALIGNMENT_TRAILING = 1,
    DWRITE_TEXT_ALIGNMENT_CENTER = 2
};
enum DWRITE_PARAGRAPH_ALIGNMENT {
    DWRITE_PARAGRAPH_ALIGNMENT_NEAR = 0,
    DWRITE_PARAGRAPH_ALIGNMENT_FAR = 1,
    DWRITE_PARAGRAPH_ALIGNMENT_CENTER = 2
};
enum DWRITE_WORD_WRAPPING {
    DWRITE_WORD_WRAPPING_WRAP = 0,
    DWRITE_WORD_WRAPPING_NO_WRAP = 1
};
enum DWRITE_FONT_WEIGHT {
    DWRITE_FONT_WEIGHT_NORMAL = 400,
    DWRITE_FONT_WEIGHT_SEMI_BOLD = 600,
    DWRITE_FONT_WEIGHT_BOLD = 700
};
enum DWRITE_FONT_STYLE { DWRITE_FONT_STYLE_NORMAL = 0 };
enum DWRITE_FONT_STRETCH { DWRITE_FONT_STRETCH_NORMAL = 5 };

enum D2D1_ANTIALIAS_MODE {
    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE = 0,
    D2D1_ANTIALIAS_MODE_ALIASED = 1
};
enum D2D1_TEXT_ANTIALIAS_MODE { D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE = 0 };
enum D2D1_DRAW_TEXT_OPTIONS { D2D1_DRAW_TEXT_OPTIONS_CLIP = 2 };
enum D2D1_BITMAP_INTERPOLATION_MODE {
    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR = 0,
    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR = 1
};

namespace D2D1 {
    inline D2D1_COLOR_F ColorF(float r, float g, float b, float a = 1.0f) { return {r, g, b, a}; }
    inline D2D1_POINT_2F Point2F(float x, float y) { return {x, y}; }
    inline D2D1_RECT_F RectF(float l, float t, float r, float b) { return {l, t, r, b}; }
    inline D2D1_ROUNDED_RECT RoundedRect(const D2D1_RECT_F& r, float rx, float ry) { return {r, rx, ry}; }
    inline D2D1_ELLIPSE Ellipse(const D2D1_POINT_2F& p, float rx, float ry) { return {p, rx, ry}; }
}

class ID2D1SolidColorBrush {
public:
    D2D1_COLOR_F color{};
    explicit ID2D1SolidColorBrush(D2D1_COLOR_F c) : color(c) {}
    void SetColor(D2D1_COLOR_F c) { color = c; }
    void Release() { delete this; }
};

class ID2D1Bitmap {
public:
    CGImageRef image = nullptr;
    explicit ID2D1Bitmap(CGImageRef img) : image(img) {}
    ~ID2D1Bitmap() { if (image) CGImageRelease(image); }
    void Release() { delete this; }
};

class IDWriteTextFormat {
public:
    CTFontRef font = nullptr;
    float size = 13.0f;
    DWRITE_TEXT_ALIGNMENT text_align = DWRITE_TEXT_ALIGNMENT_LEADING;
    DWRITE_PARAGRAPH_ALIGNMENT para_align = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;

    IDWriteTextFormat(const wchar_t* family, int weight, float sz) : size(sz) {
        std::string fam = mac_wide_to_utf8(family ? family : L"");
        const char* ps_name = "HelveticaNeue";
        if (fam.find("Consolas") != std::string::npos || fam.find("Mono") != std::string::npos) {
            ps_name = (weight >= 600) ? "Menlo-Bold" : "Menlo-Regular";
        } else {
            ps_name = (weight >= 700) ? "HelveticaNeue-Bold"
                    : (weight >= 600) ? "HelveticaNeue-Medium"
                                      : "HelveticaNeue";
        }
        CFStringRef cf_name = CFStringCreateWithCString(nullptr, ps_name, kCFStringEncodingUTF8);
        if (cf_name) {
            font = CTFontCreateWithName(cf_name, static_cast<CGFloat>(sz), nullptr);
            CFRelease(cf_name);
        }
        if (!font) {
            CFStringRef fallback = CFStringCreateWithCString(nullptr, "Helvetica", kCFStringEncodingUTF8);
            if (fallback) {
                font = CTFontCreateWithName(fallback, static_cast<CGFloat>(sz), nullptr);
                CFRelease(fallback);
            }
        }
    }
    ~IDWriteTextFormat() {
        if (font) CFRelease(font);
    }
    void SetTextAlignment(DWRITE_TEXT_ALIGNMENT a) { text_align = a; }
    void SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT a) { para_align = a; }
    void SetWordWrapping(DWRITE_WORD_WRAPPING) {}
    void Release() { delete this; }
};

class IDWriteFactory {
public:
    HRESULT CreateTextFormat(const wchar_t* family, void*, DWRITE_FONT_WEIGHT weight,
                             DWRITE_FONT_STYLE, DWRITE_FONT_STRETCH, float size,
                             const wchar_t*, IDWriteTextFormat** out) {
        if (!out) return -1;
        *out = new IDWriteTextFormat(family, static_cast<int>(weight), size);
        return 0;
    }
    void Release() { delete this; }
};

class ID2D1DCRenderTarget {
public:
    CGContextRef ctx = nullptr;
    RECT bounds{};

    void BindDC(HDC hdc, const RECT* rc) {
        ctx = hdc;
        if (rc) bounds = *rc;
    }
    void SetAntialiasMode(D2D1_ANTIALIAS_MODE) {}
    void SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE) {}
    void BeginDraw() {
        if (ctx) CGContextSaveGState(ctx);
    }
    HRESULT EndDraw() {
        if (ctx) CGContextRestoreGState(ctx);
        return 0;
    }
    void Clear(D2D1_COLOR_F c) {
        if (!ctx) return;
        CGContextSetRGBFillColor(ctx, c.r, c.g, c.b, c.a);
        CGContextFillRect(ctx, CGRectMake(0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top));
    }
    void CreateSolidColorBrush(D2D1_COLOR_F c, ID2D1SolidColorBrush** out) {
        if (out) *out = new ID2D1SolidColorBrush(c);
    }
    static CGRect to_cgrect(const D2D1_RECT_F& r) {
        float w = std::max(0.0f, r.right - r.left);
        float h = std::max(0.0f, r.bottom - r.top);
        return CGRectMake(r.left, r.top, w, h);
    }
    void FillRectangle(const D2D1_RECT_F& r, ID2D1SolidColorBrush* b) {
        if (!ctx || !b) return;
        CGContextSetRGBFillColor(ctx, b->color.r, b->color.g, b->color.b, b->color.a);
        CGContextFillRect(ctx, to_cgrect(r));
    }
    void FillRoundedRectangle(const D2D1_ROUNDED_RECT& rr, ID2D1SolidColorBrush* b) {
        if (!ctx || !b) return;
        CGRect cgr = to_cgrect(rr.rect);
        if (cgr.size.width <= 0.0f || cgr.size.height <= 0.0f) return;
        CGFloat rx = std::min<CGFloat>(rr.radiusX, std::min(cgr.size.width, cgr.size.height) * 0.5);
        CGFloat ry = std::min<CGFloat>(rr.radiusY, std::min(cgr.size.width, cgr.size.height) * 0.5);
        CGContextSetRGBFillColor(ctx, b->color.r, b->color.g, b->color.b, b->color.a);
        if (rx <= 0.5 || ry <= 0.5) {
            CGContextFillRect(ctx, cgr);
        } else {
            CGPathRef path = CGPathCreateWithRoundedRect(cgr, rx, ry, nullptr);
            if (path) {
                CGContextAddPath(ctx, path);
                CGContextFillPath(ctx);
                CGPathRelease(path);
            }
        }
    }
    void DrawRoundedRectangle(const D2D1_ROUNDED_RECT& rr, ID2D1SolidColorBrush* b, float stroke = 1.0f) {
        if (!ctx || !b) return;
        CGRect cgr = to_cgrect(rr.rect);
        if (cgr.size.width <= 0.0f || cgr.size.height <= 0.0f) return;
        CGFloat rx = std::min<CGFloat>(rr.radiusX, std::min(cgr.size.width, cgr.size.height) * 0.5);
        CGFloat ry = std::min<CGFloat>(rr.radiusY, std::min(cgr.size.width, cgr.size.height) * 0.5);
        CGContextSetRGBStrokeColor(ctx, b->color.r, b->color.g, b->color.b, b->color.a);
        CGContextSetLineWidth(ctx, stroke);
        if (rx <= 0.5 || ry <= 0.5) {
            CGContextStrokeRect(ctx, cgr);
        } else {
            CGPathRef path = CGPathCreateWithRoundedRect(cgr, rx, ry, nullptr);
            if (path) {
                CGContextAddPath(ctx, path);
                CGContextStrokePath(ctx);
                CGPathRelease(path);
            }
        }
    }
    void DrawLine(D2D1_POINT_2F p0, D2D1_POINT_2F p1, ID2D1SolidColorBrush* b, float stroke = 1.0f) {
        if (!ctx || !b) return;
        CGContextSetRGBStrokeColor(ctx, b->color.r, b->color.g, b->color.b, b->color.a);
        CGContextSetLineWidth(ctx, stroke);
        CGPoint pts[2] = {CGPointMake(p0.x, p0.y), CGPointMake(p1.x, p1.y)};
        CGContextStrokeLineSegments(ctx, pts, 2);
    }
    void FillEllipse(const D2D1_ELLIPSE& e, ID2D1SolidColorBrush* b) {
        if (!ctx || !b) return;
        CGContextSetRGBFillColor(ctx, b->color.r, b->color.g, b->color.b, b->color.a);
        CGContextFillEllipseInRect(ctx, CGRectMake(e.point.x - e.radiusX, e.point.y - e.radiusY, e.radiusX * 2.0f, e.radiusY * 2.0f));
    }
    void DrawEllipse(const D2D1_ELLIPSE& e, ID2D1SolidColorBrush* b, float stroke = 1.0f) {
        if (!ctx || !b) return;
        CGContextSetRGBStrokeColor(ctx, b->color.r, b->color.g, b->color.b, b->color.a);
        CGContextSetLineWidth(ctx, stroke);
        CGContextStrokeEllipseInRect(ctx, CGRectMake(e.point.x - e.radiusX, e.point.y - e.radiusY, e.radiusX * 2.0f, e.radiusY * 2.0f));
    }
    void PushAxisAlignedClip(const D2D1_RECT_F& r, D2D1_ANTIALIAS_MODE) {
        if (!ctx) return;
        CGContextSaveGState(ctx);
        CGContextClipToRect(ctx, to_cgrect(r));
    }
    void PopAxisAlignedClip() {
        if (!ctx) return;
        CGContextRestoreGState(ctx);
    }
    void DrawBitmap(ID2D1Bitmap* bmp, const D2D1_RECT_F& dst, float opacity = 1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE interp = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                    const D2D1_RECT_F* src = nullptr) {
        if (!ctx || !bmp || !bmp->image) return;
        CGImageRef img = bmp->image;
        bool release_sub = false;
        if (src) {
            CGRect sr = CGRectMake(src->left, src->top, src->right - src->left, src->bottom - src->top);
            img = CGImageCreateWithImageInRect(bmp->image, sr);
            release_sub = true;
        }
        if (img) {
            CGContextSaveGState(ctx);
            CGContextSetAlpha(ctx, opacity);
            CGContextSetInterpolationQuality(
                ctx, (interp == D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR) ? kCGInterpolationNone : kCGInterpolationHigh);
            // Flip locally around the destination rect so CGImage renders upright inside a flipped NSView
            CGContextTranslateCTM(ctx, 0.0, dst.top + dst.bottom);
            CGContextScaleCTM(ctx, 1.0, -1.0);
            CGContextDrawImage(ctx, to_cgrect(dst), img);
            CGContextRestoreGState(ctx);
            if (release_sub) CGImageRelease(img);
        }
    }
    void DrawText(const wchar_t* wstr, UINT32 len, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                  ID2D1SolidColorBrush* b, D2D1_DRAW_TEXT_OPTIONS = D2D1_DRAW_TEXT_OPTIONS_CLIP) {
        if (!ctx || !wstr || len == 0 || !fmt || !fmt->font || !b) return;
        if (r.right <= r.left || r.bottom <= r.top) return;
        std::string utf8 = mac_wide_to_utf8(std::wstring(wstr, len));
        if (utf8.empty()) return;

        CFStringRef cf_str = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(utf8.data()),
                                                     static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
        if (!cf_str) {
            std::string clean;
            clean.reserve(utf8.size());
            for (unsigned char c : utf8) clean.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
            cf_str = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(clean.data()),
                                             static_cast<CFIndex>(clean.size()), kCFStringEncodingUTF8, false);
            if (!cf_str) return;
        }

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        if (!cs) {
            CFRelease(cf_str);
            return;
        }
        CGFloat comps[4] = {b->color.r, b->color.g, b->color.b, b->color.a};
        CGColorRef cg_col = CGColorCreate(cs, comps);
        CGColorSpaceRelease(cs);
        if (!cg_col) {
            CFRelease(cf_str);
            return;
        }

        const void* keys[2] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
        const void* vals[2] = {fmt->font, cg_col};
        CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 2,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        if (!attrs) {
            CGColorRelease(cg_col);
            CFRelease(cf_str);
            return;
        }
        CFAttributedStringRef astr = CFAttributedStringCreate(nullptr, cf_str, attrs);
        CTLineRef line = astr ? CTLineCreateWithAttributedString(astr) : nullptr;
        if (line) {
            CGFloat ascent = 0, descent = 0, leading = 0;
            double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);

            CGFloat x = r.left;
            if (fmt->text_align == DWRITE_TEXT_ALIGNMENT_CENTER) {
                x = r.left + ((r.right - r.left) - static_cast<CGFloat>(width)) * 0.5;
            } else if (fmt->text_align == DWRITE_TEXT_ALIGNMENT_TRAILING) {
                x = r.right - static_cast<CGFloat>(width);
            }
            CGFloat baseline_y = (r.top + r.bottom) * 0.5 + (ascent - descent) * 0.5;

            CGContextSaveGState(ctx);
            CGContextClipToRect(ctx, to_cgrect(r));
            CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1.0, -1.0));
            CGContextSetTextPosition(ctx, x, baseline_y);
            CTLineDraw(line, ctx);
            CGContextRestoreGState(ctx);
            CFRelease(line);
        }
        if (astr) CFRelease(astr);
        CFRelease(attrs);
        CGColorRelease(cg_col);
        CFRelease(cf_str);
    }
    void Release() { delete this; }
};

class ID2D1Factory {
public:
    void Release() { delete this; }
};

struct IWICImagingFactory {
    void Release() {}
};

constexpr HRESULT D2DERR_RECREATE_TARGET = static_cast<HRESULT>(0x8899000CL);

inline ID2D1Bitmap* mac_decode_png_bytes(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return nullptr;
    CFDataRef data = CFDataCreate(nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()));
    if (!data) return nullptr;
    CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (!src) return nullptr;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return nullptr;
    return new ID2D1Bitmap(img);
}

#endif // __APPLE__
