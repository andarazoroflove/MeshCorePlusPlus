#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <wincodec.h>
#elif defined(__APPLE__)
#include "macos_d2d_shim.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace meshirc {

// ============================================================================
// 1-Bit Tactical Monochrome Bitmap Constants & Protocol Opcode
// ============================================================================
#ifndef MESHCORE_MONO_IMG_CONSTANTS
#define MESHCORE_MONO_IMG_CONSTANTS
constexpr uint8_t PAYLOAD_TYPE_MONO_IMG = 0x49; // 'I'
constexpr size_t MONO_IMG_W             = 48;
constexpr size_t MONO_IMG_H             = 30;
constexpr size_t MONO_IMG_PIXELS        = MONO_IMG_W * MONO_IMG_H; // 1,440 pixels
constexpr size_t MONO_IMG_BYTES         = MONO_IMG_PIXELS / 8;     // 180 bytes
constexpr size_t MONO_IMG_RF_BYTES      = 1 + MONO_IMG_BYTES;      // 181 bytes (Byte 0 = 0x49 + 180B bitmap)
#endif

// ============================================================================
// Bit-Packing & Unpacking (Fixed-Size / Stack-Allocated Buffers)
// Row-major, MSB first:
//   Bit 7 of Byte 0   = Pixel (0,0);  Bit 0 of Byte 0   = Pixel (7,0)
//   Bit 7 of Byte 179 = Pixel (40,29); Bit 0 of Byte 179 = Pixel (47,29)
// ============================================================================
inline void pack_mono_bitmap(const uint8_t binary_pixels[MONO_IMG_PIXELS],
                             std::array<uint8_t, MONO_IMG_BYTES>& out_bytes) noexcept {
    out_bytes.fill(0);
    for (size_t i = 0; i < MONO_IMG_PIXELS; ++i) {
        if (binary_pixels[i]) {
            const size_t byte_idx = i >> 3;
            const uint8_t bit_mask = static_cast<uint8_t>(1u << (7u - (i & 7u)));
            out_bytes[byte_idx] |= bit_mask;
        }
    }
}

inline void unpack_mono_bitmap(const std::array<uint8_t, MONO_IMG_BYTES>& in_bytes,
                               uint8_t out_pixels[MONO_IMG_PIXELS]) noexcept {
    for (size_t i = 0; i < MONO_IMG_PIXELS; ++i) {
        const size_t byte_idx = i >> 3;
        const uint8_t shift = static_cast<uint8_t>(7u - (i & 7u));
        out_pixels[i] = static_cast<uint8_t>((in_bytes[byte_idx] >> shift) & 1u);
    }
}

// Builds the 181-byte single-packet RF payload: [0x49, 180_bytes]
inline std::array<uint8_t, MONO_IMG_RF_BYTES> build_mono_img_rf_payload(
    const std::array<uint8_t, MONO_IMG_BYTES>& bits) noexcept {
    std::array<uint8_t, MONO_IMG_RF_BYTES> pkt{};
    pkt[0] = PAYLOAD_TYPE_MONO_IMG;
    std::memcpy(pkt.data() + 1, bits.data(), MONO_IMG_BYTES);
    return pkt;
}

// ============================================================================
// Core Image Processing Pipeline:
//   1. Center-crop to exact 1.6:1 (48:30) aspect ratio
//   2. Area-averaging downsample to 48x30 in 8-bit grayscale
//   3. Dynamic Contrast Normalization (clamp 5th..95th percentiles -> 0..255)
//   4. Floyd-Steinberg Error Diffusion Dithering (7/16, 3/16, 5/16, 1/16)
//   5. MSB-first Bit-Packing into 180 bytes
// ============================================================================
inline bool encode_rgba_to_mono_bitmap(const uint8_t* src_pixels,
                                       int src_w,
                                       int src_h,
                                       int src_stride,
                                       bool is_bgra,
                                       std::array<uint8_t, MONO_IMG_BYTES>& out_bytes) noexcept {
    if (!src_pixels || src_w <= 0 || src_h <= 0 || src_stride < src_w * 4) {
        return false;
    }

    // 1. Center-crop to exact 1.6:1 (48:30) aspect ratio
    constexpr double TARGET_ASPECT = static_cast<double>(MONO_IMG_W) / static_cast<double>(MONO_IMG_H); // 1.6
    const double src_aspect = static_cast<double>(src_w) / static_cast<double>(src_h);

    double crop_x = 0.0;
    double crop_y = 0.0;
    double crop_w = static_cast<double>(src_w);
    double crop_h = static_cast<double>(src_h);

    if (src_aspect > TARGET_ASPECT) {
        crop_w = static_cast<double>(src_h) * TARGET_ASPECT;
        crop_x = (static_cast<double>(src_w) - crop_w) * 0.5;
    } else if (src_aspect < TARGET_ASPECT) {
        crop_h = static_cast<double>(src_w) / TARGET_ASPECT;
        crop_y = (static_cast<double>(src_h) - crop_h) * 0.5;
    }

    // 2. Resample cropped region down to 48x30 in 8-bit grayscale (stack-allocated)
    uint8_t gray[MONO_IMG_PIXELS];
    const double scale_x = crop_w / static_cast<double>(MONO_IMG_W);
    const double scale_y = crop_h / static_cast<double>(MONO_IMG_H);

    for (size_t dy = 0; dy < MONO_IMG_H; ++dy) {
        const double sy0 = crop_y + static_cast<double>(dy) * scale_y;
        const double sy1 = crop_y + static_cast<double>(dy + 1) * scale_y;
        const int iy0 = std::clamp(static_cast<int>(std::floor(sy0)), 0, src_h - 1);
        const int iy1 = std::clamp(static_cast<int>(std::ceil(sy1)) - 1, iy0, src_h - 1);

        for (size_t dx = 0; dx < MONO_IMG_W; ++dx) {
            const double sx0 = crop_x + static_cast<double>(dx) * scale_x;
            const double sx1 = crop_x + static_cast<double>(dx + 1) * scale_x;
            const int ix0 = std::clamp(static_cast<int>(std::floor(sx0)), 0, src_w - 1);
            const int ix1 = std::clamp(static_cast<int>(std::ceil(sx1)) - 1, ix0, src_w - 1);

            double sum_lum = 0.0;
            int count = 0;
            for (int sy = iy0; sy <= iy1; ++sy) {
                const uint8_t* row = src_pixels + static_cast<size_t>(sy) * static_cast<size_t>(src_stride);
                for (int sx = ix0; sx <= ix1; ++sx) {
                    const uint8_t* p = row + static_cast<size_t>(sx) * 4u;
                    const uint8_t r = is_bgra ? p[2] : p[0];
                    const uint8_t g = p[1];
                    const uint8_t b = is_bgra ? p[0] : p[2];
                    const uint8_t a = p[3];
                    // Composite over dark background if alpha < 255
                    const double alpha = static_cast<double>(a) / 255.0;
                    const double lum = (0.299 * static_cast<double>(r) +
                                        0.587 * static_cast<double>(g) +
                                        0.114 * static_cast<double>(b)) * alpha;
                    sum_lum += lum;
                    ++count;
                }
            }
            const int avg = (count > 0) ? static_cast<int>(std::lround(sum_lum / static_cast<double>(count))) : 0;
            gray[dy * MONO_IMG_W + dx] = static_cast<uint8_t>(std::clamp(avg, 0, 255));
        }
    }

    // 3. Dynamic Contrast Normalization (stretch 5th..95th percentiles to 0..255)
    uint8_t sorted[MONO_IMG_PIXELS];
    std::memcpy(sorted, gray, MONO_IMG_PIXELS);
    std::sort(sorted, sorted + MONO_IMG_PIXELS);

    const int p5  = static_cast<int>(sorted[(MONO_IMG_PIXELS * 5) / 100]);   // index 72
    const int p95 = static_cast<int>(sorted[(MONO_IMG_PIXELS * 95) / 100]);  // index 1368
    const int span = p95 - p5;

    float err_buf[MONO_IMG_PIXELS];
    if (span > 4) {
        for (size_t i = 0; i < MONO_IMG_PIXELS; ++i) {
            const int clamped = std::clamp(static_cast<int>(gray[i]), p5, p95);
            err_buf[i] = static_cast<float>((clamped - p5) * 255) / static_cast<float>(span);
        }
    } else {
        for (size_t i = 0; i < MONO_IMG_PIXELS; ++i) {
            err_buf[i] = static_cast<float>(gray[i]);
        }
    }

    // 4. Floyd-Steinberg Error Diffusion Dithering (48x30 -> binary 0/1)
    uint8_t binary_pixels[MONO_IMG_PIXELS];
    for (size_t y = 0; y < MONO_IMG_H; ++y) {
        for (size_t x = 0; x < MONO_IMG_W; ++x) {
            const size_t idx = y * MONO_IMG_W + x;
            const float old_val = std::clamp(err_buf[idx], 0.0f, 255.0f);
            const uint8_t bit = (old_val >= 128.0f) ? 1u : 0u;
            binary_pixels[idx] = bit;
            const float new_val = bit ? 255.0f : 0.0f;
            const float err = old_val - new_val;

            if (x + 1 < MONO_IMG_W) {
                err_buf[y * MONO_IMG_W + (x + 1)] += err * (7.0f / 16.0f);
            }
            if (x >= 1 && y + 1 < MONO_IMG_H) {
                err_buf[(y + 1) * MONO_IMG_W + (x - 1)] += err * (3.0f / 16.0f);
            }
            if (y + 1 < MONO_IMG_H) {
                err_buf[(y + 1) * MONO_IMG_W + x] += err * (5.0f / 16.0f);
            }
            if (x + 1 < MONO_IMG_W && y + 1 < MONO_IMG_H) {
                err_buf[(y + 1) * MONO_IMG_W + (x + 1)] += err * (1.0f / 16.0f);
            }
        }
    }

    // 5. Bit-Pack 1,440 binary pixels into 180 bytes (MSB first)
    pack_mono_bitmap(binary_pixels, out_bytes);
    return true;
}

// ============================================================================
// Platform-Native Image File Loader (.heic, .png, .jpg, .jpeg, .bmp)
// ============================================================================
inline bool load_and_encode_image_file(const std::wstring& path,
                                       std::array<uint8_t, MONO_IMG_BYTES>& out_bytes,
                                       std::string& out_error) {
    if (path.empty()) {
        out_error = "No image file selected.";
        return false;
    }

#ifdef _WIN32
    IWICImagingFactory* wic = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wic));
    if (FAILED(hr) || !wic) {
        out_error = "Failed to initialize Windows Imaging Component (WIC).";
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                        WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder) {
        wic->Release();
        out_error = "Unable to decode image file (ensure HEIF Image Extensions are installed for .heic).";
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        decoder->Release();
        wic->Release();
        out_error = "Failed to read image frame.";
        return false;
    }

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) {
        frame->Release();
        decoder->Release();
        wic->Release();
        out_error = "Image has invalid dimensions.";
        return false;
    }

    IWICFormatConverter* conv = nullptr;
    hr = wic->CreateFormatConverter(&conv);
    if (FAILED(hr) || !conv) {
        frame->Release();
        decoder->Release();
        wic->Release();
        out_error = "Failed to create WIC pixel format converter.";
        return false;
    }

    hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                          nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        conv->Release();
        frame->Release();
        decoder->Release();
        wic->Release();
        out_error = "Failed to convert image to 32-bit BGRA.";
        return false;
    }

    const UINT stride = w * 4;
    std::vector<uint8_t> bgra(static_cast<size_t>(stride) * static_cast<size_t>(h));
    hr = conv->CopyPixels(nullptr, stride, static_cast<UINT>(bgra.size()), bgra.data());

    conv->Release();
    frame->Release();
    decoder->Release();
    wic->Release();

    if (FAILED(hr)) {
        out_error = "Failed to copy decoded image pixels.";
        return false;
    }

    if (!encode_rgba_to_mono_bitmap(bgra.data(), static_cast<int>(w), static_cast<int>(h),
                                    static_cast<int>(stride), true, out_bytes)) {
        out_error = "Failed to dither and pack image.";
        return false;
    }
    return true;

#elif defined(__APPLE__)
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        out_error = "Cannot open selected image file.";
        return false;
    }
    std::vector<uint8_t> file_bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    if (file_bytes.empty()) {
        out_error = "Selected image file is empty.";
        return false;
    }

    CFDataRef cf_data = CFDataCreate(nullptr, file_bytes.data(), static_cast<CFIndex>(file_bytes.size()));
    if (!cf_data) {
        out_error = "Failed to allocate CFData for image.";
        return false;
    }
    CGImageSourceRef src = CGImageSourceCreateWithData(cf_data, nullptr);
    CFRelease(cf_data);
    if (!src) {
        out_error = "ImageIO could not create image source.";
        return false;
    }
    CGImageRef cg_img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!cg_img) {
        out_error = "ImageIO could not decode image format (.heic/.png/.jpg/.bmp).";
        return false;
    }

    const int w = static_cast<int>(CGImageGetWidth(cg_img));
    const int h = static_cast<int>(CGImageGetHeight(cg_img));
    if (w <= 0 || h <= 0) {
        CGImageRelease(cg_img);
        out_error = "Image has invalid dimensions.";
        return false;
    }

    const int stride = w * 4;
    std::vector<uint8_t> rgba(static_cast<size_t>(stride) * static_cast<size_t>(h), 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    // kCGImageAlphaPremultipliedLast (1) | kCGBitmapByteOrder32Big (4 << 12) = 0x4001
    CGContextRef bmp_ctx = CGBitmapContextCreate(rgba.data(), static_cast<size_t>(w), static_cast<size_t>(h),
                                                 8, static_cast<size_t>(stride), cs, 1u);
    if (cs) CGColorSpaceRelease(cs);
    if (!bmp_ctx) {
        CGImageRelease(cg_img);
        out_error = "Failed to create CoreGraphics RGBA bitmap context.";
        return false;
    }

    CGContextDrawImage(bmp_ctx, CGRectMake(0, 0, w, h), cg_img);
    CGContextRelease(bmp_ctx);
    CGImageRelease(cg_img);

    if (!encode_rgba_to_mono_bitmap(rgba.data(), w, h, stride, false, out_bytes)) {
        out_error = "Failed to dither and pack image.";
        return false;
    }
    return true;
#endif
}

// ============================================================================
// Native OS File Picker Dialog (*.heic, *.png, *.jpg, *.jpeg, *.bmp)
// ============================================================================
inline std::wstring open_image_file_dialog(HWND hwnd) {
#ifdef _WIN32
    wchar_t file_buf[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter =
        L"Supported Images (*.heic;*.png;*.jpg;*.jpeg;*.bmp)\0*.heic;*.png;*.jpg;*.jpeg;*.bmp\0"
        L"All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select Image for 1-Bit Tactical Mesh Transmission (48x30)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        return std::wstring(file_buf);
    }
    return L"";
#elif defined(__APPLE__)
    (void)hwnd;
    id panel = mac_cls_send<id>("NSOpenPanel", sel_registerName("openPanel"));
    if (!panel) return L"";
    mac_msg_send<void>(panel, sel_registerName("setCanChooseFiles:"), static_cast<ObjcBool>(1));
    mac_msg_send<void>(panel, sel_registerName("setCanChooseDirectories:"), static_cast<ObjcBool>(0));
    mac_msg_send<void>(panel, sel_registerName("setAllowsMultipleSelection:"), static_cast<ObjcBool>(0));
    mac_msg_send<void>(panel, sel_registerName("setTitle:"),
                       mac_ns_str("Select Image for 1-Bit Tactical Mesh Transmission (48x30)"));

    id ext_arr = mac_cls_send<id>("NSMutableArray", sel_registerName("array"));
    if (ext_arr) {
        const char* exts[] = {"heic", "HEIC", "png", "PNG", "jpg", "JPG", "jpeg", "JPEG", "bmp", "BMP"};
        for (const char* e : exts) {
            mac_msg_send<void>(ext_arr, sel_registerName("addObject:"), mac_ns_str(e));
        }
        mac_msg_send<void>(panel, sel_registerName("setAllowedFileTypes:"), ext_arr);
    }

    long btn = mac_msg_send<long>(panel, sel_registerName("runModal"));
    if (btn != 1) return L""; // NSModalResponseOK == 1
    id url = mac_msg_send<id>(panel, sel_registerName("URL"));
    if (!url) return L"";
    id path_ns = mac_msg_send<id>(url, sel_registerName("path"));
    std::string utf8 = mac_ns_to_std_str(path_ns);
    return mac_utf8_to_wide(utf8);
#endif
}

} // namespace meshirc
