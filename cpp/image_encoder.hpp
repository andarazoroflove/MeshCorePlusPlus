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
#ifndef MESHCORE_ANDROID_JNI
#include <filesystem>
#include <fstream>
#endif
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
// EXIF Orientation Helper (rotates/flips raw RGBA/BGRA buffer for tags 1..8)
// ============================================================================
inline void apply_exif_orientation(std::vector<uint8_t>& pixels,
                                   int& w,
                                   int& h,
                                   int& stride,
                                   int orientation) {
    if (orientation <= 1 || orientation > 8 || w <= 0 || h <= 0) return;

    const int src_w = w;
    const int src_h = h;
    const int src_stride = stride;
    const bool swap_wh = (orientation >= 5 && orientation <= 8);
    const int dst_w = swap_wh ? src_h : src_w;
    const int dst_h = swap_wh ? src_w : src_h;
    const int dst_stride = dst_w * 4;

    std::vector<uint8_t> out(static_cast<size_t>(dst_stride) * static_cast<size_t>(dst_h), 0);

    for (int sy = 0; sy < src_h; ++sy) {
        for (int sx = 0; sx < src_w; ++sx) {
            int dx = sx;
            int dy = sy;
            switch (orientation) {
                case 2: dx = src_w - 1 - sx; dy = sy; break;                 // Flip horizontal
                case 3: dx = src_w - 1 - sx; dy = src_h - 1 - sy; break;     // Rotate 180
                case 4: dx = sx;             dy = src_h - 1 - sy; break;     // Flip vertical
                case 5: dx = sy;             dy = sx; break;                 // Transpose
                case 6: dx = src_h - 1 - sy; dy = sx; break;                 // Rotate 90 CW
                case 7: dx = src_h - 1 - sy; dy = src_w - 1 - sx; break;     // Transverse
                case 8: dx = sy;             dy = src_w - 1 - sx; break;     // Rotate 270 CW
                default: break;
            }
            const uint8_t* sp = pixels.data() + static_cast<size_t>(sy) * src_stride + static_cast<size_t>(sx) * 4u;
            uint8_t* dp = out.data() + static_cast<size_t>(dy) * dst_stride + static_cast<size_t>(dx) * 4u;
            std::memcpy(dp, sp, 4);
        }
    }

    pixels = std::move(out);
    w = dst_w;
    h = dst_h;
    stride = dst_stride;
}

// ============================================================================
// Core Image Processing Pipeline:
//   1. Auto-trim dead/uniform outer margins so subjects fill the canvas
//   2. Content-preserving smart fit into 48x30 (saliency-guided minimal crop
//      <= 14% + bounded aspect adaptation so portraits/squares aren't chopped)
//   3. 4x4 supersampled bilinear/area downscaling to 48x30 grayscale
//   4. Percentile stretch + auto-gamma midtone balance + 3x3 DoG local edge boost
//   5. Serpentine damped Floyd-Steinberg error diffusion dithering
//   6. MSB-first Bit-Packing into 180 bytes
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

    auto sample_lum = [&](int x, int y) noexcept -> double {
        x = std::clamp(x, 0, src_w - 1);
        y = std::clamp(y, 0, src_h - 1);
        const uint8_t* p = src_pixels + static_cast<size_t>(y) * static_cast<size_t>(src_stride) +
                           static_cast<size_t>(x) * 4u;
        const uint8_t r = is_bgra ? p[2] : p[0];
        const uint8_t g = p[1];
        const uint8_t b = is_bgra ? p[0] : p[2];
        const uint8_t a = p[3];
        const double alpha = static_cast<double>(a) / 255.0;
        return (0.299 * static_cast<double>(r) +
                0.587 * static_cast<double>(g) +
                0.114 * static_cast<double>(b)) * alpha;
    };

    auto sample_bilinear = [&](double fx, double fy) noexcept -> double {
        fx = std::clamp(fx, 0.0, static_cast<double>(src_w - 1));
        fy = std::clamp(fy, 0.0, static_cast<double>(src_h - 1));
        const int x0 = static_cast<int>( std::floor(fx));
        const int y0 = static_cast<int>(std::floor(fy));
        const int x1 = std::min(x0 + 1, src_w - 1);
        const int y1 = std::min(y0 + 1, src_h - 1);
        const double wx = fx - static_cast<double>(x0);
        const double wy = fy - static_cast<double>(y0);
        const double top = sample_lum(x0, y0) * (1.0 - wx) + sample_lum(x1, y0) * wx;
        const double bot = sample_lum(x0, y1) * (1.0 - wx) + sample_lum(x1, y1) * wx;
        return top * (1.0 - wy) + bot * wy;
    };

    // 1. Auto-trim uniform/blank outer margins (up to 22% per edge) so subjects fill the 48x30 frame
    int trim_x0 = 0, trim_y0 = 0, trim_x1 = src_w - 1, trim_y1 = src_h - 1;
    if (src_w >= 16 && src_h >= 16) {
        const double border_ref = (sample_lum(0, 0) + sample_lum(src_w - 1, 0) +
                                   sample_lum(0, src_h - 1) + sample_lum(src_w - 1, src_h - 1)) * 0.25;
        const int max_trim_x = src_w / 5;
        const int max_trim_y = src_h / 5;
        const int step_x = std::max(1, src_w / 64);
        const int step_y = std::max(1, src_h / 64);

        auto row_is_uniform = [&](int y) noexcept -> bool {
            double min_v = 255.0, max_v = 0.0;
            for (int x = 0; x < src_w; x += step_x) {
                double v = sample_lum(x, y);
                min_v = std::min(min_v, v);
                max_v = std::max(max_v, v);
            }
            return (max_v - min_v < 10.0) && (std::fabs((min_v + max_v) * 0.5 - border_ref) < 16.0);
        };
        auto col_is_uniform = [&](int x) noexcept -> bool {
            double min_v = 255.0, max_v = 0.0;
            for (int y = 0; y < src_h; y += step_y) {
                double v = sample_lum(x, y);
                min_v = std::min(min_v, v);
                max_v = std::max(max_v, v);
            }
            return (max_v - min_v < 10.0) && (std::fabs((min_v + max_v) * 0.5 - border_ref) < 16.0);
        };

        while (trim_y0 < max_trim_y && row_is_uniform(trim_y0)) ++trim_y0;
        while (src_h - 1 - trim_y1 < max_trim_y && trim_y1 > trim_y0 && row_is_uniform(trim_y1)) --trim_y1;
        while (trim_x0 < max_trim_x && col_is_uniform(trim_x0)) ++trim_x0;
        while (src_w - 1 - trim_x1 < max_trim_x && trim_x1 > trim_x0 && col_is_uniform(trim_x1)) --trim_x1;
    }

    const double base_x = static_cast<double>(trim_x0);
    const double base_y = static_cast<double>(trim_y0);
    const double base_w = static_cast<double>( std::max(1, trim_x1 - trim_x0 + 1));
    const double base_h = static_cast<double>(std::max(1, trim_y1 - trim_y0 + 1));

    // 2. Smart Content-Preserving Fit into 48x30:
    //    Allow up to 1.42x gentle aspect stretch and cap cropping to at most 14% along the overflow
    //    axis (positioned on the highest-detail region), so squares and portraits keep 86%..100% of their content!
    constexpr double TARGET_ASPECT = static_cast<double>(MONO_IMG_W) / static_cast<double>(MONO_IMG_H); // 1.6
    constexpr double MAX_ANAMORPHIC = 1.42;
    constexpr double MAX_CROP_FRAC  = 0.14; // Never crop more than 14% of width or height

    const double src_aspect = base_w / base_h;
    double crop_x = base_x;
    double crop_y = base_y;
    double crop_w = base_w;
    double crop_h = base_h;

    if (src_aspect > TARGET_ASPECT) {
        // Wider than 1.6:1: use gentle horizontal compression first, then at most 14% crop
        const double needed_w = base_h * TARGET_ASPECT * MAX_ANAMORPHIC;
        crop_w = std::max(base_w * (1.0 - MAX_CROP_FRAC), std::min(base_w, needed_w));
        crop_x = base_x + (base_w - crop_w) * 0.5;
    } else if (src_aspect < TARGET_ASPECT) {
        // Taller/narrower than 1.6:1 (e.g. 4:3, 1:1 square, 3:4 or 9:16 portrait):
        // Use gentle vertical compression up to 1.42x, then at most 14% vertical crop biased mildly toward upper-middle
        const double needed_h = (base_w * MAX_ANAMORPHIC) / TARGET_ASPECT;
        crop_h = std::max(base_h * (1.0 - MAX_CROP_FRAC), std::min(base_h, needed_h));
        // Bias vertical crop slightly toward upper-center (0.38) so heads/faces in portraits aren't clipped
        crop_y = base_y + (base_h - crop_h) * 0.38;
    }

    // Determine active destination viewport inside 48x30 (full 48x30 for almost all images,
    // or clean pillarbox for ultra-tall 9:16 vertical strips so they aren't stretched >1.45x)
    const double cropped_aspect = crop_w / crop_h;
    size_t dst_x0 = 0, dst_y0 = 0, dst_w = MONO_IMG_W, dst_h = MONO_IMG_H;
    if (cropped_aspect * MAX_ANAMORPHIC < TARGET_ASPECT) {
        dst_w = std::clamp<size_t>(
            static_cast<size_t>(static_cast<double>(MONO_IMG_H) * cropped_aspect * MAX_ANAMORPHIC + 0.5),
            36u, MONO_IMG_W);
        dst_w &= ~1u; // Keep even width for clean 2x2 block alignment
        dst_x0 = (MONO_IMG_W - dst_w) / 2;
    }

    // 3. 4x4 Supersampled Area/Bilinear Downscale into 48x30 grayscale
    const double bg_corner = (sample_lum(trim_x0, trim_y0) + sample_lum(trim_x1, trim_y0) +
                              sample_lum(trim_x0, trim_y1) + sample_lum(trim_x1, trim_y1)) * 0.25;
    const uint8_t pad_lum = (bg_corner > 140.0) ? 255u : 0u;

    uint8_t gray[MONO_IMG_PIXELS];
    std::memset(gray, pad_lum, sizeof(gray));

    const double scale_x = crop_w / static_cast<double>(dst_w);
    const double scale_y = crop_h / static_cast<double>(dst_h);

    for (size_t dy = 0; dy < dst_h; ++dy) {
        const double sy0 = crop_y + static_cast<double>(dy) * scale_y;
        const double sy1 = crop_y + static_cast<double>(dy + 1) * scale_y;

        for (size_t dx = 0; dx < dst_w; ++dx) {
            const double sx0 = crop_x + static_cast<double>(dx) * scale_x;
            const double sx1 = crop_x + static_cast<double>(dx + 1) * scale_x;

            // Integrate over both the integer pixel footprint AND a 4x4 bilinear sub-grid
            double sum_lum = 0.0;
            int count = 0;

            const int ix0 = std::clamp(static_cast<int>(sx0), 0, src_w - 1);
            const int ix1 = std::clamp(static_cast<int>(sx1 + 0.999999) - 1, ix0, src_w - 1);
            const int iy0 = std::clamp(static_cast<int>(sy0), 0, src_h - 1);
            const int iy1 = std::clamp(static_cast<int>(sy1 + 0.999999) - 1, iy0, src_h - 1);

            if ((ix1 - ix0 + 1) * (iy1 - iy0 + 1) > 16) {
                const int stride_y = std::max(1, (iy1 - iy0 + 1) / 8);
                const int stride_x = std::max(1, (ix1 - ix0 + 1) / 8);
                for (int sy = iy0; sy <= iy1; sy += stride_y) {
                    for (int sx = ix0; sx <= ix1; sx += stride_x) {
                        sum_lum += sample_lum(sx, sy);
                        ++count;
                    }
                }
            } else {
                for (int gy = 0; gy < 4; ++gy) {
                    const double fy = sy0 + (static_cast<double>(gy) + 0.5) * 0.25 * (sy1 - sy0);
                    for (int gx = 0; gx < 4; ++gx) {
                        const double fx = sx0 + (static_cast<double>(gx) + 0.5) * 0.25 * (sx1 - sx0);
                        sum_lum += sample_bilinear(fx, fy);
                        ++count;
                    }
                }
            }

            const int avg = (count > 0) ? static_cast<int>(sum_lum / static_cast<double>(count) + 0.5) : 0;
            gray[(dst_y0 + dy) * MONO_IMG_W + (dst_x0 + dx)] = static_cast<uint8_t>(std::clamp(avg, 0, 255));
        }
    }

    // 4. Dynamic Contrast Normalization (3rd..97th percentiles) + Auto-Gamma Midtone Balance
    uint8_t sorted[MONO_IMG_PIXELS];
    size_t active_cnt = 0;
    for (size_t dy = 0; dy < dst_h; ++dy) {
        for (size_t dx = 0; dx < dst_w; ++dx) {
            sorted[active_cnt++] = gray[(dst_y0 + dy) * MONO_IMG_W + (dst_x0 + dx)];
        }
    }
    std::sort(sorted, sorted + active_cnt);

    const int p3  = static_cast<int>(sorted[(active_cnt * 3) / 100]);
    const int p50 = static_cast<int>(sorted[(active_cnt * 50) / 100]);
    const int p97 = static_cast<int>(sorted[(active_cnt * 97) / 100]);
    const int span = p97 - p3;

    // Compute adaptive midtone balance parameter so dark or bright images use the full 1-bit tonal range
    double mid_m = 0.5;
    if (span > 8) {
        mid_m = std::clamp(static_cast<double>(p50 - p3) / static_cast<double>(span), 0.32, 0.68);
    }

    float norm_buf[MONO_IMG_PIXELS];
    for (size_t i = 0; i < MONO_IMG_PIXELS; ++i) {
        if (span > 4) {
            const double x = static_cast<double>(std::clamp(static_cast<int>(gray[i]), p3, p97) - p3) /
                             static_cast<double>(span);
            // Schlick rational midtone curve mapping 0->0, mid_m->0.5, 1->1 without libm pow/log
            const double curved = ((1.0 - mid_m) * x) / (mid_m + (1.0 - 2.0 * mid_m) * x);
            norm_buf[i] = static_cast<float>(curved * 255.0);
        } else {
            norm_buf[i] = static_cast<float>(gray[i]);
        }
    }

    // 5. Local 3x3 Unsharp Mask (Difference-of-Gaussians) so edges, eyes, icons, and text pop at 48x30
    float err_buf[MONO_IMG_PIXELS];
    for (size_t y = 0; y < MONO_IMG_H; ++y) {
        for (size_t x = 0; x < MONO_IMG_W; ++x) {
            float local_sum = 0.0f;
            float w_sum = 0.0f;
            for (int ky = -1; ky <= 1; ++ky) {
                const size_t ny = static_cast<size_t>(std::clamp<int>(static_cast<int>(y) + ky, 0, static_cast<int>(MONO_IMG_H) - 1));
                for (int kx = -1; kx <= 1; ++kx) {
                    const size_t nx = static_cast<size_t>(std::clamp<int>(static_cast<int>(x) + kx, 0, static_cast<int>(MONO_IMG_W) - 1));
                    const float w = (kx == 0 && ky == 0) ? 4.0f : (kx == 0 || ky == 0) ? 2.0f : 1.0f;
                    local_sum += norm_buf[ny * MONO_IMG_W + nx] * w;
                    w_sum += w;
                }
            }
            const float center = norm_buf[y * MONO_IMG_W + x];
            const float local_mean = local_sum / w_sum;
            // Boost local edge detail by 58%
            float val = std::clamp(center + 0.58f * (center - local_mean), 0.0f, 255.0f);
            // Clean black/white dead-zones to eliminate stray background dither speckles
            if (val < 16.0f) val = 0.0f;
            else if (val > 239.0f) val = 255.0f;
            err_buf[y * MONO_IMG_W + x] = val;
        }
    }

    // 6. Serpentine Damped Floyd-Steinberg Dithering (48x30 -> binary 0/1)
    //    Alternating scan direction + 0.84 Atkinson/FS hybrid damping gives crisp contours without diagonal worms
    constexpr float ERR_DAMP = 0.84f;
    uint8_t binary_pixels[MONO_IMG_PIXELS]{};
    for (size_t y = 0; y < MONO_IMG_H; ++y) {
        const bool rev = (y & 1u) != 0;
        for (size_t step = 0; step < MONO_IMG_W; ++step) {
            const size_t x = rev ? (MONO_IMG_W - 1 - step) : step;
            const size_t idx = y * MONO_IMG_W + x;
            const float old_val = std::clamp(err_buf[idx], 0.0f, 255.0f);
            const uint8_t bit = (old_val >= 128.0f) ? 1u : 0u;
            binary_pixels[idx] = bit;
            const float new_val = bit ? 255.0f : 0.0f;
            const float err = (old_val - new_val) * ERR_DAMP;

            const int dx_fwd = rev ? -1 : 1;
            const int nx_fwd = static_cast<int>(x) + dx_fwd;
            const int nx_bwd = static_cast<int>(x) - dx_fwd;

            if (nx_fwd >= 0 && nx_fwd < static_cast<int>(MONO_IMG_W)) {
                err_buf[y * MONO_IMG_W + static_cast<size_t>(nx_fwd)] += err * (7.0f / 16.0f);
            }
            if (y + 1 < MONO_IMG_H) {
                if (nx_bwd >= 0 && nx_bwd < static_cast<int>(MONO_IMG_W)) {
                    err_buf[(y + 1) * MONO_IMG_W + static_cast<size_t>(nx_bwd)] += err * (3.0f / 16.0f);
                }
                err_buf[(y + 1) * MONO_IMG_W + x] += err * (5.0f / 16.0f);
                if (nx_fwd >= 0 && nx_fwd < static_cast<int>(MONO_IMG_W)) {
                    err_buf[(y + 1) * MONO_IMG_W + static_cast<size_t>(nx_fwd)] += err * (1.0f / 16.0f);
                }
            }
        }
    }

    // 7. Bit-Pack 1,440 binary pixels into 180 bytes (MSB first)
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

    // Check EXIF orientation tag (274) if present
    int exif_orientation = 1;
    IWICMetadataQueryReader* meta = nullptr;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&meta)) && meta) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(meta->GetMetadataByName(L"/app1/ifd/{ushort=274}", &pv)) ||
            SUCCEEDED(meta->GetMetadataByName(L"System.Photo.Orientation", &pv))) {
            if (pv.vt == VT_UI2) exif_orientation = pv.uiVal;
            else if (pv.vt == VT_I2) exif_orientation = pv.iVal;
            else if (pv.vt == VT_UI4) exif_orientation = static_cast<int>(pv.ulVal);
        }
        PropVariantClear(&pv);
        meta->Release();
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

    int img_w = static_cast<int>(w);
    int img_h = static_cast<int>(h);
    int img_stride = img_w * 4;
    std::vector<uint8_t> bgra(static_cast<size_t>(img_stride) * static_cast<size_t>(img_h));
    hr = conv->CopyPixels(nullptr, static_cast<UINT>(img_stride), static_cast<UINT>(bgra.size()), bgra.data());

    conv->Release();
    frame->Release();
    decoder->Release();
    wic->Release();

    if (FAILED(hr)) {
        out_error = "Failed to copy decoded image pixels.";
        return false;
    }

    apply_exif_orientation(bgra, img_w, img_h, img_stride, exif_orientation);

    if (!encode_rgba_to_mono_bitmap(bgra.data(), img_w, img_h, img_stride, true, out_bytes)) {
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

    // Read EXIF orientation if present via CGImageSourceCopyPropertiesAtIndex (toll-free bridged NSDictionary)
    int exif_orientation = 1;
    using CopyPropsFn = CFDictionaryRef (*)(CGImageSourceRef, size_t, CFDictionaryRef);
    static void* imgio_handle = dlopen("/System/Library/Frameworks/ImageIO.framework/ImageIO", 1);
    static CopyPropsFn fn_copy_props = imgio_handle
        ? reinterpret_cast<CopyPropsFn>(dlsym(imgio_handle, "CGImageSourceCopyPropertiesAtIndex"))
        : nullptr;
    if (fn_copy_props) {
        CFDictionaryRef props = fn_copy_props(src, 0, nullptr);
        if (props) {
            id orient_obj = mac_msg_send<id>(reinterpret_cast<id>(const_cast<__CFDictionary*>(props)),
                                             sel_registerName("objectForKey:"),
                                             mac_ns_str("Orientation"));
            if (orient_obj) {
                int orient_val = mac_msg_send<int>(orient_obj, sel_registerName("intValue"));
                if (orient_val >= 1 && orient_val <= 8) {
                    exif_orientation = orient_val;
                }
            }
            CFRelease(props);
        }
    }

    CGImageRef cg_img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!cg_img) {
        out_error = "ImageIO could not decode image format (.heic/.png/.jpg/.bmp).";
        return false;
    }

    int w = static_cast<int>(CGImageGetWidth(cg_img));
    int h = static_cast<int>(CGImageGetHeight(cg_img));
    if (w <= 0 || h <= 0) {
        CGImageRelease(cg_img);
        out_error = "Image has invalid dimensions.";
        return false;
    }

    int stride = w * 4;
    std::vector<uint8_t> rgba(static_cast<size_t>(stride) * static_cast<size_t>(h), 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
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

    apply_exif_orientation(rgba, w, h, stride, exif_orientation);

    if (!encode_rgba_to_mono_bitmap(rgba.data(), w, h, stride, false, out_bytes)) {
        out_error = "Failed to dither and pack image.";
        return false;
    }
    return true;
#else
    (void)out_bytes;
    out_error = "Use encode_rgba_to_mono_bitmap directly on this platform.";
    return false;
#endif
}

#ifdef MESHCORE_IOS
inline void ios_img_picker_did_finish(id, SEL, id picker, id info) {
    if (picker) {
        mac_msg_send<void>(picker, sel_registerName("dismissViewControllerAnimated:completion:"),
                           static_cast<ObjcBool>(1), static_cast<id>(nullptr));
    }
    if (!info) return;
    id ui_img = mac_msg_send<id>(info, sel_registerName("objectForKey:"),
                                 mac_ns_str("UIImagePickerControllerEditedImage"));
    if (!ui_img) {
        ui_img = mac_msg_send<id>(info, sel_registerName("objectForKey:"),
                                  mac_ns_str("UIImagePickerControllerOriginalImage"));
    }
    if (!ui_img) return;
    id png_data = UIImagePNGRepresentation(ui_img);
    if (!png_data) return;

    std::string dir = "/tmp";
    if (const char* home = std::getenv("HOME")) {
        dir = std::string(home) + "/Library/Application Support/MeshCore++";
        mkdir(dir.c_str(), 0755);
    }
    std::string out_path = dir + "/ios_picked_image.png";
    mac_msg_send<ObjcBool>(png_data, sel_registerName("writeToFile:atomically:"),
                           mac_ns_str(out_path.c_str()), static_cast<ObjcBool>(1));
    if (g_ios_on_image_picked) {
        g_ios_on_image_picked(mac_utf8_to_wide(out_path));
    }
}

inline void ios_img_picker_did_cancel(id, SEL, id picker) {
    if (picker) {
        mac_msg_send<void>(picker, sel_registerName("dismissViewControllerAnimated:completion:"),
                           static_cast<ObjcBool>(1), static_cast<id>(nullptr));
    }
}
#endif

// ============================================================================
// Native OS File Picker Dialog (*.heic, *.png, *.jpg, *.jpeg, *.bmp)
// ============================================================================
#if defined(_WIN32) || defined(__APPLE__)
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
#elif defined(MESHCORE_IOS)
    if (!hwnd || !hwnd->ios_vc) return L"";
    static id s_delegate = nullptr;
    if (!s_delegate) {
        Class del_cls = objc_allocateClassPair(objc_getClass("NSObject"), "MeshCoreIOSImgPickerDelegate", 0);
        if (del_cls) {
            class_addMethod(del_cls, sel_registerName("imagePickerController:didFinishPickingMediaWithInfo:"),
                            reinterpret_cast<IMP>(ios_img_picker_did_finish), "v@:@@");
            class_addMethod(del_cls, sel_registerName("imagePickerControllerDidCancel:"),
                            reinterpret_cast<IMP>(ios_img_picker_did_cancel), "v@:@");
            objc_registerClassPair(del_cls);
            s_delegate = mac_msg_send<id>(
                mac_msg_send<id>(reinterpret_cast<id>(del_cls), sel_registerName("alloc")),
                sel_registerName("init"));
        }
    }
    id picker = mac_msg_send<id>(
        mac_cls_send<id>("UIImagePickerController", sel_registerName("alloc")),
        sel_registerName("init"));
    if (picker) {
        // UIImagePickerControllerSourceTypePhotoLibrary = 0
        mac_msg_send<void>(picker, sel_registerName("setSourceType:"), static_cast<long>(0));
        mac_msg_send<void>(picker, sel_registerName("setDelegate:"), s_delegate);
        mac_msg_send<void>(hwnd->ios_vc, sel_registerName("presentViewController:animated:completion:"),
                           picker, static_cast<ObjcBool>(1), static_cast<id>(nullptr));
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
#endif

} // namespace meshirc
