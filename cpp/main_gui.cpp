#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <winhttp.h>
#include <wincodec.h>
#elif defined(__APPLE__)
#include "macos_d2d_shim.hpp"
#endif

#include <iostream>
#include <fstream>
#include "crypto.hpp"
#include "protocol.hpp"
#include "transport.hpp"
#include "image_encoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>

using namespace meshirc;

// ============================================================================
// Live Map: Web-Mercator helpers + background tile fetcher (CARTO / OSM raster tiles)
// ============================================================================
static constexpr double MAP_PI = 3.14159265358979323846;

static double lon_to_nx(double lon) { return (lon + 180.0) / 360.0; }
static double lat_to_ny(double lat) {
    lat = std::clamp(lat, -85.0511, 85.0511);
    double r = lat * MAP_PI / 180.0;
    return (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / MAP_PI) / 2.0;
}
static double haversine_km(double lat1, double lon1, double lat2, double lon2) {
    double p = MAP_PI / 180.0;
    double a = 0.5 - std::cos((lat2 - lat1) * p) / 2 +
               std::cos(lat1 * p) * std::cos(lat2 * p) * (1 - std::cos((lon2 - lon1) * p)) / 2;
    return 12742.0 * std::asin(std::sqrt(a));
}

// key layout: style(2) | z(6) | x(28) | y(28)
static uint64_t tile_key(int style, int z, int x, int y) {
    return (static_cast<uint64_t>(style & 3) << 62) | (static_cast<uint64_t>(z & 63) << 56) |
           (static_cast<uint64_t>(x & 0xFFFFFFF) << 28) | static_cast<uint64_t>(y & 0xFFFFFFF);
}

class TileFetcher {
public:
    static const wchar_t* style_path(int) { return L"osm"; }

    ~TileFetcher() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // Queue a tile (newest requests are served first; stale ones fall off the back)
    void request(int style, int z, int x, int y) {
        uint64_t k = tile_key(style, z, x, y);
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
            if (known_.count(k)) return;
            auto f = failed_.find(k);
            if (f != failed_.end() && GetTickCount64() < f->second) return;
            known_.insert(k);
            queue_.push_front(k);
            while (queue_.size() > 120) {
                known_.erase(queue_.back());
                queue_.pop_back();
            }
        }
        cv_.notify_one();
    }

    // Grab a finished download (PNG bytes). Returns false if not ready.
    bool take(uint64_t k, std::vector<uint8_t>& out) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = done_.find(k);
        if (it == done_.end()) return false;
        out = std::move(it->second);
        done_.erase(it);
        return true;
    }

    // Called when a decoded bitmap is evicted so it can be re-requested later
    void forget(uint64_t k) {
        std::lock_guard<std::mutex> lk(mu_);
        known_.erase(k);
        done_.erase(k);
    }

    size_t pending() {
        std::lock_guard<std::mutex> lk(mu_);
        return queue_.size();
    }
    bool offline() const { return offline_.load(); }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_ = false;
    std::deque<uint64_t> queue_;
    std::unordered_set<uint64_t> known_;                       // pending, in flight, done or decoded
    std::unordered_map<uint64_t, std::vector<uint8_t>> done_;  // downloaded, waiting for UI decode
    std::unordered_map<uint64_t, ULONGLONG> failed_;           // retry-after tick
    std::atomic<bool> offline_{false};

    static std::wstring cache_path(uint64_t k) {
        int style = static_cast<int>(k >> 62), z = static_cast<int>((k >> 56) & 63);
        int x = static_cast<int>((k >> 28) & 0xFFFFFFF), y = static_cast<int>(k & 0xFFFFFFF);
        wchar_t exe[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        size_t slash = dir.find_last_of(L"\\/");
        dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);
#ifdef _WIN32
        dir += L"\\tiles\\";
        dir += style_path(style);
        dir += L"\\" + std::to_wstring(z);
        return dir + L"\\" + std::to_wstring(x) + L"_" + std::to_wstring(y) + L".png";
#else
        dir += L"/tiles/";
        dir += style_path(style);
        dir += L"/" + std::to_wstring(z);
        return dir + L"/" + std::to_wstring(x) + L"_" + std::to_wstring(y) + L".png";
#endif
    }

    static bool read_file(const std::wstring& p, std::vector<uint8_t>& out) {
        std::ifstream f(std::filesystem::path(p), std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return out.size() > 64;
    }

#ifdef _WIN32
    bool download(HINTERNET conn, uint64_t k, std::vector<uint8_t>& out) {
        int z = static_cast<int>((k >> 56) & 63);
        int x = static_cast<int>((k >> 28) & 0xFFFFFFF), y = static_cast<int>(k & 0xFFFFFFF);
        std::wstring path = L"/" + std::to_wstring(z) + L"/" + std::to_wstring(x) + L"/" + std::to_wstring(y) + L".png";
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) return false;
        bool ok = false;
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &sz, WINHTTP_NO_HEADER_INDEX);
            if (status == 200) {
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                    size_t off = out.size();
                    out.resize(off + avail);
                    DWORD got = 0;
                    if (!WinHttpReadData(req, out.data() + off, avail, &got)) break;
                    out.resize(off + got);
                }
                ok = out.size() > 64;
            }
        }
        WinHttpCloseHandle(req);
        return ok;
    }
#elif defined(__APPLE__)
    bool download_curl(uint64_t k, std::vector<uint8_t>& out) {
        int z = static_cast<int>((k >> 56) & 63);
        int x = static_cast<int>((k >> 28) & 0xFFFFFFF), y = static_cast<int>(k & 0xFFFFFFF);
        char url_buf[256];
        std::snprintf(url_buf, sizeof(url_buf), "https://tile.openstreetmap.org/%d/%d/%d.png", z, x, y);
#ifdef MESHCORE_IOS
        id url = mac_cls_send<id>("NSURL", sel_registerName("URLWithString:"), mac_ns_str(url_buf));
        id data = url ? mac_cls_send<id>("NSData", sel_registerName("dataWithContentsOfURL:"), url) : nullptr;
        if (!data) return false;
        const uint8_t* ptr = mac_msg_send<const uint8_t*>(data, sel_registerName("bytes"));
        unsigned long len = mac_msg_send<unsigned long>(data, sel_registerName("length"));
        if (!ptr || len <= 64) return false;
        out.assign(ptr, ptr + len);
        return true;
#else
        char cmd[512];
        std::snprintf(cmd, sizeof(cmd),
                      "/usr/bin/curl -sfL -m 8 -A 'MeshCorePlusPlus/1.0 (macOS Sonoma)' '%s'",
                      url_buf);
        FILE* fp = popen(cmd, "r");
        if (!fp) return false;
        uint8_t buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
            out.insert(out.end(), buf, buf + n);
        }
        int rc = pclose(fp);
        return rc == 0 && out.size() > 64;
#endif
    }
#endif

    void run() {
#ifdef _WIN32
        HINTERNET session = WinHttpOpen(L"MeshCorePlusPlus/1.0 (personal LoRa mesh map viewer; Windows)",
                                        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        if (session) WinHttpSetTimeouts(session, 4000, 4000, 6000, 8000);
        HINTERNET conn = session ? WinHttpConnect(session, L"tile.openstreetmap.org", INTERNET_DEFAULT_HTTPS_PORT, 0)
                                 : nullptr;
#endif
        for (;;) {
            uint64_t k = 0;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_) break;
                k = queue_.front();
                queue_.pop_front();
            }
            std::vector<uint8_t> bytes;
            std::wstring cp = cache_path(k);
            bool ok = read_file(cp, bytes);
#ifdef _WIN32
            if (!ok && conn) {
                ok = download(conn, k, bytes);
#elif defined(__APPLE__)
            if (!ok) {
                ok = download_curl(k, bytes);
#endif
                offline_ = !ok;
                if (ok) {
                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::path(cp).parent_path(), ec);
                    std::ofstream f(std::filesystem::path(cp), std::ios::binary);
                    if (f) f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                }
            }
            std::lock_guard<std::mutex> lk(mu_);
            if (ok) {
                done_[k] = std::move(bytes);
            } else {
                known_.erase(k);
                failed_[k] = GetTickCount64() + 30000; // back off 30s
            }
        }
#ifdef _WIN32
        if (conn) WinHttpCloseHandle(conn);
        if (session) WinHttpCloseHandle(session);
#endif
    }
};

static TileFetcher& get_tiles() {
    static TileFetcher* s = new TileFetcher();
    return *s;
}
#define g_tiles (get_tiles())

// ============================================================================
// UTF-8 <-> UTF-16 Conversion & Time Helpers
// ============================================================================
static std::wstring to_wide(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
#elif defined(__APPLE__)
    return mac_utf8_to_wide(utf8);
#endif
}

static std::string now_hms_str() {
    std::time_t t = std::time(nullptr);
    std::tm tm_val{};
    localtime_s(&tm_val, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
    return std::string(buf);
}

static uint64_t now_epoch_secs() {
    return static_cast<uint64_t>(std::time(nullptr));
}

// ============================================================================
// Snooping Toolkit Helpers (packet inspector / adverts / paths / exports)
// ============================================================================
static std::string now_hms_ms_str() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buf);
}

static std::string epoch_to_iso(uint64_t epoch) {
    if (epoch == 0) return "";
    std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm_val{};
    if (localtime_s(&tm_val, &t) != 0) return "";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", tm_val.tm_year + 1900, tm_val.tm_mon + 1,
                  tm_val.tm_mday, tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
    return std::string(buf);
}

static std::string hex_string(const uint8_t* data, size_t len, const char* sep = " ") {
    static const char* HX = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 3);
    for (size_t i = 0; i < len; ++i) {
        if (i > 0) out += sep;
        out += HX[data[i] >> 4];
        out += HX[data[i] & 0x0F];
    }
    return out;
}

static std::vector<std::string> hex_dump_lines(const std::vector<uint8_t>& bytes) {
    std::vector<std::string> lines;
    for (size_t off = 0; off < bytes.size(); off += 16) {
        size_t n = std::min<size_t>(16, bytes.size() - off);
        char head[8];
        std::snprintf(head, sizeof(head), "%04X  ", static_cast<unsigned>(off));
        std::string line = head;
        std::string hex = hex_string(bytes.data() + off, n);
        hex.resize(16 * 3 - 1, ' ');
        line += hex + "  ";
        for (size_t i = 0; i < n; ++i) {
            uint8_t c = bytes[off + i];
            line += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
        }
        lines.push_back(line);
    }
    return lines;
}

static const char* companion_code_name(uint8_t code, bool tx) {
    if (tx) {
        switch (code) {
            case 0x01: return "CMD_APP_START";
            case 0x02: return "CMD_SEND_TXT_MSG";
            case 0x03: return "CMD_SEND_CHANNEL_TXT_MSG";
            case 0x04: return "CMD_GET_CONTACTS";
            case 0x07: return "CMD_SEND_SELF_ADVERT";
            case 0x0A: return "CMD_SYNC_NEXT_MESSAGE";
            case 0x14: return "CMD_GET_BAT";
            case 0x16: return "CMD_DEVICE_QUERY";
            case 0x1F: return "CMD_GET_CHANNEL";
            case 0x20: return "CMD_SET_CHANNEL";
            default:   return "CMD_UNKNOWN";
        }
    }
    switch (code) {
        case 0x00: return "RESP_OK";
        case 0x01: return "RESP_ERR";
        case 0x02: return "RESP_CONTACTS_START";
        case 0x03: return "RESP_CONTACT";
        case 0x04: return "RESP_END_OF_CONTACTS";
        case 0x05: return "RESP_SELF_INFO";
        case 0x06: return "RESP_SENT";
        case 0x07: return "RESP_CONTACT_MSG";
        case 0x08: return "RESP_CHANNEL_MSG";
        case 0x0A: return "RESP_NO_MORE_MSGS";
        case 0x0C: return "RESP_BATTERY";
        case 0x0D: return "RESP_DEVICE_INFO";
        case 0x10: return "RESP_CONTACT_MSG_V3";
        case 0x11: return "RESP_CHANNEL_MSG_V3";
        case 0x12: return "RESP_CHANNEL_INFO";
        case 0x80: return "PUSH_ADVERT";
        case 0x81: return "PUSH_PATH_UPDATED";
        case 0x82: return "PUSH_SEND_CONFIRMED";
        case 0x83: return "PUSH_MSG_WAITING";
        case 0x84: return "PUSH_RAW_DATA";
        case 0x88: return "PUSH_LOG_RX_DATA";
        case 0x8A: return "PUSH_NEW_ADVERT";
        default:   return "UNKNOWN";
    }
}

static const char* lora_route_name(uint8_t rt) {
    switch (rt) {
        case 0x00: return "TRANSPORT_FLOOD";
        case 0x01: return "FLOOD";
        case 0x02: return "DIRECT";
        case 0x03: return "TRANSPORT_DIRECT";
        default:   return "?";
    }
}

static const char* lora_payload_name(uint8_t pt) {
    switch (pt) {
        case 0x00: return "REQ";
        case 0x01: return "RESPONSE";
        case 0x02: return "TXT_MSG";
        case 0x03: return "ACK";
        case 0x04: return "ADVERT";
        case 0x05: return "GRP_TXT";
        case 0x06: return "GRP_DATA";
        case 0x07: return "ANON_REQ";
        case 0x08: return "PATH";
        case 0x09: return "TRACE";
        case 0x0A: return "MULTIPART";
        case 0x0F: return "RAW_CUSTOM";
        case 0x49: return "MONO_IMG";
        default:   return "PAYLOAD?";
    }
}

static const char* contact_type_name(uint8_t t) {
    switch (t) {
        case 1: return "Companion";
        case 2: return "Repeater";
        case 3: return "Room Server";
        case 4: return "Sensor";
        default: return "Node";
    }
}

static std::string csv_escape(const std::string& s) {
    bool needs = s.find_first_of(",\"\r\n") != std::string::npos;
    if (!needs) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

static std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20 || c == '\t') out += c;
                break;
        }
    }
    return out;
}

// Returns "<exe folder>\exports\" (created on demand)
static std::wstring exports_dir() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);
#ifdef _WIN32
    dir += L"\\exports";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\";
#else
    dir += L"/exports";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"/";
#endif
}

static std::string from_wide(const std::wstring& w) {
#ifdef _WIN32
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, len)), '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
#elif defined(__APPLE__)
    return mac_wide_to_utf8(w);
#endif
}

static std::wstring exe_dir() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L"\\/");
#ifdef _WIN32
    return (slash == std::wstring::npos) ? L".\\" : (dir.substr(0, slash) + L"\\");
#else
    return (slash == std::wstring::npos) ? L"./" : (dir.substr(0, slash) + L"/");
#endif
}

static std::string tsv_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

static std::string tsv_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            if (n == 't') out += '\t';
            else if (n == 'n') out += '\n';
            else if (n == 'r') out += '\r';
            else out += n;
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::vector<std::string> split_tsv(const std::string& line) {
    std::vector<std::string> cols;
    size_t start = 0;
    for (;;) {
        size_t pos = line.find('\t', start);
        if (pos == std::string::npos) {
            cols.push_back(tsv_unescape(line.substr(start)));
            break;
        }
        cols.push_back(tsv_unescape(line.substr(start, pos - start)));
        start = pos + 1;
    }
    return cols;
}

static bool hex_to_bytes(const std::string& hex, uint8_t* out, size_t n) {
    if (hex.size() < n * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

static std::wstring export_filename(const wchar_t* stem, const wchar_t* ext) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[96];
    swprintf(buf, 96, L"%ls_%04d%02d%02d_%02d%02d%02d.%ls", stem, st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, ext);
    return exports_dir() + buf;
}

static void reveal_in_explorer(const std::wstring& file) {
#ifdef _WIN32
    std::wstring args = L"/select,\"" + file + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
#elif defined(MESHCORE_IOS)
    (void)file;
#elif defined(__APPLE__)
    std::string utf8 = mac_wide_to_utf8(file);
    std::string cmd = "/usr/bin/open -R \"" + utf8 + "\" >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#endif
}

static void copy_text_to_clipboard(HWND hwnd, const std::string& text) {
#ifdef _WIN32
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (hGlob) {
        void* ptr = GlobalLock(hGlob);
        if (ptr) {
            std::memcpy(ptr, text.c_str(), text.size() + 1);
            GlobalUnlock(hGlob);
            SetClipboardData(CF_TEXT, hGlob);
        }
    }
    CloseClipboard();
#elif defined(MESHCORE_IOS)
    (void)hwnd;
    id pb = mac_cls_send<id>("UIPasteboard", sel_registerName("generalPasteboard"));
    if (pb) {
        mac_msg_send<void>(pb, sel_registerName("setString:"), mac_ns_str(text.c_str()));
    }
#elif defined(__APPLE__)
    (void)hwnd;
    FILE* fp = popen("/usr/bin/pbcopy", "w");
    if (fp) {
        std::fwrite(text.data(), 1, text.size(), fp);
        pclose(fp);
    }
#endif
}

static std::string get_clipboard_text(HWND hwnd) {
#ifdef _WIN32
    if (!OpenClipboard(hwnd)) return "";
    std::string out;
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (hData) {
        const char* ptr = static_cast<const char*>(GlobalLock(hData));
        if (ptr) {
            out = ptr;
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return out;
#elif defined(MESHCORE_IOS)
    (void)hwnd;
    id pb = mac_cls_send<id>("UIPasteboard", sel_registerName("generalPasteboard"));
    if (!pb) return "";
    id s = mac_msg_send<id>(pb, sel_registerName("string"));
    return mac_ns_to_std_str(s);
#elif defined(__APPLE__)
    (void)hwnd;
    std::string out;
    FILE* fp = popen("/usr/bin/pbpaste", "r");
    if (fp) {
        char buf[1024];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
            out.append(buf, n);
        }
        pclose(fp);
    }
    return out;
#endif
}

// ============================================================================
// Application Data Structures
// ============================================================================
struct PathHop {
    std::string label;           // resolved node name or hex prefix
    bool resolved = false;
    int32_t lat_e6 = 0;
    int32_t lon_e6 = 0;
};

// One over-the-air copy of a message and the repeaters it was relayed through
struct MessageRoute {
    std::vector<PathHop> hops;   // in relay order: first repeater after origin ... last repeater before us
    int16_t rssi = 0;
    float snr = 0.0f;
    std::string time;
};

struct ChatMessage {
    std::string timestamp;
    std::string sender;
    bool has_rf = false;
    int16_t rssi = -80;
    float snr = 0.0f;
    uint8_t hops = 1;
    std::string text;
    bool is_system = false;
    bool is_mention = false;
    bool is_self = false;
    bool has_mono_image = false;
    std::array<uint8_t, MONO_IMG_BYTES> mono_image{};
    std::string trace_key;              // channel|sender|text, used to merge later flood copies
    std::vector<MessageRoute> routes;   // every distinct repeater path this message was heard over
};

struct HeardStation {
    std::string callsign;
    int16_t last_rssi = -80;
    float last_snr = 0.0f;
    uint8_t hops = 1;
    std::string last_heard_time;
    uint64_t last_heard_epoch = 0;
    size_t msg_count = 0;
};

struct ChannelTab {
    std::string name;
    std::vector<ChatMessage> messages;
    size_t unread_count = 0;
    bool has_mention = false;
    int scroll_offset = 0;
    std::vector<HeardStation> nicks;

    explicit ChannelTab(std::string n = "") : name(std::move(n)) {}

    void update_station(const std::string& call, int16_t rssi, float snr, uint8_t hops, const std::string& custom_time = "") {
        if (call.empty() || call == "*" || call == "SYSTEM" || call == "RADIO") return;
        std::string t_str = custom_time.empty() ? now_hms_str() : custom_time;
        uint64_t ep = custom_time.empty() ? now_epoch_secs() : 1;
        for (auto& st : nicks) {
            if (_stricmp(st.callsign.c_str(), call.c_str()) == 0) {
                if (rssi != 0 || snr != 0.0f) {
                    st.last_rssi = rssi;
                    st.last_snr = snr;
                    st.hops = hops;
                }
                st.last_heard_time = t_str;
                if (ep > st.last_heard_epoch) st.last_heard_epoch = ep;
                st.msg_count++;
                std::sort(nicks.begin(), nicks.end(), [](const auto& a, const auto& b) {
                    if (a.last_heard_epoch != b.last_heard_epoch) return a.last_heard_epoch > b.last_heard_epoch;
                    return a.msg_count > b.msg_count;
                });
                return;
            }
        }
        nicks.push_back(HeardStation{call, rssi, snr, hops, t_str, ep, 1});
        std::sort(nicks.begin(), nicks.end(), [](const auto& a, const auto& b) {
            if (a.last_heard_epoch != b.last_heard_epoch) return a.last_heard_epoch > b.last_heard_epoch;
            return a.msg_count > b.msg_count;
        });
    }

    void ensure_mentioned_station(const std::string& call, const std::string& t_str = "") {
        if (call.empty() || call == "*" || call == "SYSTEM" || call == "RADIO") return;
        for (const auto& st : nicks) {
            if (_stricmp(st.callsign.c_str(), call.c_str()) == 0) return;
        }
        nicks.push_back(HeardStation{call, -80, 0.0f, 0, t_str.empty() ? now_hms_str() : t_str, 0, 0});
    }

    void scan_text_mentions(const std::string& txt, const std::string& t_str = "") {
        size_t pos = 0;
        while ((pos = txt.find("@[", pos)) != std::string::npos) {
            auto end = txt.find(']', pos + 2);
            if (end == std::string::npos) break;
            std::string m = txt.substr(pos + 2, end - (pos + 2));
            if (!m.empty() && m.size() <= 28) {
                ensure_mentioned_station(m, t_str);
            }
            pos = end + 1;
        }
    }
};

struct DiscoveredChannel {
    std::string name;
    uint8_t channel_hash = 0;
    std::string psk_hex;
    size_t msg_count = 0;
    bool has_rf = false;
    int16_t last_rssi = -80;
    float last_snr = 0.0f;
    bool mentioned_only = true;
    uint64_t last_activity_epoch = 0;
};

enum class ActiveInputTarget {
    ChatComposer,
    SnifferFilter,
    NicklistFilter,
    CallsignEditor
};

enum class DockSide {
    Left,
    Right
};

enum ModuleId : int {
    MOD_JOINED_CHANNELS = 0,
    MOD_RAINBOW_SNIFFER = 1,
    MOD_PUBLIC_MESH     = 2,
    MOD_HW_SLOTS        = 3
};

struct ModuleColumn {
    std::vector<int> modules;   // ModuleId values (0..3), up to 4 stacked vertically
    std::vector<float> weights; // Relative height weights for each stacked module
    float width = 275.0f;
};

struct VertSplitterHit {
    D2D1_RECT_F rect{};
    int col_idx = -1;
    bool invert_delta = false; // true for right-side columns where dragging left widens the column
};

struct HorizSplitterHit {
    D2D1_RECT_F rect{};
    int col_idx = -1;
    int row_idx = -1; // splitter between row_idx and row_idx + 1
};

enum class DraggingSplitter {
    None,
    ColumnWidth,
    ModuleHeight
};

struct HitRect {
    D2D1_RECT_F rect{};
    int id = 0;
    std::string payload;
};

// ============================================================================
struct MeshSystemNode {
    std::array<uint8_t, 32> pub_key{};
    std::string name;
    uint8_t contact_type = 1; // 1=Chat, 2=Repeater, 3=Room, 4=Sensor
    int8_t out_path_len = -1;
    uint32_t last_advert_ts = 0;
    int32_t lat_e6 = 0;
    int32_t lon_e6 = 0;
    bool heard_live_rf = false;
    int16_t live_rssi = -80;
    float live_snr = 0.0f;
    uint8_t live_hops = 0;
    std::deque<float> snr_hist; // rolling per-node SNR history for sparkline
    std::string last_status_summary; // latest repeater status/CLI summary
};

// ============================================================================
// Snooping Toolkit Records
// ============================================================================
enum class CenterView { Chat, Packets, Adverts, Paths, Map };
enum class PacketFilter { All, RfOnly, Companion, TxOnly };

struct RawFrameRecord {
    uint64_t seq = 0;
    std::string time;
    bool tx = false;
    uint8_t code = 0;
    bool is_rf = false;          // PUSH_LOG_RX_DATA / PUSH_RAW_DATA over-the-air frame
    int16_t rssi = 0;
    float snr = 0.0f;
    std::vector<uint8_t> bytes;
    std::string summary;
    std::vector<std::string> details;
};

struct AdvertRecord {
    uint64_t seq = 0;
    std::string time;
    std::string name;
    std::string key_prefix;
    uint8_t contact_type = 0;
    int32_t lat_e6 = 0;
    int32_t lon_e6 = 0;
    bool has_rf = false;
    int16_t rssi = 0;
    float snr = 0.0f;
    uint8_t hops = 0;
    std::string path;
    std::string source;          // "RF ADVERT" / "PUSH_NEW_ADVERT" / "PUSH_ADVERT"
};

struct PathRecord {
    uint64_t seq = 0;
    std::string time;
    std::string what;            // e.g. "GRP_TXT #emcomm", "ADVERT KK7XYZ"
    std::string route;
    std::string origin;          // origin name if known (adverts / decrypted senders)
    std::vector<PathHop> hops;
    int16_t rssi = 0;
    float snr = 0.0f;
};

struct RelayStat {
    std::string label;
    size_t relay_count = 0;      // times seen anywhere in a path
    size_t last_hop_count = 0;   // times it was the final hop that delivered to us
    float best_snr = -99.0f;     // best SNR when it was our last hop
};

// ============================================================================
// Main GUI Application State
// ============================================================================
class MeshGuiApp {
public:
    static constexpr const char* CONSOLE_TAB = "*Console*";
    static constexpr const char* ALL_HEARD_TAB = "All Heard Members";

    std::string callsign = "N0CALL";
    std::string connected_port;
    std::string transport_label = "Scanning USB Serial...";
    bool transport_connected = false;
    bool is_sim = false;
    uint16_t battery_mv = 0;
    uint8_t battery_pct = 0;
    bool has_last_rf = false;
    int16_t last_rssi = -80;
    float last_snr = 0.0f;
    std::string device_model = "MeshCore Companion";
    std::string firmware_ver;
    std::string build_date;
    uint32_t radio_freq_khz = 910525;
    uint32_t radio_bw_hz = 62500;
    uint8_t radio_sf = 7;
    uint8_t radio_cr = 5;
    std::deque<std::pair<uint64_t, double>> rf_airtime_window; // (epoch_sec, airtime_ms) over 60s window
    uint8_t radio_tx_dbm = 22;

    std::deque<float> snr_history; // Rolling 40-sample SNR sparkline

    std::vector<ChannelTab> tabs;
    size_t active_tab_idx = 1; // Default to `1: Public`
    std::map<std::string, ChannelTab> background_buffers;

    std::vector<DiscoveredChannel> discovered;
    int sniffer_scroll = 0;
    bool show_all_rainbow_in_sniffer = false;
    std::string sniffer_filter;

    // Public MeshCore System Nodes (Repeaters, Room Servers, Companion Stations)
    std::vector<MeshSystemNode> mesh_nodes;
    int nodes_scroll = 0;
    bool show_room_only_in_nodes = false;
    size_t repeater_count = 0;
    size_t room_server_count = 0;
    size_t chat_node_count = 0;

    struct RecentDedup {
        std::string key;
        uint64_t epoch_sec;
    };
    std::deque<RecentDedup> recent_dedup;

    RainbowTable rainbow;
    HardwareSlotTable hw_slots;
    TransportManager transport;

    std::string chat_input;
    size_t chat_cursor = 0;
    std::vector<std::string> cmd_history;
    int history_idx = -1;
    std::string selected_station;
    std::string replying_to_user;

    // Pending 1-Bit Tactical Monochrome Bitmap attachment state (48x30, 180B packed, 181B opcode 0x49)
    bool pending_image_ready = false;
    std::array<uint8_t, MONO_IMG_BYTES> pending_image_bits{};
    std::string pending_image_name;
    std::mutex pending_image_mtx;
    bool async_img_done = false;
    bool async_img_ok = false;
    std::array<uint8_t, MONO_IMG_BYTES> async_img_bits{};
    std::string async_img_error;
    std::string async_img_filename;

    ActiveInputTarget active_input = ActiveInputTarget::ChatComposer;
    bool serial_dropdown_open = false;
    bool ble_dropdown_open = false;
    std::vector<DetectedSerialPort> cached_ports;
    std::vector<DetectedBleDevice> cached_ble_devices;
    uint64_t copy_psk_feedback_until = 0;

    // Theme & 4-Column Drag-and-Drop Module Layout State
    // Cols 0..1 are on the Left of the main chat window (up to 2 wide, up to 4 stacked each)
    // Cols 2..3 are on the Right of the main chat window (up to 2 wide, up to 4 stacked each)
    bool light_mode = false;
    int mobile_tab = 0; // 0=Chat, 1=Nodes, 2=#Sniffer, 3=#Channels, 4=Radio
    int show_room_nicklist_override = 0; // 0=auto (open on desktop, toggleable on mobile), 1=open, -1=closed
    std::array<ModuleColumn, 4> module_cols;

    int dragging_module = -1;       // -1 if not dragging a module, else 0..3
    bool module_drag_active = false;
    bool drop_target_valid = false;
    int drop_target_col = 0;        // 0..3 (or 10/11 for inserting a new outer/inner col on Left, 20/21 on Right)
    int drop_target_row = 0;        // 0..4 vertical insertion slot
    D2D1_RECT_F drop_preview_rect{};

    DraggingSplitter dragging_splitter = DraggingSplitter::None;
    int active_splitter_col = -1;
    int active_splitter_row = -1;
    bool active_splitter_invert = false;
    POINT drag_start_mouse{0, 0};
    float drag_start_val = 0.0f;
    float drag_start_val2 = 0.0f;

    std::vector<VertSplitterHit> vert_splitter_hits;
    std::vector<HorizSplitterHit> horiz_splitter_hits;
    std::vector<HitRect> module_header_hits;

    // Clickable hit regions populated every frame
    std::vector<HitRect> tab_hits;
    std::vector<HitRect> tab_close_hits;
    std::vector<HitRect> joined_hits;
    std::vector<HitRect> discovered_hits;
    std::vector<HitRect> button_hits;
    std::vector<HitRect> chat_hashtag_hits;
    std::vector<HitRect> chat_user_hits;
    std::vector<HitRect> nicklist_hits;
    std::vector<HitRect> station_hits;
    std::vector<HitRect> slot_hits;
    std::vector<HitRect> serial_menu_hits;
    std::vector<HitRect> ble_menu_hits;

    D2D1_RECT_F chat_view_rect{};
    D2D1_RECT_F nick_view_rect{};
    int nicklist_scroll = 0;
    std::string nicklist_filter;
    D2D1_RECT_F sniffer_view_rect{};
    D2D1_RECT_F nodes_view_rect{};
    POINT mouse_pt{0, 0};

    void init_default_module_layout() {
        for (auto& c : module_cols) {
            c.modules.clear();
            c.weights.clear();
            c.width = 275.0f;
        }
        module_cols[0].modules = {MOD_JOINED_CHANNELS, MOD_RAINBOW_SNIFFER};
        module_cols[0].weights = {0.38f, 0.62f};
        module_cols[0].width   = 290.0f;

        module_cols[2].modules = {MOD_PUBLIC_MESH, MOD_HW_SLOTS};
        module_cols[2].weights = {0.56f, 0.44f};
        module_cols[2].width   = 290.0f;
    }

    void ensure_column_weights(ModuleColumn& c) {
        if (c.weights.size() != c.modules.size()) {
            c.weights.assign(c.modules.size(), 1.0f);
        }
        float sum = 0.0f;
        for (float& w : c.weights) {
            w = std::max(0.12f, w);
            sum += w;
        }
        if (sum > 0.0f) {
            for (float& w : c.weights) w /= sum;
        }
    }

    // Moves module `mod_id` (0..3) into `target_col` (0..3, or 10=LeftOuterNew, 11=LeftInnerNew, 20=RightInnerNew, 21=RightOuterNew) at `target_row`
    void apply_module_drop(int mod_id, int target_col, int target_row) {
        if (mod_id < 0 || mod_id > 3) return;

        // Remove mod_id from its current column
        for (int c = 0; c < 4; ++c) {
            auto& mvec = module_cols[c].modules;
            auto& wvec = module_cols[c].weights;
            for (size_t i = 0; i < mvec.size(); ++i) {
                if (mvec[i] == mod_id) {
                    mvec.erase(mvec.begin() + static_cast<ptrdiff_t>(i));
                    if (i < wvec.size()) wvec.erase(wvec.begin() + static_cast<ptrdiff_t>(i));
                    ensure_column_weights(module_cols[c]);
                    break;
                }
            }
        }

        // Handle special "split into 2 columns on Left (10/11) or Right (20/21)" targets
        if (target_col == 10) { // New outer-left column (col 0), shift existing left column to col 1 if col 0 was occupied
            if (!module_cols[0].modules.empty() && module_cols[1].modules.empty()) {
                module_cols[1] = std::move(module_cols[0]);
                module_cols[0] = ModuleColumn{};
            }
            if (module_cols[0].width > 250.0f) module_cols[0].width = 245.0f;
            if (module_cols[1].width > 250.0f) module_cols[1].width = 245.0f;
            target_col = 0;
            target_row = 0;
        } else if (target_col == 11) { // New inner-left column (col 1), shift existing left column to col 0 if col 1 was occupied
            if (!module_cols[1].modules.empty() && module_cols[0].modules.empty()) {
                module_cols[0] = std::move(module_cols[1]);
                module_cols[1] = ModuleColumn{};
            }
            if (module_cols[0].width > 250.0f) module_cols[0].width = 245.0f;
            if (module_cols[1].width > 250.0f) module_cols[1].width = 245.0f;
            target_col = 1;
            target_row = 0;
        } else if (target_col == 20) { // New inner-right column (col 2), shift existing right column to col 3 if col 2 was occupied
            if (!module_cols[2].modules.empty() && module_cols[3].modules.empty()) {
                module_cols[3] = std::move(module_cols[2]);
                module_cols[2] = ModuleColumn{};
            }
            if (module_cols[2].width > 250.0f) module_cols[2].width = 245.0f;
            if (module_cols[3].width > 250.0f) module_cols[3].width = 245.0f;
            target_col = 2;
            target_row = 0;
        } else if (target_col == 21) { // New outer-right column (col 3), shift existing right column to col 2 if col 3 was occupied
            if (!module_cols[3].modules.empty() && module_cols[2].modules.empty()) {
                module_cols[2] = std::move(module_cols[3]);
                module_cols[3] = ModuleColumn{};
            }
            if (module_cols[2].width > 250.0f) module_cols[2].width = 245.0f;
            if (module_cols[3].width > 250.0f) module_cols[3].width = 245.0f;
            target_col = 3;
            target_row = 0;
        }

        target_col = std::clamp(target_col, 0, 3);
        auto& dst = module_cols[target_col];
        int ins_idx = std::clamp(target_row, 0, static_cast<int>(dst.modules.size()));
        dst.modules.insert(dst.modules.begin() + ins_idx, mod_id);
        dst.weights.assign(dst.modules.size(), 1.0f / static_cast<float>(dst.modules.size()));
        ensure_column_weights(dst);

        if (module_cols[0].modules.empty() && !module_cols[1].modules.empty()) {
            module_cols[0] = std::move(module_cols[1]);
            module_cols[1] = ModuleColumn{};
        }
        if (module_cols[2].modules.empty() && !module_cols[3].modules.empty()) {
            module_cols[2] = std::move(module_cols[3]);
            module_cols[3] = ModuleColumn{};
        }
        state_dirty = true;
    }

    static const char* module_title(int mod_id) {
        switch (mod_id) {
            case MOD_JOINED_CHANNELS: return "JOINED CHANNELS";
            case MOD_RAINBOW_SNIFFER: return "256-BUCKET RAINBOW SNIFFER";
            case MOD_PUBLIC_MESH:     return "PUBLIC MESH SYSTEM";
            case MOD_HW_SLOTS:        return "RADIO HARDWARE SLOTS";
            default:                  return "MODULE";
        }
    }

    // ---- Snooping toolkit state (Packet Inspector / Adverts / Paths / Exports) ----
    static constexpr size_t MAX_FRAMES = 4000;
    static constexpr size_t MAX_ADVERTS = 2000;
    static constexpr size_t MAX_PATHS = 2000;
    CenterView center_view = CenterView::Chat;
    PacketFilter packet_filter = PacketFilter::All;
    std::deque<RawFrameRecord> raw_frames;
    std::deque<AdvertRecord> adverts;
    std::deque<PathRecord> path_traces;
    std::map<std::string, RelayStat> relay_stats;
    uint64_t next_seq = 1;
    uint64_t selected_frame_seq = 0;
    int packets_scroll = 0;   // rows scrolled back from newest (0 = follow live)
    int adverts_scroll = 0;
    int paths_scroll = 0;
    size_t rx_frame_count = 0;
    size_t tx_frame_count = 0;
    size_t rf_frame_count = 0;
    int32_t self_lat_e6 = 0;
    int32_t self_lon_e6 = 0;
    D2D1_RECT_F tool_view_rect{};
    std::vector<HitRect> packet_row_hits;
    bool recording_session = false;
    std::wstring record_file_path;
    std::string record_file_label;
    size_t recorded_frame_count = 0;

    // ---- Live Map view ----
    double map_nx = 0.5, map_ny = 0.5;   // camera center, normalized Web-Mercator [0..1]
    int map_zoom = 3;
    bool map_fitted = false;             // auto-fit once when GPS nodes first appear
    bool map_dragging = false;
    bool map_drag_moved = false;
    POINT map_drag_start{};
    double map_drag_nx = 0.0, map_drag_ny = 0.0;
    bool map_repeaters_only = false;
    bool map_show_links = true;
    int map_style = 0;                   // 0 = dark, 1 = light
    std::string map_selected_key;        // 6-byte pubkey hex of selected node
    D2D1_RECT_F map_rect{};
    D2D1_RECT_F map_card_rect{};
    std::vector<HitRect> map_pin_hits;

    static std::string node_key_hex(const MeshSystemNode& n) { return hex_string(n.pub_key.data(), 6, ""); }

    const MeshSystemNode* find_node_by_key_hex(const std::string& k) const {
        if (k.empty()) return nullptr;
        for (const auto& n : mesh_nodes)
            if (node_key_hex(n) == k) return &n;
        return nullptr;
    }

    const MeshSystemNode* find_node_by_name(const std::string& name) const {
        for (const auto& n : mesh_nodes)
            if (n.name == name) return &n;
        return nullptr;
    }

    void map_zoom_at(POINT p, int dz) {
        int nz = std::clamp(map_zoom + dz, 2, 18);
        if (nz == map_zoom) return;
        double cx = (map_rect.left + map_rect.right) / 2.0, cy = (map_rect.top + map_rect.bottom) / 2.0;
        double ws_old = 256.0 * std::ldexp(1.0, map_zoom), ws_new = 256.0 * std::ldexp(1.0, nz);
        double ux = map_nx + (p.x - cx) / ws_old, uy = map_ny + (p.y - cy) / ws_old;
        map_nx = ux - (p.x - cx) / ws_new;
        map_ny = std::clamp(uy - (p.y - cy) / ws_new, 0.0, 1.0);
        map_nx -= std::floor(map_nx);
        map_zoom = nz;
    }

    // Fit camera to nodes with GPS (trims 5% outliers each side so one bogus position doesn't zoom out to the planet)
    void map_fit() {
        std::vector<double> xs, ys;
        for (const auto& n : mesh_nodes) {
            if (n.lat_e6 == 0 && n.lon_e6 == 0) continue;
            if (map_repeaters_only && n.contact_type != 2) continue;
            xs.push_back(lon_to_nx(n.lon_e6 / 1e6));
            ys.push_back(lat_to_ny(n.lat_e6 / 1e6));
        }
        if (self_lat_e6 || self_lon_e6) {
            xs.push_back(lon_to_nx(self_lon_e6 / 1e6));
            ys.push_back(lat_to_ny(self_lat_e6 / 1e6));
        }
        if (xs.empty()) return;
        std::sort(xs.begin(), xs.end());
        std::sort(ys.begin(), ys.end());
        size_t trim = xs.size() >= 20 ? xs.size() / 20 : 0;
        double x0 = xs[trim], x1 = xs[xs.size() - 1 - trim];
        double y0 = ys[trim], y1 = ys[ys.size() - 1 - trim];
        map_nx = (x0 + x1) / 2.0;
        map_ny = (y0 + y1) / 2.0;
        double w = std::max(100.0, static_cast<double>(map_rect.right - map_rect.left) - 80.0);
        double h = std::max(100.0, static_cast<double>(map_rect.bottom - map_rect.top) - 80.0);
        int z = 15;
        while (z > 2) {
            double ws = 256.0 * std::ldexp(1.0, z);
            if ((x1 - x0) * ws <= w && (y1 - y0) * ws <= h) break;
            --z;
        }
        map_zoom = z;
        map_fitted = true;
    }

    bool state_dirty = false;
    uint64_t last_state_save_epoch = 0;

    static std::wstring state_file_path() { return exe_dir() + L"meshcore_state.tsv"; }

    void save_state() {
        std::ofstream f(std::filesystem::path(state_file_path()), std::ios::binary | std::ios::trunc);
        if (!f) return;
        std::string active_name = (active_tab_idx < tabs.size()) ? tabs[active_tab_idx].name : PUBLIC_CHANNEL_NAME;
        f << "CFG\t" << tsv_escape(callsign)
          << '\t' << (light_mode ? 1 : 0)
          << '\t' << 1
          << '\t' << 0
          << '\t' << 1
          << '\t' << module_cols[0].width
          << '\t' << module_cols[2].width
          << '\t' << 0.38f
          << '\t' << 0.56f
          << '\t' << std::setprecision(10) << map_nx
          << '\t' << std::setprecision(10) << map_ny
          << '\t' << map_zoom
          << '\t' << (map_fitted ? 1 : 0)
          << '\t' << (map_repeaters_only ? 1 : 0)
          << '\t' << (map_show_links ? 1 : 0)
          << '\t' << map_style
          << '\t' << self_lat_e6
          << '\t' << self_lon_e6
          << '\t' << tsv_escape(active_name) << '\n';

        for (int c = 0; c < 4; ++c) {
            std::ostringstream mods_s, weights_s;
            for (size_t i = 0; i < module_cols[c].modules.size(); ++i) {
                if (i > 0) mods_s << ',';
                mods_s << module_cols[c].modules[i];
            }
            for (size_t i = 0; i < module_cols[c].weights.size(); ++i) {
                if (i > 0) weights_s << ',';
                weights_s << std::fixed << std::setprecision(3) << module_cols[c].weights[i];
            }
            f << "LAYOUT\t" << c
              << '\t' << module_cols[c].width
              << '\t' << mods_s.str()
              << '\t' << weights_s.str() << '\n';
        }

        for (size_t i = 2; i < tabs.size(); ++i) {
            f << "TAB\t" << tsv_escape(tabs[i].name) << '\n';
        }
        for (const auto& d : discovered) {
            f << "DISC\t" << tsv_escape(d.name)
              << '\t' << d.msg_count
              << '\t' << (d.has_rf ? 1 : 0)
              << '\t' << d.last_rssi
              << '\t' << d.last_snr
              << '\t' << (d.mentioned_only ? 1 : 0)
              << '\t' << d.last_activity_epoch << '\n';
        }
        for (const auto& n : mesh_nodes) {
            f << "NODE\t" << hex_string(n.pub_key.data(), 32, "")
              << '\t' << tsv_escape(n.name)
              << '\t' << static_cast<int>(n.contact_type)
              << '\t' << static_cast<int>(n.out_path_len)
              << '\t' << n.last_advert_ts
              << '\t' << n.lat_e6
              << '\t' << n.lon_e6 << '\n';
        }
        for (size_t ti = 1; ti < tabs.size(); ++ti) {
            const auto& tab = tabs[ti];
            std::vector<const ChatMessage*> to_save;
            for (auto it = tab.messages.rbegin(); it != tab.messages.rend() && to_save.size() < 60; ++it) {
                if (!it->is_system) to_save.push_back(&*it);
            }
            for (auto it = to_save.rbegin(); it != to_save.rend(); ++it) {
                const ChatMessage& m = **it;
                f << "MSG\t" << tsv_escape(tab.name)
                  << '\t' << tsv_escape(m.timestamp)
                  << '\t' << tsv_escape(m.sender)
                  << '\t' << (m.has_rf ? 1 : 0)
                  << '\t' << m.rssi
                  << '\t' << m.snr
                  << '\t' << static_cast<int>(m.hops)
                  << '\t' << tsv_escape(m.text)
                  << '\t' << (m.is_mention ? 1 : 0)
                  << '\t' << (m.is_self ? 1 : 0)
                  << '\t' << (m.has_mono_image ? hex_string(m.mono_image.data(), MONO_IMG_BYTES, "") : "") << '\n';
            }
        }
        state_dirty = false;
        last_state_save_epoch = now_epoch_secs();
    }

    void load_state() {
        std::ifstream f(std::filesystem::path(state_file_path()), std::ios::binary);
        if (!f) return;
        std::string line;
        std::string target_active = PUBLIC_CHANNEL_NAME;
        size_t loaded_msgs = 0;
        bool has_layout_rows = false;
        std::array<ModuleColumn, 4> loaded_cols;
        for (auto& c : loaded_cols) c.width = 275.0f;

        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto cols = split_tsv(line);
            if (cols.empty()) continue;
            const std::string& tag = cols[0];
            try {
                if (tag == "CFG" && cols.size() >= 20) {
                    if (!cols[1].empty()) callsign = cols[1];
                    map_nx = std::clamp(std::stod(cols[10]), 0.0, 1.0);
                    map_ny = std::clamp(std::stod(cols[11]), 0.0, 1.0);
                    map_zoom = std::clamp(std::stoi(cols[12]), 2, 18);
                    map_fitted = std::stoi(cols[13]) != 0;
                    map_repeaters_only = std::stoi(cols[14]) != 0;
                    map_show_links = std::stoi(cols[15]) != 0;
                    map_style = std::clamp(std::stoi(cols[16]), 0, 1);
                    self_lat_e6 = std::stoi(cols[17]);
                    self_lon_e6 = std::stoi(cols[18]);
                    target_active = cols[19];
                    // If LAYOUT rows exist in this file, cols[2] is light_mode
                    light_mode = (std::stoi(cols[2]) != 0 && std::stoi(cols[3]) == 1 && std::stof(cols[8]) == 0.38f);
                } else if (tag == "LAYOUT" && cols.size() >= 5) {
                    has_layout_rows = true;
                    int cidx = std::clamp(std::stoi(cols[1]), 0, 3);
                    loaded_cols[cidx].width = std::clamp(std::stof(cols[2]), 180.0f, 600.0f);
                    loaded_cols[cidx].modules.clear();
                    loaded_cols[cidx].weights.clear();
                    if (!cols[3].empty()) {
                        std::stringstream ss(cols[3]);
                        std::string item;
                        while (std::getline(ss, item, ',')) {
                            if (!item.empty()) {
                                int mid = std::stoi(item);
                                if (mid >= 0 && mid <= 3) loaded_cols[cidx].modules.push_back(mid);
                            }
                        }
                    }
                    if (!cols[4].empty()) {
                        std::stringstream ss(cols[4]);
                        std::string item;
                        while (std::getline(ss, item, ',')) {
                            if (!item.empty()) {
                                loaded_cols[cidx].weights.push_back(std::stof(item));
                            }
                        }
                    }
                    ensure_column_weights(loaded_cols[cidx]);
                } else if (tag == "TAB" && cols.size() >= 2) {
                    std::string ch = trim_str(cols[1]);
                    if (!ch.empty() && ch != CONSOLE_TAB && ch != ALL_HEARD_TAB &&
                        _stricmp(ch.c_str(), ALL_HEARD_TAB) != 0 &&
                        normalize_channel_name(ch) != PUBLIC_CHANNEL_NAME) {
                        bool exists = false;
                        for (const auto& t : tabs) {
                            if (_stricmp(t.name.c_str(), ch.c_str()) == 0 ||
                                normalize_channel_name(t.name) == normalize_channel_name(ch)) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) {
                            if (ch[0] == '#') {
                                auto cand = rainbow.add_channel(ch);
                                tabs.emplace_back(cand.name);
                            } else {
                                tabs.emplace_back(ch);
                            }
                        }
                    }
                } else if (tag == "DISC" && cols.size() >= 8) {
                    DiscoveredChannel d;
                    d.name = trim_str(cols[1]);
                    if (!d.name.empty() && normalize_channel_name(d.name) != PUBLIC_CHANNEL_NAME) {
                        if (d.name[0] != '#') d.name = "#" + d.name;
                        auto psk = derive_psk(d.name);
                        d.channel_hash = compute_channel_hash(psk);
                        d.psk_hex = psk_to_hex(psk);
                        d.msg_count = static_cast<size_t>(std::stoull(cols[2]));
                        d.has_rf = std::stoi(cols[3]) != 0;
                        d.last_rssi = static_cast<int16_t>(std::stoi(cols[4]));
                        d.last_snr = std::stof(cols[5]);
                        d.mentioned_only = std::stoi(cols[6]) != 0;
                        d.last_activity_epoch = std::stoull(cols[7]);
                        rainbow.add_channel(d.name);
                        bool dup = false;
                        for (const auto& ex : discovered) {
                            if (normalize_channel_name(ex.name) == normalize_channel_name(d.name)) { dup = true; break; }
                        }
                        if (!dup) discovered.push_back(std::move(d));
                    }
                } else if (tag == "NODE" && cols.size() >= 8) {
                    MeshSystemNode n;
                    if (hex_to_bytes(cols[1], n.pub_key.data(), 32) && !cols[2].empty()) {
                        n.name = cols[2];
                        n.contact_type = static_cast<uint8_t>(std::stoi(cols[3]));
                        n.out_path_len = static_cast<int8_t>(std::stoi(cols[4]));
                        n.last_advert_ts = static_cast<uint32_t>(std::stoul(cols[5]));
                        n.lat_e6 = std::stoi(cols[6]);
                        n.lon_e6 = std::stoi(cols[7]);
                        mesh_nodes.push_back(std::move(n));
                    }
                } else if (tag == "MSG" && cols.size() >= 11) {
                    const std::string& tname = cols[1];
                    // Filter out legacy mirrored #hashtag notices that were previously saved into Public
                    if (normalize_channel_name(tname) == PUBLIC_CHANNEL_NAME &&
                        cols[3].find(" @ #") != std::string::npos) {
                        continue;
                    }
                    for (auto& tab : tabs) {
                        if (_stricmp(tab.name.c_str(), tname.c_str()) == 0 ||
                            normalize_channel_name(tab.name) == normalize_channel_name(tname)) {
                            ChatMessage m;
                            m.timestamp = cols[2];
                            m.sender = cols[3];
                            m.has_rf = std::stoi(cols[4]) != 0;
                            m.rssi = static_cast<int16_t>(std::stoi(cols[5]));
                            m.snr = std::stof(cols[6]);
                            m.hops = static_cast<uint8_t>(std::stoi(cols[7]));
                            m.text = cols[8];
                            m.is_mention = std::stoi(cols[9]) != 0;
                            m.is_self = std::stoi(cols[10]) != 0;
                            if (cols.size() >= 12 && cols[11].size() == MONO_IMG_BYTES * 2) {
                                if (hex_to_bytes(cols[11], m.mono_image.data(), MONO_IMG_BYTES)) {
                                    m.has_mono_image = true;
                                }
                            }
                            if (!m.is_system && !m.sender.empty()) {
                                tab.update_station(m.sender, m.rssi, m.snr, m.hops, m.timestamp);
                                tab.scan_text_mentions(m.text, m.timestamp);
                            }
                            tab.messages.push_back(std::move(m));
                            ++loaded_msgs;
                            break;
                        }
                    }
                }
            } catch (...) {}
        }

        if (has_layout_rows) {
            // Verify all 4 modules (0..3) are present exactly once across loaded_cols
            std::array<bool, 4> seen{false, false, false, false};
            for (int c = 0; c < 4; ++c) {
                std::vector<int> clean_mods;
                for (int m : loaded_cols[c].modules) {
                    if (m >= 0 && m <= 3 && !seen[m]) {
                        seen[m] = true;
                        clean_mods.push_back(m);
                    }
                }
                loaded_cols[c].modules = std::move(clean_mods);
                ensure_column_weights(loaded_cols[c]);
            }
            for (int m = 0; m < 4; ++m) {
                if (!seen[m]) {
                    int target_c = (m < 2) ? 0 : 2;
                    loaded_cols[target_c].modules.push_back(m);
                    ensure_column_weights(loaded_cols[target_c]);
                }
            }
            module_cols = std::move(loaded_cols);
        }

        sort_and_recount_mesh_nodes();
        sort_discovered();
        if (tabs.size() > 1 && !mesh_nodes.empty()) {
            for (const auto& nd : mesh_nodes) {
                if (!nd.name.empty()) {
                    tabs[1].ensure_mentioned_station(nd.name);
                }
            }
        }
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (_stricmp(tabs[i].name.c_str(), target_active.c_str()) == 0 ||
                normalize_channel_name(tabs[i].name) == normalize_channel_name(target_active)) {
                active_tab_idx = i;
                break;
            }
        }
        if (!mesh_nodes.empty() || loaded_msgs > 0) {
            log_console("[PERSISTENCE] Restored " + std::to_string(mesh_nodes.size()) + " cached nodes, " +
                        std::to_string(discovered.size()) + " discovered channels, and " +
                        std::to_string(loaded_msgs) + " chat messages from meshcore_state.tsv.");
        }
    }

    MeshGuiApp() {
        for (int i = 0; i < 36; ++i) snr_history.push_back(0.0f);
        init_default_module_layout();

        // Load wordlist from exe directory as well
        rainbow.load_wordlist(from_wide(exe_dir() + L"wordlist.txt"));

        ChannelTab console_tab(CONSOLE_TAB);
        ChannelTab public_tab(PUBLIC_CHANNEL_NAME);

        std::string t = now_hms_str();
        console_tab.messages.push_back(ChatMessage{
            t, "SYSTEM", false, 0, 0.0f, 0,
            "MeshCore++ Native C++20 Direct2D/DirectWrite SDR/IRC Client & 256-Bucket Sniffer initialized (" +
                std::to_string(rainbow.total_candidates()) + " channels across 256 SHA-256 buckets).",
            true, false, false
        });

        tabs.push_back(std::move(console_tab));
        tabs.push_back(std::move(public_tab));
        active_tab_idx = 1; // Focus Public tab initially

        load_state();

        // Start idle: the operator picks a radio from the USB Serial dropdown (or BLE / RF Sim).
        cached_ports = detect_serial_ports();
        transport_label = "DISCONNECTED - click USB Serial to pick a radio";
        std::string port_list;
        for (const auto& p : cached_ports) {
            if (!port_list.empty()) port_list += ", ";
            port_list += p.port_name + (p.is_meshcore_candidate ? "*" : "");
        }
        log_public_system("Ready. Click [USB Serial v] to choose a radio" +
                          (port_list.empty() ? std::string(" (no COM ports detected yet).")
                                             : (" - detected: " + port_list + "  (* = likely MeshCore)")));
    }

    void tick() {
        auto events = transport.drain_events();
        for (auto& ev : events) {
            handle_event(ev);
        }
        bool auto_send_now = false;
        {
            std::lock_guard<std::mutex> lk(pending_image_mtx);
            if (async_img_done) {
                async_img_done = false;
                if (async_img_ok) {
                    pending_image_bits = async_img_bits;
                    pending_image_name = async_img_filename;
                    pending_image_ready = true;
                    auto_send_now = true;
                    log_console("[IMAGE ENCODER] Dithered '" + pending_image_name +
                                "' -> 48x30 1-bit Floyd-Steinberg (180B payload / 181B opcode 0x49). Auto-transmitting...");
                } else {
                    log_active_tab("[IMAGE ENCODER ERROR] " + async_img_error);
                }
            }
        }
        if (auto_send_now) {
            confirm_send_pending_image();
        }
        if (state_dirty && now_epoch_secs() >= last_state_save_epoch + 8) {
            save_state();
        }
    }

    static void trim_tab_messages(ChannelTab& t, size_t max_msgs = 1000) {
        if (t.messages.size() > max_msgs + 100) {
            size_t drop = t.messages.size() - max_msgs;
            t.messages.erase(t.messages.begin(), t.messages.begin() + static_cast<std::ptrdiff_t>(drop));
        }
    }

    void handle_event(const MeshEvent& ev) {
        switch (ev.type) {
            case EventType::TransportStatus:
                transport_label = ev.status_label;
                transport_connected = ev.connected;
                is_sim = ev.is_sim;
                if (ev.connected && !ev.is_sim) {
                    hw_slots.mark_non_public_unsynced();
                }
                log_console("[STATUS] " + ev.status_label + (ev.connected ? " (connected)" : " (not connected)"));
                break;
            case EventType::ConsoleLog:
                log_console(ev.log_line);
                break;
            case EventType::SerialPortsListed:
                cached_ports = ev.ports;
                break;
            case EventType::BleDevicesListed:
                cached_ble_devices = ev.ble_devices;
                break;
            case EventType::RadioPacket:
                record_rx_frame(ev.packet);
                handle_packet(ev.packet);
                break;
            case EventType::RawTx:
                record_tx_frame(ev.tx_payload);
                break;
        }
    }

    void log_console(const std::string& text) {
        {
            static std::ofstream dbg_log(from_wide(exe_dir() + L"meshcore_session.log"), std::ios::trunc);
            static size_t dbg_lines = 0;
            if (++dbg_lines > 25000) {
                dbg_lines = 0;
                dbg_log.close();
                dbg_log.open(from_wide(exe_dir() + L"meshcore_session.log"), std::ios::trunc);
            }
            if (dbg_log) {
                dbg_log << now_hms_str() << " " << text << '\n';
                if ((dbg_lines & 0x1F) == 0) dbg_log.flush();
            }
        }
        if (tabs.empty()) return;
        tabs[0].messages.push_back(ChatMessage{
            now_hms_str(), "SYSTEM", false, 0, 0.0f, 0, text, true, false, false
        });
        trim_tab_messages(tabs[0], 800);
        if (active_tab_idx != 0) tabs[0].unread_count++;
    }

    void log_public_system(const std::string& text) {
        log_console(text);
        if (tabs.size() > 1) {
            tabs[1].messages.push_back(ChatMessage{
                now_hms_str(), "SYSTEM", false, 0, 0.0f, 0, text, true, false, false
            });
            trim_tab_messages(tabs[1], 1000);
        }
    }

    void log_active_tab(const std::string& text) {
        if (active_tab_idx >= tabs.size()) return;
        tabs[active_tab_idx].messages.push_back(ChatMessage{
            now_hms_str(), "*", false, 0, 0.0f, 0, text, true, false, false
        });
        trim_tab_messages(tabs[active_tab_idx], 1000);
    }

    void log_all_heard(const std::string& sender,
                       const std::string& text,
                       int16_t rssi = 0,
                       float snr = 0.0f,
                       uint8_t hops = 0,
                       bool is_sys = false,
                       bool has_rf = false) {
        (void)text;
        (void)has_rf;
        if (!is_sys && !sender.empty() && sender != "SYSTEM" && sender != "*" && tabs.size() > 1) {
            tabs[1].update_station(sender, rssi, snr, hops);
        }
    }

    std::string resolve_hop_path(uint8_t hops, uint8_t hash_size, const std::vector<uint8_t>& hop_bytes) const {
        if (hops == 0 || hop_bytes.empty()) return "Direct (0 hops)";
        std::ostringstream oss;
        oss << static_cast<int>(hops) << (hops == 1 ? " hop via " : " hops via ");
        size_t step = std::max<size_t>(1, hash_size);
        size_t count = 0;
        for (size_t off = 0; off + step <= hop_bytes.size(); off += step) {
            if (count++ > 0) oss << " -> ";
            std::string matched_name;
            for (const auto& n : mesh_nodes) {
                if (std::memcmp(n.pub_key.data(), hop_bytes.data() + off, step) == 0) {
                    matched_name = n.name;
                    break;
                }
            }
            if (!matched_name.empty()) {
                if (matched_name.size() > 18) matched_name = matched_name.substr(0, 18);
                oss << matched_name;
            } else {
                oss << std::hex << std::uppercase;
                for (size_t b = 0; b < step; ++b) {
                    oss << std::setw(2) << std::setfill('0') << static_cast<int>(hop_bytes[off + b]);
                }
                oss << std::dec;
            }
        }
        return oss.str();
    }

    void update_or_insert_mesh_node(const DecodedRadioPacket& pkt, bool is_live_rf) {
        if (pkt.node_name.empty()) return;
        for (auto& n : mesh_nodes) {
            if (n.pub_key == pkt.pub_key || (!pkt.node_name.empty() && n.name == pkt.node_name)) {
                n.name = pkt.node_name;
                if (pkt.contact_type != 0) n.contact_type = pkt.contact_type;
                n.out_path_len = pkt.out_path_len;
                if (pkt.timestamp > n.last_advert_ts) n.last_advert_ts = pkt.timestamp;
                if (pkt.lat_e6 != 0) n.lat_e6 = pkt.lat_e6;
                if (pkt.lon_e6 != 0) n.lon_e6 = pkt.lon_e6;
                if (is_live_rf) {
                    n.heard_live_rf = true;
                    n.live_rssi = pkt.rssi;
                    n.live_snr = pkt.snr;
                    n.live_hops = pkt.hops;
                    n.last_advert_ts = static_cast<uint32_t>(now_epoch_secs());
                    n.snr_hist.push_back(pkt.snr);
                    while (n.snr_hist.size() > 24) n.snr_hist.pop_front();
                }
                return;
            }
        }
        MeshSystemNode node;
        node.pub_key = pkt.pub_key;
        node.name = pkt.node_name;
        node.contact_type = (pkt.contact_type != 0) ? pkt.contact_type : 1;
        node.out_path_len = pkt.out_path_len;
        node.last_advert_ts = pkt.timestamp;
        node.lat_e6 = pkt.lat_e6;
        node.lon_e6 = pkt.lon_e6;
        if (is_live_rf) {
            node.heard_live_rf = true;
            node.live_rssi = pkt.rssi;
            node.live_snr = pkt.snr;
            node.live_hops = pkt.hops;
            node.last_advert_ts = static_cast<uint32_t>(now_epoch_secs());
            node.snr_hist.push_back(pkt.snr);
            while (node.snr_hist.size() > 24) node.snr_hist.pop_front();
        }
        mesh_nodes.push_back(std::move(node));
    }

    void record_node_snr_by_name(const std::string& name, int16_t rssi, float snr, uint8_t hops) {
        if (name.empty()) return;
        for (auto& n : mesh_nodes) {
            if (_stricmp(n.name.c_str(), name.c_str()) == 0) {
                n.heard_live_rf = true;
                n.live_rssi = rssi;
                n.live_snr = snr;
                n.live_hops = hops;
                n.snr_hist.push_back(snr);
                while (n.snr_hist.size() > 24) n.snr_hist.pop_front();
                break;
            }
        }
    }

    void sort_and_recount_mesh_nodes() {
        std::sort(mesh_nodes.begin(), mesh_nodes.end(), [](const MeshSystemNode& a, const MeshSystemNode& b) {
            if (a.heard_live_rf != b.heard_live_rf) return a.heard_live_rf > b.heard_live_rf;
            return a.last_advert_ts > b.last_advert_ts;
        });
        repeater_count = 0;
        room_server_count = 0;
        chat_node_count = 0;
        for (const auto& n : mesh_nodes) {
            if (n.contact_type == 2) ++repeater_count;
            else if (n.contact_type == 3) ++room_server_count;
            else ++chat_node_count;
        }
    }

    void open_radio_slot_tab(uint8_t slot_idx, const std::string& raw_name, const std::array<uint8_t, 16>& psk) {
        std::string trimmed = trim_str(raw_name);
        if (trimmed.empty()) return;
        std::string tab_name = (normalize_channel_name(trimmed) == PUBLIC_CHANNEL_NAME)
                                   ? PUBLIC_CHANNEL_NAME
                                   : trimmed;

        if (tab_name != PUBLIC_CHANNEL_NAME) {
            // Add/update discovered list so it's visible in the left sniffer dock with the exact radio PSK
            bool found = false;
            for (auto& d : discovered) {
                if (_stricmp(d.name.c_str(), tab_name.c_str()) == 0 ||
                    normalize_channel_name(d.name) == normalize_channel_name(tab_name)) {
                    d.name = tab_name;
                    d.channel_hash = compute_channel_hash(psk);
                    d.psk_hex = psk_to_hex(psk);
                    found = true;
                    break;
                }
            }
            if (!found) {
                DiscoveredChannel item;
                item.name = tab_name;
                item.channel_hash = compute_channel_hash(psk);
                item.psk_hex = psk_to_hex(psk);
                item.msg_count = 0;
                item.has_rf = false;
                item.mentioned_only = false;
                item.last_activity_epoch = now_epoch_secs();
                discovered.push_back(item);
            }
        }

        for (auto& t : tabs) {
            if (_stricmp(t.name.c_str(), tab_name.c_str()) == 0 ||
                normalize_channel_name(t.name) == normalize_channel_name(tab_name)) {
                t.name = tab_name;
                return;
            }
        }

        ChannelTab new_tab(tab_name);
        std::ostringstream oss;
        oss << "Loaded from Radio Hardware Slot " << static_cast<int>(slot_idx)
            << " (" << tab_name << ") | Bucket: 0x" << std::hex << std::uppercase
            << std::setw(2) << std::setfill('0') << static_cast<int>(compute_channel_hash(psk))
            << " | AES-128 PSK: " << psk_to_hex(psk);
        new_tab.messages.push_back(ChatMessage{
            now_hms_str(), "RADIO", false, 0, 0.0f, 0, oss.str(), true, false, false
        });
        tabs.push_back(std::move(new_tab));
    }

    void handle_packet(const DecodedRadioPacket& pkt) {
        switch (pkt.kind) {
            case RadioPacketKind::DeviceInfo: {
                hw_slots.mark_non_public_unsynced();
                device_model = pkt.model.empty() ? "MeshCore Radio" : pkt.model;
                firmware_ver = pkt.firmware;
                build_date = pkt.build_date;
                bool is_ble = (transport_label.rfind("BLE", 0) == 0);
                std::string port_lbl = is_ble ? "BLE NUS" : (connected_port.empty() ? "USB" : connected_port);
                transport_label = (is_ble ? "BLE: " : ("SERIAL: " + port_lbl + " • ")) + device_model + " (" + firmware_ver + ")";
                log_public_system("[HARDWARE CONNECTED] " + device_model + " • Firmware " + firmware_ver +
                                  (build_date.empty() ? "" : (" (" + build_date + ")")) + " on " + port_lbl);
                break;
            }
            case RadioPacketKind::SelfInfo: {
                if (!pkt.node_name.empty()) {
                    callsign = pkt.node_name;
                }
                if (pkt.freq_khz > 0) radio_freq_khz = pkt.freq_khz;
                if (pkt.bw_hz > 0) radio_bw_hz = pkt.bw_hz;
                if (pkt.sf > 0) radio_sf = pkt.sf;
                if (pkt.cr > 0) radio_cr = pkt.cr;
                if (pkt.tx_power_dbm > 0) radio_tx_dbm = pkt.tx_power_dbm;
                if (pkt.lat_e6 != 0 || pkt.lon_e6 != 0) {
                    self_lat_e6 = pkt.lat_e6;
                    self_lon_e6 = pkt.lon_e6;
                }

                char rf_buf[160];
                std::snprintf(rf_buf, sizeof(rf_buf),
                              "[RADIO SYSTEM] Node: %s | LoRa: %.3f MHz | BW: %.1f kHz | SF%u / CR%u | TX Power: %u dBm",
                              callsign.c_str(),
                              radio_freq_khz / 1000.0,
                              radio_bw_hz / 1000.0,
                              radio_sf, radio_cr, radio_tx_dbm);
                log_public_system(rf_buf);
                break;
            }
            case RadioPacketKind::Battery:
                battery_mv = pkt.battery_mv;
                battery_pct = pkt.battery_pct;
                break;
            case RadioPacketKind::ChannelInfo:
                hw_slots.update_from_radio(pkt.slot_idx, pkt.channel_name, pkt.psk);
                if (!pkt.channel_name.empty()) {
                    rainbow.add_channel_with_psk(pkt.channel_name, pkt.psk);
                    rainbow.add_channel(pkt.channel_name);
                    open_radio_slot_tab(pkt.slot_idx, pkt.channel_name, pkt.psk);
                    std::ostringstream oss;
                    oss << "[RADIO SLOT " << static_cast<int>(pkt.slot_idx) << "] Active Channel: "
                        << pkt.channel_name << " (Bucket 0x" << std::hex << std::uppercase
                        << std::setw(2) << std::setfill('0') << static_cast<int>(compute_channel_hash(pkt.psk))
                        << " | PSK: " << psk_to_hex(pkt.psk) << ")";
                    log_public_system(oss.str());
                }
                break;
            case RadioPacketKind::ContactsStart:
                break;
            case RadioPacketKind::ContactInfo:
                update_or_insert_mesh_node(pkt, false);
                state_dirty = true;
                if (mesh_nodes.size() % 50 == 0) {
                    sort_and_recount_mesh_nodes();
                }
                break;
            case RadioPacketKind::ContactsEnd: {
                sort_and_recount_mesh_nodes();
                save_state();
                std::ostringstream oss;
                oss << "[PUBLIC MESH SYSTEM] Synced " << mesh_nodes.size()
                    << " Public System Stations (" << repeater_count << " Repeaters, "
                    << room_server_count << " Room Servers, " << chat_node_count
                    << " Companion Nodes). Live 910.525 MHz RX Active!";
                log_public_system(oss.str());
                if (tabs.size() > 1) {
                    for (const auto& nd : mesh_nodes) {
                        if (!nd.name.empty()) {
                            tabs[1].ensure_mentioned_station(nd.name);
                        }
                    }
                }
                break;
            }
            case RadioPacketKind::ChannelMsgRecv: {
                record_rf_metrics(pkt.rssi, pkt.snr, 48);
                record_node_snr_by_name(pkt.sender, pkt.rssi, pkt.snr, pkt.hops);
                std::string ch = hw_slots.channel_for_slot(pkt.slot_idx);
                ingest_decrypted_message(ch, pkt.sender, pkt.text, pkt.rssi, pkt.snr, pkt.hops, "", nullptr,
                                         pkt.is_mono_img ? &pkt.mono_img_bytes : nullptr, pkt.timestamp);
                break;
            }
            case RadioPacketKind::DirectMsgRecv: {
                record_rf_metrics(pkt.rssi, pkt.snr, 48);
                const auto* nd = find_node_by_prefix(pkt.pub_key.data(), 6);
                std::string sender_name = (nd && !nd->name.empty())
                                              ? nd->name
                                              : hex_string(pkt.pub_key.data(), 6, "");
                record_node_snr_by_name(sender_name, pkt.rssi, pkt.snr, pkt.hops);
                ingest_direct_message(sender_name, pkt.pub_key, pkt.text, pkt.rssi, pkt.snr, pkt.hops, pkt.txt_type);
                break;
            }
            case RadioPacketKind::MsgSent: {
                std::ostringstream oss;
                oss << "[TX QUEUED] Direct packet transmitted ("
                    << (pkt.sent_flood ? "Flood route" : "Direct route")
                    << " | Tag 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
                    << pkt.ack_tag << std::dec << " | Est. timeout " << pkt.round_trip_ms << "ms)";
                log_console(oss.str());
                break;
            }
            case RadioPacketKind::SendConfirmed: {
                std::ostringstream oss;
                oss << "[ACK CONFIRMED] Direct message delivered! Round-trip time: "
                    << pkt.round_trip_ms << " ms (Tag 0x"
                    << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
                    << pkt.ack_tag << std::dec << ")";
                log_public_system(oss.str());
                // Append delivery ACK badge to the latest outgoing DM in any @ tab
                for (auto& t : tabs) {
                    if (!t.name.empty() && t.name[0] == '@') {
                        for (auto it = t.messages.rbegin(); it != t.messages.rend(); ++it) {
                            if (it->is_self && it->text.find("[ACK") == std::string::npos) {
                                it->text += "  [ACK " + std::to_string(pkt.round_trip_ms) + "ms]";
                                state_dirty = true;
                                break;
                            }
                        }
                    }
                }
                break;
            }
            case RadioPacketKind::TraceData: {
                record_rf_metrics(pkt.rssi, pkt.snr, 36);
                auto hops = resolve_hops(pkt);
                std::ostringstream oss;
                oss << "[TRACE PATH RESULT] Tag 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
                    << pkt.ack_tag << std::dec << " (" << hops.size() << " hop" << (hops.size() == 1 ? "" : "s") << "): ME";
                for (size_t i = 0; i < hops.size(); ++i) {
                    oss << " -> " << (hops[i].resolved ? hops[i].label : ("[" + hops[i].label + "]"));
                    if (i < pkt.trace_snrs.size()) {
                        oss << " (" << std::showpos << std::fixed << std::setprecision(1) << pkt.trace_snrs[i] << "dB" << std::noshowpos << ")";
                        if (hops[i].resolved) record_node_snr_by_name(hops[i].label, -76, pkt.trace_snrs[i], static_cast<uint8_t>(i + 1));
                    }
                }
                if (pkt.trace_snrs.size() > hops.size()) {
                    oss << " -> ME (" << std::showpos << std::fixed << std::setprecision(1) << pkt.trace_snrs.back() << "dB" << std::noshowpos << ")";
                }
                log_public_system(oss.str());
                log_active_tab(oss.str());
                record_path("ACTIVE TRACE", "TRACE", callsign, hops, pkt.rssi, pkt.snr);
                break;
            }
            case RadioPacketKind::LoginResult: {
                const auto* nd = find_node_by_prefix(pkt.pub_key.data(), 6);
                std::string rname = nd ? nd->name : hex_string(pkt.pub_key.data(), 6, "");
                std::string msg = pkt.login_ok
                                      ? ("[REPEATER LOGIN OK] Authenticated with " + rname +
                                         (pkt.login_permissions & 1 ? " (ADMIN privileges granted)" : " (Guest privileges)"))
                                      : ("[REPEATER LOGIN FAILED] Authentication rejected by " + rname);
                log_public_system(msg);
                log_active_tab(msg);
                break;
            }
            case RadioPacketKind::StatusResponse: {
                const auto* nd = find_node_by_prefix(pkt.pub_key.data(), 6);
                std::string rname = nd ? nd->name : hex_string(pkt.pub_key.data(), 6, "");
                std::ostringstream oss;
                oss << "[REPEATER STATUS] " << rname << " (" << pkt.encrypted_frame.size() << "B telemetry)";
                if (pkt.encrypted_frame.size() >= 2) {
                    uint16_t mv = static_cast<uint16_t>(pkt.encrypted_frame[0]) |
                                  (static_cast<uint16_t>(pkt.encrypted_frame[1]) << 8);
                    if (mv >= 2500 && mv <= 5000) {
                        oss << " | Batt: " << mv << "mV (" << static_cast<int>(battery_mv_to_percent(mv)) << "%)";
                    }
                }
                log_public_system(oss.str());
                log_active_tab(oss.str());
                break;
            }
            case RadioPacketKind::TelemetryResponse: {
                const auto* nd = find_node_by_prefix(pkt.pub_key.data(), 6);
                std::string rname = nd ? nd->name : hex_string(pkt.pub_key.data(), 6, "");
                std::string msg = "[TELEMETRY] Received CayenneLPP sensor payload from " + rname +
                                  " (" + std::to_string(pkt.encrypted_frame.size()) + " bytes): " +
                                  hex_string(pkt.encrypted_frame.data(), pkt.encrypted_frame.size(), " ");
                log_public_system(msg);
                log_active_tab(msg);
                break;
            }
            case RadioPacketKind::RawSniffedGroupFrame: {
                record_rf_metrics(pkt.rssi, pkt.snr, pkt.lora_raw_len ? pkt.lora_raw_len : (pkt.encrypted_frame.size() + 6));
                std::string path_desc = resolve_hop_path(pkt.hops, pkt.hop_hash_size, pkt.hop_bytes);
                if (auto sniffed = rainbow.sniff_frame(pkt.encrypted_frame)) {
                    record_node_snr_by_name(sniffed->sender, pkt.rssi, pkt.snr, pkt.hops);
                    MessageRoute route;
                    route.hops = resolve_hops(pkt);
                    route.rssi = pkt.rssi;
                    route.snr = pkt.snr;
                    route.time = now_hms_str();
                    ingest_decrypted_message(sniffed->channel_name, sniffed->sender, sniffed->text,
                                             pkt.rssi, pkt.snr, pkt.hops, path_desc, &route,
                                             sniffed->is_mono_img ? &sniffed->mono_img_bytes : nullptr,
                                             sniffed->timestamp);
                } else if (!pkt.encrypted_frame.empty()) {
                    std::ostringstream oss;
                    oss << "[RF GROUP PKT] Encrypted channel frame (Bucket 0x"
                        << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                        << static_cast<int>(pkt.encrypted_frame[0]) << std::dec
                        << ") | " << pkt.rssi << "dBm / " << std::fixed << std::setprecision(1) << pkt.snr
                        << "dB | " << path_desc;
                    log_console(oss.str());
                }
                break;
            }
            case RadioPacketKind::LiveAdvert: {
                record_rf_metrics(pkt.rssi, pkt.snr, pkt.lora_raw_len ? pkt.lora_raw_len : 112);
                update_or_insert_mesh_node(pkt, true);
                sort_and_recount_mesh_nodes();
                std::string path_desc = resolve_hop_path(pkt.hops, pkt.hop_hash_size, pkt.hop_bytes);
                const char* type_lbl = (pkt.contact_type == 2) ? "Repeater"
                                     : (pkt.contact_type == 3) ? "Room Server"
                                                               : "Node";
                std::ostringstream oss;
                oss << "[LIVE BEACON] " << (pkt.node_name.empty() ? "Station" : pkt.node_name)
                    << " (" << type_lbl << ") heard @ " << pkt.rssi << "dBm / "
                    << std::showpos << std::fixed << std::setprecision(1) << pkt.snr << "dB" << std::noshowpos
                    << " [" << path_desc << "]";
                log_console(oss.str());
                break;
            }
            case RadioPacketKind::RawRfActivity: {
                record_rf_metrics(pkt.rssi, pkt.snr, pkt.lora_raw_len ? pkt.lora_raw_len : 32);
                std::string path_desc = resolve_hop_path(pkt.hops, pkt.hop_hash_size, pkt.hop_bytes);
                const char* ptype = (pkt.lora_payload_type == 0x00) ? "REQ"
                                  : (pkt.lora_payload_type == 0x01) ? "RESPONSE"
                                  : (pkt.lora_payload_type == 0x02) ? "DIRECT_TXT"
                                  : (pkt.lora_payload_type == 0x03) ? "ACK"
                                  : (pkt.lora_payload_type == 0x08) ? "PATH"
                                  : (pkt.lora_payload_type == 0x09) ? "TRACE"
                                                                    : "LORA_PKT";
                std::ostringstream oss;
                oss << "[LIVE RF " << ptype << "] " << pkt.lora_raw_len << "B frame @ "
                    << pkt.rssi << "dBm / " << std::showpos << std::fixed << std::setprecision(1)
                    << pkt.snr << "dB" << std::noshowpos << " [" << path_desc << "]";
                log_console(oss.str());
                break;
            }
            default:
                break;
        }
    }

    // ========================================================================
    // Snooping toolkit: recorders
    // ========================================================================
    const MeshSystemNode* find_node_by_prefix(const uint8_t* p, size_t n) const {
        if (n == 0) return nullptr;
        for (const auto& nd : mesh_nodes) {
            if (std::memcmp(nd.pub_key.data(), p, std::min<size_t>(n, 32)) == 0) return &nd;
        }
        return nullptr;
    }

    const MeshSystemNode* find_mesh_node(const std::string& raw_query) const {
        std::string q = trim_str(raw_query);
        if (!q.empty() && q[0] == '@') q = trim_str(q.substr(1));
        if (q.empty()) return nullptr;
        for (const auto& nd : mesh_nodes) {
            if (_stricmp(nd.name.c_str(), q.c_str()) == 0) return &nd;
        }
        for (const auto& nd : mesh_nodes) {
            std::string kh = node_key_hex(nd);
            if (_stricmp(kh.c_str(), q.c_str()) == 0 ||
                (q.size() >= 2 && q.size() <= kh.size() && _strnicmp(kh.c_str(), q.c_str(), q.size()) == 0)) {
                return &nd;
            }
        }
        std::string lq = q;
        std::transform(lq.begin(), lq.end(), lq.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto& nd : mesh_nodes) {
            std::string ln = nd.name;
            std::transform(ln.begin(), ln.end(), ln.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ln.find(lq) != std::string::npos) return &nd;
        }
        return nullptr;
    }

    std::vector<PathHop> resolve_hops(const DecodedRadioPacket& pkt) const {
        std::vector<PathHop> out;
        size_t step = std::max<size_t>(1, pkt.hop_hash_size);
        for (size_t off = 0; off + step <= pkt.hop_bytes.size(); off += step) {
            PathHop hop;
            if (const auto* nd = find_node_by_prefix(pkt.hop_bytes.data() + off, step)) {
                hop.label = nd->name;
                hop.resolved = true;
                hop.lat_e6 = nd->lat_e6;
                hop.lon_e6 = nd->lon_e6;
            } else {
                hop.label = hex_string(pkt.hop_bytes.data() + off, step, "");
            }
            out.push_back(hop);
        }
        return out;
    }

    void push_frame(RawFrameRecord&& r) {
        if (recording_session && !record_file_path.empty() && !r.bytes.empty()) {
            std::string line = r.time + "\t" + (r.tx ? "TX" : "RX") + "\t" +
                               hex_string(r.bytes.data(), r.bytes.size(), "") + "\t" + r.summary + "\n";
            {
                std::ofstream out(std::filesystem::path(record_file_path), std::ios::app | std::ios::binary);
                if (out) out.write(line.data(), static_cast<std::streamsize>(line.size()));
            }
            {
                std::wstring latest = exe_dir() + L"exports\\latest_capture.tsv";
                std::ofstream out2(std::filesystem::path(latest), std::ios::app | std::ios::binary);
                if (out2) out2.write(line.data(), static_cast<std::streamsize>(line.size()));
            }
            ++recorded_frame_count;
        }
        raw_frames.push_back(std::move(r));
        while (raw_frames.size() > MAX_FRAMES) raw_frames.pop_front();
        if (packets_scroll > 0) ++packets_scroll; // keep the operator's scroll position stable
    }

    void record_tx_frame(const std::vector<uint8_t>& payload) {
        if (payload.empty()) return;
        RawFrameRecord r;
        r.seq = next_seq++;
        r.time = now_hms_ms_str();
        r.tx = true;
        r.code = payload[0];
        r.bytes = payload;
        ++tx_frame_count;
        r.summary = std::string("TX  ") + companion_code_name(r.code, true);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Direction: HOST -> RADIO  |  command 0x%02X %s  |  %zu bytes",
                      r.code, companion_code_name(r.code, true), payload.size());
        r.details.push_back(buf);
        if (r.code == 0x1F && payload.size() >= 2) {
            r.summary += "  slot " + std::to_string(payload[1]);
        } else if (r.code == 0x20 && payload.size() >= 34) {
            std::string name = read_cstr_slice(payload.data() + 2, 32);
            r.summary += "  slot " + std::to_string(payload[1]) + " = " + name;
            r.details.push_back("Set channel slot " + std::to_string(payload[1]) + " -> " + name +
                                (payload.size() >= 50 ? ("  PSK " + hex_string(payload.data() + 34, 16, "")) : ""));
        } else if (r.code == 0x03 && payload.size() > 7) {
            if (payload[1] == PAYLOAD_TYPE_MONO_IMG || payload[7] == PAYLOAD_TYPE_MONO_IMG) {
                std::string text = "[0x49 MONO_IMG 48x30 (181B)]";
                r.summary += "  slot " + std::to_string(payload[2]) + ": " + text;
                r.details.push_back("Channel slot " + std::to_string(payload[2]) + " 1-bit bitmap: " + text);
            } else {
                std::string text(reinterpret_cast<const char*>(payload.data() + 7), payload.size() - 7);
                r.summary += "  slot " + std::to_string(payload[2]) + ": " + text;
                r.details.push_back("Channel slot " + std::to_string(payload[2]) + " text: " + text);
            }
        } else if (r.code == 0x02 && payload.size() > 13) {
            std::string khex = hex_string(payload.data() + 7, 6, "");
            std::string text(reinterpret_cast<const char*>(payload.data() + 13), payload.size() - 13);
            const auto* nd = find_node_by_prefix(payload.data() + 7, 6);
            std::string target = nd ? (nd->name + " [" + khex + "]") : khex;
            r.summary += (payload[1] == TXT_TYPE_CLI_DATA ? "  CLI -> " : "  DM -> ") + target + ": " + text;
            r.details.push_back(std::string(payload[1] == TXT_TYPE_CLI_DATA ? "Repeater CLI to " : "Direct message to ") +
                                target + ": " + text);
        }
        push_frame(std::move(r));
    }

    void record_rx_frame(const DecodedRadioPacket& pkt) {
        if (pkt.raw.empty()) return;
        RawFrameRecord r;
        r.seq = next_seq++;
        r.time = now_hms_ms_str();
        r.code = pkt.raw_code;
        r.bytes = pkt.raw;
        r.is_rf = (pkt.raw_code == PUSH_LOG_RX_DATA || pkt.raw_code == PUSH_RAW_DATA);
        r.rssi = pkt.rssi;
        r.snr = pkt.snr;
        ++rx_frame_count;
        if (r.is_rf) ++rf_frame_count;

        char buf[256];
        std::snprintf(buf, sizeof(buf), "Direction: RADIO -> HOST  |  code 0x%02X %s  |  %zu bytes",
                      r.code, companion_code_name(r.code, false), pkt.raw.size());
        r.details.push_back(buf);
        std::string summary = std::string("RX  ") + companion_code_name(r.code, false);

        if (pkt.raw_code == PUSH_LOG_RX_DATA) {
            const char* ptype = lora_payload_name(pkt.lora_payload_type);
            const char* route = lora_route_name(pkt.route_type);
            std::vector<PathHop> hops = resolve_hops(pkt);

            std::snprintf(buf, sizeof(buf), "Over-the-air: RSSI %d dBm  |  SNR %+.2f dB  |  LoRa frame %zu bytes",
                          pkt.rssi, pkt.snr, pkt.lora_raw_len);
            r.details.push_back(buf);
            if (pkt.raw.size() > 3) {
                std::snprintf(buf, sizeof(buf), "Header 0x%02X  ->  route %s (%u)  |  payload %s (0x%02X)  |  version %u",
                              pkt.raw[3], route, pkt.route_type, ptype, pkt.lora_payload_type, (pkt.raw[3] >> 6) & 0x03);
                r.details.push_back(buf);
            }
            std::snprintf(buf, sizeof(buf), "Path: %u hop(s), %u-byte hop hashes  [%s]", pkt.hops, pkt.hop_hash_size,
                          hex_string(pkt.hop_bytes.data(), pkt.hop_bytes.size()).c_str());
            r.details.push_back(buf);
            for (size_t i = 0; i < hops.size(); ++i) {
                r.details.push_back("    hop " + std::to_string(i + 1) + ": " + hops[i].label +
                                    (hops[i].resolved ? "" : "  (unknown - not in contact DB)"));
            }

            std::string what = ptype;
            std::string origin;
            if (pkt.kind == RadioPacketKind::RawSniffedGroupFrame && !pkt.encrypted_frame.empty()) {
                uint8_t ch_hash = pkt.encrypted_frame[0];
                const auto& bucket = rainbow.buckets[ch_hash];
                std::string names;
                for (size_t i = 0; i < bucket.size() && i < 6; ++i) names += (i ? ", " : "") + bucket[i].name;
                std::snprintf(buf, sizeof(buf), "Group text: channel hash 0x%02X  |  %zu rainbow candidate(s)%s%s",
                              ch_hash, bucket.size(), names.empty() ? "" : ": ", names.c_str());
                r.details.push_back(buf);
                if (auto sn = rainbow.sniff_frame(pkt.encrypted_frame)) {
                    r.details.push_back("DECRYPTED  [" + sn->channel_name + "]  <" + sn->sender + "> " + sn->text);
                    what += " " + sn->channel_name;
                    origin = sn->sender;
                    summary += "  GRP_TXT " + sn->channel_name + " <" + sn->sender + "> " + sn->text;
                } else {
                    r.details.push_back("Not decrypted - channel key unknown (try /addword #guess)");
                    std::snprintf(buf, sizeof(buf), "  GRP_TXT ch 0x%02X (encrypted)", ch_hash);
                    summary += buf;
                    std::snprintf(buf, sizeof(buf), " 0x%02X", ch_hash);
                    what += buf;
                }
            } else if (pkt.kind == RadioPacketKind::LiveAdvert) {
                std::string nm = pkt.node_name.empty() ? "(unnamed)" : pkt.node_name;
                r.details.push_back("Advert: " + nm + "  |  " + contact_type_name(pkt.contact_type) +
                                    "  |  key " + hex_string(pkt.pub_key.data(), 8, ""));
                if (pkt.lat_e6 || pkt.lon_e6) {
                    std::snprintf(buf, sizeof(buf), "Advert GPS: %.6f, %.6f", pkt.lat_e6 / 1e6, pkt.lon_e6 / 1e6);
                    r.details.push_back(buf);
                }
                r.details.push_back("Advert timestamp: " + epoch_to_iso(pkt.timestamp));
                summary += "  ADVERT " + nm;
                what += " " + nm;
                origin = nm;
                record_advert(pkt, "RF ADVERT", hops);
            } else {
                summary += std::string("  ") + ptype;
            }

            std::snprintf(buf, sizeof(buf), "  %s %uh  %ddBm %+.1fdB", route, pkt.hops, pkt.rssi, pkt.snr);
            summary += buf;
            record_path(what, route, origin, hops, pkt.rssi, pkt.snr);
        } else {
            switch (pkt.kind) {
                case RadioPacketKind::DeviceInfo:
                    summary += "  " + pkt.model + " " + pkt.firmware;
                    r.details.push_back("Model: " + pkt.model + "  |  Firmware: " + pkt.firmware + "  |  Built: " + pkt.build_date);
                    break;
                case RadioPacketKind::SelfInfo:
                    summary += "  " + pkt.node_name;
                    std::snprintf(buf, sizeof(buf), "Node %s  |  %.3f MHz  BW %.1f kHz  SF%u CR%u  %u dBm  |  key %s",
                                  pkt.node_name.c_str(), pkt.freq_khz / 1000.0, pkt.bw_hz / 1000.0, pkt.sf, pkt.cr,
                                  pkt.tx_power_dbm, hex_string(pkt.pub_key.data(), 8, "").c_str());
                    r.details.push_back(buf);
                    break;
                case RadioPacketKind::Battery:
                    std::snprintf(buf, sizeof(buf), "  %.2fV (%u%%)", pkt.battery_mv / 1000.0, pkt.battery_pct);
                    summary += buf;
                    break;
                case RadioPacketKind::ChannelInfo:
                    summary += "  slot " + std::to_string(pkt.slot_idx) + " = " +
                               (pkt.channel_name.empty() ? std::string("<empty>") : pkt.channel_name);
                    r.details.push_back("Slot " + std::to_string(pkt.slot_idx) + "  |  " + pkt.channel_name +
                                        "  |  PSK " + hex_string(pkt.psk.data(), 16, ""));
                    break;
                case RadioPacketKind::ContactInfo:
                    summary += "  " + pkt.node_name + " (" + contact_type_name(pkt.contact_type) + ")";
                    r.details.push_back("Contact: " + pkt.node_name + "  |  " + contact_type_name(pkt.contact_type) +
                                        "  |  key " + hex_string(pkt.pub_key.data(), 8, "") +
                                        "  |  out_path_len " + std::to_string(pkt.out_path_len));
                    if (pkt.lat_e6 || pkt.lon_e6) {
                        std::snprintf(buf, sizeof(buf), "GPS: %.6f, %.6f  |  last advert %s", pkt.lat_e6 / 1e6,
                                      pkt.lon_e6 / 1e6, epoch_to_iso(pkt.timestamp).c_str());
                        r.details.push_back(buf);
                    }
                    if (pkt.raw_code == PUSH_NEW_ADVERT) record_advert(pkt, "PUSH_NEW_ADVERT", {});
                    break;
                case RadioPacketKind::ChannelMsgRecv:
                    summary += "  [" + hw_slots.channel_for_slot(pkt.slot_idx) + "] <" + pkt.sender + "> " + pkt.text;
                    std::snprintf(buf, sizeof(buf), "Slot %u  |  %u hop(s)  |  SNR %+.2f dB  |  sent %s", pkt.slot_idx,
                                  pkt.hops, pkt.snr, epoch_to_iso(pkt.timestamp).c_str());
                    r.details.push_back(buf);
                    r.details.push_back("<" + pkt.sender + "> " + pkt.text);
                    break;
                case RadioPacketKind::DirectMsgRecv:
                    summary += "  from " + hex_string(pkt.pub_key.data(), 6, "") + ": " + pkt.text;
                    r.details.push_back("Direct message from key prefix " + hex_string(pkt.pub_key.data(), 6, "") + ": " + pkt.text);
                    break;
                case RadioPacketKind::Error:
                    summary += "  error " + std::to_string(pkt.error_code);
                    break;
                default:
                    if (pkt.raw_code == PUSH_ADVERT && pkt.raw.size() >= 33) {
                        const auto* nd = find_node_by_prefix(pkt.raw.data() + 1, 32);
                        std::string nm = nd ? nd->name : hex_string(pkt.raw.data() + 1, 6, "");
                        summary += "  " + nm;
                        r.details.push_back("Known contact re-advertised: " + nm);
                        DecodedRadioPacket tmp;
                        tmp.node_name = nm;
                        std::memcpy(tmp.pub_key.data(), pkt.raw.data() + 1, 32);
                        if (nd) {
                            tmp.contact_type = nd->contact_type;
                            tmp.lat_e6 = nd->lat_e6;
                            tmp.lon_e6 = nd->lon_e6;
                        }
                        update_or_insert_mesh_node(tmp, true);
                        sort_and_recount_mesh_nodes();
                        record_advert(tmp, "PUSH_ADVERT", {});
                    }
                    break;
            }
        }
        r.summary = summary;
        push_frame(std::move(r));
    }

    void record_advert(const DecodedRadioPacket& pkt, const std::string& source, const std::vector<PathHop>& hops) {
        AdvertRecord a;
        a.seq = next_seq++;
        a.time = now_hms_ms_str();
        a.name = pkt.node_name.empty() ? "(unnamed)" : pkt.node_name;
        a.key_prefix = hex_string(pkt.pub_key.data(), 6, "");
        a.contact_type = pkt.contact_type;
        a.lat_e6 = pkt.lat_e6;
        a.lon_e6 = pkt.lon_e6;
        a.has_rf = (source == "RF ADVERT");
        a.rssi = pkt.rssi;
        a.snr = pkt.snr;
        a.hops = pkt.hops;
        a.source = source;
        for (size_t i = 0; i < hops.size(); ++i) a.path += (i ? " > " : "") + hops[i].label;
        if (a.has_rf && hops.empty()) a.path = "direct";
        {
            std::ostringstream a_oss;
            a_oss << "[ADVERT] " << contact_type_name(a.contact_type) << " beacon on-air [" << a.key_prefix << "]";
            if (a.has_rf) {
                a_oss << " @ " << a.rssi << "dBm / " << std::showpos << std::fixed << std::setprecision(1)
                      << a.snr << "dB" << std::noshowpos << " (" << static_cast<int>(a.hops) << "h)";
            }
            if (!a.path.empty()) a_oss << " via " << a.path;
            else if (!a.source.empty()) a_oss << " (" << a.source << ")";
            log_all_heard(a.name, a_oss.str(), a.rssi, a.snr, a.hops, false, a.has_rf);
        }
        adverts.push_back(std::move(a));
        while (adverts.size() > MAX_ADVERTS) adverts.pop_front();
        if (adverts_scroll > 0) ++adverts_scroll;
    }

    void record_path(const std::string& what, const std::string& route, const std::string& origin,
                     const std::vector<PathHop>& hops, int16_t rssi, float snr) {
        PathRecord p;
        p.seq = next_seq++;
        p.time = now_hms_ms_str();
        p.what = what;
        p.route = route;
        p.origin = origin;
        p.hops = hops;
        p.rssi = rssi;
        p.snr = snr;
        for (size_t i = 0; i < hops.size(); ++i) {
            auto& st = relay_stats[hops[i].label];
            st.label = hops[i].label;
            st.relay_count++;
            if (i + 1 == hops.size()) {
                st.last_hop_count++;
                st.best_snr = std::max(st.best_snr, snr);
                if (hops[i].resolved) record_node_snr_by_name(hops[i].label, rssi, snr, 1);
            }
        }
        path_traces.push_back(std::move(p));
        while (path_traces.size() > MAX_PATHS) path_traces.pop_front();
        if (paths_scroll > 0) ++paths_scroll;
    }

    std::vector<RelayStat> sorted_relays(bool by_last_hop) const {
        std::vector<RelayStat> v;
        for (const auto& kv : relay_stats) v.push_back(kv.second);
        std::sort(v.begin(), v.end(), [by_last_hop](const RelayStat& a, const RelayStat& b) {
            return by_last_hop ? (a.last_hop_count > b.last_hop_count) : (a.relay_count > b.relay_count);
        });
        return v;
    }

    bool frame_passes_filter(const RawFrameRecord& r) const {
        switch (packet_filter) {
            case PacketFilter::RfOnly:    return r.is_rf;
            case PacketFilter::Companion: return !r.is_rf && !r.tx;
            case PacketFilter::TxOnly:    return r.tx;
            default:                      return true;
        }
    }

    const RawFrameRecord* selected_frame() const {
        for (auto it = raw_frames.rbegin(); it != raw_frames.rend(); ++it) {
            if (it->seq == selected_frame_seq) return &*it;
        }
        return nullptr;
    }

    // ========================================================================
    // Snooping toolkit: exporters (written to <exe>\exports\)
    // ========================================================================
    bool suppress_reveal = false;

    void finish_export(const std::wstring& file, bool ok, const std::string& what, size_t count) {
        if (!ok) {
            log_public_system("[EXPORT] Failed to write " + from_wide(file));
            return;
        }
        log_public_system("[EXPORT] " + what + ": " + std::to_string(count) + " rows -> " + from_wide(file));
        if (!suppress_reveal) reveal_in_explorer(file);
    }

    void export_nodes_csv() {
        std::wstring file = export_filename(L"meshcore_nodes", L"csv");
        std::ofstream f(std::filesystem::path(file), std::ios::binary);
        if (f) {
            f << "name,type,pubkey,latitude,longitude,last_advert,route,heard_live_rf,live_rssi_dbm,live_snr_db,live_hops\r\n";
            for (const auto& n : mesh_nodes) {
                f << csv_escape(n.name) << ',' << contact_type_name(n.contact_type) << ','
                  << hex_string(n.pub_key.data(), 32, "") << ',';
                if (n.lat_e6 || n.lon_e6) f << std::fixed << std::setprecision(6) << n.lat_e6 / 1e6 << ',' << n.lon_e6 / 1e6;
                else f << ',';
                f << ',' << epoch_to_iso(n.last_advert_ts) << ',' << (n.out_path_len < 0 ? "flood" : "direct") << ','
                  << (n.heard_live_rf ? "yes" : "no") << ',';
                if (n.heard_live_rf) f << n.live_rssi << ',' << std::setprecision(2) << n.live_snr << ',' << int(n.live_hops);
                else f << ",,";
                f << "\r\n";
            }
        }
        finish_export(file, static_cast<bool>(f), "Nodes CSV", mesh_nodes.size());
    }

    void export_packets_csv() {
        std::wstring file = export_filename(L"meshcore_packets", L"csv");
        std::ofstream f(std::filesystem::path(file), std::ios::binary);
        if (f) {
            f << "seq,time,direction,code,code_name,over_the_air,rssi_dbm,snr_db,length,summary,hex\r\n";
            for (const auto& r : raw_frames) {
                char code_buf[8];
                std::snprintf(code_buf, sizeof(code_buf), "0x%02X", r.code);
                f << r.seq << ',' << r.time << ',' << (r.tx ? "TX" : "RX") << ',' << code_buf << ','
                  << companion_code_name(r.code, r.tx) << ',' << (r.is_rf ? "yes" : "no") << ',';
                if (r.is_rf) f << r.rssi << ',' << std::fixed << std::setprecision(2) << r.snr;
                else f << ',';
                f << ',' << r.bytes.size() << ',' << csv_escape(r.summary) << ','
                  << hex_string(r.bytes.data(), r.bytes.size(), "") << "\r\n";
            }
        }
        finish_export(file, static_cast<bool>(f), "Packets CSV", raw_frames.size());
    }

    void export_adverts_csv() {
        std::wstring file = export_filename(L"meshcore_adverts", L"csv");
        std::ofstream f(std::filesystem::path(file), std::ios::binary);
        if (f) {
            f << "time,source,name,type,key_prefix,latitude,longitude,rssi_dbm,snr_db,hops,path\r\n";
            for (const auto& a : adverts) {
                f << a.time << ',' << a.source << ',' << csv_escape(a.name) << ',' << contact_type_name(a.contact_type)
                  << ',' << a.key_prefix << ',';
                if (a.lat_e6 || a.lon_e6) f << std::fixed << std::setprecision(6) << a.lat_e6 / 1e6 << ',' << a.lon_e6 / 1e6;
                else f << ',';
                f << ',';
                if (a.has_rf) f << a.rssi << ',' << std::setprecision(2) << a.snr << ',' << int(a.hops);
                else f << ",,";
                f << ',' << csv_escape(a.path) << "\r\n";
            }
        }
        finish_export(file, static_cast<bool>(f), "Adverts CSV", adverts.size());
    }

    void export_kml() {
        std::wstring file = export_filename(L"meshcore_map", L"kml");
        std::ofstream f(std::filesystem::path(file), std::ios::binary);
        size_t placed = 0;
        if (f) {
            f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document>\n"
                 "<name>MeshCore++ Snapshot " << xml_escape(epoch_to_iso(now_epoch_secs())) << "</name>\n";
            struct Sty { const char* id; const char* icon; };
            static const Sty STYLES[] = {
                {"rpt", "http://maps.google.com/mapfiles/kml/paddle/blu-circle.png"},
                {"room", "http://maps.google.com/mapfiles/kml/paddle/purple-circle.png"},
                {"node", "http://maps.google.com/mapfiles/kml/paddle/grn-circle.png"},
                {"live", "http://maps.google.com/mapfiles/kml/paddle/ylw-stars.png"},
                {"me", "http://maps.google.com/mapfiles/kml/paddle/red-stars.png"},
            };
            for (const auto& s : STYLES) {
                f << "<Style id=\"" << s.id << "\"><IconStyle><Icon><href>" << s.icon << "</href></Icon></IconStyle></Style>\n";
            }
            f << "<Style id=\"path\"><LineStyle><color>c8ffe500</color><width>2.5</width></LineStyle></Style>\n";

            f << std::fixed << std::setprecision(6);
            if (self_lat_e6 || self_lon_e6) {
                f << "<Placemark><name>" << xml_escape(callsign) << " (this radio)</name><styleUrl>#me</styleUrl>"
                  << "<Point><coordinates>" << self_lon_e6 / 1e6 << ',' << self_lat_e6 / 1e6 << ",0</coordinates></Point></Placemark>\n";
            }

            const char* folders[] = {"Repeaters", "Room Servers", "Companions & Sensors"};
            for (int fi = 0; fi < 3; ++fi) {
                f << "<Folder><name>" << folders[fi] << "</name>\n";
                for (const auto& n : mesh_nodes) {
                    int group = (n.contact_type == 2) ? 0 : (n.contact_type == 3) ? 1 : 2;
                    if (group != fi || (n.lat_e6 == 0 && n.lon_e6 == 0)) continue;
                    const char* sty = n.heard_live_rf ? "live" : (group == 0 ? "rpt" : group == 1 ? "room" : "node");
                    f << "<Placemark><name>" << xml_escape(n.name) << "</name><styleUrl>#" << sty << "</styleUrl>"
                      << "<description>" << xml_escape(std::string(contact_type_name(n.contact_type)) +
                                                       " | key " + hex_string(n.pub_key.data(), 6, "") +
                                                       " | last advert " + epoch_to_iso(n.last_advert_ts));
                    if (n.heard_live_rf) {
                        f << xml_escape(" | heard live " + std::to_string(n.live_rssi) + " dBm");
                    }
                    f << "</description><Point><coordinates>" << n.lon_e6 / 1e6 << ',' << n.lat_e6 / 1e6
                      << ",0</coordinates></Point></Placemark>\n";
                    ++placed;
                }
                f << "</Folder>\n";
            }

            // Heard relay paths: hop -> hop -> this radio (only segments where hop GPS is known)
            f << "<Folder><name>Heard Relay Paths</name>\n";
            std::map<std::string, bool> seen_paths;
            for (const auto& p : path_traces) {
                std::vector<std::pair<int32_t, int32_t>> pts;
                std::string key;
                for (const auto& h : p.hops) {
                    if (h.lat_e6 || h.lon_e6) { pts.push_back({h.lat_e6, h.lon_e6}); key += h.label + ">"; }
                }
                if (self_lat_e6 || self_lon_e6) pts.push_back({self_lat_e6, self_lon_e6});
                if (pts.size() < 2 || seen_paths[key]) continue;
                seen_paths[key] = true;
                f << "<Placemark><name>" << xml_escape(p.what) << "</name><styleUrl>#path</styleUrl><LineString>"
                     "<tessellate>1</tessellate><coordinates>";
                for (const auto& pt : pts) f << pt.second / 1e6 << ',' << pt.first / 1e6 << ",0 ";
                f << "</coordinates></LineString></Placemark>\n";
            }
            f << "</Folder>\n</Document></kml>\n";
        }
        finish_export(file, static_cast<bool>(f), "KML map (open in Google Earth / maps)", placed);
    }

    void export_all() {
        suppress_reveal = true;
        export_nodes_csv();
        export_adverts_csv();
        export_packets_csv();
        suppress_reveal = false;
        export_kml();
    }

    void record_rf_metrics(int16_t rssi, float snr, size_t raw_bytes = 40) {
        has_last_rf = true;
        last_rssi = rssi;
        last_snr = snr;
        snr_history.push_back(snr);
        while (snr_history.size() > 40) snr_history.pop_front();

        // Estimate LoRa Time-on-Air (ms) from SF, BW, and payload bytes
        double bw = std::max(7800.0, static_cast<double>(radio_bw_hz));
        double t_sym_ms = (std::ldexp(1.0, std::clamp<int>(radio_sf, 6, 12)) / bw) * 1000.0;
        double n_payload = 8.0 + std::max(0.0, std::ceil((8.0 * static_cast<double>(raw_bytes) - 4.0 * radio_sf + 28.0) /
                                                         (4.0 * radio_sf)) * (radio_cr <= 8 ? radio_cr : 5));
        double toa_ms = (12.25 + n_payload) * t_sym_ms;
        uint64_t now_s = now_epoch_secs();
        rf_airtime_window.push_back({now_s, toa_ms});
        while (!rf_airtime_window.empty() && now_s > rf_airtime_window.front().first + 60) {
            rf_airtime_window.pop_front();
        }
    }

    double channel_busy_pct() {
        uint64_t now_s = now_epoch_secs();
        while (!rf_airtime_window.empty() && now_s > rf_airtime_window.front().first + 60) {
            rf_airtime_window.pop_front();
        }
        double total_ms = 0.0;
        for (const auto& p : rf_airtime_window) total_ms += p.second;
        return std::clamp((total_ms / 60000.0) * 100.0, 0.0, 100.0);
    }

    void send_self_advert_beacon(bool flood = true) {
        transport.send_self_advert(flood);
        std::string msg = std::string("[TX ADVERT] Broadcasted self node advert beacon (") +
                          (flood ? "Flood" : "Direct/Zero-Hop") + " mode, CMD_SEND_SELF_ADVERT 0x07)";
        log_public_system(msg);
        log_active_tab(msg);
    }

    void trace_to_node(const std::string& raw_target) {
        std::string q = trim_str(raw_target);
        if (q.empty()) {
            log_active_tab("Usage: /tracepath <node_name_or_hex_hops>  (e.g. /tracepath Summit Peak  or  /tracepath 4A 7C)");
            return;
        }
        std::vector<uint8_t> path_hashes;
        std::vector<std::string> hop_labels;
        if (const auto* nd = find_mesh_node(q)) {
            path_hashes.push_back(nd->pub_key[0]);
            hop_labels.push_back(nd->name);
        } else {
            std::istringstream iss(q);
            std::string tok;
            while (iss >> tok) {
                if (const auto* nd = find_mesh_node(tok)) {
                    path_hashes.push_back(nd->pub_key[0]);
                    hop_labels.push_back(nd->name);
                } else if (tok.size() == 2) {
                    uint8_t b = 0;
                    if (hex_to_bytes(tok, &b, 1)) {
                        path_hashes.push_back(b);
                        hop_labels.push_back("[" + tok + "]");
                    }
                }
            }
        }
        if (path_hashes.empty()) {
            auto psk = derive_psk(q);
            path_hashes.push_back(psk[0]);
            hop_labels.push_back(q);
        }
        uint32_t tag = static_cast<uint32_t>(GetTickCount64() ^ 0xA53C9E11u);
        transport.send_trace_path(tag, path_hashes);
        std::ostringstream oss;
        oss << "[TX TRACE] Sent active RF trace route (Tag 0x" << std::hex << std::uppercase
            << std::setw(8) << std::setfill('0') << tag << std::dec << ") -> ";
        for (size_t i = 0; i < hop_labels.size(); ++i) {
            if (i > 0) oss << " -> ";
            oss << hop_labels[i];
        }
        log_public_system(oss.str());
        log_active_tab(oss.str());
    }

    MeshSystemNode* resolve_repeater_target(const std::string& raw_target) {
        std::string q = trim_str(raw_target);
        if (!q.empty()) {
            if (const auto* nd = find_mesh_node(q)) return const_cast<MeshSystemNode*>(nd);
        }
        if (active_tab_idx < tabs.size() && !tabs[active_tab_idx].name.empty() && tabs[active_tab_idx].name[0] == '@') {
            if (const auto* nd = find_mesh_node(tabs[active_tab_idx].name.substr(1))) return const_cast<MeshSystemNode*>(nd);
        }
        if (!map_selected_key.empty()) {
            if (const auto* nd = find_node_by_key_hex(map_selected_key)) return const_cast<MeshSystemNode*>(nd);
        }
        for (auto& nd : mesh_nodes) {
            if (nd.contact_type == 2 || nd.contact_type == 3) return &nd;
        }
        return mesh_nodes.empty() ? nullptr : &mesh_nodes.front();
    }

    void repeater_login(const std::string& raw_target, const std::string& password) {
        MeshSystemNode* nd = resolve_repeater_target(raw_target);
        if (!nd) {
            log_active_tab("No repeater/room node found. Usage: /rlogin <repeater_name> <password>");
            return;
        }
        open_dm_tab(nd->name, false);
        transport.send_login(nd->pub_key, password);
        std::string msg = "[REPEATER ADMIN] Sent CMD_SEND_LOGIN (0x1A) to " + nd->name +
                          " [" + node_key_hex(*nd) + "]";
        log_public_system(msg);
        log_active_tab(msg);
    }

    void repeater_cli_cmd(const std::string& raw_target, const std::string& cli_cmd) {
        std::string cmd_str = trim_str(cli_cmd);
        if (cmd_str.empty()) {
            log_active_tab("Usage: /rcmd <repeater_name> <command>  (or /cli <ver|neighbors|clock|stats|reboot>)");
            return;
        }
        MeshSystemNode* nd = resolve_repeater_target(raw_target);
        std::string target_name = nd ? nd->name : (raw_target.empty() ? "Repeater" : raw_target);
        size_t tidx = open_dm_tab(target_name, false);
        std::string disp_target;
        auto prefix = resolve_dm_prefix(target_name, disp_target);
        transport.send_direct_message(prefix, disp_target, cmd_str, TXT_TYPE_CLI_DATA);
        if (tidx < tabs.size()) {
            tabs[tidx].messages.push_back(ChatMessage{
                now_hms_str(), callsign, false, 0, 0.0f, 0, "[CLI >] " + cmd_str, false, false, true
            });
            tabs[tidx].scroll_offset = 0;
            state_dirty = true;
        }
    }

    void repeater_status_req(const std::string& raw_target) {
        MeshSystemNode* nd = resolve_repeater_target(raw_target);
        if (!nd) {
            log_active_tab("No repeater/node found for status request.");
            return;
        }
        transport.send_status_req(nd->pub_key);
        std::string msg = "[REPEATER STATUS REQ] Sent CMD_SEND_STATUS_REQ (0x1B) -> " + nd->name +
                          " [" + node_key_hex(*nd) + "]";
        log_public_system(msg);
        log_active_tab(msg);
    }

    void repeater_telemetry_req(const std::string& raw_target) {
        MeshSystemNode* nd = resolve_repeater_target(raw_target);
        if (!nd) {
            log_active_tab("No node found for telemetry request.");
            return;
        }
        transport.send_telemetry_req(nd->pub_key);
        std::string msg = "[TELEMETRY REQ] Sent CMD_SEND_TELEMETRY_REQ (0x27) -> " + nd->name +
                          " [" + node_key_hex(*nd) + "]";
        log_public_system(msg);
        log_active_tab(msg);
    }

    void reset_node_path(const std::string& raw_target) {
        MeshSystemNode* nd = resolve_repeater_target(raw_target);
        if (!nd) {
            log_active_tab("No node found to reset path.");
            return;
        }
        nd->out_path_len = -1;
        state_dirty = true;
        transport.send_reset_path(nd->pub_key);
        std::string msg = "[PATH RESET] Cleared cached route for " + nd->name +
                          " (CMD_RESET_PATH 0x0D) — next packet will use Flood route discovery";
        log_public_system(msg);
        log_active_tab(msg);
    }

    void toggle_session_recording(const std::string& arg = "") {
        std::string low = trim_str(arg);
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool want_on = !recording_session;
        if (low == "on" || low == "start" || low == "1") want_on = true;
        else if (low == "off" || low == "stop" || low == "0") want_on = false;

        if (!want_on) {
            recording_session = false;
            std::string msg = "[SESSION RECORDER] Stopped recording (" + std::to_string(recorded_frame_count) +
                              " frames saved to exports/" + record_file_label + " & exports/latest_capture.tsv)";
            log_public_system(msg);
            log_active_tab(msg);
            return;
        }

        record_file_path = export_filename(L"meshcore_capture", L"tsv");
        record_file_label = from_wide(record_file_path);
        recorded_frame_count = 0;

        // Also initialize latest_capture.tsv with currently buffered frames if starting fresh
        std::wstring latest = exe_dir() + L"exports\\latest_capture.tsv";
        {
            std::ofstream f1(std::filesystem::path(record_file_path), std::ios::binary | std::ios::trunc);
            std::ofstream f2(std::filesystem::path(latest), std::ios::binary | std::ios::trunc);
            const char* hdr = "# MeshCore++ Session Capture (time\tdir\thex\tsummary)\n";
            if (f1) f1.write(hdr, static_cast<std::streamsize>(std::strlen(hdr)));
            if (f2) f2.write(hdr, static_cast<std::streamsize>(std::strlen(hdr)));
            // Snapshot existing raw_frames so an immediate stop+replay already has session packets!
            for (const auto& r : raw_frames) {
                if (r.bytes.empty()) continue;
                std::string line = r.time + "\t" + (r.tx ? "TX" : "RX") + "\t" +
                                   hex_string(r.bytes.data(), r.bytes.size(), "") + "\t" + r.summary + "\n";
                if (f1) f1.write(line.data(), static_cast<std::streamsize>(line.size()));
                if (f2) f2.write(line.data(), static_cast<std::streamsize>(line.size()));
                ++recorded_frame_count;
            }
        }
        recording_session = true;
        std::string msg = "[SESSION RECORDER] LIVE RECORDING ON -> " + record_file_label +
                          " (snapshotted " + std::to_string(recorded_frame_count) + " frames + streaming all new RX/TX frames)";
        log_public_system(msg);
        log_active_tab(msg);
    }

    void replay_session_file(const std::string& custom_file = "") {
        std::wstring dir = exe_dir() + L"exports";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring path = custom_file.empty()
                                ? (dir + L"\\latest_capture.tsv")
                                : ((custom_file.find(':') != std::string::npos || custom_file.find('\\') != std::string::npos || custom_file.find('/') != std::string::npos)
                                       ? to_wide(custom_file)
                                       : (dir + L"\\" + to_wide(custom_file)));
        std::ifstream in((std::filesystem::path(path)));
        if (!in) {
            log_active_tab("[SESSION REPLAY] No capture file found at exports/latest_capture.tsv — click 'Rec' or run /record first!");
            return;
        }
        bool was_rec = recording_session;
        recording_session = false; // prevent re-recording while replaying
        size_t rx_replayed = 0, tx_replayed = 0;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            auto cols = split_tsv(line);
            if (cols.size() < 3) continue;
            const std::string& dir_str = cols[1];
            const std::string& hex_str = cols[2];
            if (hex_str.size() < 2 || (hex_str.size() % 2) != 0) continue;
            std::vector<uint8_t> bytes(hex_str.size() / 2);
            if (!hex_to_bytes(hex_str, bytes.data(), bytes.size())) continue;
            if (dir_str == "TX") {
                record_tx_frame(bytes);
                ++tx_replayed;
            } else {
                transport.inject_raw_frame(bytes);
                ++rx_replayed;
            }
        }
        recording_session = was_rec;
        std::ostringstream oss;
        oss << "[SESSION REPLAY] Replayed " << (rx_replayed + tx_replayed) << " frames ("
            << rx_replayed << " RX, " << tx_replayed << " TX) from capture file!";
        log_public_system(oss.str());
        log_active_tab(oss.str());
    }

    static bool same_route(const MessageRoute& a, const MessageRoute& b) {
        if (a.hops.size() != b.hops.size()) return false;
        for (size_t i = 0; i < a.hops.size(); ++i)
            if (a.hops[i].label != b.hops[i].label) return false;
        return true;
    }

    // Adds a route to a message if it's a path we haven't seen for it yet. Returns true if added.
    static bool add_route_to_message(ChatMessage& m, const MessageRoute& r) {
        for (auto& existing : m.routes) {
            if (same_route(existing, r)) {
                if (r.snr > existing.snr) { existing.snr = r.snr; existing.rssi = r.rssi; }
                return false;
            }
        }
        m.routes.push_back(r);
        return true;
    }

    // Merge a route into every on-screen copy of the message with this trace key (channel tab + Public mirror).
    bool merge_route_into_messages(const std::string& key, const MessageRoute& r) {
        bool any = false;
        auto scan = [&](ChannelTab& t) {
            int checked = 0;
            for (auto it = t.messages.rbegin(); it != t.messages.rend() && checked < 200; ++it, ++checked) {
                if (it->trace_key == key) { add_route_to_message(*it, r); any = true; }
            }
        };
        for (auto& t : tabs) scan(t);
        for (auto& kv : background_buffers) scan(kv.second);
        return any;
    }

    void ingest_decrypted_message(const std::string& raw_ch,
                                  const std::string& sender,
                                  const std::string& text,
                                  int16_t rssi,
                                  float snr,
                                  uint8_t hops,
                                  const std::string& path_desc = "",
                                  const MessageRoute* route = nullptr,
                                  const std::array<uint8_t, MONO_IMG_BYTES>* mono_img = nullptr,
                                  uint32_t pkt_ts = 0) {
        std::string ch = (normalize_channel_name(raw_ch) == PUBLIC_CHANNEL_NAME)
                             ? PUBLIC_CHANNEL_NAME
                             : raw_ch;

        // Deduplicate multi-repeater LoRa flood copies within 45 seconds
        uint64_t now_s = now_epoch_secs();
        while (!recent_dedup.empty() && now_s > recent_dedup.front().epoch_sec + 45) {
            recent_dedup.pop_front();
        }
        std::string dedup_payload = mono_img ? hex_string(mono_img->data(), 24, "") : text;
        std::string dedup_key_base = normalize_channel_name(ch) + "|" + sender + "|" + dedup_payload;
        bool is_self_echo = (!callsign.empty() && trim_str(sender) == trim_str(callsign));
        std::string dedup_key = (!is_self_echo && pkt_ts != 0)
                                    ? (dedup_key_base + "|" + std::to_string(pkt_ts))
                                    : dedup_key_base;
        for (const auto& item : recent_dedup) {
            if (item.key == dedup_key || (is_self_echo && item.key == dedup_key_base)) {
                // Already displayed: attach this copy's repeater path to the message's trace
                if (route) merge_route_into_messages(item.key, *route);
                if (!path_desc.empty()) {
                    log_console("[FLOOD RELAY] Duplicate copy of <" + sender + "> in " + ch + " (" + path_desc + ")");
                }
                return;
            }
        }
        recent_dedup.push_back({dedup_key, now_s});

        if (!ch.empty() && ch[0] == '#') {
            register_or_update_discovered(ch, true, rssi, snr, false);
        }

        // Scrape #hashtags via regex/scanner (only for plain-text messages)
        if (!mono_img) {
            for (const auto& tag : extract_hashtags(text)) {
                bool was_known = rainbow.contains(tag);
                auto cand = rainbow.add_channel(tag);
                bool already_disc = false;
                for (const auto& d : discovered) {
                    if (d.name == cand.name) { already_disc = true; break; }
                }
                register_or_update_discovered(cand.name, false, 0, 0.0f, true);
                if (!already_disc) {
                    std::ostringstream oss;
                    oss << "[SNIFFER] Scraped " << cand.name << " (hash=0x"
                        << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                        << static_cast<int>(cand.channel_hash) << ") from <" << sender << "> in " << ch
                        << (was_known ? "" : " [Added to 256-bucket table]");
                    log_console(oss.str());
                }
            }
        }

        bool is_mention = false;
        if (!mono_img) {
            std::string lower_txt = text;
            std::string lower_call = callsign;
            std::transform(lower_txt.begin(), lower_txt.end(), lower_txt.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(lower_call.begin(), lower_call.end(), lower_call.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            is_mention = !lower_call.empty() && lower_txt.find(lower_call) != std::string::npos;
        }

        std::string full_text = (path_desc.empty() || route) ? text : (text + "  [" + path_desc + "]");
        ChatMessage msg{
            now_hms_str(), sender, true, rssi, snr, hops, full_text, false, is_mention, false
        };
        if (mono_img) {
            msg.has_mono_image = true;
            msg.mono_image = *mono_img;
        }
        msg.trace_key = dedup_key;
        if (route) msg.routes.push_back(*route);

        log_console("[CHAT " + ch + "] <" + sender + "> " + text +
                    (path_desc.empty() ? "" : (" [" + path_desc + "]")));
        log_all_heard(sender, "[" + ch + "] " + full_text, rssi, snr, hops, false, true);

        bool delivered_to_tab = false;
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (_stricmp(tabs[i].name.c_str(), ch.c_str()) == 0 ||
                normalize_channel_name(tabs[i].name) == normalize_channel_name(ch)) {
                tabs[i].update_station(sender, rssi, snr, hops);
                tabs[i].scan_text_mentions(text);
                tabs[i].messages.push_back(msg);
                trim_tab_messages(tabs[i], 1000);
                if (i != active_tab_idx) {
                    tabs[i].unread_count++;
                    if (is_mention) tabs[i].has_mention = true;
                }
                delivered_to_tab = true;
            }
        }
        if (delivered_to_tab) {
            state_dirty = true;
            return;
        }

        auto& bg = background_buffers[normalize_channel_name(ch)];
        if (bg.name.empty()) bg.name = ch;
        bg.update_station(sender, rssi, snr, hops);
        bg.scan_text_mentions(text);
        bg.messages.push_back(msg);
        trim_tab_messages(bg, 800);
        bg.unread_count++;
        if (is_mention) bg.has_mention = true;
        state_dirty = true;
    }

    void register_or_update_discovered(const std::string& raw_name,
                                       bool has_rf,
                                       int16_t rssi,
                                       float snr,
                                       bool is_mention_scrape) {
        std::string name = normalize_channel_name(raw_name);
        if (name.empty() || name[0] != '#') return;

        auto psk = derive_psk(raw_name);
        uint8_t h = compute_channel_hash(psk);
        uint64_t now = now_epoch_secs();

        for (auto& d : discovered) {
            if (normalize_channel_name(d.name) == name) {
                if (has_rf) {
                    d.msg_count++;
                    d.has_rf = true;
                    d.last_rssi = rssi;
                    d.last_snr = snr;
                    d.mentioned_only = false;
                    d.last_activity_epoch = now;
                }
                sort_discovered();
                return;
            }
        }

        DiscoveredChannel item;
        item.name = raw_name[0] == '#' ? raw_name : name;
        item.channel_hash = h;
        item.psk_hex = psk_to_hex(psk);
        item.msg_count = has_rf ? 1 : 0;
        item.has_rf = has_rf;
        item.last_rssi = rssi;
        item.last_snr = snr;
        item.mentioned_only = !has_rf && is_mention_scrape;
        item.last_activity_epoch = now;
        discovered.push_back(item);
        sort_discovered();
    }

    void sort_discovered() {
        std::sort(discovered.begin(), discovered.end(), [](const auto& a, const auto& b) {
            if (a.msg_count != b.msg_count) return a.msg_count > b.msg_count;
            if (a.last_activity_epoch != b.last_activity_epoch) return a.last_activity_epoch > b.last_activity_epoch;
            return a.name < b.name;
        });
    }

    std::array<uint8_t, 6> resolve_dm_prefix(const std::string& raw_target, std::string& out_display_name) const {
        std::string clean = trim_str(raw_target);
        if (!clean.empty() && clean[0] == '@') clean = trim_str(clean.substr(1));
        std::array<uint8_t, 6> prefix{};
        if (const auto* nd = find_mesh_node(clean)) {
            std::memcpy(prefix.data(), nd->pub_key.data(), 6);
            out_display_name = nd->name;
            return prefix;
        }
        out_display_name = clean;
        if (clean.size() >= 12) {
            std::array<uint8_t, 6> hex_p{};
            if (hex_to_bytes(clean.substr(0, 12), hex_p.data(), 6)) {
                return hex_p;
            }
        }
        // Fallback deterministic 6-byte prefix from name SHA-256 (useful in RF Simulator mode)
        auto psk = derive_psk(clean);
        std::memcpy(prefix.data(), psk.data(), 6);
        return prefix;
    }

    size_t open_dm_tab(const std::string& raw_target, bool silent = false) {
        std::string clean = trim_str(raw_target);
        if (!clean.empty() && clean[0] == '@') clean = trim_str(clean.substr(1));
        if (clean.empty()) return active_tab_idx;

        const auto* nd = find_mesh_node(clean);
        std::string canonical = "@" + (nd && !nd->name.empty() ? nd->name : clean);

        for (size_t i = 0; i < tabs.size(); ++i) {
            if (_stricmp(tabs[i].name.c_str(), canonical.c_str()) == 0) {
                if (!silent) {
                    center_view = CenterView::Chat;
                    select_tab(i);
                }
                return i;
            }
        }

        ChannelTab new_tab(canonical);
        auto it = background_buffers.find(canonical);
        if (it != background_buffers.end()) {
            new_tab = std::move(it->second);
            background_buffers.erase(it);
        }
        new_tab.unread_count = 0;
        new_tab.has_mention = false;
        new_tab.ensure_mentioned_station(canonical.substr(1));

        std::ostringstream oss;
        if (nd) {
            oss << "Direct Message session with " << nd->name << " (" << contact_type_name(nd->contact_type)
                << ") | Key Prefix: " << node_key_hex(*nd)
                << " | Route: " << (nd->out_path_len < 0 ? "Flood" : ("Direct (" + std::to_string(nd->out_path_len) + " hop)"));
        } else {
            oss << "Direct Message session with " << canonical.substr(1)
                << " | Unicast Companion CMD_SEND_TXT_MSG (0x02)";
        }
        new_tab.messages.push_back(ChatMessage{
            now_hms_str(), "*", false, 0, 0.0f, 0, oss.str(), true, false, false
        });
        if (nd && (nd->contact_type == 2 || nd->contact_type == 3)) {
            new_tab.messages.push_back(ChatMessage{
                now_hms_str(), "*", false, 0, 0.0f, 0,
                "Repeater/Room CLI shortcuts: /rlogin <password>, /rcmd <ver|clock|neighbors|stats|reboot>, or /rstatus",
                true, false, false
            });
        }

        tabs.push_back(std::move(new_tab));
        state_dirty = true;
        size_t idx = tabs.size() - 1;
        if (!silent) {
            center_view = CenterView::Chat;
            select_tab(idx);
        }
        return idx;
    }

    void ingest_direct_message(const std::string& sender_name,
                               const std::array<uint8_t, 32>& sender_key,
                               const std::string& text,
                               int16_t rssi,
                               float snr,
                               uint8_t hops,
                               uint8_t txt_type) {
        (void)sender_key;
        size_t idx = open_dm_tab(sender_name, true);
        std::string display_txt = (txt_type == TXT_TYPE_CLI_DATA) ? ("[CLI] " + text) : text;
        ChatMessage msg{
            now_hms_str(), sender_name, true, rssi, snr, hops, display_txt, false, true, false
        };
        tabs[idx].update_station(sender_name, rssi, snr, hops);
        tabs[idx].messages.push_back(msg);
        trim_tab_messages(tabs[idx], 1000);
        if (idx != active_tab_idx) {
            tabs[idx].unread_count++;
            tabs[idx].has_mention = true;
        }
        if (tabs.size() > 1) {
            ChatMessage pub_notice{
                now_hms_str(),
                sender_name + " -> DM",
                true, rssi, snr, hops,
                display_txt + " (Direct Message in @" + sender_name + ")",
                false, true, false
            };
            tabs[1].update_station(sender_name, rssi, snr, hops);
            tabs[1].messages.push_back(pub_notice);
            trim_tab_messages(tabs[1], 1000);
        }
        log_console("[DIRECT MSG from " + sender_name + "] " + display_txt);
        log_all_heard(sender_name, "[DIRECT MSG -> YOU] " + display_txt, rssi, snr, hops, false, true);
        state_dirty = true;
    }

    void join_channel(const std::string& raw_name, bool silent_startup = false) {
        std::string trimmed_raw = trim_str(raw_name);
        if (!trimmed_raw.empty() && trimmed_raw[0] == '@') {
            open_dm_tab(trimmed_raw, silent_startup);
            return;
        }
        if (_stricmp(trimmed_raw.c_str(), ALL_HEARD_TAB) == 0) {
            for (size_t i = 0; i < tabs.size(); ++i) {
                if (tabs[i].name == ALL_HEARD_TAB) {
                    center_view = CenterView::Chat;
                    select_tab(i);
                    return;
                }
            }
            return;
        }
        if (normalize_channel_name(trimmed_raw) == PUBLIC_CHANNEL_NAME) {
            if (!silent_startup && tabs.size() > 1) {
                center_view = CenterView::Chat;
                select_tab(1);
            }
            return;
        }
        std::string name = (trimmed_raw.size() > 1 && trimmed_raw[0] == '#') ? trimmed_raw : normalize_channel_name(trimmed_raw);
        rainbow.add_channel(name);

        std::array<uint8_t, 16> psk{};
        std::string evicted;
        bool needs_sync = false;
        uint8_t slot_idx = hw_slots.allocate_or_touch(name, psk, evicted, &needs_sync);
        if (silent_startup) {
            if (slot_idx < NUM_HW_CHANNEL_SLOTS && slot_idx > 0) {
                hw_slots.slots[slot_idx].synced = false;
            }
        } else if (needs_sync) {
            transport.sync_channel_slot(slot_idx, name, psk);
        }

        for (size_t i = 0; i < tabs.size(); ++i) {
            if (_stricmp(tabs[i].name.c_str(), name.c_str()) == 0 ||
                normalize_channel_name(tabs[i].name) == normalize_channel_name(name)) {
                select_tab(i);
                return;
            }
        }

        ChannelTab new_tab(name);
        auto it = background_buffers.find(normalize_channel_name(name));
        if (it != background_buffers.end()) {
            new_tab = std::move(it->second);
            background_buffers.erase(it);
        }
        new_tab.unread_count = 0;
        new_tab.has_mention = false;

        std::ostringstream oss;
        oss << "Joined " << name << " — Synced to Radio Slot " << static_cast<int>(slot_idx)
            << " | On-Air Hash: 0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
            << static_cast<int>(compute_channel_hash(psk))
            << " | AES-128 PSK: " << psk_to_hex(psk);

        new_tab.messages.push_back(ChatMessage{
            now_hms_str(), "*", false, 0, 0.0f, 0, oss.str(), true, false, false
        });

        tabs.push_back(std::move(new_tab));
        state_dirty = true;
        if (!silent_startup) {
            select_tab(tabs.size() - 1);
        }
    }

    void select_tab(size_t idx) {
        if (idx < tabs.size()) {
            active_tab_idx = idx;
            tabs[idx].unread_count = 0;
            tabs[idx].has_mention = false;
            tabs[idx].scroll_offset = 0;
            mobile_tab = 0;
        }
    }

    void close_tab(size_t idx) {
        if (idx <= 1 || idx >= tabs.size()) {
            log_active_tab("Cannot close permanent tab (*Console* or Public).");
            return;
        }
        tabs.erase(tabs.begin() + static_cast<ptrdiff_t>(idx));
        if (active_tab_idx >= tabs.size()) {
            active_tab_idx = tabs.size() - 1;
        }
        select_tab(active_tab_idx);
        state_dirty = true;
    }

    void reply_to_user(const std::string& raw_target) {
        std::string user = trim_str(raw_target);
        if (user.empty() || user == "*" || user == "SYSTEM") return;

        // If double-clicked a DM notice mirrored in Public (e.g. "Node -> DM"), switch directly to @Node tab!
        auto dm_pos = user.find(" -> DM");
        if (dm_pos != std::string::npos) {
            std::string dm_user = trim_str(user.substr(0, dm_pos));
            if (!dm_user.empty()) {
                open_dm_tab(dm_user);
                return;
            }
        }

        // If double-clicked a cross-channel message mirrored in Public (e.g. "refracture @ #test"),
        // extract the clean username and automatically switch to that #channel tab!
        auto at_pos = user.find(" @ #");
        if (at_pos != std::string::npos) {
            std::string target_ch = trim_str(user.substr(at_pos + 3));
            user = trim_str(user.substr(0, at_pos));
            if (!target_ch.empty() && target_ch[0] == '#') {
                join_channel(target_ch);
            }
        } else if (active_tab_idx == 0 && tabs.size() > 1) {
            select_tab(1);
        }

        selected_station = user;
        replying_to_user = user;
        active_input = ActiveInputTarget::ChatComposer;

        std::string prefix = "@[" + user + "] ";
        if (chat_input.rfind("@[", 0) == 0) {
            auto close_br = chat_input.find("] ");
            if (close_br != std::string::npos) {
                chat_input = prefix + chat_input.substr(close_br + 2);
            } else {
                chat_input = prefix;
            }
        } else {
            chat_input = prefix + chat_input;
        }
        chat_cursor = chat_input.size();
    }

    void clear_reply_target() {
        if (!replying_to_user.empty()) {
            std::string prefix = "@[" + replying_to_user + "] ";
            if (chat_input.rfind(prefix, 0) == 0) {
                chat_input.erase(0, prefix.size());
            }
            replying_to_user.clear();
            chat_cursor = chat_input.size();
        }
    }

    void attach_image_from_path(const std::wstring& path_w) {
        if (path_w.empty()) return;
        std::string utf8_p = from_wide(path_w);
        size_t slash = utf8_p.find_last_of("/\\");
        std::string fname = (slash == std::string::npos) ? utf8_p : utf8_p.substr(slash + 1);
        if (fname.empty()) fname = "image";
        // Run the conversion pipeline asynchronously on a background worker thread
        std::thread([this, path_w, fname]() {
#ifdef _WIN32
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
            std::array<uint8_t, MONO_IMG_BYTES> bits{};
            std::string err;
            bool ok = load_and_encode_image_file(path_w, bits, err);
#ifdef _WIN32
            if (SUCCEEDED(hr)) CoUninitialize();
#endif
            std::lock_guard<std::mutex> lk(pending_image_mtx);
            async_img_ok = ok;
            async_img_bits = bits;
            async_img_error = err;
            async_img_filename = fname;
            async_img_done = true;
        }).detach();
    }

    void attach_image_from_picker(HWND hwnd) {
        std::wstring picked = open_image_file_dialog(hwnd);
        if (picked.empty()) return;
        std::string utf8_p = from_wide(picked);
        size_t slash = utf8_p.find_last_of("/\\");
        std::string fname = (slash == std::string::npos) ? utf8_p : utf8_p.substr(slash + 1);
        if (fname.empty()) fname = "image";

        std::array<uint8_t, MONO_IMG_BYTES> bits{};
        std::string err;
        if (load_and_encode_image_file(picked, bits, err)) {
            pending_image_bits = bits;
            pending_image_name = fname;
            pending_image_ready = true;
            log_console("[IMAGE ENCODER] Dithered '" + pending_image_name +
                        "' -> 48x30 1-bit Floyd-Steinberg (180B payload / 181B opcode 0x49). Auto-transmitting...");
            confirm_send_pending_image();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            log_active_tab("[IMAGE ENCODER ERROR] " + err);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    void confirm_send_pending_image() {
        if (!pending_image_ready) return;
        std::array<uint8_t, MONO_IMG_BYTES> bits = pending_image_bits;
        std::string img_name = pending_image_name;
        pending_image_ready = false;
        pending_image_name.clear();

        // Normalize through the single-packet 0x49 wire codec so sender & receiver bitmaps and dedup keys match 100%
        std::string wire = encode_mono_img_companion_wire(bits);
        std::array<uint8_t, MONO_IMG_BYTES> wire_bits = bits;
        decode_mono_img_companion_wire(reinterpret_cast<const uint8_t*>(wire.data()), wire.size(), wire_bits);

        center_view = CenterView::Chat;
        if (active_tab_idx == 0 ||
            (active_tab_idx < tabs.size() && tabs[active_tab_idx].name == ALL_HEARD_TAB) ||
            (active_tab_idx < tabs.size() && !tabs[active_tab_idx].name.empty() && tabs[active_tab_idx].name[0] == '@')) {
            select_tab(1); // Switch to Public channel tab if on *Console*, All Heard Members, or @DM tab
        }

        std::string ch = tabs[active_tab_idx].name;
        std::array<uint8_t, 16> psk{};
        std::string evicted;
        bool needs_sync = false;
        uint8_t slot_idx = hw_slots.allocate_or_touch(ch, psk, evicted, &needs_sync);

        if (ch != PUBLIC_CHANNEL_NAME && needs_sync) {
            transport.sync_channel_slot(slot_idx, ch, psk);
        }
        transport.send_channel_mono_image(slot_idx, ch, callsign, wire_bits);

        std::string dedup_payload = hex_string(wire_bits.data(), 24, "");
        std::string dedup_key = normalize_channel_name(ch) + "|" + callsign + "|" + dedup_payload;
        recent_dedup.push_back({dedup_key, now_epoch_secs()});

        std::string label = "[1-Bit Tactical Bitmap 48x30 (180B / Opcode 0x49) — " +
                            (img_name.empty() ? std::string("bitmap") : img_name) + "]";
        ChatMessage msg{
            now_hms_str(), callsign, false, 0, 0.0f, 0, label, false, false, true
        };
        msg.has_mono_image = true;
        msg.mono_image = wire_bits;
        msg.trace_key = dedup_key;
        tabs[active_tab_idx].update_station(callsign, 0, 0.0f, 0);
        tabs[active_tab_idx].messages.push_back(std::move(msg));
        tabs[active_tab_idx].scroll_offset = 0;
        log_console("[TX 0x49 MONO_IMG] Transmitted '" + (img_name.empty() ? std::string("bitmap") : img_name) +
                    "' to " + ch + " (Slot " + std::to_string(slot_idx) + ")");
        state_dirty = true;
    }

    void cancel_pending_image() {
        pending_image_ready = false;
        pending_image_name.clear();
    }

    void submit_chat_input() {
        std::string line = trim_str(chat_input);
        chat_input.clear();
        chat_cursor = 0;
        history_idx = -1;
        replying_to_user.clear();
        if (line.empty()) {
            if (pending_image_ready) {
                confirm_send_pending_image();
            }
            return;
        }

        cmd_history.push_back(line);
        if (line[0] == '/') {
            execute_slash_command(line);
            return;
        }

        if (active_tab_idx == 0) {
            // If user typed a #hashtag in *Console*, join it; otherwise switch to Public and send
            if (line[0] == '#' || line[0] == '@') {
                join_channel(line);
                return;
            }
            select_tab(1);
        } else if (active_tab_idx < tabs.size() && tabs[active_tab_idx].name == ALL_HEARD_TAB) {
            // In "All Heard Members": if a user is targeted via @[user] or selected_station, open a Direct Message to them!
            std::string dm_target;
            if (line.rfind("@[", 0) == 0) {
                auto cb = line.find("]");
                if (cb != std::string::npos && cb > 2) {
                    dm_target = trim_str(line.substr(2, cb - 2));
                    std::string rest_msg = trim_str(line.substr(cb + 1));
                    if (!rest_msg.empty()) line = rest_msg;
                }
            }
            if (dm_target.empty() && !selected_station.empty()) {
                dm_target = selected_station;
            }
            if (!dm_target.empty()) {
                open_dm_tab(dm_target, false);
            } else {
                select_tab(1);
            }
        }

        std::string ch = tabs[active_tab_idx].name;
        if (!ch.empty() && ch[0] == '@') {
            std::string disp_target;
            auto prefix = resolve_dm_prefix(ch.substr(1), disp_target);
            transport.send_direct_message(prefix, disp_target, line, TXT_TYPE_PLAIN);
            tabs[active_tab_idx].messages.push_back(ChatMessage{
                now_hms_str(), callsign, false, 0, 0.0f, 0, line, false, false, true
            });
            tabs[active_tab_idx].scroll_offset = 0;
            state_dirty = true;
            return;
        }

        std::array<uint8_t, 16> psk{};
        std::string evicted;
        bool needs_sync = false;
        uint8_t slot_idx = hw_slots.allocate_or_touch(ch, psk, evicted, &needs_sync);

        for (const auto& tag : extract_hashtags(line)) {
            rainbow.add_channel(tag);
            register_or_update_discovered(tag, false, 0, 0.0f, true);
        }

        if (ch != PUBLIC_CHANNEL_NAME && needs_sync) {
            transport.sync_channel_slot(slot_idx, ch, psk);
        }
        transport.send_channel_message(slot_idx, ch, callsign, line);

        tabs[active_tab_idx].update_station(callsign, 0, 0.0f, 0);
        tabs[active_tab_idx].scan_text_mentions(line);
        tabs[active_tab_idx].messages.push_back(ChatMessage{
            now_hms_str(), callsign, false, 0, 0.0f, 0, line, false, false, true
        });
        tabs[active_tab_idx].scroll_offset = 0;
        state_dirty = true;
    }

    void execute_slash_command(const std::string& raw) {
        std::istringstream iss(raw);
        std::string cmd;
        iss >> cmd;
        std::string rest;
        std::getline(iss, rest);
        rest = trim_str(rest);

        std::transform(cmd.begin(), cmd.end(), cmd.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (cmd == "/join" || cmd == "/j") {
            if (!rest.empty()) join_channel(rest);
            else log_active_tab("Usage: /join #channel  (or /dm <node> [message])");
        } else if (cmd == "/dm" || cmd == "/msg" || cmd == "/query") {
            if (rest.empty()) {
                log_active_tab("Usage: /dm <node_name_or_key_prefix> [optional message]");
            } else {
                std::string target_node;
                std::string dm_text;
                // First check if rest starts with a quoted name or matches a known node name prefix
                if (rest[0] == '"') {
                    auto q2 = rest.find('"', 1);
                    if (q2 != std::string::npos) {
                        target_node = rest.substr(1, q2 - 1);
                        dm_text = trim_str(rest.substr(q2 + 1));
                    }
                }
                if (target_node.empty()) {
                    // Check longest matching known node name at start of rest
                    size_t best_len = 0;
                    for (const auto& nd : mesh_nodes) {
                        if (!nd.name.empty() && rest.size() >= nd.name.size() &&
                            _strnicmp(rest.c_str(), nd.name.c_str(), nd.name.size()) == 0 &&
                            (rest.size() == nd.name.size() || rest[nd.name.size()] == ' ')) {
                            if (nd.name.size() > best_len) {
                                best_len = nd.name.size();
                                target_node = nd.name;
                                dm_text = trim_str(rest.substr(best_len));
                            }
                        }
                    }
                }
                if (target_node.empty()) {
                    std::istringstream riss(rest);
                    riss >> target_node;
                    std::getline(riss, dm_text);
                    dm_text = trim_str(dm_text);
                }
                size_t tidx = open_dm_tab(target_node, false);
                if (!dm_text.empty() && tidx < tabs.size()) {
                    std::string disp_target;
                    auto prefix = resolve_dm_prefix(tabs[tidx].name.substr(1), disp_target);
                    transport.send_direct_message(prefix, disp_target, dm_text, TXT_TYPE_PLAIN);
                    tabs[tidx].messages.push_back(ChatMessage{
                        now_hms_str(), callsign, false, 0, 0.0f, 0, dm_text, false, false, true
                    });
                    tabs[tidx].scroll_offset = 0;
                    state_dirty = true;
                }
            }
        } else if (cmd == "/part" || cmd == "/close") {
            close_tab(active_tab_idx);
        } else if (cmd == "/sim") {
            transport.toggle_simulator();
        } else if (cmd == "/serial") {
            transport.connect_serial(rest);
        } else if (cmd == "/ble") {
            transport.connect_ble();
        } else if (cmd == "/nick") {
            if (!rest.empty()) {
                callsign = rest;
                std::transform(callsign.begin(), callsign.end(), callsign.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                state_dirty = true;
                log_active_tab("Operator callsign updated to: " + callsign);
            }
        } else if (cmd == "/addword") {
            if (!rest.empty()) {
                auto cand = rainbow.add_and_persist_word(rest);
                register_or_update_discovered(cand.name, false, 0, 0.0f, true);
                state_dirty = true;
                log_active_tab("Added " + cand.name + " (PSK: " + psk_to_hex(cand.psk) + ") to 256-bucket rainbow table & wordlist.txt");
            }
        } else if (cmd == "/slots") {
            log_active_tab("--- Radio Hardware Channel Slots (0..7 LRU) ---");
            for (const auto& s : hw_slots.slots) {
                if (s.channel_name.empty()) {
                    log_active_tab("  Slot " + std::to_string(s.slot_idx) + ": <empty>");
                } else {
                    std::ostringstream oss;
                    oss << "  Slot " << static_cast<int>(s.slot_idx) << ": " << s.channel_name
                        << " | Hash 0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                        << static_cast<int>(s.channel_hash) << " | PSK " << psk_to_hex(s.psk);
                    log_active_tab(oss.str());
                }
            }
        } else if (cmd == "/list") {
            show_all_rainbow_in_sniffer = true;
            sniffer_filter = rest;
            log_active_tab("Switched Left Sniffer Dock to browse all " +
                           std::to_string(rainbow.total_candidates()) + " rainbow channels.");
        } else if (cmd == "/clear") {
            tabs[active_tab_idx].messages.clear();
            tabs[active_tab_idx].scroll_offset = 0;
            state_dirty = true;
        } else if (cmd == "/save") {
            save_state();
            log_active_tab("Saved session state (channels, contacts, messages, map/layout) to meshcore_state.tsv");
        } else if (cmd == "/packets" || cmd == "/inspector") {
            center_view = CenterView::Packets;
        } else if (cmd == "/adverts") {
            center_view = CenterView::Adverts;
        } else if (cmd == "/paths" || cmd == "/trace") {
            center_view = CenterView::Paths;
        } else if (cmd == "/map") {
            center_view = CenterView::Map;
        } else if (cmd == "/chat") {
            center_view = CenterView::Chat;
        } else if (cmd == "/advert" || cmd == "/beacon") {
            bool flood = true;
            if (!rest.empty()) {
                std::string arg = rest;
                std::transform(arg.begin(), arg.end(), arg.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (arg == "direct" || arg == "0" || arg == "zero") flood = false;
            }
            send_self_advert_beacon(flood);
        } else if (cmd == "/tracepath" || cmd == "/ping") {
            if (rest.empty()) {
                if (active_tab_idx < tabs.size() && !tabs[active_tab_idx].name.empty() && tabs[active_tab_idx].name[0] == '@') {
                    trace_to_node(tabs[active_tab_idx].name.substr(1));
                } else {
                    log_active_tab("Usage: /tracepath <node_name_or_key_prefix>  (or /ping <node>)");
                }
            } else {
                trace_to_node(rest);
            }
        } else if (cmd == "/rlogin" || cmd == "/login") {
            if (rest.empty()) {
                log_active_tab("Usage: /rlogin [repeater_name] <password>");
            } else {
                // Split last word as password if multiple words and first matches a repeater, else if in @tab use whole rest as password
                std::string target_node, pwd;
                size_t best_len = 0;
                for (const auto& nd : mesh_nodes) {
                    if (!nd.name.empty() && rest.size() > nd.name.size() &&
                        _strnicmp(rest.c_str(), nd.name.c_str(), nd.name.size()) == 0 &&
                        rest[nd.name.size()] == ' ') {
                        if (nd.name.size() > best_len) {
                            best_len = nd.name.size();
                            target_node = nd.name;
                            pwd = trim_str(rest.substr(best_len));
                        }
                    }
                }
                if (target_node.empty()) {
                    auto sp = rest.rfind(' ');
                    if (sp != std::string::npos && !(active_tab_idx < tabs.size() && !tabs[active_tab_idx].name.empty() && tabs[active_tab_idx].name[0] == '@')) {
                        target_node = trim_str(rest.substr(0, sp));
                        pwd = trim_str(rest.substr(sp + 1));
                    } else {
                        pwd = rest;
                    }
                }
                repeater_login(target_node, pwd);
            }
        } else if (cmd == "/cli") {
            repeater_cli_cmd("", rest);
        } else if (cmd == "/rcmd") {
            if (rest.empty()) {
                log_active_tab("Usage: /rcmd <repeater_name> <cli_command>  (or /cli <ver|neighbors|clock|stats|reboot>)");
            } else {
                std::string target_node, cli_c;
                size_t best_len = 0;
                for (const auto& nd : mesh_nodes) {
                    if (!nd.name.empty() && rest.size() > nd.name.size() &&
                        _strnicmp(rest.c_str(), nd.name.c_str(), nd.name.size()) == 0 &&
                        rest[nd.name.size()] == ' ') {
                        if (nd.name.size() > best_len) {
                            best_len = nd.name.size();
                            target_node = nd.name;
                            cli_c = trim_str(rest.substr(best_len));
                        }
                    }
                }
                if (target_node.empty()) {
                    std::istringstream riss(rest);
                    riss >> target_node;
                    std::getline(riss, cli_c);
                    cli_c = trim_str(cli_c);
                    if (cli_c.empty()) {
                        // Single token: treat as CLI command to active/selected repeater
                        cli_c = target_node;
                        target_node.clear();
                    }
                }
                repeater_cli_cmd(target_node, cli_c);
            }
        } else if (cmd == "/rstatus" || cmd == "/status") {
            repeater_status_req(rest);
        } else if (cmd == "/rtelemetry" || cmd == "/rtel" || cmd == "/telemetry") {
            repeater_telemetry_req(rest);
        } else if (cmd == "/resetpath") {
            reset_node_path(rest);
        } else if (cmd == "/record" || cmd == "/rec") {
            toggle_session_recording(rest);
        } else if (cmd == "/replay") {
            replay_session_file(rest);
        } else if (cmd == "/image" || cmd == "/img" || cmd == "/bitmap") {
            if (rest.empty()) {
                attach_image_from_picker(nullptr);
            } else {
                attach_image_from_path(to_wide(rest));
            }
        } else if (cmd == "/export") {
            std::string what = rest;
            std::transform(what.begin(), what.end(), what.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (what == "nodes" || what == "csv") export_nodes_csv();
            else if (what == "kml" || what == "map") export_kml();
            else if (what == "packets") export_packets_csv();
            else if (what == "adverts") export_adverts_csv();
            else if (what == "all" || what.empty()) export_all();
            else log_active_tab("Usage: /export [nodes|kml|packets|adverts|all]");
        } else if (cmd == "/users" || cmd == "/names" || cmd == "/who") {
            show_room_nicklist_override = (show_room_nicklist_override == 1) ? -1 : 1;
            if (active_tab_idx < tabs.size()) {
                const auto& t = tabs[active_tab_idx];
                std::ostringstream oss;
                size_t cnt = 1;
                for (const auto& st : t.nicks) {
                    if (_stricmp(st.callsign.c_str(), callsign.c_str()) != 0) ++cnt;
                }
                oss << "[ROOM USERS in " << t.name << " (" << cnt << ")]: +" << callsign << " [YOU]";
                for (const auto& st : t.nicks) {
                    if (_stricmp(st.callsign.c_str(), callsign.c_str()) == 0) continue;
                    const auto* nd = find_mesh_node(st.callsign);
                    char prefix = (nd && nd->contact_type == 3) ? '@' : (nd && nd->contact_type == 2) ? '^' : '+';
                    oss << ", " << prefix << st.callsign;
                    if (st.msg_count > 0) oss << " (" << st.msg_count << "m)";
                }
                log_active_tab(oss.str());
            }
        } else if (cmd == "/help") {
            log_active_tab("Commands: /join #tag, /users (/names), /image [file], /dm <node> [msg], /advert [flood|direct], /tracepath <node>, /part, /serial [COMx], /ble, /sim, /nick <CALL>, /addword #tag, /slots, /list, /clear, /save");
            log_active_tab("Repeater Admin & Capture: /rlogin [rpt] <pass>, /rcmd [rpt] <ver|neighbors|clock|stats|reboot>, /cli <cmd>, /rstatus [rpt], /rtelemetry [rpt], /resetpath [node], /record [on|off], /replay [file]");
            log_active_tab("Snooping: /packets (hex inspector), /adverts (advert log), /paths (relay path tracer), /map, /chat, /export [nodes|kml|packets|adverts|all]");
        } else {
            log_active_tab("Unknown command '" + cmd + "'. Try /help or click any GUI control.");
        }
    }
};

// ============================================================================
// Direct2D + DirectWrite Renderer
// ============================================================================
enum ButtonId {
    BTN_USB_SERIAL = 1,
    BTN_BLE_NUS = 2,
    BTN_RF_SIM = 3,
    BTN_ADD_TAG = 4,
    BTN_CALLSIGN = 5,
    BTN_COPY_PSK = 6,
    BTN_SNIFFER_MODE_LIVE = 7,
    BTN_SNIFFER_MODE_ALL = 8,
    BTN_SNIFFER_FILTER_BOX = 9,
    BTN_CHAT_INPUT_BOX = 10,
    BTN_TRANSMIT_TX = 11,
    BTN_QUICK_EMCOMM = 12,
    BTN_QUICK_MESHCORE = 13,
    BTN_QUICK_WEATHER = 14,
    BTN_QUICK_HAMRADIO = 15,
    BTN_QUICK_GENERAL = 16,
    BTN_QUICK_SLOTS = 17,
    BTN_CLEAR_REPLY = 18,
    BTN_TOGGLE_CHANNELS = 19,
    BTN_TOGGLE_MESH = 20,
    BTN_SWAP_DOCKS = 21,
    BTN_CLOSE_CHANNELS = 22,
    BTN_CLOSE_MESH = 23,
    BTN_VIEW_CHAT = 30,
    BTN_VIEW_PACKETS = 31,
    BTN_VIEW_ADVERTS = 32,
    BTN_VIEW_PATHS = 33,
    BTN_EXPORT_NODES = 34,
    BTN_EXPORT_KML = 35,
    BTN_EXPORT_PACKETS = 36,
    BTN_EXPORT_ADVERTS = 37,
    BTN_EXPORT_ALL = 38,
    BTN_PKT_FILTER_ALL = 40,
    BTN_PKT_FILTER_RF = 41,
    BTN_PKT_FILTER_COMP = 42,
    BTN_PKT_FILTER_TX = 43,
    BTN_PKT_CLEAR = 44,
    BTN_PKT_FOLLOW = 45,
    BTN_PKT_COPY_HEX = 46,
    BTN_ADV_CLEAR = 47,
    BTN_PATH_CLEAR = 48,
    BTN_VIEW_MAP = 49,
    BTN_MAP_FIT = 50,
    BTN_MAP_REPEATERS = 51,
    BTN_MAP_LINKS = 52,
    BTN_MAP_STYLE = 53,
    BTN_MAP_ZOOM_IN = 54,
    BTN_MAP_ZOOM_OUT = 55,
    BTN_MAP_MESSAGE = 56,
    BTN_MAP_CLOSE_CARD = 57,
    BTN_MAP_CENTER = 58,
    BTN_SEND_ADVERT = 59,
    BTN_MAP_TRACE = 60,
    BTN_MAP_RSTATUS = 61,
    BTN_MAP_RVER = 62,
    BTN_MAP_RNEIGHBORS = 63,
    BTN_MAP_RESET_PATH = 64,
    BTN_PKT_RECORD = 65,
    BTN_PKT_REPLAY = 66,
    BTN_TOGGLE_THEME = 67,
    BTN_RESET_LAYOUT = 68,
    BTN_ATTACH_IMAGE = 69,
    BTN_CONFIRM_SEND_IMAGE = 70,
    BTN_CANCEL_SEND_IMAGE = 71,
    BTN_MOB_TAB_CHAT = 72,
    BTN_MOB_TAB_NODES = 73,
    BTN_MOB_TAB_SNIFFER = 74,
    BTN_MOB_TAB_CHANNELS = 75,
    BTN_MOB_TAB_RADIO = 76,
    BTN_TOGGLE_NICKLIST = 77,
    BTN_NICKLIST_FILTER_BOX = 78,
    BTN_NICKLIST_FILTER_CLEAR = 79
};

class D2DRenderer {
public:
    ID2D1Factory* d2d_factory = nullptr;
    IDWriteFactory* dwrite_factory = nullptr;
    ID2D1DCRenderTarget* rt = nullptr;
    bool light_mode = false;

    IDWriteTextFormat* font_title = nullptr;
    IDWriteTextFormat* font_ui = nullptr;
    IDWriteTextFormat* font_ui_bold = nullptr;
    IDWriteTextFormat* font_small = nullptr;
    IDWriteTextFormat* font_mono = nullptr;
    IDWriteTextFormat* font_mono_small = nullptr;

    // ---- Map tile bitmaps (device-dependent: dropped when the render target is recreated) ----
    IWICImagingFactory* wic = nullptr;
    struct TileBmp { ID2D1Bitmap* bmp = nullptr; uint64_t last_used = 0; };
    std::unordered_map<uint64_t, TileBmp> tile_bitmaps;
    uint64_t frame_no = 0;
    int decodes_this_frame = 0;

    void release_tiles() {
        for (auto& kv : tile_bitmaps) {
            if (kv.second.bmp) kv.second.bmp->Release();
            g_tiles.forget(kv.first);
        }
        tile_bitmaps.clear();
    }

    ID2D1Bitmap* decode_png(const std::vector<uint8_t>& bytes) {
#ifdef _WIN32
        if (!wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (!wic || !rt) return nullptr;
        ID2D1Bitmap* out = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        if (SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) &&
            SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeMedianCut))) {
            rt->CreateBitmapFromWicBitmap(conv, nullptr, &out);
        }
        if (conv) conv->Release();
        if (frame) frame->Release();
        if (dec) dec->Release();
        if (stream) stream->Release();
        return out;
#elif defined(__APPLE__)
        return mac_decode_png_bytes(bytes);
#endif
    }

    // Returns a ready bitmap for the tile, pulling a finished download if one is waiting. Never blocks.
    ID2D1Bitmap* get_tile(uint64_t k) {
        auto it = tile_bitmaps.find(k);
        if (it != tile_bitmaps.end()) {
            it->second.last_used = frame_no;
            return it->second.bmp;
        }
        if (decodes_this_frame >= 8) return nullptr; // spread decoding across frames to keep panning smooth
        std::vector<uint8_t> bytes;
        if (!g_tiles.take(k, bytes)) return nullptr;
        ++decodes_this_frame;
        ID2D1Bitmap* bmp = decode_png(bytes);
        if (!bmp) {
            g_tiles.forget(k);
            return nullptr;
        }
        tile_bitmaps[k] = {bmp, frame_no};
        if (tile_bitmaps.size() > 400) {
            for (auto e = tile_bitmaps.begin(); e != tile_bitmaps.end();) {
                if (e->second.last_used + 90 < frame_no) {
                    e->second.bmp->Release();
                    g_tiles.forget(e->first);
                    e = tile_bitmaps.erase(e);
                } else {
                    ++e;
                }
            }
        }
        return bmp;
    }

    D2DRenderer() {
#ifdef _WIN32
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&dwrite_factory));
#elif defined(__APPLE__)
        d2d_factory = new ID2D1Factory();
        dwrite_factory = new IDWriteFactory();
        rt = new ID2D1DCRenderTarget();
#endif

        if (dwrite_factory) {
            dwrite_factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             16.0f, L"en-us", &font_title);
            dwrite_factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             13.5f, L"en-us", &font_ui);
            dwrite_factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             13.5f, L"en-us", &font_ui_bold);
            dwrite_factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             11.5f, L"en-us", &font_small);
            dwrite_factory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             13.5f, L"en-us", &font_mono);
            dwrite_factory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             11.5f, L"en-us", &font_mono_small);

            if (font_ui) font_ui->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (font_ui_bold) font_ui_bold->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (font_small) font_small->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (font_mono) font_mono->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (font_mono_small) font_mono_small->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    ~D2DRenderer() {
        if (font_title) font_title->Release();
        if (font_ui) font_ui->Release();
        if (font_ui_bold) font_ui_bold->Release();
        if (font_small) font_small->Release();
        if (font_mono) font_mono->Release();
        if (font_mono_small) font_mono_small->Release();
        for (auto& kv : tile_bitmaps) if (kv.second.bmp) kv.second.bmp->Release();
        tile_bitmaps.clear();
        if (wic) wic->Release();
        if (rt) rt->Release();
        if (dwrite_factory) dwrite_factory->Release();
        if (d2d_factory) d2d_factory->Release();
    }

    void ensure_target() {
#ifdef _WIN32
        if (!rt && d2d_factory) {
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96.0f, 96.0f,
                D2D1_RENDER_TARGET_USAGE_NONE,
                D2D1_FEATURE_LEVEL_DEFAULT);
            d2d_factory->CreateDCRenderTarget(&props, &rt);
            if (rt) {
                rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
            }
        }
#elif defined(__APPLE__)
        if (!rt) rt = new ID2D1DCRenderTarget();
#endif
    }

    void resize(UINT, UINT) {}

    D2D1_COLOR_F hex_color(uint32_t rgb, float alpha = 1.0f) const {
        if (light_mode) {
            switch (rgb) {
                // Backgrounds & Surfaces (Dark -> Crisp Light Slate/White)
                case 0x05080F: rgb = 0xE2E8F0; break;
                case 0x090D16: rgb = 0xEEF2F6; break;
                case 0x0B111E: rgb = 0xF1F5F9; break;
                case 0x0F172A: rgb = 0xFFFFFF; break;
                case 0x111A2B: rgb = 0xF8FAFC; break;
                case 0x131C2E: rgb = 0xF1F5F9; break;
                case 0x141E30: rgb = 0xF1F5F9; break;
                case 0x162032: rgb = 0xE2E8F0; break;
                case 0x162238: rgb = 0xE0F2FE; break;
                case 0x16243D: rgb = 0xE0F2FE; break;
                // Borders & Dividers
                case 0x1E293B: rgb = 0xCBD5E1; break;
                case 0x26354D: rgb = 0x94A3B8; break;
                case 0x334155: rgb = 0x94A3B8; break;
                case 0x475569: rgb = 0x64748B; break;
                // Foregrounds & Text (Light -> Deep Ink Slate)
                case 0xF8FAFC: rgb = 0x0F172A; break;
                case 0xF1F5F9: rgb = 0x0F172A; break;
                case 0xE2E8F0: rgb = 0x1E293B; break;
                case 0xCBD5E1: rgb = 0x334155; break;
                case 0x94A3B8: rgb = 0x475569; break;
                case 0x64748B: rgb = 0x64748B; break;
                // Tactical Accents (Tuned for high readability on white/light surfaces)
                case 0x00E5FF: rgb = 0x0284C7; break;
                case 0x38BDF8: rgb = 0x0369A1; break;
                case 0x10B981: rgb = 0x059669; break;
                case 0x34D399: rgb = 0x047857; break;
                case 0xFBBF24: rgb = 0xB45309; break;
                case 0xF59E0B: rgb = 0xD97706; break;
                case 0xA855F7: rgb = 0x7E22CE; break;
                case 0xC084FC: rgb = 0x6B21A8; break;
                case 0xA78BFA: rgb = 0x5B21B6; break;
                case 0xF472B6: rgb = 0xBE185D; break;
                case 0x2DD4BF: rgb = 0x0F766E; break;
                case 0xFB923C: rgb = 0xC2410C; break;
                case 0x60A5FA: rgb = 0x1D4ED8; break;
                case 0xFCA5A5: rgb = 0xB91C1C; break;
                case 0xEF4444: rgb = 0xDC2626; break;
                default: break;
            }
        }
        return D2D1::ColorF(
            static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
            static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(rgb & 0xFF) / 255.0f,
            alpha);
    }

    static bool pt_in_rect(POINT pt, const D2D1_RECT_F& r) {
        return pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom;
    }

    void fill_round_rect(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F color) {
        ID2D1SolidColorBrush* b = nullptr;
        rt->CreateSolidColorBrush(color, &b);
        if (b) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
            b->Release();
        }
    }

    void draw_round_rect(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F color, float stroke = 1.0f) {
        ID2D1SolidColorBrush* b = nullptr;
        rt->CreateSolidColorBrush(color, &b);
        if (b) {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, stroke);
            b->Release();
        }
    }

    void draw_text(const std::string& utf8,
                   IDWriteTextFormat* fmt,
                   const D2D1_RECT_F& r,
                   D2D1_COLOR_F color,
                   DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || utf8.empty()) return;
        fmt->SetTextAlignment(align);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ID2D1SolidColorBrush* b = nullptr;
        rt->CreateSolidColorBrush(color, &b);
        if (b) {
            std::wstring w = to_wide(utf8);
            rt->DrawText(w.c_str(), static_cast<UINT32>(w.size()), fmt, r, b,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
            b->Release();
        }
    }

    // Unpacks a 180-byte 1-bit bitmap into a stack-allocated 1,440-pixel (48x30) buffer and renders it
    // at pixel_scale (e.g. 5x -> 240x150 screen pixels) using strictly Nearest-Neighbor interpolation.
    void draw_mono_bitmap_nearest(const std::array<uint8_t, MONO_IMG_BYTES>& bits,
                                  float x,
                                  float y,
                                  float pixel_scale = 5.0f) {
        if (!rt) return;
        uint8_t pixels[MONO_IMG_PIXELS]; // 1,440 bytes stack-allocated
        unpack_mono_bitmap(bits, pixels);

        float cw = static_cast<float>(MONO_IMG_W) * pixel_scale;
        float ch = static_cast<float>(MONO_IMG_H) * pixel_scale;
        D2D1_RECT_F frame_r = D2D1::RectF(x - 2.0f, y - 2.0f, x + cw + 2.0f, y + ch + 2.0f);
        D2D1_RECT_F canvas_r = D2D1::RectF(x, y, x + cw, y + ch);
        D2D1_RECT_F src_r = D2D1::RectF(0.0f, 0.0f, static_cast<float>(MONO_IMG_W), static_cast<float>(MONO_IMG_H));

        fill_round_rect(frame_r, 4.0f, D2D1::ColorF(0.02f, 0.03f, 0.06f, 1.0f));
        draw_round_rect(frame_r, 4.0f, hex_color(0x00E5FF, 0.75f), 1.2f);

        bool drawn = false;
#ifdef _WIN32
        // Stack-allocated 48x30 BGRA texture (5,760 bytes on stack)
        uint32_t bgra[MONO_IMG_PIXELS];
        for (size_t i = 0; i < MONO_IMG_PIXELS; ++i) {
            bgra[i] = pixels[i] ? 0xFFF8FAFCu : 0xFF05080Fu;
        }
        ID2D1Bitmap* bmp = nullptr;
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(MONO_IMG_W, MONO_IMG_H), bgra, MONO_IMG_W * 4, &props, &bmp)) && bmp) {
            rt->DrawBitmap(bmp, canvas_r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src_r);
            bmp->Release();
            drawn = true;
        }
#elif defined(__APPLE__)
        // Stack-allocated 48x30 RGBA texture (5,760 bytes on stack) rendered with kCGInterpolationNone
        uint8_t rgba[MONO_IMG_PIXELS * 4];
        for (size_t i = 0; i < MONO_IMG_PIXELS; ++i) {
            if (pixels[i]) {
                rgba[i * 4 + 0] = 0xF8;
                rgba[i * 4 + 1] = 0xFA;
                rgba[i * 4 + 2] = 0xFC;
                rgba[i * 4 + 3] = 0xFF;
            } else {
                rgba[i * 4 + 0] = 0x05;
                rgba[i * 4 + 1] = 0x08;
                rgba[i * 4 + 2] = 0x0F;
                rgba[i * 4 + 3] = 0xFF;
            }
        }
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        if (cs) {
            CGContextRef bctx = CGBitmapContextCreate(rgba, MONO_IMG_W, MONO_IMG_H, 8, MONO_IMG_W * 4, cs,
                                                      1u | (3u << 12)); // kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big
            if (bctx) {
                CGImageRef cg_img = CGBitmapContextCreateImage(bctx);
                if (cg_img) {
                    ID2D1Bitmap bmp(cg_img);
                    rt->DrawBitmap(&bmp, canvas_r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src_r);
                    drawn = true;
                }
                CGContextRelease(bctx);
            }
            CGColorSpaceRelease(cs);
        }
#endif
        if (!drawn) {
            // Exact integer-aligned Nearest-Neighbor block fallback
            ID2D1SolidColorBrush* bg_b = nullptr;
            ID2D1SolidColorBrush* fg_b = nullptr;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.02f, 0.03f, 0.06f, 1.0f), &bg_b);
            rt->CreateSolidColorBrush(D2D1::ColorF(0.97f, 0.98f, 0.99f, 1.0f), &fg_b);
            if (bg_b && fg_b) {
                rt->FillRectangle(canvas_r, bg_b);
                for (size_t py = 0; py < MONO_IMG_H; ++py) {
                    float ry0 = y + static_cast<float>(py) * pixel_scale;
                    float ry1 = ry0 + pixel_scale;
                    size_t px = 0;
                    while (px < MONO_IMG_W) {
                        if (!pixels[py * MONO_IMG_W + px]) { ++px; continue; }
                        size_t run_end = px + 1;
                        while (run_end < MONO_IMG_W && pixels[py * MONO_IMG_W + run_end]) ++run_end;
                        rt->FillRectangle(D2D1::RectF(x + static_cast<float>(px) * pixel_scale, ry0,
                                                      x + static_cast<float>(run_end) * pixel_scale, ry1), fg_b);
                        px = run_end;
                    }
                }
            }
            if (bg_b) bg_b->Release();
            if (fg_b) fg_b->Release();
        }
    }

    void draw_signal_bars(float x, float y, float snr) {
        int bars = 1;
        if (snr >= 8.0f) bars = 5;
        else if (snr >= 5.0f) bars = 4;
        else if (snr >= 2.0f) bars = 3;
        else if (snr >= -2.0f) bars = 2;

        D2D1_COLOR_F active_col = (bars >= 4) ? hex_color(0x10B981)
                                : (bars >= 2) ? hex_color(0x00E5FF)
                                              : hex_color(0xF59E0B);
        D2D1_COLOR_F dim_col = hex_color(0x1E293B);

        for (int i = 0; i < 5; ++i) {
            float bh = 4.0f + i * 2.2f;
            float bx = x + i * 4.0f;
            float by = y + (13.0f - bh);
            fill_round_rect(D2D1::RectF(bx, by, bx + 2.8f, y + 13.0f), 1.0f,
                            (i < bars) ? active_col : dim_col);
        }
    }

    D2D1_COLOR_F callsign_color(const std::string& call) {
        static const uint32_t PALETTE[] = {
            0x38BDF8, 0x34D399, 0xFBBF24, 0xF472B6, 0xA78BFA, 0x2DD4BF, 0xFB923C, 0x60A5FA
        };
        size_t sum = 0;
        for (unsigned char c : call) sum += c;
        return hex_color(PALETTE[sum % 8]);
    }

    void render(HWND hwnd, HDC target_hdc, MeshGuiApp& app) {
        ensure_target();
        if (!rt || !target_hdc) return;
        light_mode = app.light_mode;

        RECT rc;
        GetClientRect(hwnd, &rc);
        int w_px = rc.right - rc.left;
        int h_px = rc.bottom - rc.top;
        if (w_px <= 0 || h_px <= 0) return;

        HDC mem_dc = CreateCompatibleDC(target_hdc);
        HBITMAP mem_bmp = CreateCompatibleBitmap(target_hdc, w_px, h_px);
        HGDIOBJ old_bmp = SelectObject(mem_dc, mem_bmp);

        rt->BindDC(mem_dc, &rc);

        app.tab_hits.clear();
        app.tab_close_hits.clear();
        app.joined_hits.clear();
        app.discovered_hits.clear();
        app.button_hits.clear();
        app.chat_hashtag_hits.clear();
        app.chat_user_hits.clear();
        app.nicklist_hits.clear();
        app.station_hits.clear();
        app.slot_hits.clear();
        app.serial_menu_hits.clear();
        app.ble_menu_hits.clear();
        app.packet_row_hits.clear();
        app.map_pin_hits.clear();
        app.vert_splitter_hits.clear();
        app.horiz_splitter_hits.clear();
        app.module_header_hits.clear();
        ++frame_no;
        decodes_this_frame = 0;

        float W = static_cast<float>(w_px);
        float H = static_cast<float>(h_px);

        rt->BeginDraw();
        rt->Clear(hex_color(0x090D16));

        ID2D1SolidColorBrush* line_brush = nullptr;
        rt->CreateSolidColorBrush(hex_color(0x1E293B), &line_brush);

#ifdef MESHCORE_IOS
        constexpr bool force_mobile = true;
        const float safe_top = 48.0f;
        const float safe_bot = 22.0f;
#else
        constexpr bool force_mobile = false;
        const float safe_top = 0.0f;
        const float safe_bot = 0.0f;
#endif
        const bool is_mobile = force_mobile || (W < 700.0f);

        if (is_mobile) {
            float top_h = safe_top + 46.0f;
            fill_round_rect(D2D1::RectF(0, 0, W, top_h), 0.0f, hex_color(0x0F172A));
            if (line_brush) {
                rt->DrawLine(D2D1::Point2F(0, top_h), D2D1::Point2F(W, top_h), line_brush, 1.5f);
            }

            float ty = safe_top + 7.0f;
            float th = 32.0f;
            draw_text("MESHCORE++", font_ui_bold, D2D1::RectF(10, ty, 118, ty + th), hex_color(0x00E5FF));

            auto draw_mob_top_btn = [&](int id, float x, float w, const std::string& lbl, bool active, uint32_t acc) {
                D2D1_RECT_F r = D2D1::RectF(x, ty, x + w, ty + th);
                fill_round_rect(r, 6.0f, active ? hex_color(acc, 0.22f) : hex_color(0x131C2E));
                draw_round_rect(r, 6.0f, active ? hex_color(acc) : hex_color(0x26354D), 1.2f);
                draw_text(lbl, font_small, r, active ? hex_color(acc) : hex_color(0xE2E8F0), DWRITE_TEXT_ALIGNMENT_CENTER);
                app.button_hits.push_back({r, id, ""});
            };

            bool ble_act = (!app.is_sim && app.transport_label.find("BLE") != std::string::npos) || app.ble_dropdown_open;
            draw_mob_top_btn(BTN_BLE_NUS, 114, 72, ble_act ? "BLE: ON" : "BLE v", ble_act, 0x38BDF8);
            draw_mob_top_btn(BTN_ADD_TAG, 190, 56, "+ #Tag", false, 0xA855F7);
            draw_mob_top_btn(BTN_TOGGLE_THEME, 250, 52, app.light_mode ? "Dark" : "Light", app.light_mode, 0xFBBF24);

            char bat_s[32];
            std::snprintf(bat_s, sizeof(bat_s), "%u%%", app.battery_pct);
            D2D1_RECT_F bat_r = D2D1::RectF(W - 58.0f, ty, W - 8.0f, ty + th);
            fill_round_rect(bat_r, 6.0f, hex_color(0x111A2B));
            draw_round_rect(bat_r, 6.0f, hex_color(0x10B981, 0.6f), 1.0f);
            draw_text(bat_s, font_mono_small, bat_r, hex_color(0x34D399), DWRITE_TEXT_ALIGNMENT_CENTER);

            float nav_h = 46.0f;
            float nav_y = H - safe_bot - nav_h;
            fill_round_rect(D2D1::RectF(0, nav_y, W, H), 0.0f, hex_color(0x0F172A));
            if (line_brush) {
                rt->DrawLine(D2D1::Point2F(0, nav_y), D2D1::Point2F(W, nav_y), line_brush, 1.5f);
            }

            struct MobNavItem { int id; int tab; const char* label; uint32_t color; };
            static const MobNavItem NAV_ITEMS[5] = {
                {BTN_MOB_TAB_CHAT,     0, "Chat",     0x00E5FF},
                {BTN_MOB_TAB_NODES,    1, "Nodes",    0x10B981},
                {BTN_MOB_TAB_SNIFFER,  2, "#Sniffer", 0xA855F7},
                {BTN_MOB_TAB_CHANNELS, 3, "Chans",    0x38BDF8},
                {BTN_MOB_TAB_RADIO,    4, "Radio",    0xFBBF24}
            };
            float tab_w = (W - 16.0f) / 5.0f;
            for (int i = 0; i < 5; ++i) {
                float tx = 8.0f + i * tab_w;
                D2D1_RECT_F tr = D2D1::RectF(tx + 2.0f, nav_y + 5.0f, tx + tab_w - 2.0f, nav_y + nav_h - 5.0f);
                bool sel = (app.mobile_tab == NAV_ITEMS[i].tab);
                fill_round_rect(tr, 7.0f, sel ? hex_color(NAV_ITEMS[i].color, 0.22f) : hex_color(0x131C2E));
                draw_round_rect(tr, 7.0f, sel ? hex_color(NAV_ITEMS[i].color) : hex_color(0x26354D), sel ? 1.5f : 1.0f);
                draw_text(NAV_ITEMS[i].label, font_ui_bold, tr,
                          sel ? hex_color(NAV_ITEMS[i].color) : hex_color(0x94A3B8), DWRITE_TEXT_ALIGNMENT_CENTER);
                app.button_hits.push_back({tr, NAV_ITEMS[i].id, ""});
            }

            float cy = top_h + 6.0f;
            float ch = std::max(120.0f, nav_y - cy - 6.0f);
            float cx = 6.0f;
            float cw = W - 12.0f;

            if (app.mobile_tab == 0) {
                render_tool_strip(app, cx, cy, cw);
                float vy = cy + 36.0f;
                float vh = ch - 36.0f;
                app.tool_view_rect = D2D1::RectF(cx, vy, cx + cw, vy + vh);
                switch (app.center_view) {
                    case CenterView::Packets: render_packets_view(app, cx, vy, cw, vh); break;
                    case CenterView::Adverts: render_adverts_view(app, cx, vy, cw, vh); break;
                    case CenterView::Paths:   render_paths_view(app, cx, vy, cw, vh); break;
                    case CenterView::Map:     render_map_view(app, cx, vy, cw, vh); break;
                    default:                  render_center_workstation(app, cx, vy, cw, vh); break;
                }
            } else if (app.mobile_tab == 1) {
                render_single_module(app, MOD_PUBLIC_MESH, cx, cy, cw, ch);
            } else if (app.mobile_tab == 2) {
                render_single_module(app, MOD_RAINBOW_SNIFFER, cx, cy, cw, ch);
            } else if (app.mobile_tab == 3) {
                render_single_module(app, MOD_JOINED_CHANNELS, cx, cy, cw, ch);
            } else if (app.mobile_tab == 4) {
                render_single_module(app, MOD_HW_SLOTS, cx, cy, cw, ch);
            }

            if (app.ble_dropdown_open) {
                render_ble_dropdown(app, 12.0f, top_h + 4.0f);
            }

            if (line_brush) line_brush->Release();
            HRESULT hr = rt->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET) {
                release_tiles();
                rt->Release();
                rt = nullptr;
            }
            BitBlt(target_hdc, 0, 0, w_px, h_px, mem_dc, 0, 0, SRCCOPY);
            SelectObject(mem_dc, old_bmp);
            DeleteObject(mem_bmp);
            DeleteDC(mem_dc);
            return;
        }

        // ====================================================================
        // 1. TOP TELEMETRY & TRANSPORT CONTROL BAR (y: 0..62)
        // ====================================================================
        D2D1_RECT_F top_bar = D2D1::RectF(0, 0, W, 62);
        fill_round_rect(top_bar, 0.0f, hex_color(0x0F172A));

        if (line_brush) {
            rt->DrawLine(D2D1::Point2F(0, 62), D2D1::Point2F(W, 62), line_brush, 1.5f);
        }

        // Brand badge
        D2D1_RECT_F brand_rect = D2D1::RectF(14, 12, 168, 50);
        fill_round_rect(brand_rect, 8.0f, hex_color(0x162238));
        draw_round_rect(brand_rect, 8.0f, hex_color(0x00E5FF, 0.5f), 1.2f);
        draw_text("MESHCORE++", font_title, D2D1::RectF(26, 12, 162, 34), hex_color(0x00E5FF));
        draw_text("C++20 NATIVE SDR/IRC", font_small, D2D1::RectF(26, 32, 162, 48), hex_color(0x94A3B8));

        // Transport & Theme Buttons: [USB Serial ▾] [BLE NUS ▾] [RF Sim] [+ #Tag] [☀ Light / ☾ Dark]
        auto draw_top_btn = [&](int id, float x, float w, const std::string& label, bool active, uint32_t accent_hex) {
            D2D1_RECT_F r = D2D1::RectF(x, 14, x + w, 48);
            bool hovered = pt_in_rect(app.mouse_pt, r);
            D2D1_COLOR_F bg = active ? hex_color(accent_hex, 0.22f)
                            : hovered ? hex_color(0x1E293B)
                                      : hex_color(0x131C2E);
            D2D1_COLOR_F border = active ? hex_color(accent_hex)
                                : hovered ? hex_color(0x475569)
                                          : hex_color(0x26354D);
            fill_round_rect(r, 7.0f, bg);
            draw_round_rect(r, 7.0f, border, active ? 1.6f : 1.0f);
            draw_text(label, font_ui_bold, r, active ? hex_color(accent_hex) : hex_color(0xE2E8F0),
                      DWRITE_TEXT_ALIGNMENT_CENTER);
            app.button_hits.push_back({r, id, ""});
        };

        bool serial_active = app.transport_connected && !app.is_sim && app.transport_label.find("SERIAL") != std::string::npos;
        bool ble_active    = (!app.is_sim && app.transport_label.find("BLE") != std::string::npos) || app.ble_dropdown_open;

        std::string usb_btn_lbl = serial_active
                                      ? ((app.connected_port.empty() ? "USB" : app.connected_port) + ": " + app.device_model + " v")
                                      : "USB Serial v";

        draw_top_btn(BTN_USB_SERIAL,   176, 142, usb_btn_lbl, serial_active, 0x10B981);
        draw_top_btn(BTN_BLE_NUS,      324, 88,  "BLE NUS v", ble_active,    0x38BDF8);
        draw_top_btn(BTN_RF_SIM,       418, 114, app.is_sim ? "RF Sim: LIVE" : "RF Sim: OFF", app.is_sim, 0x00E5FF);
        draw_top_btn(BTN_ADD_TAG,      538, 86,  "+ #Tag",    false,        0xA855F7);
        draw_top_btn(BTN_TOGGLE_THEME, 630, 88,  app.light_mode ? "\xe2\x98\xbe Dark" : "\xe2\x98\x80 Light", app.light_mode, 0xFBBF24);

        // Right-aligned Battery Gauge & Callsign Badge (no overlapping box behind battery!)
        float right_x = W - 14.0f;

        D2D1_RECT_F call_rect = D2D1::RectF(right_x - 186, 14, right_x, 48);
        bool call_focus = (app.active_input == ActiveInputTarget::CallsignEditor);
        fill_round_rect(call_rect, 7.0f, call_focus ? hex_color(0x1E293B) : hex_color(0x141E30));
        draw_round_rect(call_rect, 7.0f, call_focus ? hex_color(0xFBBF24) : hex_color(0x334155), 1.3f);
        draw_text("OP: " + app.callsign + (call_focus ? "_" : " [Edit]"), font_ui_bold, call_rect,
                  hex_color(0xFBBF24), DWRITE_TEXT_ALIGNMENT_CENTER);
        app.button_hits.push_back({call_rect, BTN_CALLSIGN, ""});

        D2D1_RECT_F bat_rect = D2D1::RectF(right_x - 342, 14, right_x - 194, 48);
        fill_round_rect(bat_rect, 7.0f, hex_color(0x111A2B));
        draw_round_rect(bat_rect, 7.0f, hex_color(0x1E293B), 1.0f);
        char bat_buf[64];
        std::snprintf(bat_buf, sizeof(bat_buf), "BAT %.2fV (%u%%)", app.battery_mv / 1000.0f, app.battery_pct);
        draw_text(bat_buf, font_mono_small, D2D1::RectF(bat_rect.left + 10, 15, bat_rect.right - 10, 32),
                  hex_color(0x34D399), DWRITE_TEXT_ALIGNMENT_CENTER);
        // Battery fill bar
        D2D1_RECT_F bbar_bg = D2D1::RectF(bat_rect.left + 14, 36, bat_rect.right - 14, 43);
        fill_round_rect(bbar_bg, 3.0f, hex_color(0x1E293B));
        float fill_w = (bbar_bg.right - bbar_bg.left) * (std::clamp<int>(app.battery_pct, 0, 100) / 100.0f);
        fill_round_rect(D2D1::RectF(bbar_bg.left, bbar_bg.top, bbar_bg.left + fill_w, bbar_bg.bottom),
                        3.0f, hex_color(0x10B981));

        // Clean inline RF status text if there is space between the buttons and the battery gauge
        if (bat_rect.left - 730.0f >= 140.0f) {
            std::ostringstream rf_oss;
            rf_oss << std::fixed << std::setprecision(3) << (app.radio_freq_khz / 1000.0) << "MHz SF"
                   << static_cast<int>(app.radio_sf) << " | " << app.mesh_nodes.size() << " NODES";
            if (app.has_last_rf) {
                rf_oss << " | RX " << app.last_rssi << "dBm/"
                       << std::showpos << std::setprecision(1) << app.last_snr << "dB" << std::noshowpos;
            }
            rf_oss << " | AIR " << std::fixed << std::setprecision(1) << app.channel_busy_pct() << "%";
            draw_text(rf_oss.str(), font_mono_small, D2D1::RectF(730.0f, 14.0f, bat_rect.left - 10.0f, 48.0f),
                      hex_color(0x38BDF8), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // ====================================================================
        // 2. 4-MODULE DRAG-AND-DROP WORKSPACE LAYOUT (y: 72 .. H - 12)
        // Up to 2 columns wide on either side of the main chat window,
        // and up to 4 modules stacked vertically in any column!
        // ====================================================================
        float pane_top = 72.0f;
        float pane_bottom = H - 12.0f;
        float pane_h = pane_bottom - pane_top;
        float splitter_w = 6.0f;

        // Calculate active column widths for Left (cols 0, 1) and Right (cols 2, 3)
        std::array<float, 4> col_w{0.0f, 0.0f, 0.0f, 0.0f};
        float total_side_w = 0.0f;
        int active_col_count = 0;
        for (int c = 0; c < 4; ++c) {
            if (!app.module_cols[c].modules.empty()) {
                col_w[c] = std::clamp(app.module_cols[c].width, 190.0f, 520.0f);
                total_side_w += col_w[c];
                ++active_col_count;
            }
        }
        // Ensure at least 320px remains for the center chat workstation
        float max_side_total = std::max(240.0f, W - 24.0f - 320.0f - active_col_count * splitter_w);
        if (total_side_w > max_side_total && total_side_w > 0.0f) {
            float scale = max_side_total / total_side_w;
            for (int c = 0; c < 4; ++c) col_w[c] *= scale;
            total_side_w = max_side_total;
        }

        std::array<D2D1_RECT_F, 4> col_rects{};
        std::array<std::vector<D2D1_RECT_F>, 4> mod_rects{};

        auto render_module_column = [&](int c, float cx, float cw, bool is_right_side) {
            auto& col = app.module_cols[c];
            app.ensure_column_weights(col);
            col_rects[c] = D2D1::RectF(cx, pane_top, cx + cw, pane_bottom);

            size_t n = col.modules.size();
            float split_h = 8.0f;
            float avail_h = std::max(80.0f, pane_h - static_cast<float>(n > 0 ? (n - 1) : 0) * split_h);
            float my = pane_top;

            for (size_t i = 0; i < n; ++i) {
                float mh = (i + 1 == n) ? (pane_bottom - my) : std::max(72.0f, avail_h * col.weights[i]);
                D2D1_RECT_F mr = D2D1::RectF(cx, my, cx + cw, my + mh);
                mod_rects[c].push_back(mr);
                render_single_module(app, col.modules[i], cx, my, cw, mh);
                my += mh;

                if (i + 1 < n) {
                    D2D1_RECT_F hsr = D2D1::RectF(cx, my, cx + cw, my + split_h);
                    bool h_hov = pt_in_rect(app.mouse_pt, hsr) ||
                                 (app.dragging_splitter == DraggingSplitter::ModuleHeight &&
                                  app.active_splitter_col == c && app.active_splitter_row == static_cast<int>(i));
                    fill_round_rect(hsr, 2.0f, h_hov ? hex_color(0x00E5FF, 0.45f) : hex_color(0x0B111E));
                    float cx_grip = cx + cw * 0.5f;
                    fill_round_rect(D2D1::RectF(cx_grip - 14, my + 3, cx_grip + 14, my + 5), 1.0f,
                                    h_hov ? hex_color(0x00E5FF) : hex_color(0x334155));
                    app.horiz_splitter_hits.push_back({hsr, c, static_cast<int>(i)});
                    my += split_h;
                }
            }
        };

        float cur_x = 12.0f;
        // Left Side Columns (0 and 1)
        for (int c = 0; c < 2; ++c) {
            if (!app.module_cols[c].modules.empty()) {
                render_module_column(c, cur_x, col_w[c], false);
                cur_x += col_w[c];

                D2D1_RECT_F vsr = D2D1::RectF(cur_x, pane_top, cur_x + splitter_w, pane_bottom);
                bool v_hov = pt_in_rect(app.mouse_pt, vsr) ||
                             (app.dragging_splitter == DraggingSplitter::ColumnWidth && app.active_splitter_col == c);
                fill_round_rect(vsr, 2.0f, v_hov ? hex_color(0x00E5FF, 0.7f) : hex_color(0x1E293B, 0.4f));
                app.vert_splitter_hits.push_back({vsr, c, false});
                cur_x += splitter_w;
            }
        }

        float right_total_w = 0.0f;
        for (int c = 2; c < 4; ++c) {
            if (!app.module_cols[c].modules.empty()) {
                right_total_w += col_w[c] + splitter_w;
            }
        }

        float center_x = cur_x;
        float center_w = std::max(280.0f, (W - 12.0f - right_total_w) - center_x);
        D2D1_RECT_F center_zone_rect = D2D1::RectF(center_x, pane_top, center_x + center_w, pane_bottom);

        render_tool_strip(app, center_x, pane_top, center_w);
        {
            float vy = pane_top + 38.0f;
            float vh = pane_h - 38.0f;
            app.tool_view_rect = D2D1::RectF(center_x, vy, center_x + center_w, vy + vh);
            switch (app.center_view) {
                case CenterView::Packets: render_packets_view(app, center_x, vy, center_w, vh); break;
                case CenterView::Adverts: render_adverts_view(app, center_x, vy, center_w, vh); break;
                case CenterView::Paths:   render_paths_view(app, center_x, vy, center_w, vh); break;
                case CenterView::Map:     render_map_view(app, center_x, vy, center_w, vh); break;
                default:                  render_center_workstation(app, center_x, vy, center_w, vh); break;
            }
        }
        cur_x += center_w;

        // Right Side Columns (2 and 3)
        for (int c = 2; c < 4; ++c) {
            if (!app.module_cols[c].modules.empty()) {
                D2D1_RECT_F vsr = D2D1::RectF(cur_x, pane_top, cur_x + splitter_w, pane_bottom);
                bool v_hov = pt_in_rect(app.mouse_pt, vsr) ||
                             (app.dragging_splitter == DraggingSplitter::ColumnWidth && app.active_splitter_col == c);
                fill_round_rect(vsr, 2.0f, v_hov ? hex_color(0x00E5FF, 0.7f) : hex_color(0x1E293B, 0.4f));
                app.vert_splitter_hits.push_back({vsr, c, true});
                cur_x += splitter_w;

                render_module_column(c, cur_x, col_w[c], true);
                cur_x += col_w[c];
            }
        }

        // ====================================================================
        // 3. LIVE DRAG-AND-DROP OVERLAY & DROP ZONE COMPUTATION
        // ====================================================================
        if (app.module_drag_active && app.dragging_module >= 0) {
            update_and_draw_module_drag_overlay(app, W, pane_top, pane_bottom, center_zone_rect, col_rects, mod_rects);
        }

        // Render COM Port or BLE NUS Selector Dropdown Modal if open
        if (app.serial_dropdown_open) {
            render_serial_dropdown(app, 180.0f, 52.0f);
        }
        if (app.ble_dropdown_open) {
            render_ble_dropdown(app, 320.0f, 52.0f);
        }

        if (line_brush) line_brush->Release();
        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            release_tiles();
            rt->Release();
            rt = nullptr;
        }
        BitBlt(target_hdc, 0, 0, w_px, h_px, mem_dc, 0, 0, SRCCOPY);
        SelectObject(mem_dc, old_bmp);
        DeleteObject(mem_bmp);
        DeleteDC(mem_dc);
    }

private:
    void draw_module_drag_header(MeshGuiApp& app, int mod_id, float x, float y, float w,
                                 const std::string& title, uint32_t accent_hex, float right_pad = 10.0f) {
        D2D1_RECT_F hdr_r = D2D1::RectF(x, y, x + w - right_pad, y + 28.0f);
        bool hov = pt_in_rect(app.mouse_pt, hdr_r);
        bool is_dragging_this = (app.module_drag_active && app.dragging_module == mod_id);
        if (hov || is_dragging_this) {
            fill_round_rect(D2D1::RectF(x + 4, y + 3, x + w - 4, y + 26), 6.0f,
                            hex_color(accent_hex, is_dragging_this ? 0.25f : 0.12f));
        }
        draw_text("\xe2\xa0\xbf " + title, font_small,
                  D2D1::RectF(x + 12, y + 6, x + w - 42, y + 26), hex_color(accent_hex));
        draw_text(hov ? "DRAG" : "\xe2\x87\x86", font_mono_small,
                  D2D1::RectF(x + w - 48, y + 6, x + w - 10, y + 26),
                  hov ? hex_color(accent_hex) : hex_color(0x64748B), DWRITE_TEXT_ALIGNMENT_TRAILING);
        app.module_header_hits.push_back({hdr_r, mod_id, title});
    }

    void update_and_draw_module_drag_overlay(MeshGuiApp& app, float W, float pane_top, float pane_bottom,
                                             const D2D1_RECT_F& center_zone,
                                             const std::array<D2D1_RECT_F, 4>& col_rects,
                                             const std::array<std::vector<D2D1_RECT_F>, 4>& mod_rects) {
        float mx = static_cast<float>(app.mouse_pt.x);
        float my = static_cast<float>(app.mouse_pt.y);
        app.drop_target_valid = false;

        bool left_has_0 = !app.module_cols[0].modules.empty();
        bool left_has_1 = !app.module_cols[1].modules.empty();
        int left_cols = (left_has_0 ? 1 : 0) + (left_has_1 ? 1 : 0);

        bool right_has_2 = !app.module_cols[2].modules.empty();
        bool right_has_3 = !app.module_cols[3].modules.empty();
        int right_cols = (right_has_2 ? 1 : 0) + (right_has_3 ? 1 : 0);

        auto compute_stack_drop = [&](int c) {
            const auto& r = col_rects[c];
            const auto& mrs = mod_rects[c];
            app.drop_target_valid = true;
            app.drop_target_col = c;
            if (mrs.empty()) {
                app.drop_target_row = 0;
                app.drop_preview_rect = r;
                return;
            }
            int row = static_cast<int>(mrs.size());
            for (size_t i = 0; i < mrs.size(); ++i) {
                float mid_y = (mrs[i].top + mrs[i].bottom) * 0.5f;
                if (my < mid_y) {
                    row = static_cast<int>(i);
                    break;
                }
            }
            app.drop_target_row = row;
            float preview_h = std::min(90.0f, (r.bottom - r.top) / static_cast<float>(mrs.size() + 1));
            if (row == 0) {
                app.drop_preview_rect = D2D1::RectF(r.left + 3, r.top + 3, r.right - 3, r.top + preview_h);
            } else if (row >= static_cast<int>(mrs.size())) {
                app.drop_preview_rect = D2D1::RectF(r.left + 3, r.bottom - preview_h, r.right - 3, r.bottom - 3);
            } else {
                float sy = mrs[row].top;
                app.drop_preview_rect = D2D1::RectF(r.left + 3, sy - preview_h * 0.5f, r.right - 3, sy + preview_h * 0.5f);
            }
        };

        float center_mid_x = (center_zone.left + center_zone.right) * 0.5f;
        if (mx < center_mid_x) {
            // Left side drop logic (supports 0 -> 1 col, 1 -> 2 cols side-by-side, or stacking up to 4 high)
            if (left_cols == 0) {
                app.drop_target_valid = true;
                app.drop_target_col = 0;
                app.drop_target_row = 0;
                app.drop_preview_rect = D2D1::RectF(12.0f, pane_top, 260.0f, pane_bottom);
            } else if (left_cols == 1) {
                int c = left_has_0 ? 0 : 1;
                const auto& r = col_rects[c];
                float edge_w = std::min(58.0f, (r.right - r.left) * 0.26f);
                if (mx < r.left + edge_w) {
                    // Drop as new Outer-Left column (2-wide on Left!)
                    app.drop_target_valid = true;
                    app.drop_target_col = 10;
                    app.drop_target_row = 0;
                    app.drop_preview_rect = D2D1::RectF(r.left, pane_top, (r.left + r.right) * 0.5f - 2.0f, pane_bottom);
                } else if (mx > r.right - edge_w) {
                    // Drop as new Inner-Left column (2-wide on Left!)
                    app.drop_target_valid = true;
                    app.drop_target_col = 11;
                    app.drop_target_row = 0;
                    app.drop_preview_rect = D2D1::RectF((r.left + r.right) * 0.5f + 2.0f, pane_top, r.right + 40.0f, pane_bottom);
                } else {
                    compute_stack_drop(c);
                }
            } else {
                float split_x = col_rects[0].right + 3.0f;
                compute_stack_drop(mx < split_x ? 0 : 1);
            }
        } else {
            // Right side drop logic (supports 0 -> 1 col, 1 -> 2 cols side-by-side, or stacking up to 4 high)
            if (right_cols == 0) {
                app.drop_target_valid = true;
                app.drop_target_col = 2;
                app.drop_target_row = 0;
                app.drop_preview_rect = D2D1::RectF(W - 260.0f, pane_top, W - 12.0f, pane_bottom);
            } else if (right_cols == 1) {
                int c = right_has_2 ? 2 : 3;
                const auto& r = col_rects[c];
                float edge_w = std::min(58.0f, (r.right - r.left) * 0.26f);
                if (mx < r.left + edge_w) {
                    // Drop as new Inner-Right column (2-wide on Right!)
                    app.drop_target_valid = true;
                    app.drop_target_col = 20;
                    app.drop_target_row = 0;
                    app.drop_preview_rect = D2D1::RectF(r.left - 40.0f, pane_top, (r.left + r.right) * 0.5f - 2.0f, pane_bottom);
                } else if (mx > r.right - edge_w) {
                    // Drop as new Outer-Right column (2-wide on Right!)
                    app.drop_target_valid = true;
                    app.drop_target_col = 21;
                    app.drop_target_row = 0;
                    app.drop_preview_rect = D2D1::RectF((r.left + r.right) * 0.5f + 2.0f, pane_top, r.right, pane_bottom);
                } else {
                    compute_stack_drop(c);
                }
            } else {
                float split_x = col_rects[2].right + 3.0f;
                compute_stack_drop(mx < split_x ? 2 : 3);
            }
        }

        if (app.drop_target_valid) {
            fill_round_rect(app.drop_preview_rect, 8.0f, hex_color(0x00E5FF, 0.22f));
            draw_round_rect(app.drop_preview_rect, 8.0f, hex_color(0x00E5FF), 2.2f);
            std::string drop_lbl = (app.drop_target_col >= 10)
                                       ? "DROP HERE: NEW SIDE-BY-SIDE COLUMN"
                                       : ("DROP HERE: STACK ROW " + std::to_string(app.drop_target_row + 1));
            draw_text(drop_lbl, font_ui_bold, app.drop_preview_rect, hex_color(0x00E5FF), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // Draw floating ghost badge following cursor
        D2D1_RECT_F ghost = D2D1::RectF(mx - 110.0f, my - 16.0f, mx + 110.0f, my + 16.0f);
        fill_round_rect(ghost, 7.0f, hex_color(0x0F172A, 0.92f));
        draw_round_rect(ghost, 7.0f, hex_color(0x00E5FF), 1.8f);
        draw_text(std::string("\xe2\xa0\xbf ") + MeshGuiApp::module_title(app.dragging_module),
                  font_ui_bold, ghost, hex_color(0x00E5FF), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void render_single_module(MeshGuiApp& app, int mod_id, float x, float y, float w, float h) {
        switch (mod_id) {
            case MOD_JOINED_CHANNELS: render_module_joined_channels(app, x, y, w, h); break;
            case MOD_RAINBOW_SNIFFER: render_module_rainbow_sniffer(app, x, y, w, h); break;
            case MOD_PUBLIC_MESH:     render_module_public_mesh(app, x, y, w, h);     break;
            case MOD_HW_SLOTS:        render_module_hw_slots(app, x, y, w, h);        break;
            default: break;
        }
    }

    // ========================================================================
    // MODULE 0: JOINED CHANNELS
    // ========================================================================
    void render_module_joined_channels(MeshGuiApp& app, float x, float y, float w, float h) {
        D2D1_RECT_F joined_card = D2D1::RectF(x, y, x + w, y + h);
        fill_round_rect(joined_card, 10.0f, hex_color(0x0F172A));
        draw_round_rect(joined_card, 10.0f, hex_color(0x1E293B), 1.2f);

        draw_module_drag_header(app, MOD_JOINED_CHANNELS, x, y, w, "JOINED CHANNELS", 0x94A3B8);

        float row_y = y + 32.0f;
        for (size_t i = 0; i < app.tabs.size() && (row_y + 30.0f) < joined_card.bottom - 6.0f; ++i) {
            const auto& tab = app.tabs[i];
            bool active = (i == app.active_tab_idx);
            D2D1_RECT_F row_r = D2D1::RectF(x + 10, row_y, x + w - 10, row_y + 28.0f);
            bool hovered = pt_in_rect(app.mouse_pt, row_r);

            if (active) {
                fill_round_rect(row_r, 6.0f, hex_color(0x00E5FF, 0.16f));
                draw_round_rect(row_r, 6.0f, hex_color(0x00E5FF, 0.7f), 1.2f);
                fill_round_rect(D2D1::RectF(row_r.left, row_r.top + 4, row_r.left + 3.5f, row_r.bottom - 4),
                                1.5f, hex_color(0x00E5FF));
            } else if (hovered) {
                fill_round_rect(row_r, 6.0f, hex_color(0x1E293B));
            }

            std::string label = std::to_string(i) + ": " + tab.name;
            D2D1_COLOR_F t_col = active ? hex_color(0x00E5FF)
                               : tab.has_mention ? hex_color(0xEF4444)
                               : (tab.unread_count > 0) ? hex_color(0xF8FAFC)
                                                        : hex_color(0x94A3B8);
            draw_text(label, active ? font_ui_bold : font_ui,
                      D2D1::RectF(row_r.left + 12, row_r.top, row_r.right - 72, row_r.bottom), t_col);

            // Hardware Slot badge
            if (auto slot = app.hw_slots.slot_for_channel(tab.name)) {
                D2D1_RECT_F s_rect = D2D1::RectF(row_r.right - 68, row_r.top + 5, row_r.right - 36, row_r.bottom - 5);
                fill_round_rect(s_rect, 4.0f, hex_color(0x1E293B));
                draw_text("S" + std::to_string(*slot), font_mono_small, s_rect,
                          hex_color(0x38BDF8), DWRITE_TEXT_ALIGNMENT_CENTER);
            }

            // Unread badge or Close 'x' button
            if (tab.unread_count > 0 && !active) {
                D2D1_RECT_F u_rect = D2D1::RectF(row_r.right - 32, row_r.top + 5, row_r.right - 6, row_r.bottom - 5);
                fill_round_rect(u_rect, 4.0f, tab.has_mention ? hex_color(0xEF4444) : hex_color(0xF59E0B));
                draw_text("+" + std::to_string(tab.unread_count), font_small, u_rect,
                          hex_color(0x090D16), DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i > 1) {
                D2D1_RECT_F x_rect = D2D1::RectF(row_r.right - 26, row_r.top + 4, row_r.right - 6, row_r.bottom - 4);
                if (pt_in_rect(app.mouse_pt, x_rect)) {
                    fill_round_rect(x_rect, 4.0f, hex_color(0xEF4444, 0.3f));
                }
                draw_text("x", font_ui_bold, x_rect, hex_color(0x94A3B8), DWRITE_TEXT_ALIGNMENT_CENTER);
                app.tab_close_hits.push_back({x_rect, static_cast<int>(i), ""});
            }

            app.joined_hits.push_back({row_r, static_cast<int>(i), tab.name});
            row_y += 31.0f;
        }
    }

    // ========================================================================
    // MODULE 1: 256-BUCKET RAINBOW SNIFFER
    // ========================================================================
    void render_module_rainbow_sniffer(MeshGuiApp& app, float x, float y, float w, float h) {
        float sniffer_y = y;
        float sniffer_h = h;
        D2D1_RECT_F sniffer_card = D2D1::RectF(x, sniffer_y, x + w, sniffer_y + sniffer_h);
        app.sniffer_view_rect = sniffer_card;
        fill_round_rect(sniffer_card, 10.0f, hex_color(0x0F172A));
        draw_round_rect(sniffer_card, 10.0f, hex_color(0x1E293B), 1.2f);
        draw_module_drag_header(app, MOD_RAINBOW_SNIFFER, x, sniffer_y, w, "256-BUCKET RAINBOW SNIFFER", 0x10B981);

        // Mode switcher pills: [Live Traffic] vs [All 303+ Tags]
        float half_w = (w - 26.0f) * 0.5f;
        D2D1_RECT_F mode_live_r = D2D1::RectF(x + 10, sniffer_y + 30, x + 10 + half_w, sniffer_y + 54);
        D2D1_RECT_F mode_all_r  = D2D1::RectF(x + 16 + half_w, sniffer_y + 30, x + w - 10, sniffer_y + 54);

        fill_round_rect(mode_live_r, 5.0f, !app.show_all_rainbow_in_sniffer ? hex_color(0x10B981, 0.22f) : hex_color(0x162032));
        draw_round_rect(mode_live_r, 5.0f, !app.show_all_rainbow_in_sniffer ? hex_color(0x10B981) : hex_color(0x26354D), 1.0f);
        draw_text("Live Sniffed (" + std::to_string(app.discovered.size()) + ")", font_small, mode_live_r,
                  !app.show_all_rainbow_in_sniffer ? hex_color(0x10B981) : hex_color(0x94A3B8), DWRITE_TEXT_ALIGNMENT_CENTER);
        app.button_hits.push_back({mode_live_r, BTN_SNIFFER_MODE_LIVE, ""});

        fill_round_rect(mode_all_r, 5.0f, app.show_all_rainbow_in_sniffer ? hex_color(0x00E5FF, 0.22f) : hex_color(0x162032));
        draw_round_rect(mode_all_r, 5.0f, app.show_all_rainbow_in_sniffer ? hex_color(0x00E5FF) : hex_color(0x26354D), 1.0f);
        draw_text("All Buckets (" + std::to_string(app.rainbow.total_candidates()) + ")", font_small, mode_all_r,
                  app.show_all_rainbow_in_sniffer ? hex_color(0x00E5FF) : hex_color(0x94A3B8), DWRITE_TEXT_ALIGNMENT_CENTER);
        app.button_hits.push_back({mode_all_r, BTN_SNIFFER_MODE_ALL, ""});

        // Search / Filter Input Box
        D2D1_RECT_F filter_r = D2D1::RectF(x + 10, sniffer_y + 60, x + w - 10, sniffer_y + 86);
        bool filter_focused = (app.active_input == ActiveInputTarget::SnifferFilter);
        fill_round_rect(filter_r, 5.0f, hex_color(0x090D16));
        draw_round_rect(filter_r, 5.0f, filter_focused ? hex_color(0x00E5FF) : hex_color(0x26354D), 1.2f);
        std::string filter_disp = app.sniffer_filter.empty()
                                      ? (filter_focused ? "Filter #tags: _" : "Filter #hashtags (click to type)...")
                                      : ("Filter: " + app.sniffer_filter + (filter_focused ? "_" : ""));
        draw_text(filter_disp, font_mono_small, D2D1::RectF(filter_r.left + 8, filter_r.top, filter_r.right - 8, filter_r.bottom),
                  app.sniffer_filter.empty() && !filter_focused ? hex_color(0x64748B) : hex_color(0xE2E8F0));
        app.button_hits.push_back({filter_r, BTN_SNIFFER_FILTER_BOX, ""});

        // Populate rows to display
        float list_y = sniffer_y + 92.0f;
        float list_bottom = sniffer_card.bottom - 8.0f;

        std::string f_lower = app.sniffer_filter;
        std::transform(f_lower.begin(), f_lower.end(), f_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (!app.show_all_rainbow_in_sniffer) {
            int idx = 0;
            for (const auto& d : app.discovered) {
                if (!f_lower.empty() && d.name.find(f_lower) == std::string::npos) continue;
                if (idx++ < app.sniffer_scroll) continue;
                if (list_y + 34.0f > list_bottom) break;

                draw_sniffer_row(app, x + 10, list_y, w - 20, d);
                list_y += 36.0f;
            }
        } else {
            auto all = app.rainbow.all_candidates();
            int idx = 0;
            for (const auto& c : all) {
                if (!f_lower.empty() && c.name.find(f_lower) == std::string::npos) continue;
                if (idx++ < app.sniffer_scroll) continue;
                if (list_y + 34.0f > list_bottom) break;

                DiscoveredChannel d;
                d.name = c.name;
                d.channel_hash = c.channel_hash;
                d.psk_hex = psk_to_hex(c.psk);
                d.mentioned_only = true;
                for (const auto& live : app.discovered) {
                    if (live.name == c.name) { d = live; break; }
                }
                draw_sniffer_row(app, x + 10, list_y, w - 20, d);
                list_y += 36.0f;
            }
        }
    }

    void draw_sniffer_row(MeshGuiApp& app, float x, float y, float w, const DiscoveredChannel& d) {
        D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + 32.0f);
        bool hovered = pt_in_rect(app.mouse_pt, r);
        bool is_joined = false;
        for (const auto& t : app.tabs) {
            if (t.name == d.name) { is_joined = true; break; }
        }

        fill_round_rect(r, 6.0f, hovered ? hex_color(0x1E293B) : hex_color(0x131C2E));
        draw_round_rect(r, 6.0f, is_joined ? hex_color(0x10B981, 0.5f)
                               : hovered   ? hex_color(0x00E5FF, 0.7f)
                                           : hex_color(0x1E293B), 1.0f);

        // Status dot
        D2D1_COLOR_F dot_col = is_joined ? hex_color(0x10B981)
                             : d.mentioned_only ? hex_color(0xA855F7)
                                                : hex_color(0x00E5FF);
        fill_round_rect(D2D1::RectF(x + 8, y + 12, x + 15, y + 19), 3.5f, dot_col);

        draw_text(d.name, font_ui_bold, D2D1::RectF(x + 20, y + 2, x + w - 108, y + 20),
                  is_joined ? hex_color(0x34D399) : hex_color(0xF1F5F9));

        char hash_buf[32];
        std::snprintf(hash_buf, sizeof(hash_buf), "Bucket 0x%02X", d.channel_hash);
        draw_text(hash_buf, font_mono_small, D2D1::RectF(x + 20, y + 17, x + 140, y + 31),
                  hex_color(0x64748B));

        if (d.has_rf) {
            draw_signal_bars(x + w - 98, y + 9, d.last_snr);
            char snr_buf[32];
            std::snprintf(snr_buf, sizeof(snr_buf), "%+.1fdB (%zu)", d.last_snr, d.msg_count);
            draw_text(snr_buf, font_mono_small, D2D1::RectF(x + w - 74, y + 6, x + w - 6, y + 26),
                      hex_color(0x38BDF8), DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else {
            D2D1_RECT_F badge = D2D1::RectF(x + w - 82, y + 7, x + w - 8, y + 25);
            fill_round_rect(badge, 4.0f, hex_color(0xA855F7, 0.18f));
            draw_text(app.show_all_rainbow_in_sniffer && d.last_activity_epoch == 0 ? "Rainbow" : "Mentioned",
                      font_small, badge, hex_color(0xC084FC), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        app.discovered_hits.push_back({r, 0, d.name});
    }

    // ========================================================================
    // CENTER WORKSTATION: Tab Bar + Crypto Header + Chat Feed + Composer
    // ========================================================================
    void render_center_workstation(MeshGuiApp& app, float x, float y, float w, float h) {
        D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h);
        fill_round_rect(card, 10.0f, hex_color(0x0F172A));
        draw_round_rect(card, 10.0f, hex_color(0x1E293B), 1.2f);

        // 1. Horizontal Tab Bar (y .. y + 36)
        float tx = x + 10.0f;
        for (size_t i = 0; i < app.tabs.size(); ++i) {
            const auto& tab = app.tabs[i];
            bool active = (i == app.active_tab_idx);
            std::string label = tab.name;
            if (tab.unread_count > 0 && !active) {
                label += " (+" + std::to_string(tab.unread_count) + ")";
            }
            float tw = std::clamp(static_cast<float>(label.size()) * 8.2f + (i > 1 ? 38.0f : 24.0f), 82.0f, 185.0f);
            if (tx + tw > x + w - 10.0f) break;

            D2D1_RECT_F tr = D2D1::RectF(tx, y + 6, tx + tw, y + 36);
            bool hovered = pt_in_rect(app.mouse_pt, tr);

            fill_round_rect(tr, 6.0f, active ? hex_color(0x16243D)
                                    : hovered ? hex_color(0x1E293B)
                                              : hex_color(0x0B111E));
            draw_round_rect(tr, 6.0f, active ? hex_color(0x00E5FF) : hex_color(0x1E293B), active ? 1.4f : 1.0f);

            draw_text(label, active ? font_ui_bold : font_ui,
                      D2D1::RectF(tr.left + 10, tr.top, tr.right - (i > 1 ? 22 : 8), tr.bottom),
                      active ? hex_color(0x00E5FF) : hex_color(0xCBD5E1));

            if (i > 1) {
                D2D1_RECT_F xr = D2D1::RectF(tr.right - 22, tr.top + 5, tr.right - 4, tr.bottom - 5);
                draw_text("x", font_small, xr, hex_color(0x94A3B8), DWRITE_TEXT_ALIGNMENT_CENTER);
                app.tab_close_hits.push_back({xr, static_cast<int>(i), ""});
            }

            app.tab_hits.push_back({tr, static_cast<int>(i), tab.name});
            tx += tw + 6.0f;
        }

        // 2. Cryptographic Channel Inspector Sub-Banner (y + 40 .. y + 74)
        D2D1_RECT_F crypto_bar = D2D1::RectF(x + 10, y + 42, x + w - 10, y + 74);
        fill_round_rect(crypto_bar, 6.0f, hex_color(0x0B111E));
        draw_round_rect(crypto_bar, 6.0f, hex_color(0x1E293B), 1.0f);

        const auto& active_tab = app.tabs[app.active_tab_idx];
        const bool is_console_tab = (active_tab.name == MeshGuiApp::CONSOLE_TAB);
        const bool is_public_tab = (normalize_channel_name(active_tab.name) == PUBLIC_CHANNEL_NAME);

        // Build unified participant list for this room:
        // 1. Speakers & @[mentioned] stations in this tab
        // 2. For Public, include EVERY heard user across all tabs, live adverts, and mesh_nodes!
        std::vector<HeardStation> room_users;
        room_users.reserve(active_tab.nicks.size() + (is_public_tab ? app.mesh_nodes.size() + 32 : 0));
        std::unordered_set<std::string> seen_users;
        seen_users.reserve(active_tab.nicks.size() * 2 + (is_public_tab ? app.mesh_nodes.size() * 2 + 64 : 16));
        auto lower_key = [](const std::string& s) {
            std::string k = s;
            std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return k;
        };
        if (!app.callsign.empty()) seen_users.insert(lower_key(app.callsign));
        auto has_or_mark_user = [&](const std::string& name) {
            if (name.empty() || name == "SYSTEM" || name == "*" || name == "(unnamed)") {
                return true;
            }
            return !seen_users.insert(lower_key(name)).second;
        };
        for (const auto& st : active_tab.nicks) {
            if (!has_or_mark_user(st.callsign)) room_users.push_back(st);
        }
        if (is_public_tab) {
            // Include speakers from every channel / DM tab
            for (const auto& t : app.tabs) {
                for (const auto& st : t.nicks) {
                    if (!has_or_mark_user(st.callsign)) room_users.push_back(st);
                }
            }
            // Include every live advert station (newest first)
            for (auto it = app.adverts.rbegin(); it != app.adverts.rend(); ++it) {
                if (!has_or_mark_user(it->name)) {
                    HeardStation hs;
                    hs.callsign = it->name;
                    hs.last_rssi = it->rssi;
                    hs.last_snr = it->snr;
                    hs.hops = it->hops;
                    hs.last_heard_time = it->time.size() >= 8 ? it->time.substr(0, 8) : it->time;
                    hs.last_heard_epoch = now_epoch_secs();
                    hs.msg_count = 0;
                    room_users.push_back(std::move(hs));
                }
            }
            // Include EVERY single node in mesh_nodes (Companions, Repeaters, Room Servers, Sensors)
            for (const auto& nd : app.mesh_nodes) {
                if (!has_or_mark_user(nd.name)) {
                    HeardStation hs;
                    hs.callsign = nd.name;
                    hs.last_rssi = nd.live_rssi;
                    hs.last_snr = nd.live_snr;
                    hs.hops = nd.live_hops;
                    hs.last_heard_epoch = nd.last_advert_ts;
                    hs.msg_count = 0;
                    room_users.push_back(std::move(hs));
                }
            }
        }

        size_t room_user_count = 1 + room_users.size(); // 1 (self) + room_users
        const bool show_nicks_pane = !is_console_tab && (app.show_room_nicklist_override >= 0);

        if (is_console_tab) {
            draw_text("SYSTEM CONSOLE & 256-BUCKET RAINBOW PACKET SNIFFER TELEMETRY",
                      font_mono_small, D2D1::RectF(crypto_bar.left + 12, crypto_bar.top, crypto_bar.right - 12, crypto_bar.bottom),
                      hex_color(0x38BDF8));
        } else if (!active_tab.name.empty() && active_tab.name[0] == '@') {
            std::string peer = active_tab.name.substr(1);
            const auto* nd = app.find_mesh_node(peer);
            std::string khex = nd ? MeshGuiApp::node_key_hex(*nd) : "auto";
            std::ostringstream d_oss;
            d_oss << "DIRECT MSG: " << active_tab.name
                  << " | TYPE: " << (nd ? contact_type_name(nd->contact_type) : "Station")
                  << " | KEY PREFIX: " << khex
                  << " | ROUTE: " << ((nd && nd->out_path_len >= 0) ? ("Direct (" + std::to_string(nd->out_path_len) + "h)") : "Flood");
            draw_text(d_oss.str(), font_mono_small,
                      D2D1::RectF(crypto_bar.left + 10, crypto_bar.top, crypto_bar.right - 190, crypto_bar.bottom),
                      hex_color(0x34D399));

            D2D1_RECT_F users_btn = D2D1::RectF(crypto_bar.right - 186, crypto_bar.top + 4, crypto_bar.right - 102, crypto_bar.bottom - 4);
            fill_round_rect(users_btn, 5.0f, show_nicks_pane ? hex_color(0x10B981, 0.25f) : hex_color(0x162238));
            draw_round_rect(users_btn, 5.0f, show_nicks_pane ? hex_color(0x10B981) : hex_color(0x38BDF8, 0.6f), 1.0f);
            draw_text("Users: " + std::to_string(room_user_count), font_small, users_btn,
                      show_nicks_pane ? hex_color(0x10B981) : hex_color(0x38BDF8), DWRITE_TEXT_ALIGNMENT_CENTER);
            app.button_hits.push_back({users_btn, BTN_TOGGLE_NICKLIST, ""});

            if (nd) {
                std::string full_k = hex_string(nd->pub_key.data(), 32, "");
                D2D1_RECT_F copy_btn = D2D1::RectF(crypto_bar.right - 98, crypto_bar.top + 4, crypto_bar.right - 8, crypto_bar.bottom - 4);
                bool copied_now = (now_epoch_secs() < app.copy_psk_feedback_until);
                fill_round_rect(copy_btn, 5.0f, copied_now ? hex_color(0x10B981, 0.25f) : hex_color(0x162238));
                draw_round_rect(copy_btn, 5.0f, copied_now ? hex_color(0x10B981) : hex_color(0x38BDF8, 0.6f), 1.0f);
                draw_text(copied_now ? "Copied Key!" : "Copy Key", font_small, copy_btn,
                          copied_now ? hex_color(0x10B981) : hex_color(0x38BDF8), DWRITE_TEXT_ALIGNMENT_CENTER);
                app.button_hits.push_back({copy_btn, BTN_COPY_PSK, full_k});
            }
        } else {
            auto slot_opt = app.hw_slots.slot_for_channel(active_tab.name);
            auto psk = slot_opt ? app.hw_slots.slots[*slot_opt].psk : derive_psk(active_tab.name);
            uint8_t ch_hash = compute_channel_hash(psk);
            std::string slot_str = slot_opt ? ("S" + std::to_string(*slot_opt) + " [SYNCED]") : "Unassigned";

            std::ostringstream c_oss;
            c_oss << "ROOM: " << active_tab.name
                  << " | SLOT: " << slot_str
                  << " | BUCKET: 0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<int>(ch_hash)
                  << " | PSK: " << psk_to_hex(psk);

            draw_text(c_oss.str(), font_mono_small,
                      D2D1::RectF(crypto_bar.left + 10, crypto_bar.top, crypto_bar.right - 190, crypto_bar.bottom),
                      hex_color(0x94A3B8));

            // [Users: N] toggle button
            D2D1_RECT_F users_btn = D2D1::RectF(crypto_bar.right - 186, crypto_bar.top + 4, crypto_bar.right - 102, crypto_bar.bottom - 4);
            fill_round_rect(users_btn, 5.0f, show_nicks_pane ? hex_color(0x10B981, 0.25f) : hex_color(0x162238));
            draw_round_rect(users_btn, 5.0f, show_nicks_pane ? hex_color(0x10B981) : hex_color(0x38BDF8, 0.6f), 1.0f);
            draw_text("Users: " + std::to_string(room_user_count), font_small, users_btn,
                      show_nicks_pane ? hex_color(0x10B981) : hex_color(0x38BDF8), DWRITE_TEXT_ALIGNMENT_CENTER);
            app.button_hits.push_back({users_btn, BTN_TOGGLE_NICKLIST, ""});

            // [Copy PSK] button
            D2D1_RECT_F copy_btn = D2D1::RectF(crypto_bar.right - 98, crypto_bar.top + 4, crypto_bar.right - 8, crypto_bar.bottom - 4);
            bool copied_now = (now_epoch_secs() < app.copy_psk_feedback_until);
            fill_round_rect(copy_btn, 5.0f, copied_now ? hex_color(0x10B981, 0.25f) : hex_color(0x162238));
            draw_round_rect(copy_btn, 5.0f, copied_now ? hex_color(0x10B981) : hex_color(0x38BDF8, 0.6f), 1.0f);
            draw_text(copied_now ? "Copied PSK!" : "Copy PSK", font_small, copy_btn,
                      copied_now ? hex_color(0x10B981) : hex_color(0x38BDF8), DWRITE_TEXT_ALIGNMENT_CENTER);
            app.button_hits.push_back({copy_btn, BTN_COPY_PSK, psk_to_hex(psk)});
        }

        // 3. Message Feed Scrollback + mIRC-Style Room Participant Sidebar (y + 80 .. y + h - 82)
        const float nick_pane_w = show_nicks_pane ? ((w < 520.0f) ? 142.0f : 190.0f) : 0.0f;
        const float chat_right_x = show_nicks_pane ? (x + w - 16.0f - nick_pane_w) : (x + w - 10.0f);
        D2D1_RECT_F chat_box = D2D1::RectF(x + 10, y + 80, chat_right_x, y + h - 82);
        app.chat_view_rect = chat_box;
        fill_round_rect(chat_box, 8.0f, hex_color(0x090D16));
        draw_round_rect(chat_box, 8.0f, hex_color(0x1E293B), 1.0f);

        if (show_nicks_pane) {
            D2D1_RECT_F nick_box = D2D1::RectF(chat_right_x + 6.0f, y + 80, x + w - 10.0f, y + h - 82);
            app.nick_view_rect = nick_box;
            fill_round_rect(nick_box, 8.0f, hex_color(0x0B111E));
            draw_round_rect(nick_box, 8.0f, hex_color(0x1E293B), 1.0f);

            std::string nf_lower = app.nicklist_filter;
            std::transform(nf_lower.begin(), nf_lower.end(), nf_lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            std::vector<const HeardStation*> filtered_users;
            filtered_users.reserve(room_users.size());
            for (const auto& st : room_users) {
                if (!nf_lower.empty()) {
                    std::string cs_low = st.callsign;
                    std::transform(cs_low.begin(), cs_low.end(), cs_low.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (cs_low.find(nf_lower) == std::string::npos) continue;
                }
                filtered_users.push_back(&st);
            }

            // Header bar
            D2D1_RECT_F nhdr = D2D1::RectF(nick_box.left + 4, nick_box.top + 4, nick_box.right - 4, nick_box.top + 25);
            fill_round_rect(nhdr, 5.0f, hex_color(0x131C2E));
            std::string hdr_lbl = nf_lower.empty()
                                      ? ("ROOM USERS (" + std::to_string(room_user_count) + ")")
                                      : ("ROOM USERS (" + std::to_string(filtered_users.size()) + "/" + std::to_string(room_user_count) + ")");
            draw_text(hdr_lbl, font_small,
                      D2D1::RectF(nhdr.left + 8, nhdr.top, nhdr.right - 6, nhdr.bottom), hex_color(0x00E5FF));

            // Search / Filter Input Box right at the top of ROOM USERS
            D2D1_RECT_F nf_r = D2D1::RectF(nick_box.left + 4, nhdr.bottom + 3, nick_box.right - 4, nhdr.bottom + 29);
            bool nf_focused = (app.active_input == ActiveInputTarget::NicklistFilter);
            fill_round_rect(nf_r, 5.0f, nf_focused ? hex_color(0x112238) : hex_color(0x0D1829));
            draw_round_rect(nf_r, 5.0f, nf_focused ? hex_color(0x00E5FF) : hex_color(0x38BDF8, 0.75f), nf_focused ? 1.5f : 1.1f);
            std::string nf_disp = app.nicklist_filter.empty()
                                      ? (nf_focused ? "Search: _" : "Search users...")
                                      : (app.nicklist_filter + (nf_focused ? "_" : ""));
            draw_text(nf_disp, font_mono_small,
                      D2D1::RectF(nf_r.left + 7, nf_r.top, nf_r.right - (app.nicklist_filter.empty() ? 6 : 20), nf_r.bottom),
                      app.nicklist_filter.empty() && !nf_focused ? hex_color(0x94A3B8) : hex_color(0xF8FAFC));
            app.button_hits.push_back({nf_r, BTN_NICKLIST_FILTER_BOX, ""});
            if (!app.nicklist_filter.empty()) {
                D2D1_RECT_F nfc_r = D2D1::RectF(nf_r.right - 20, nf_r.top + 1, nf_r.right - 2, nf_r.bottom - 1);
                draw_text("x", font_small, nfc_r, hex_color(0x94A3B8), DWRITE_TEXT_ALIGNMENT_CENTER);
                app.button_hits.push_back({nfc_r, BTN_NICKLIST_FILTER_CLEAR, ""});
            }

            float ny = nf_r.bottom + 5.0f;
            const float nrow_h = 26.0f;
            const float nmax_y = nick_box.bottom - 20.0f;
            const uint64_t now_s = now_epoch_secs();

            // Row 0: Self ([YOU]) when unfiltered
            if (nf_lower.empty() && ny + nrow_h <= nmax_y) {
                D2D1_RECT_F sr = D2D1::RectF(nick_box.left + 5, ny, nick_box.right - 5, ny + nrow_h - 2);
                fill_round_rect(sr, 4.0f, hex_color(0x10B981, 0.14f));
                draw_round_rect(sr, 4.0f, hex_color(0x10B981, 0.45f), 1.0f);
                float scy = (sr.top + sr.bottom) * 0.5f;
                fill_round_rect(D2D1::RectF(sr.left + 5.8f, scy - 3.2f, sr.left + 12.2f, scy + 3.2f), 3.2f, hex_color(0x10B981));
                draw_text("+" + app.callsign, font_mono_small,
                          D2D1::RectF(sr.left + 16, sr.top, sr.right - 36, sr.bottom), hex_color(0x10B981));
                draw_text("YOU", font_mono_small,
                          D2D1::RectF(sr.right - 34, sr.top, sr.right - 5, sr.bottom),
                          hex_color(0x34D399), DWRITE_TEXT_ALIGNMENT_TRAILING);
                ny += nrow_h;
            }

            int max_nick_scroll = std::max(0, static_cast<int>(filtered_users.size()) - 4);
            app.nicklist_scroll = std::clamp(app.nicklist_scroll, 0, max_nick_scroll);

            // Room participants sorted by recency
            for (size_t idx = static_cast<size_t>(app.nicklist_scroll); idx < filtered_users.size(); ++idx) {
                const auto& st = *filtered_users[idx];
                if (ny + nrow_h > nmax_y) break;

                D2D1_RECT_F ur = D2D1::RectF(nick_box.left + 5, ny, nick_box.right - 5, ny + nrow_h - 2);
                bool uhov = pt_in_rect(app.mouse_pt, ur);
                bool usel = (_stricmp(app.replying_to_user.c_str(), st.callsign.c_str()) == 0) ||
                            (_stricmp(app.selected_station.c_str(), st.callsign.c_str()) == 0);
                fill_round_rect(ur, 4.0f, usel ? hex_color(0x00E5FF, 0.20f)
                                        : uhov ? hex_color(0x1E293B)
                                               : hex_color(0x0F172A));
                if (usel || uhov) {
                    draw_round_rect(ur, 4.0f, usel ? hex_color(0x00E5FF) : hex_color(0x334155), 1.0f);
                }

                // Online recency dot: green (<15m or heard live RF), amber (<2h), slate (history/contact)
                const auto* nd = app.find_mesh_node(st.callsign);
                uint32_t dot_hex = (nd && nd->heard_live_rf) ? 0x10B981 : 0x64748B;
                if (st.last_heard_epoch > 1000 && now_s >= st.last_heard_epoch) {
                    uint64_t age = now_s - st.last_heard_epoch;
                    if (age <= 900) dot_hex = 0x10B981;
                    else if (age <= 7200) dot_hex = 0xFBBF24;
                }
                float ucy = (ur.top + ur.bottom) * 0.5f;
                fill_round_rect(D2D1::RectF(ur.left + 5.8f, ucy - 3.2f, ur.left + 12.2f, ucy + 3.2f), 3.2f, hex_color(dot_hex));

                char prefix = (nd && nd->contact_type == 3) ? '@'
                            : (nd && nd->contact_type == 2) ? '^'
                                                            : '+';
                D2D1_COLOR_F ncol = (nd && nd->contact_type == 3) ? hex_color(0xC084FC)
                                  : (nd && nd->contact_type == 2) ? hex_color(0x38BDF8)
                                                                  : callsign_color(st.callsign);

                char meta_b[32] = "";
                if (st.msg_count > 0) {
                    std::snprintf(meta_b, sizeof(meta_b), "%zum", st.msg_count);
                } else if (nd && nd->heard_live_rf) {
                    std::snprintf(meta_b, sizeof(meta_b), "live");
                }

                draw_text(std::string(1, prefix) + st.callsign, font_mono_small,
                          D2D1::RectF(ur.left + 16, ur.top, ur.right - 34, ur.bottom), ncol);
                draw_text(meta_b, font_mono_small,
                          D2D1::RectF(ur.right - 34, ur.top, ur.right - 5, ur.bottom),
                          hex_color(0x64748B), DWRITE_TEXT_ALIGNMENT_TRAILING);

                app.nicklist_hits.push_back({ur, 0, st.callsign});
                ny += nrow_h;
            }

            // Footer hint
            draw_text("Tap: @Reply • 2x: DM", font_mono_small,
                      D2D1::RectF(nick_box.left + 6, nick_box.bottom - 18, nick_box.right - 6, nick_box.bottom - 2),
                      hex_color(0x64748B), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            app.nick_view_rect = D2D1::RectF(0, 0, 0, 0);
        }

        constexpr size_t MAX_ROUTE_LINES = 3;
        auto route_line_count = [=](const ChatMessage& m) -> size_t {
            if (m.is_system || m.routes.empty()) return 0;
            return std::min(m.routes.size(), MAX_ROUTE_LINES) + (m.routes.size() > MAX_ROUTE_LINES ? 1 : 0);
        };
        auto msg_height = [&](const ChatMessage& m) -> float {
            float hgt = 28.0f;
            if (!m.is_system && m.has_mono_image) hgt += 160.0f; // 150px (48x30 @ 5x Nearest-Neighbor) + 10px margin
            if (!m.is_system && !m.has_mono_image && !extract_hashtags(m.text).empty()) hgt += 24.0f;
            hgt += static_cast<float>(route_line_count(m)) * 16.0f;
            return hgt;
        };

        int total_msgs = static_cast<int>(active_tab.messages.size());
        int end_idx = std::max(0, total_msgs - active_tab.scroll_offset);
        // Walk backwards from the newest visible message until the box is full
        float avail_h = chat_box.bottom - chat_box.top - 10.0f;
        int start_idx = end_idx;
        float used_h = 0.0f;
        while (start_idx > 0) {
            float hgt = msg_height(active_tab.messages[start_idx - 1]);
            if (used_h + hgt > avail_h && start_idx < end_idx) break;
            used_h += hgt;
            --start_idx;
        }

        float my = chat_box.top + 6.0f;
        for (int i = start_idx; i < end_idx; ++i) {
            const auto& msg = active_tab.messages[i];
            auto tags_in_msg = (msg.is_system || msg.has_mono_image) ? std::vector<std::string>{} : extract_hashtags(msg.text);
            float item_h = msg_height(msg);
            if (my + item_h > chat_box.bottom - 4.0f && i > start_idx) break;

            D2D1_RECT_F card_r = D2D1::RectF(chat_box.left + 8, my, chat_box.right - 8, my + item_h - 2.0f);
            D2D1_RECT_F line_r = D2D1::RectF(chat_box.left + 8, my, chat_box.right - 8, my + 26.0f);

            if (msg.is_mention) {
                fill_round_rect(card_r, 4.0f, hex_color(0xEF4444, 0.14f));
            } else if (i % 2 == 1) {
                fill_round_rect(card_r, 4.0f, hex_color(0x0F172A, 0.55f));
            }

            // Timestamp
            draw_text("[" + msg.timestamp + "]", font_mono_small,
                      D2D1::RectF(line_r.left + 6, line_r.top, line_r.left + 78, line_r.bottom),
                      hex_color(0x64748B));

            if (msg.is_system) {
                // If this is a [LIVE BEACON] <NodeName> (...) line, allow double-clicking it to reply to that node!
                const std::string beacon_prefix = "[LIVE BEACON] ";
                if (msg.text.rfind(beacon_prefix, 0) == 0) {
                    auto paren_pos = msg.text.find(" (", beacon_prefix.size());
                    if (paren_pos != std::string::npos) {
                        std::string b_node = msg.text.substr(beacon_prefix.size(), paren_pos - beacon_prefix.size());
                        if (pt_in_rect(app.mouse_pt, line_r)) {
                            fill_round_rect(line_r, 4.0f, hex_color(0x1E293B, 0.7f));
                        }
                        app.chat_user_hits.push_back({line_r, 0, b_node});
                    }
                }
                draw_text("* " + msg.text, font_mono,
                          D2D1::RectF(line_r.left + 82, line_r.top, line_r.right - 6, line_r.bottom),
                          hex_color(0xFBBF24));
            } else {
                // Sender badge (double-clickable anywhere on row or sender badge to reply directly!)
                float cur_x = line_r.left + 82.0f;
                std::string nick_str = "<" + msg.sender + ">";
                float nick_w = static_cast<float>(nick_str.size()) * 8.2f + 6.0f;
                D2D1_RECT_F nick_r = D2D1::RectF(cur_x - 2.0f, line_r.top + 2.0f, cur_x + nick_w, line_r.bottom - 2.0f);
                bool row_hov = pt_in_rect(app.mouse_pt, line_r);
                if (row_hov) {
                    fill_round_rect(nick_r, 4.0f, hex_color(0x1E293B, 0.9f));
                    draw_round_rect(nick_r, 4.0f, hex_color(0x00E5FF, 0.55f), 1.0f);
                }
                D2D1_COLOR_F n_col = msg.is_self ? hex_color(0x10B981) : callsign_color(msg.sender);
                draw_text(nick_str, font_mono, D2D1::RectF(cur_x, line_r.top, cur_x + nick_w, line_r.bottom), n_col);
                app.chat_user_hits.push_back({line_r, 0, msg.sender});
                cur_x += nick_w + 4.0f;

                // RF badge pill (-75dBm/+8.5dB)
                if (msg.has_rf) {
                    char rf_b[48];
                    std::snprintf(rf_b, sizeof(rf_b), "%ddBm %+.1fdB", msg.rssi, msg.snr);
                    float rf_w = 104.0f;
                    D2D1_RECT_F rf_pill = D2D1::RectF(cur_x, line_r.top + 3, cur_x + rf_w, line_r.bottom - 3);
                    fill_round_rect(rf_pill, 4.0f, hex_color(0x162238));
                    draw_text(rf_b, font_mono_small, rf_pill, hex_color(0x38BDF8), DWRITE_TEXT_ALIGNMENT_CENTER);
                    cur_x += rf_w + 8.0f;
                }

                // Full unobscured message text
                draw_text(msg.text, font_ui,
                          D2D1::RectF(cur_x, line_r.top, line_r.right - 8, line_r.bottom),
                          msg.is_mention ? hex_color(0xFCA5A5) : hex_color(0xF1F5F9));

                float sub_y = line_r.bottom;

                // Inline 1-Bit Monochrome Bitmap Canvas (48x30 scaled 5x -> 240x150 Nearest-Neighbor)
                if (msg.has_mono_image) {
                    float img_x = line_r.left + 82.0f;
                    float img_y = sub_y + 4.0f;
                    draw_mono_bitmap_nearest(msg.mono_image, img_x, img_y, 5.0f);

                    D2D1_RECT_F badge_r = D2D1::RectF(img_x + 252.0f, img_y + 2.0f,
                                                      std::min(line_r.right - 8.0f, img_x + 478.0f), img_y + 68.0f);
                    if (badge_r.right > badge_r.left + 70.0f) {
                        fill_round_rect(badge_r, 5.0f, hex_color(0x111A2B, 0.85f));
                        draw_round_rect(badge_r, 5.0f, hex_color(0x00E5FF, 0.45f), 1.0f);
                        draw_text("0x49 MONO_IMG • 48x30 (1-Bit)", font_mono_small,
                                  D2D1::RectF(badge_r.left + 8, badge_r.top + 4, badge_r.right - 6, badge_r.top + 24),
                                  hex_color(0x00E5FF));
                        draw_text("180B Packed • 181B Single-Pkt", font_mono_small,
                                  D2D1::RectF(badge_r.left + 8, badge_r.top + 24, badge_r.right - 6, badge_r.top + 44),
                                  hex_color(0x10B981));
                        draw_text("5x Nearest-Neighbor (240x150)", font_mono_small,
                                  D2D1::RectF(badge_r.left + 8, badge_r.top + 44, badge_r.right - 6, badge_r.top + 64),
                                  hex_color(0x94A3B8));
                    }
                    sub_y += 160.0f;
                }

                // Clickable #hashtag pills on dedicated action sub-row below the message text
                if (!tags_in_msg.empty()) {
                    float pill_x = line_r.left + 82.0f;
                    float pill_top = sub_y + 1.0f;
                    float pill_bot = pill_top + 22.0f;
                    draw_text("DISCOVERED CHANNELS:", font_mono_small,
                              D2D1::RectF(pill_x, pill_top, pill_x + 132.0f, pill_bot),
                              hex_color(0x64748B));
                    pill_x += 136.0f;

                    for (const auto& tag : tags_in_msg) {
                        std::string btn_label = "+ Join " + tag;
                        float bw = static_cast<float>(btn_label.size()) * 7.2f + 16.0f;
                        if (pill_x + bw > line_r.right - 8.0f) break;
                        D2D1_RECT_F tag_r = D2D1::RectF(pill_x, pill_top, pill_x + bw, pill_bot);
                        bool thov = pt_in_rect(app.mouse_pt, tag_r);
                        fill_round_rect(tag_r, 4.0f, thov ? hex_color(0x00E5FF, 0.3f) : hex_color(0x16243D));
                        draw_round_rect(tag_r, 4.0f, hex_color(0x00E5FF, 0.75f), 1.0f);
                        draw_text(btn_label, font_small, tag_r, hex_color(0x00E5FF), DWRITE_TEXT_ALIGNMENT_CENTER);
                        app.chat_hashtag_hits.push_back({tag_r, 0, tag});
                        pill_x += bw + 6.0f;
                    }
                    sub_y += 24.0f;
                }

                // Repeater trace: one line per distinct over-the-air route this message took
                if (!msg.routes.empty()) {
                    float ry = sub_y;
                    float rx0 = line_r.left + 82.0f;
                    size_t shown = std::min(msg.routes.size(), MAX_ROUTE_LINES);
                    for (size_t ri = 0; ri < shown; ++ri) {
                        const auto& rt = msg.routes[ri];
                        float rx = rx0;
                        float rbot = ry + 15.0f;
                        const float right_lim = line_r.right - 120.0f;

                        auto seg = [&](const std::string& s, D2D1_COLOR_F col, IDWriteTextFormat* fmt) {
                            float sw = static_cast<float>(s.size()) * 7.0f + 2.0f;
                            if (rx >= right_lim) return;
                            draw_text(s, fmt, D2D1::RectF(rx, ry, std::min(rx + sw, right_lim), rbot), col);
                            rx += sw;
                        };

                        seg(ri == 0 ? "route " : "  alt ", hex_color(0x64748B), font_mono_small);
                        seg(msg.sender, callsign_color(msg.sender), font_mono_small);
                        if (rt.hops.empty()) {
                            seg(" => direct => ", hex_color(0x34D399), font_mono_small);
                        } else {
                            for (const auto& hop : rt.hops) {
                                seg(" > ", hex_color(0x475569), font_mono_small);
                                if (hop.resolved) seg(hop.label, hex_color(0xC084FC), font_mono_small);
                                else seg("[" + hop.label + "]", hex_color(0x94A3B8), font_mono_small);
                            }
                            seg(" > ", hex_color(0x475569), font_mono_small);
                        }
                        seg("ME", hex_color(0x10B981), font_mono_small);

                        char sig[64];
                        std::snprintf(sig, sizeof(sig), "%zuh  %ddBm %+.1fdB", rt.hops.size(), rt.rssi, rt.snr);
                        draw_text(sig, font_mono_small, D2D1::RectF(line_r.right - 118.0f, ry, line_r.right - 8.0f, rbot),
                                  hex_color(0x38BDF8), DWRITE_TEXT_ALIGNMENT_TRAILING);
                        ry += 16.0f;
                    }
                    if (msg.routes.size() > MAX_ROUTE_LINES) {
                        draw_text("  +" + std::to_string(msg.routes.size() - MAX_ROUTE_LINES) +
                                      " more route(s) - see Paths view",
                                  font_mono_small, D2D1::RectF(rx0, ry, line_r.right - 8.0f, ry + 15.0f), hex_color(0x64748B));
                    }
                }
            }

            my += item_h;
        }

        // 4. Quick-Action Chip Bar + Direct Reply Target Indicator (y + h - 76 .. y + h - 50)
        struct QuickChip { int id; const char* label; const char* payload; };
        static const QuickChip CHIPS[] = {
            {BTN_QUICK_EMCOMM,    "#emcomm",    "#emcomm"},
            {BTN_QUICK_MESHCORE,  "#meshcore",  "#meshcore"},
            {BTN_QUICK_WEATHER,   "#weather",   "#weather"},
            {BTN_QUICK_HAMRADIO,  "#hamradio",  "#hamradio"},
            {BTN_QUICK_GENERAL,   "#general",   "#general"},
            {BTN_QUICK_SLOTS,     "/slots",     "/slots"}
        };

        float cx = x + 10.0f;
        float cy = y + h - 75.0f;
        draw_text("QUICK JOIN:", font_small, D2D1::RectF(cx, cy, cx + 76, cy + 22), hex_color(0x64748B));
        cx += 78.0f;
        for (const auto& chip : CHIPS) {
            float cw = static_cast<float>(std::strlen(chip.label)) * 7.8f + 18.0f;
            D2D1_RECT_F cr = D2D1::RectF(cx, cy, cx + cw, cy + 22.0f);
            bool chov = pt_in_rect(app.mouse_pt, cr);
            fill_round_rect(cr, 5.0f, chov ? hex_color(0x1E293B) : hex_color(0x131C2E));
            draw_round_rect(cr, 5.0f, chov ? hex_color(0x00E5FF) : hex_color(0x26354D), 1.0f);
            draw_text(chip.label, font_mono_small, cr, hex_color(0x38BDF8), DWRITE_TEXT_ALIGNMENT_CENTER);
            app.button_hits.push_back({cr, chip.id, chip.payload});
            cx += cw + 6.0f;
        }

        // Right side of Quick-Action Bar: Direct Reply Badge or Double-Click Hint
        if (!app.replying_to_user.empty()) {
            std::string rep_lbl = "REPLYING TO @[" + app.replying_to_user + "]   [x]";
            float rw = std::min(280.0f, static_cast<float>(rep_lbl.size()) * 7.2f + 20.0f);
            D2D1_RECT_F rep_r = D2D1::RectF(x + w - 10.0f - rw, cy, x + w - 10.0f, cy + 22.0f);
            if (rep_r.left > cx + 8.0f) {
                bool rhov = pt_in_rect(app.mouse_pt, rep_r);
                fill_round_rect(rep_r, 5.0f, rhov ? hex_color(0x10B981, 0.30f) : hex_color(0x10B981, 0.18f));
                draw_round_rect(rep_r, 5.0f, hex_color(0x10B981), 1.2f);
                draw_text(rep_lbl, font_mono_small, rep_r, hex_color(0x34D399), DWRITE_TEXT_ALIGNMENT_CENTER);
                app.button_hits.push_back({rep_r, BTN_CLEAR_REPLY, ""});
            }
        } else if (x + w - 10.0f - cx > 205.0f) {
            draw_text("Double-click any user to reply @[user]", font_mono_small,
                      D2D1::RectF(cx + 8.0f, cy, x + w - 12.0f, cy + 22.0f),
                      hex_color(0x64748B), DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        // 5. Bottom Message Composer Input Box + [📎 IMG] Attach Button + [Transmit TX] Button
        D2D1_RECT_F input_r = D2D1::RectF(x + 10, y + h - 46, x + w - 208, y + h - 10);
        bool chat_focused = (app.active_input == ActiveInputTarget::ChatComposer);
        bool has_reply = !app.replying_to_user.empty();
        fill_round_rect(input_r, 7.0f, hex_color(0x090D16));
        draw_round_rect(input_r, 7.0f, has_reply ? hex_color(0x10B981)
                                     : chat_focused ? hex_color(0x00E5FF)
                                                    : hex_color(0x334155), 1.5f);

        float text_right = input_r.right - 12.0f;
        if (has_reply) {
            std::string rep_lbl = "REPLY @[" + app.replying_to_user + "] [x]";
            float rw = std::min(220.0f, static_cast<float>(rep_lbl.size()) * 7.0f + 16.0f);
            D2D1_RECT_F rep_r = D2D1::RectF(input_r.right - rw - 6.0f, input_r.top + 6.0f,
                                            input_r.right - 6.0f, input_r.bottom - 6.0f);
            bool rhov = pt_in_rect(app.mouse_pt, rep_r);
            fill_round_rect(rep_r, 5.0f, rhov ? hex_color(0x10B981, 0.32f) : hex_color(0x10B981, 0.18f));
            draw_round_rect(rep_r, 5.0f, hex_color(0x10B981), 1.0f);
            draw_text(rep_lbl, font_mono_small, rep_r, hex_color(0x34D399), DWRITE_TEXT_ALIGNMENT_CENTER);
            app.button_hits.push_back({rep_r, BTN_CLEAR_REPLY, ""});
            text_right = rep_r.left - 8.0f;
        }

        std::string prompt = "[" + app.callsign + " @ " + active_tab.name + "] > ";
        std::string disp_input = app.chat_input.empty() && !chat_focused
                                     ? (prompt + "Type message or attach 1-bit image (+IMG)...")
                                     : (prompt + app.chat_input + (chat_focused ? "_" : ""));
        draw_text(disp_input, font_mono,
                  D2D1::RectF(input_r.left + 12, input_r.top, text_right, input_r.bottom),
                  app.chat_input.empty() && !chat_focused ? hex_color(0x64748B) : hex_color(0xF8FAFC));
        app.button_hits.push_back({D2D1::RectF(input_r.left, input_r.top, text_right, input_r.bottom), BTN_CHAT_INPUT_BOX, ""});

        // [+IMG] Attach Image button next to the channel text input box
        D2D1_RECT_F attach_btn = D2D1::RectF(x + w - 202, y + h - 46, x + w - 132, y + h - 10);
        bool att_hov = pt_in_rect(app.mouse_pt, attach_btn);
        fill_round_rect(attach_btn, 7.0f, app.pending_image_ready ? hex_color(0x10B981, 0.28f)
                                        : att_hov                 ? hex_color(0x1E293B)
                                                                  : hex_color(0x131C2E));
        draw_round_rect(attach_btn, 7.0f, app.pending_image_ready ? hex_color(0x10B981)
                                        : att_hov                 ? hex_color(0x00E5FF)
                                                                  : hex_color(0x334155), 1.4f);
#ifdef __APPLE__
        draw_text("[+IMG]", font_ui_bold, attach_btn,
                  app.pending_image_ready ? hex_color(0x10B981) : hex_color(0x38BDF8), DWRITE_TEXT_ALIGNMENT_CENTER);
#else
        draw_text("\xF0\x9F\x93\x8E IMG", font_ui_bold, attach_btn,
                  app.pending_image_ready ? hex_color(0x10B981) : hex_color(0x38BDF8), DWRITE_TEXT_ALIGNMENT_CENTER);
#endif
        app.button_hits.push_back({attach_btn, BTN_ATTACH_IMAGE, ""});

        D2D1_RECT_F tx_btn = D2D1::RectF(x + w - 126, y + h - 46, x + w - 10, y + h - 10);
        bool tx_hov = pt_in_rect(app.mouse_pt, tx_btn);
        fill_round_rect(tx_btn, 7.0f, tx_hov ? hex_color(0x00E5FF, 0.32f) : hex_color(0x00E5FF, 0.18f));
        draw_round_rect(tx_btn, 7.0f, hex_color(0x00E5FF), 1.5f);
        draw_text("TRANSMIT TX", font_ui_bold, tx_btn, hex_color(0x00E5FF), DWRITE_TEXT_ALIGNMENT_CENTER);
        app.button_hits.push_back({tx_btn, BTN_TRANSMIT_TX, ""});

        // 6. Pending 48x30 Dithered Thumbnail Preview Modal/Badge (when an image is staged for transmission)
        if (app.pending_image_ready) {
            float mw = std::min(w - 28.0f, 515.0f);
            float mh = 114.0f;
            float mx = std::max(x + 14.0f, x + w - mw - 14.0f);
            float my_modal = y + h - 82.0f - mh;
            D2D1_RECT_F modal_r = D2D1::RectF(mx, my_modal, mx + mw, my_modal + mh);

            fill_round_rect(modal_r, 8.0f, hex_color(0x0B111E, 0.97f));
            draw_round_rect(modal_r, 8.0f, hex_color(0x00E5FF), 1.6f);

            // Render 48x30 dithered thumbnail at 3x Nearest-Neighbor scale (144x90)
            draw_mono_bitmap_nearest(app.pending_image_bits, mx + 12.0f, my_modal + 12.0f, 3.0f);

            float info_x = mx + 168.0f;
            std::string target_ch = (app.active_tab_idx == 0 || (!active_tab.name.empty() && active_tab.name[0] == '@'))
                                        ? std::string(PUBLIC_CHANNEL_NAME)
                                        : active_tab.name;
            draw_text("READY: 48x30 1-BIT TACTICAL BITMAP (" + app.pending_image_name + ")", font_ui_bold,
                      D2D1::RectF(info_x, my_modal + 10.0f, modal_r.right - 10.0f, my_modal + 32.0f),
                      hex_color(0x00E5FF));
            draw_text("Floyd-Steinberg Dithered • 180B Packed • 181B Single-Packet (Opcode 0x49) -> " + target_ch,
                      font_mono_small,
                      D2D1::RectF(info_x, my_modal + 34.0f, modal_r.right - 10.0f, my_modal + 54.0f),
                      hex_color(0x94A3B8));

            D2D1_RECT_F send_img_btn = D2D1::RectF(info_x, my_modal + 64.0f, info_x + 188.0f, my_modal + 100.0f);
            bool shov = pt_in_rect(app.mouse_pt, send_img_btn);
            fill_round_rect(send_img_btn, 6.0f, shov ? hex_color(0x10B981, 0.36f) : hex_color(0x10B981, 0.22f));
            draw_round_rect(send_img_btn, 6.0f, hex_color(0x10B981), 1.5f);
            draw_text("SEND 181B IMAGE", font_ui_bold, send_img_btn, hex_color(0x10B981), DWRITE_TEXT_ALIGNMENT_CENTER);
            app.button_hits.push_back({send_img_btn, BTN_CONFIRM_SEND_IMAGE, ""});

            D2D1_RECT_F cancel_img_btn = D2D1::RectF(info_x + 196.0f, my_modal + 64.0f,
                                                     std::min(modal_r.right - 10.0f, info_x + 306.0f), my_modal + 100.0f);
            bool chov_c = pt_in_rect(app.mouse_pt, cancel_img_btn);
            fill_round_rect(cancel_img_btn, 6.0f, chov_c ? hex_color(0xEF4444, 0.28f) : hex_color(0x1E293B));
            draw_round_rect(cancel_img_btn, 6.0f, chov_c ? hex_color(0xEF4444) : hex_color(0x475569), 1.2f);
            draw_text("CANCEL [X]", font_small, cancel_img_btn,
                      chov_c ? hex_color(0xFCA5A5) : hex_color(0x94A3B8), DWRITE_TEXT_ALIGNMENT_CENTER);
            app.button_hits.push_back({cancel_img_btn, BTN_CANCEL_SEND_IMAGE, ""});
        }
    }

    // ========================================================================
    // MODULE 2: PUBLIC MESH SYSTEM (Repeaters/Rooms/Nodes)
    // ========================================================================
    void render_module_public_mesh(MeshGuiApp& app, float x, float y, float w, float h) {
        D2D1_RECT_F st_card = D2D1::RectF(x, y, x + w, y + h);
        app.nodes_view_rect = st_card;
        fill_round_rect(st_card, 10.0f, hex_color(0x0F172A));
        draw_round_rect(st_card, 10.0f, hex_color(0x1E293B), 1.2f);

        std::string nf_lower = app.nicklist_filter;
        std::transform(nf_lower.begin(), nf_lower.end(), nf_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        const auto& active_tab = app.tabs[app.active_tab_idx];
        if (!app.mesh_nodes.empty()) {
            draw_module_drag_header(app, MOD_PUBLIC_MESH, x, y, w,
                                    "PUBLIC MESH SYSTEM (" + std::to_string(app.mesh_nodes.size()) + ")", 0x00E5FF);

            char count_buf[96];
            std::snprintf(count_buf, sizeof(count_buf), "%zu Repeaters | %zu Rooms | %zu Stations",
                          app.repeater_count, app.room_server_count, app.chat_node_count);
            draw_text(count_buf, font_mono_small,
                      D2D1::RectF(x + 12, y + 24, x + w - 12, y + 40), hex_color(0x94A3B8));

            // Search / Filter Input Box inside PUBLIC MESH SYSTEM
            D2D1_RECT_F nf_r = D2D1::RectF(x + 10, y + 42, x + w - 10, y + 65);
            bool nf_focused = (app.active_input == ActiveInputTarget::NicklistFilter);
            fill_round_rect(nf_r, 4.0f, hex_color(0x090D16));
            draw_round_rect(nf_r, 4.0f, nf_focused ? hex_color(0x00E5FF) : hex_color(0x26354D), 1.1f);
            std::string nf_disp = app.nicklist_filter.empty()
                                      ? (nf_focused ? "Search nodes/users: _" : "Search nodes/users...")
                                      : (app.nicklist_filter + (nf_focused ? "_" : ""));
            draw_text(nf_disp, font_mono_small,
                      D2D1::RectF(nf_r.left + 8, nf_r.top, nf_r.right - (app.nicklist_filter.empty() ? 8 : 22), nf_r.bottom),
                      app.nicklist_filter.empty() && !nf_focused ? hex_color(0x64748B) : hex_color(0xE2E8F0));
            app.button_hits.push_back({nf_r, BTN_NICKLIST_FILTER_BOX, ""});
            if (!app.nicklist_filter.empty()) {
                D2D1_RECT_F nfc_r = D2D1::RectF(nf_r.right - 22, nf_r.top + 1, nf_r.right - 2, nf_r.bottom - 1);
                draw_text("x", font_small, nfc_r, hex_color(0x94A3B8), DWRITE_TEXT_ALIGNMENT_CENTER);
                app.button_hits.push_back({nfc_r, BTN_NICKLIST_FILTER_CLEAR, ""});
            }

            float sy = y + 69.0f;
            int idx = 0;
            for (const auto& nd : app.mesh_nodes) {
                if (!nf_lower.empty()) {
                    std::string nl = nd.name;
                    std::transform(nl.begin(), nl.end(), nl.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (nl.find(nf_lower) == std::string::npos) continue;
                }
                if (idx++ < app.nodes_scroll) continue;
                if (sy + 42.0f > st_card.bottom - 6.0f) break;

                D2D1_RECT_F sr = D2D1::RectF(x + 10, sy, x + w - 10, sy + 40.0f);
                bool shov = pt_in_rect(app.mouse_pt, sr);
                bool is_sel = (!app.selected_station.empty() && app.selected_station == nd.name) ||
                              (!app.replying_to_user.empty() && app.replying_to_user == nd.name);
                fill_round_rect(sr, 6.0f, is_sel ? hex_color(0x00E5FF, 0.16f)
                                        : shov   ? hex_color(0x1E293B)
                                        : nd.heard_live_rf ? hex_color(0x10B981, 0.14f)
                                                           : hex_color(0x131C2E));
                draw_round_rect(sr, 6.0f, is_sel ? hex_color(0x00E5FF, 0.85f)
                                        : nd.heard_live_rf ? hex_color(0x10B981, 0.7f)
                                                           : hex_color(0x26354D), 1.0f);

                // Node name
                uint32_t name_col = (nd.contact_type == 2) ? 0x38BDF8
                                  : (nd.contact_type == 3) ? 0xC084FC
                                                           : 0x34D399;
                draw_text(nd.name, font_ui_bold,
                          D2D1::RectF(x + 16, sy + 2, x + w - 92, sy + 21),
                          hex_color(name_col));

                // Type + 2-byte hop ID badge (or DBL-CLICK REPLY hint on hover)
                const char* t_prefix = (nd.contact_type == 2) ? "RPT"
                                     : (nd.contact_type == 3) ? "ROOM"
                                                              : "NODE";
                char badge_b[24];
                if (shov) {
                    std::snprintf(badge_b, sizeof(badge_b), "DBL-CLICK");
                } else {
                    std::snprintf(badge_b, sizeof(badge_b), "%s %02X%02X",
                                  t_prefix, nd.pub_key[0], nd.pub_key[1]);
                }
                D2D1_RECT_F h_pill = D2D1::RectF(x + w - 88, sy + 3, x + w - 14, sy + 19);
                fill_round_rect(h_pill, 4.0f, shov ? hex_color(0x00E5FF, 0.25f) : hex_color(name_col, 0.18f));
                draw_text(badge_b, font_mono_small, h_pill,
                          shov ? hex_color(0x00E5FF) : hex_color(name_col), DWRITE_TEXT_ALIGNMENT_CENTER);

                // Sub-line: Live RF metrics OR GPS coordinates + path
                char meta_b[96];
                if (nd.heard_live_rf) {
                    std::snprintf(meta_b, sizeof(meta_b), "LIVE RX: %ddBm | %+.1fdB SNR | %u hop%s",
                                  nd.live_rssi, nd.live_snr, nd.live_hops, (nd.live_hops == 1 ? "" : "s"));
                } else if (nd.lat_e6 != 0 || nd.lon_e6 != 0) {
                    std::snprintf(meta_b, sizeof(meta_b), "GPS: %.4f, %.4f | %s",
                                  nd.lat_e6 / 1000000.0, nd.lon_e6 / 1000000.0,
                                  (nd.out_path_len < 0 ? "Flood" : "Direct"));
                } else {
                    std::snprintf(meta_b, sizeof(meta_b), "Mesh Station | Route: %s",
                                  (nd.out_path_len < 0 ? "Flood" : "Direct"));
                }
                draw_text(meta_b, font_mono_small,
                          D2D1::RectF(x + 16, sy + 20, x + w - 14, sy + 38),
                          nd.heard_live_rf ? hex_color(0x10B981) : hex_color(0x64748B));

                app.station_hits.push_back({sr, 0, nd.name});
                sy += 43.0f;
            }
        } else {
            draw_module_drag_header(app, MOD_PUBLIC_MESH, x, y, w,
                                    "HEARD STATIONS (" + std::to_string(active_tab.nicks.size()) + ")", 0xF472B6);

            // Search / Filter Input Box inside HEARD STATIONS
            D2D1_RECT_F nf_r = D2D1::RectF(x + 10, y + 30, x + w - 10, y + 54);
            bool nf_focused = (app.active_input == ActiveInputTarget::NicklistFilter);
            fill_round_rect(nf_r, 5.0f, nf_focused ? hex_color(0x112238) : hex_color(0x0D1829));
            draw_round_rect(nf_r, 5.0f, nf_focused ? hex_color(0x00E5FF) : hex_color(0x38BDF8, 0.75f), nf_focused ? 1.5f : 1.1f);
            std::string nf_disp = app.nicklist_filter.empty()
                                      ? (nf_focused ? "Search stations: _" : "Search stations...")
                                      : (app.nicklist_filter + (nf_focused ? "_" : ""));
            draw_text(nf_disp, font_mono_small,
                      D2D1::RectF(nf_r.left + 8, nf_r.top, nf_r.right - (app.nicklist_filter.empty() ? 8 : 22), nf_r.bottom),
                      app.nicklist_filter.empty() && !nf_focused ? hex_color(0x94A3B8) : hex_color(0xF8FAFC));
            app.button_hits.push_back({nf_r, BTN_NICKLIST_FILTER_BOX, ""});
            if (!app.nicklist_filter.empty()) {
                D2D1_RECT_F nfc_r = D2D1::RectF(nf_r.right - 22, nf_r.top + 1, nf_r.right - 2, nf_r.bottom - 1);
                draw_text("x", font_small, nfc_r, hex_color(0x94A3B8), DWRITE_TEXT_ALIGNMENT_CENTER);
                app.button_hits.push_back({nfc_r, BTN_NICKLIST_FILTER_CLEAR, ""});
            }

            float sy = y + 59.0f;
            if (active_tab.nicks.empty()) {
                draw_text("Syncing Public MeshCore nodes...", font_ui,
                          D2D1::RectF(x + 14, sy + 8, x + w - 14, sy + 36), hex_color(0x64748B));
            } else {
                for (const auto& st : active_tab.nicks) {
                    if (!nf_lower.empty()) {
                        std::string sl = st.callsign;
                        std::transform(sl.begin(), sl.end(), sl.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (sl.find(nf_lower) == std::string::npos) continue;
                    }
                    if (sy + 44.0f > st_card.bottom - 6.0f) break;
                    D2D1_RECT_F sr = D2D1::RectF(x + 10, sy, x + w - 10, sy + 42.0f);
                    bool shov = pt_in_rect(app.mouse_pt, sr);
                    bool is_sel = (!app.selected_station.empty() && app.selected_station == st.callsign) ||
                                  (!app.replying_to_user.empty() && app.replying_to_user == st.callsign);
                    fill_round_rect(sr, 6.0f, is_sel ? hex_color(0x00E5FF, 0.16f)
                                            : shov   ? hex_color(0x1E293B)
                                                     : hex_color(0x131C2E));
                    draw_round_rect(sr, 6.0f, is_sel ? hex_color(0x00E5FF, 0.85f) : hex_color(0x26354D), 1.0f);

                    draw_signal_bars(x + 18, sy + 7, st.last_snr);
                    draw_text(st.callsign, font_ui_bold,
                              D2D1::RectF(x + 44, sy + 2, x + w - 90, sy + 22),
                              callsign_color(st.callsign));

                    std::string hop_lbl = shov ? "DBL-CLICK"
                                               : ((st.hops <= 1) ? "DIRECT" : (std::to_string(st.hops) + " HOPS"));
                    D2D1_RECT_F h_pill = D2D1::RectF(x + w - 82, sy + 4, x + w - 16, sy + 20);
                    fill_round_rect(h_pill, 4.0f, shov ? hex_color(0x00E5FF, 0.25f) : hex_color(0x1E293B));
                    draw_text(hop_lbl, font_small, h_pill,
                              shov ? hex_color(0x00E5FF) : hex_color(0x94A3B8), DWRITE_TEXT_ALIGNMENT_CENTER);

                    char meta_b[64];
                    std::snprintf(meta_b, sizeof(meta_b), "%ddBm | %+.1fdB SNR | %s",
                                  st.last_rssi, st.last_snr, st.last_heard_time.c_str());
                    draw_text(meta_b, font_mono_small,
                              D2D1::RectF(x + 18, sy + 22, x + w - 16, sy + 40), hex_color(0x38BDF8));

                    app.station_hits.push_back({sr, 0, st.callsign});
                    sy += 46.0f;
                }
            }
        }
    }

    // ========================================================================
    // MODULE 3: RADIO HARDWARE SLOTS (0..7 LRU)
    // ========================================================================
    void render_module_hw_slots(MeshGuiApp& app, float x, float y, float w, float h) {
        float slots_y = y;
        float slots_h = h;
        D2D1_RECT_F slots_card = D2D1::RectF(x, slots_y, x + w, slots_y + slots_h);
        fill_round_rect(slots_card, 10.0f, hex_color(0x0F172A));
        draw_round_rect(slots_card, 10.0f, hex_color(0x1E293B), 1.2f);

        draw_module_drag_header(app, MOD_HW_SLOTS, x, slots_y, w, "RADIO HARDWARE SLOTS (0..7 LRU)", 0x38BDF8);

        const auto& active_tab = app.tabs[app.active_tab_idx];
        float ky = slots_y + 32.0f;
        float row_step = std::clamp((slots_h - 38.0f) / 8.0f, 18.0f, 34.0f);
        for (size_t i = 0; i < NUM_HW_CHANNEL_SLOTS && (ky + row_step - 3.0f) <= slots_card.bottom - 4.0f; ++i) {
            const auto& slot = app.hw_slots.slots[i];
            D2D1_RECT_F kr = D2D1::RectF(x + 10, ky, x + w - 10, ky + row_step - 3.0f);
            bool occupied = !slot.channel_name.empty();
            bool is_active_ch = occupied && (slot.channel_name == active_tab.name);

            fill_round_rect(kr, 5.0f, is_active_ch ? hex_color(0x00E5FF, 0.16f)
                                    : occupied     ? hex_color(0x131C2E)
                                                   : hex_color(0x0B111E));
            draw_round_rect(kr, 5.0f, is_active_ch ? hex_color(0x00E5FF) : hex_color(0x1E293B), 1.0f);

            std::string left_lbl = "S" + std::to_string(i) + ": " + (occupied ? slot.channel_name : "<empty>");
            draw_text(left_lbl, occupied ? font_ui_bold : font_ui,
                      D2D1::RectF(kr.left + 8, kr.top, kr.right - 95, kr.bottom),
                      occupied ? hex_color(0xE2E8F0) : hex_color(0x475569));

            if (occupied) {
                char r_lbl[32];
                std::snprintf(r_lbl, sizeof(r_lbl), "0x%02X %s",
                              slot.channel_hash, (i == 0 ? "PINNED" : "SYNCED"));
                draw_text(r_lbl, font_mono_small,
                          D2D1::RectF(kr.right - 95, kr.top, kr.right - 8, kr.bottom),
                          i == 0 ? hex_color(0xFBBF24) : hex_color(0x10B981),
                          DWRITE_TEXT_ALIGNMENT_TRAILING);
                app.slot_hits.push_back({kr, static_cast<int>(i), slot.channel_name});
            }

            ky += row_step;
        }
    }

    // ========================================================================
    // SNOOPING TOOLKIT VIEWS
    // ========================================================================
    float pill(MeshGuiApp& app, float x, float y, const std::string& label, bool active, uint32_t accent, int id,
               float h = 24.0f) {
        float bw = static_cast<float>(label.size()) * 7.1f + 20.0f;
        D2D1_RECT_F r = D2D1::RectF(x, y, x + bw, y + h);
        bool hov = pt_in_rect(app.mouse_pt, r);
        fill_round_rect(r, 6.0f, active ? hex_color(accent, 0.22f) : hov ? hex_color(0x1E293B) : hex_color(0x131C2E));
        draw_round_rect(r, 6.0f, active ? hex_color(accent) : hov ? hex_color(0x475569) : hex_color(0x26354D),
                        active ? 1.4f : 1.0f);
        draw_text(label, font_small, r, active ? hex_color(accent) : hex_color(0xCBD5E1), DWRITE_TEXT_ALIGNMENT_CENTER);
        app.button_hits.push_back({r, id, ""});
        return bw;
    }

    void render_tool_strip(MeshGuiApp& app, float x, float y, float w) {
        D2D1_RECT_F strip = D2D1::RectF(x, y, x + w, y + 32.0f);
        fill_round_rect(strip, 8.0f, hex_color(0x0F172A));
        draw_round_rect(strip, 8.0f, hex_color(0x1E293B), 1.0f);

        float px = x + 6.0f;
        float py = y + 4.0f;
        px += pill(app, px, py, "Chat", app.center_view == CenterView::Chat, 0x00E5FF, BTN_VIEW_CHAT) + 5.0f;
        px += pill(app, px, py, "Packets " + std::to_string(app.raw_frames.size()),
                   app.center_view == CenterView::Packets, 0x22D3EE, BTN_VIEW_PACKETS) + 5.0f;
        px += pill(app, px, py, "Adverts " + std::to_string(app.adverts.size()),
                   app.center_view == CenterView::Adverts, 0xFBBF24, BTN_VIEW_ADVERTS) + 5.0f;
        px += pill(app, px, py, "Paths " + std::to_string(app.path_traces.size()),
                   app.center_view == CenterView::Paths, 0xA855F7, BTN_VIEW_PATHS) + 5.0f;
        {
            size_t gps = 0;
            for (const auto& n : app.mesh_nodes) if (n.lat_e6 || n.lon_e6) ++gps;
            px += pill(app, px, py, "Map " + std::to_string(gps), app.center_view == CenterView::Map, 0x10B981,
                       BTN_VIEW_MAP) + 5.0f;
        }
        px += pill(app, px, py, "Tx Advert", false, 0xF59E0B, BTN_SEND_ADVERT) + 5.0f;

        // Right-aligned export buttons (only if they fit)
        struct Ex { const char* label; int id; };
        static const Ex EXPORTS[] = {{"Export All", BTN_EXPORT_ALL}, {"KML Map", BTN_EXPORT_KML}, {"Nodes CSV", BTN_EXPORT_NODES}};
        float rx = x + w - 6.0f;
        for (const auto& e : EXPORTS) {
            float bw = static_cast<float>(std::strlen(e.label)) * 7.1f + 20.0f;
            if (rx - bw < px + 8.0f) break;
            rx -= bw;
            pill(app, rx, py, e.label, false, 0x10B981, e.id);
            rx -= 5.0f;
        }
    }

    static uint32_t node_type_color(uint8_t t) {
        switch (t) {
            case 2: return 0x38BDF8;  // repeater
            case 3: return 0xC084FC;  // room server
            case 4: return 0xFBBF24;  // sensor
            default: return 0x34D399; // companion
        }
    }

    void render_map_view(MeshGuiApp& app, float x, float y, float w, float h) {
        D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h);
        fill_round_rect(card, 10.0f, hex_color(0x0F172A));
        draw_round_rect(card, 10.0f, hex_color(0x1E293B), 1.2f);

        size_t gps_nodes = 0, gps_rpt = 0;
        for (const auto& n : app.mesh_nodes) {
            if (!(n.lat_e6 || n.lon_e6)) continue;
            ++gps_nodes;
            if (n.contact_type == 2) ++gps_rpt;
        }

        // ---- Header + controls ----
        char hdr[160];
        std::snprintf(hdr, sizeof(hdr), "LIVE MESH MAP   %zu of %zu nodes share GPS  |  %zu repeaters", gps_nodes,
                      app.mesh_nodes.size(), gps_rpt);
        float bx = x + w - 8.0f;
        auto rpill = [&](const std::string& label, bool active, uint32_t accent, int id) {
            float bw = static_cast<float>(label.size()) * 7.1f + 20.0f;
            bx -= bw;
            pill(app, bx, y + 4, label, active, accent, id, 22.0f);
            bx -= 5.0f;
        };
        rpill(app.map_style == 0 ? "Basemap: Dark" : "Basemap: Light", false, 0x94A3B8, BTN_MAP_STYLE);
        rpill("Links", app.map_show_links, 0xA855F7, BTN_MAP_LINKS);
        rpill("Repeaters only", app.map_repeaters_only, 0x38BDF8, BTN_MAP_REPEATERS);
        rpill("Fit", false, 0x10B981, BTN_MAP_FIT);
        draw_text(hdr, font_small, D2D1::RectF(x + 14, y + 6, bx - 6, y + 26), hex_color(0x34D399));

        // ---- Map surface ----
        D2D1_RECT_F mr = D2D1::RectF(x + 8, y + 32, x + w - 8, y + h - 8);
        app.map_rect = mr;
        app.map_card_rect = D2D1::RectF(0, 0, 0, 0);
        if (!app.map_fitted && gps_nodes > 0) app.map_fit();

        const bool light = app.map_style == 1;
        rt->PushAxisAlignedClip(mr, D2D1_ANTIALIAS_MODE_ALIASED);
        ID2D1SolidColorBrush* br = nullptr;
        rt->CreateSolidColorBrush(hex_color(light ? 0xE5E7EB : 0x0B1220), &br);
        if (!br) {
            rt->PopAxisAlignedClip();
            return;
        }
        rt->FillRectangle(mr, br);

        const int z = app.map_zoom;
        const int n = 1 << z;
        const double ws = 256.0 * n;
        const double cx = (mr.left + mr.right) / 2.0, cy = (mr.top + mr.bottom) / 2.0;
        const double cam_x = app.map_nx * ws, cam_y = app.map_ny * ws;
        auto to_screen = [&](double nx, double ny) {
            double dx = nx - app.map_nx;
            if (dx > 0.5) dx -= 1.0;
            else if (dx < -0.5) dx += 1.0;
            return D2D1::Point2F(static_cast<float>(cx + dx * ws), static_cast<float>(cy + (ny - app.map_ny) * ws));
        };
        auto geo_pt = [&](int32_t lat_e6, int32_t lon_e6) {
            return to_screen(lon_to_nx(lon_e6 / 1e6), lat_to_ny(lat_e6 / 1e6));
        };

        // Tiles (draw a scaled ancestor while the real tile is downloading)
        int loading = 0;
        int tx0 = static_cast<int>(std::floor((cam_x - (cx - mr.left)) / 256.0));
        int tx1 = static_cast<int>(std::floor((cam_x + (mr.right - cx)) / 256.0));
        int ty0 = std::max(0, static_cast<int>(std::floor((cam_y - (cy - mr.top)) / 256.0)));
        int ty1 = std::min(n - 1, static_cast<int>(std::floor((cam_y + (mr.bottom - cy)) / 256.0)));
        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                int wx = ((tx % n) + n) % n;
                float sx = static_cast<float>(cx + tx * 256.0 - cam_x);
                float sy = static_cast<float>(cy + ty * 256.0 - cam_y);
                D2D1_RECT_F dst = D2D1::RectF(sx, sy, sx + 256.0f, sy + 256.0f);
                if (ID2D1Bitmap* bmp = get_tile(tile_key(0, z, wx, ty))) {
                    rt->DrawBitmap(bmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    continue;
                }
                ++loading;
                g_tiles.request(0, z, wx, ty);
                for (int up = 1; up <= 6 && z - up >= 0; ++up) {
                    auto it = tile_bitmaps.find(tile_key(0, z - up, wx >> up, ty >> up));
                    if (it == tile_bitmaps.end()) continue;
                    it->second.last_used = frame_no;
                    float sub = 256.0f / static_cast<float>(1 << up);
                    float ox = static_cast<float>(wx & ((1 << up) - 1)) * sub;
                    float oy = static_cast<float>(ty & ((1 << up) - 1)) * sub;
                    D2D1_RECT_F src = D2D1::RectF(ox, oy, ox + sub, oy + sub);
                    rt->DrawBitmap(it->second.bmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
                    break;
                }
            }
        }
        if (!light) {
            // OSM only ships a light style; tint it toward the app's navy so pins/links stand out
            br->SetColor(hex_color(0x0B1220, 0.58f));
            rt->FillRectangle(mr, br);
        }

        const bool has_self = app.self_lat_e6 || app.self_lon_e6;
        const D2D1_POINT_2F self_pt = has_self ? geo_pt(app.self_lat_e6, app.self_lon_e6) : D2D1::Point2F(0, 0);
        const MeshSystemNode* sel = app.find_node_by_key_hex(app.map_selected_key);

        std::unordered_map<std::string, const MeshSystemNode*> by_name;
        for (const auto& nd : app.mesh_nodes)
            if ((nd.lat_e6 || nd.lon_e6) && !nd.name.empty()) by_name.emplace(nd.name, &nd);

        // ---- Relay links aggregated from recent path traces ----
        static const std::string ME = "\x01ME";
        std::map<std::pair<std::string, std::string>, int> links;
        std::map<std::string, int> neighbors; // for the selected node
        size_t scanned = 0;
        for (auto it = app.path_traces.rbegin(); it != app.path_traces.rend() && scanned < 400; ++it, ++scanned) {
            std::vector<std::string> chain;
            if (!it->origin.empty()) chain.push_back(it->origin);
            for (const auto& hop : it->hops) chain.push_back(hop.resolved ? hop.label : std::string());
            chain.push_back(ME);
            for (size_t i = 0; i + 1 < chain.size(); ++i) {
                const auto& a = chain[i];
                const auto& b = chain[i + 1];
                if (a.empty() || b.empty() || a == b) continue;
                links[a < b ? std::make_pair(a, b) : std::make_pair(b, a)]++;
                if (sel) {
                    if (a == sel->name) neighbors[b]++;
                    else if (b == sel->name) neighbors[a]++;
                }
            }
        }
        if (app.map_show_links) {
            auto pos_of = [&](const std::string& nm, D2D1_POINT_2F& out) {
                if (nm == ME) {
                    out = self_pt;
                    return has_self;
                }
                auto f = by_name.find(nm);
                if (f == by_name.end()) return false;
                out = geo_pt(f->second->lat_e6, f->second->lon_e6);
                return true;
            };
            for (const auto& kv : links) {
                D2D1_POINT_2F a, b;
                if (!pos_of(kv.first.first, a) || !pos_of(kv.first.second, b)) continue;
                bool touches_sel = sel && (kv.first.first == sel->name || kv.first.second == sel->name);
                float wdt = 1.2f + std::log2(1.0f + static_cast<float>(kv.second)) * 1.1f;
                br->SetColor(touches_sel ? hex_color(0xFBBF24, 0.95f) : hex_color(0xA855F7, light ? 0.75f : 0.55f));
                rt->DrawLine(a, b, br, touches_sel ? wdt + 1.0f : wdt);
            }
        }

        // ---- Node pins ----
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(GetTickCount64() % 2000) / 2000.0f * 6.2831853f);
        const MeshSystemNode* hovered = nullptr;
        D2D1_POINT_2F sel_pt{};
        auto draw_pin = [&](const MeshSystemNode& nd, bool is_sel) {
            D2D1_POINT_2F pt = geo_pt(nd.lat_e6, nd.lon_e6);
            if (pt.x < mr.left - 30 || pt.x > mr.right + 30 || pt.y < mr.top - 30 || pt.y > mr.bottom + 30) return;
            uint32_t col = node_type_color(nd.contact_type);
            bool rpt = nd.contact_type == 2;
            float r = (rpt ? 6.0f : 4.8f) + (is_sel ? 2.5f : 0.0f);
            if (nd.heard_live_rf) {
                br->SetColor(hex_color(col, 0.25f + 0.45f * (1.0f - pulse)));
                float rr = r + 3.0f + 6.0f * pulse;
                rt->DrawEllipse(D2D1::Ellipse(pt, rr, rr), br, 2.0f);
            }
            br->SetColor(hex_color(light ? 0xFFFFFF : 0x020617, 0.95f));
            if (rpt) rt->FillRectangle(D2D1::RectF(pt.x - r - 1.6f, pt.y - r - 1.6f, pt.x + r + 1.6f, pt.y + r + 1.6f), br);
            else rt->FillEllipse(D2D1::Ellipse(pt, r + 1.6f, r + 1.6f), br);
            br->SetColor(hex_color(col));
            if (rpt) rt->FillRectangle(D2D1::RectF(pt.x - r, pt.y - r, pt.x + r, pt.y + r), br);
            else rt->FillEllipse(D2D1::Ellipse(pt, r, r), br);
            if (is_sel) {
                br->SetColor(hex_color(light ? 0x0F172A : 0xFFFFFF));
                rt->DrawEllipse(D2D1::Ellipse(pt, r + 6.0f, r + 6.0f), br, 2.0f);
                sel_pt = pt;
            }
            D2D1_RECT_F hit = D2D1::RectF(pt.x - r - 4, pt.y - r - 4, pt.x + r + 4, pt.y + r + 4);
            app.map_pin_hits.push_back({hit, nd.contact_type, MeshGuiApp::node_key_hex(nd)});
            if (pt_in_rect(app.mouse_pt, hit)) hovered = &nd;
            if (is_sel || z >= 12 || (rpt && z >= 10)) {
                D2D1_RECT_F lr = D2D1::RectF(pt.x + r + 5, pt.y - 9, pt.x + r + 190, pt.y + 9);
                draw_text(nd.name, font_small, D2D1::RectF(lr.left + 1, lr.top + 1, lr.right + 1, lr.bottom + 1),
                          hex_color(light ? 0xFFFFFF : 0x000000, 0.85f));
                draw_text(nd.name, font_small, lr, light ? hex_color(0x0F172A) : hex_color(0xE2E8F0));
            }
        };
        // companions first, repeaters on top, selected last
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& nd : app.mesh_nodes) {
                if (!(nd.lat_e6 || nd.lon_e6) || &nd == sel) continue;
                if (app.map_repeaters_only && nd.contact_type != 2) continue;
                if ((nd.contact_type == 2) != (pass == 1)) continue;
                draw_pin(nd, false);
            }
        }
        if (sel && (sel->lat_e6 || sel->lon_e6)) draw_pin(*sel, true);

        // ---- Our own position ----
        if (has_self) {
            br->SetColor(hex_color(0x10B981, 0.35f * (1.0f - pulse)));
            rt->FillEllipse(D2D1::Ellipse(self_pt, 10.0f + 8.0f * pulse, 10.0f + 8.0f * pulse), br);
            br->SetColor(hex_color(0xFFFFFF));
            rt->FillEllipse(D2D1::Ellipse(self_pt, 7.5f, 7.5f), br);
            br->SetColor(hex_color(0x10B981));
            rt->FillEllipse(D2D1::Ellipse(self_pt, 5.5f, 5.5f), br);
            draw_text("ME", font_ui_bold, D2D1::RectF(self_pt.x + 10, self_pt.y - 10, self_pt.x + 60, self_pt.y + 10),
                      hex_color(0x10B981));
        }

        // ---- Hover tooltip ----
        if (hovered && hovered != sel && !app.map_dragging) {
            std::string tip = hovered->name + "  (" + contact_type_name(hovered->contact_type) + ")";
            if (hovered->heard_live_rf) {
                char b[48];
                std::snprintf(b, sizeof(b), "  %ddBm %+.1fdB", hovered->live_rssi, hovered->live_snr);
                tip += b;
            }
            float tw = static_cast<float>(tip.size()) * 7.0f + 16.0f;
            float tx = std::min(static_cast<float>(app.mouse_pt.x) + 14.0f, mr.right - tw - 4.0f);
            float ty = static_cast<float>(app.mouse_pt.y) - 30.0f;
            D2D1_RECT_F tr = D2D1::RectF(tx, ty, tx + tw, ty + 22.0f);
            fill_round_rect(tr, 5.0f, hex_color(0x0F172A, 0.95f));
            draw_round_rect(tr, 5.0f, hex_color(node_type_color(hovered->contact_type)), 1.0f);
            draw_text(tip, font_small, tr, hex_color(0xE2E8F0), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // ---- Overlays: status, zoom buttons, legend, attribution ----
        {
            char st[96];
            if (gps_nodes == 0 && !has_self)
                std::snprintf(st, sizeof(st), "z%d  |  waiting for nodes with GPS...", z);
            else if (g_tiles.offline() && loading > 0)
                std::snprintf(st, sizeof(st), "z%d  |  map tiles offline - showing cached tiles only", z);
            else if (loading > 0)
                std::snprintf(st, sizeof(st), "z%d  |  loading %d tile(s)...", z, loading);
            else
                std::snprintf(st, sizeof(st), "z%d  |  drag to pan, scroll to zoom, click a pin", z);
            D2D1_RECT_F sr = D2D1::RectF(mr.left + 8, mr.top + 8, mr.left + 360, mr.top + 28);
            fill_round_rect(sr, 5.0f, hex_color(0x0F172A, 0.82f));
            draw_text(st, font_mono_small, D2D1::RectF(sr.left + 8, sr.top, sr.right - 6, sr.bottom), hex_color(0x94A3B8));
        }
        pill(app, mr.left + 8, mr.bottom - 64, " + ", false, 0x00E5FF, BTN_MAP_ZOOM_IN, 26.0f);
        pill(app, mr.left + 8, mr.bottom - 34, " - ", false, 0x00E5FF, BTN_MAP_ZOOM_OUT, 26.0f);
        {
            struct Lg { const char* t; uint32_t c; };
            static const Lg LEG[] = {{"Repeater", 0x38BDF8}, {"Companion", 0x34D399}, {"Room", 0xC084FC}, {"Sensor", 0xFBBF24}};
            float lx = mr.left + 54, ly = mr.bottom - 26;
            D2D1_RECT_F lb = D2D1::RectF(lx - 6, ly - 2, lx + 330, ly + 20);
            fill_round_rect(lb, 5.0f, hex_color(0x0F172A, 0.82f));
            for (const auto& l : LEG) {
                br->SetColor(hex_color(l.c));
                if (l.c == 0x38BDF8) rt->FillRectangle(D2D1::RectF(lx, ly + 5, lx + 9, ly + 14), br);
                else rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(lx + 4.5f, ly + 9.5f), 4.5f, 4.5f), br);
                draw_text(l.t, font_small, D2D1::RectF(lx + 13, ly, lx + 90, ly + 19), hex_color(0xCBD5E1));
                lx += 13.0f + static_cast<float>(std::strlen(l.t)) * 6.6f + 12.0f;
            }
        }
        {
            const char* attr = "\xC2\xA9 OpenStreetMap contributors";
            D2D1_RECT_F ar = D2D1::RectF(mr.right - 250, mr.bottom - 18, mr.right, mr.bottom);
            fill_round_rect(ar, 3.0f, hex_color(0x0F172A, 0.7f));
            draw_text(attr, font_small, D2D1::RectF(ar.left + 6, ar.top, ar.right - 6, ar.bottom), hex_color(0x94A3B8),
                      DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        // ---- Selected node info card ----
        if (sel) {
            std::vector<std::pair<std::string, uint32_t>> lines;
            char b[200];
            lines.push_back({std::string(contact_type_name(sel->contact_type)) + "   key " + MeshGuiApp::node_key_hex(*sel),
                             0x94A3B8});
            if (sel->lat_e6 || sel->lon_e6) {
                std::snprintf(b, sizeof(b), "GPS  %.5f, %.5f", sel->lat_e6 / 1e6, sel->lon_e6 / 1e6);
                lines.push_back({b, 0xCBD5E1});
                if (has_self) {
                    double km = haversine_km(app.self_lat_e6 / 1e6, app.self_lon_e6 / 1e6, sel->lat_e6 / 1e6, sel->lon_e6 / 1e6);
                    std::snprintf(b, sizeof(b), "%.1f km (%.1f mi) from you", km, km * 0.621371);
                    lines.push_back({b, 0xCBD5E1});
                }
            } else {
                lines.push_back({"No GPS position shared", 0x64748B});
            }
            lines.push_back({"Last advert  " + (sel->last_advert_ts ? epoch_to_iso(sel->last_advert_ts) : std::string("unknown")),
                             0xCBD5E1});
            if (sel->heard_live_rf) {
                std::snprintf(b, sizeof(b), "Heard live  %d dBm  SNR %+.1f dB  %u hop(s)", sel->live_rssi, sel->live_snr,
                              sel->live_hops);
                lines.push_back({b, 0x22D3EE});
            } else {
                lines.push_back({"Not heard over RF this session (from contact list)", 0x64748B});
            }
            if (sel->out_path_len < 0) lines.push_back({"Routing  flood (no direct path learned)", 0xCBD5E1});
            else {
                std::snprintf(b, sizeof(b), "Routing  direct path, %d hop(s)", sel->out_path_len);
                lines.push_back({b, 0xCBD5E1});
            }
            auto rs = app.relay_stats.find(sel->name);
            if (rs != app.relay_stats.end()) {
                std::snprintf(b, sizeof(b), "Relayed %zu traced packet(s)", rs->second.relay_count);
                lines.push_back({b, 0xC084FC});
                if (rs->second.last_hop_count > 0) {
                    std::snprintf(b, sizeof(b), "Delivered %zu straight to you (best SNR %+.1f dB)",
                                  rs->second.last_hop_count, rs->second.best_snr);
                    lines.push_back({b, 0xC084FC});
                }
            } else if (sel->contact_type == 2) {
                lines.push_back({"Not seen relaying traffic yet", 0x64748B});
            }
            if (!neighbors.empty()) {
                std::vector<std::pair<std::string, int>> nb(neighbors.begin(), neighbors.end());
                std::sort(nb.begin(), nb.end(), [](const auto& p, const auto& q) { return p.second > q.second; });
                lines.push_back({"Links seen in traces:", 0xFBBF24});
                for (size_t i = 0; i < nb.size() && i < 6; ++i) {
                    std::string nm = nb[i].first == ME ? std::string("ME (direct to you)") : nb[i].first;
                    lines.push_back({"   " + nm + "  x" + std::to_string(nb[i].second), 0xFDE68A});
                }
            }

            const bool has_node_spark = sel->snr_hist.size() > 1;
            const bool is_rpt_or_room = (sel->contact_type == 2 || sel->contact_type == 3);
            float spark_extra = has_node_spark ? 34.0f : 0.0f;
            float rpt_extra = is_rpt_or_room ? 30.0f : 0.0f;
            float cw = std::min(340.0f, (mr.right - mr.left) - 20.0f);
            float ch = 40.0f + static_cast<float>(lines.size()) * 18.0f + spark_extra + rpt_extra + 36.0f;
            D2D1_RECT_F cr = D2D1::RectF(mr.right - cw - 10, mr.top + 10, mr.right - 10, mr.top + 10 + ch);
            app.map_card_rect = cr;
            fill_round_rect(cr, 8.0f, hex_color(0x0B111E, 0.96f));
            draw_round_rect(cr, 8.0f, hex_color(node_type_color(sel->contact_type)), 1.4f);
            draw_text(sel->name, font_title, D2D1::RectF(cr.left + 12, cr.top + 8, cr.right - 36, cr.top + 32),
                      hex_color(node_type_color(sel->contact_type)));
            pill(app, cr.right - 32, cr.top + 8, "x", false, 0xEF4444, BTN_MAP_CLOSE_CARD, 22.0f);
            float ly = cr.top + 38.0f;
            for (const auto& l : lines) {
                draw_text(l.first, font_mono_small, D2D1::RectF(cr.left + 12, ly, cr.right - 10, ly + 17), hex_color(l.second));
                ly += 18.0f;
            }
            if (has_node_spark) {
                D2D1_RECT_F sr = D2D1::RectF(cr.left + 12, ly + 2.0f, cr.right - 12, ly + 28.0f);
                fill_round_rect(sr, 4.0f, hex_color(0x0F172A));
                draw_round_rect(sr, 4.0f, hex_color(0x1E293B), 1.0f);
                draw_text("SNR trend", font_mono_small, D2D1::RectF(sr.left + 6, sr.top + 2, sr.left + 84, sr.bottom - 2),
                          hex_color(0x64748B));
                float gx0 = sr.left + 78.0f, gx1 = sr.right - 8.0f;
                float gy0 = sr.top + 4.0f, gy1 = sr.bottom - 4.0f;
                float step = (gx1 - gx0) / static_cast<float>(sel->snr_hist.size() - 1);
                br->SetColor(hex_color(0x10B981));
                for (size_t i = 0; i + 1 < sel->snr_hist.size(); ++i) {
                    float n0 = std::clamp((sel->snr_hist[i] + 10.0f) / 24.0f, 0.0f, 1.0f);
                    float n1 = std::clamp((sel->snr_hist[i + 1] + 10.0f) / 24.0f, 0.0f, 1.0f);
                    rt->DrawLine(D2D1::Point2F(gx0 + i * step, gy1 - n0 * (gy1 - gy0)),
                                 D2D1::Point2F(gx0 + (i + 1) * step, gy1 - n1 * (gy1 - gy0)), br, 1.6f);
                }
                ly += spark_extra;
            }
            float btn_row1_y = is_rpt_or_room ? (cr.bottom - 60.0f) : (cr.bottom - 32.0f);
            float px = cr.left + 12;
            px += pill(app, px, btn_row1_y, "Message", false, 0x00E5FF, BTN_MAP_MESSAGE, 24.0f) + 6.0f;
            px += pill(app, px, btn_row1_y, "Trace", false, 0xA855F7, BTN_MAP_TRACE, 24.0f) + 6.0f;
            px += pill(app, px, btn_row1_y, "Reset Path", false, 0xF59E0B, BTN_MAP_RESET_PATH, 24.0f) + 6.0f;
            pill(app, px, btn_row1_y, "Center", false, 0x10B981, BTN_MAP_CENTER, 24.0f);
            if (is_rpt_or_room) {
                float rpx = cr.left + 12;
                float rby = cr.bottom - 30.0f;
                rpx += pill(app, rpx, rby, "Status", false, 0x38BDF8, BTN_MAP_RSTATUS, 22.0f) + 5.0f;
                rpx += pill(app, rpx, rby, "CLI: ver", false, 0xFBBF24, BTN_MAP_RVER, 22.0f) + 5.0f;
                pill(app, rpx, rby, "CLI: neighbors", false, 0xFBBF24, BTN_MAP_RNEIGHBORS, 22.0f);
            }
        }

        if (gps_nodes == 0 && !has_self) {
            D2D1_RECT_F er = D2D1::RectF(cx - 260, cy - 30, cx + 260, cy + 30);
            fill_round_rect(er, 8.0f, hex_color(0x0F172A, 0.9f));
            draw_text(app.mesh_nodes.empty()
                          ? "Connect a radio - contacts that share a GPS position will appear here as pins."
                          : "None of your contacts share a GPS position yet.",
                      font_ui, er, hex_color(0x94A3B8), DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        br->Release();
        rt->PopAxisAlignedClip();
    }

    void render_packets_view(MeshGuiApp& app, float x, float y, float w, float h) {
        D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h);
        fill_round_rect(card, 10.0f, hex_color(0x0F172A));
        draw_round_rect(card, 10.0f, hex_color(0x1E293B), 1.2f);

        char stats[160];
        std::snprintf(stats, sizeof(stats), "PACKET INSPECTOR   RX %zu  |  TX %zu  |  over-the-air %zu  |  buffered %zu",
                      app.rx_frame_count, app.tx_frame_count, app.rf_frame_count, app.raw_frames.size());
        draw_text(stats, font_small, D2D1::RectF(x + 14, y + 6, x + w - 14, y + 26), hex_color(0x22D3EE));

        float fx = x + 10.0f;
        float fy = y + 30.0f;
        fx += pill(app, fx, fy, "All", app.packet_filter == PacketFilter::All, 0x22D3EE, BTN_PKT_FILTER_ALL) + 4.0f;
        fx += pill(app, fx, fy, "Over-the-air", app.packet_filter == PacketFilter::RfOnly, 0x10B981, BTN_PKT_FILTER_RF) + 4.0f;
        fx += pill(app, fx, fy, "Companion", app.packet_filter == PacketFilter::Companion, 0x94A3B8, BTN_PKT_FILTER_COMP) + 4.0f;
        fx += pill(app, fx, fy, "TX", app.packet_filter == PacketFilter::TxOnly, 0xFBBF24, BTN_PKT_FILTER_TX) + 12.0f;
        fx += pill(app, fx, fy, app.packets_scroll == 0 ? "Following live" : "Paused - resume", app.packets_scroll == 0,
                   0x10B981, BTN_PKT_FOLLOW) + 4.0f;
        std::string rec_lbl = app.recording_session
                                  ? ("Stop Rec (" + std::to_string(app.recorded_frame_count) + ")")
                                  : "Rec Session";
        fx += pill(app, fx, fy, rec_lbl, app.recording_session, 0xEF4444, BTN_PKT_RECORD) + 4.0f;
        fx += pill(app, fx, fy, "Replay", false, 0xA855F7, BTN_PKT_REPLAY) + 8.0f;
        if (fx + 240.0f < x + w) {
            fx += pill(app, fx, fy, "Copy hex", false, 0x38BDF8, BTN_PKT_COPY_HEX) + 4.0f;
            fx += pill(app, fx, fy, "Export CSV", false, 0x10B981, BTN_EXPORT_PACKETS) + 4.0f;
            pill(app, fx, fy, "Clear", false, 0xEF4444, BTN_PKT_CLEAR);
        }

        // ---- Frame list (newest at bottom) ----
        float list_top = y + 62.0f;
        float list_bottom = y + 62.0f + (h - 70.0f) * 0.52f;
        const float row_h = 19.0f;
        std::vector<const RawFrameRecord*> rows;
        rows.reserve(app.raw_frames.size());
        for (const auto& r : app.raw_frames) if (app.frame_passes_filter(r)) rows.push_back(&r);

        int visible = std::max(1, static_cast<int>((list_bottom - list_top) / row_h));
        int total = static_cast<int>(rows.size());
        app.packets_scroll = std::clamp(app.packets_scroll, 0, std::max(0, total - visible));
        int start = std::max(0, total - visible - app.packets_scroll);

        fill_round_rect(D2D1::RectF(x + 8, list_top - 2, x + w - 8, list_bottom + 2), 6.0f, hex_color(0x090D16));
        if (rows.empty()) {
            draw_text("No frames yet - connect a radio (USB Serial) and traffic will stream here.", font_ui,
                      D2D1::RectF(x + 20, list_top, x + w - 20, list_top + 30), hex_color(0x64748B));
        }
        float ry = list_top;
        for (int i = start; i < total && i < start + visible; ++i) {
            const RawFrameRecord& r = *rows[i];
            D2D1_RECT_F rr = D2D1::RectF(x + 10, ry, x + w - 10, ry + row_h - 1);
            bool sel = (r.seq == app.selected_frame_seq);
            bool hov = pt_in_rect(app.mouse_pt, rr);
            if (sel) fill_round_rect(rr, 4.0f, hex_color(0x22D3EE, 0.18f));
            else if (hov) fill_round_rect(rr, 4.0f, hex_color(0x1E293B));

            uint32_t col = r.tx ? 0xFBBF24
                         : r.is_rf ? (r.summary.find('<') != std::string::npos ? 0x34D399 : 0x22D3EE)
                                   : 0xCBD5E1;
            draw_text(r.time, font_mono_small, D2D1::RectF(rr.left + 6, rr.top, rr.left + 96, rr.bottom), hex_color(0x64748B));
            D2D1_RECT_F badge = D2D1::RectF(rr.left + 98, rr.top + 2, rr.left + 124, rr.bottom - 2);
            fill_round_rect(badge, 3.0f, hex_color(col, 0.2f));
            draw_text(r.tx ? "TX" : (r.is_rf ? "RF" : "RX"), font_small, badge, hex_color(col), DWRITE_TEXT_ALIGNMENT_CENTER);
            draw_text(r.summary.size() > 3 ? r.summary.substr(4) : r.summary, font_mono_small,
                      D2D1::RectF(rr.left + 130, rr.top, rr.right - 48, rr.bottom), hex_color(col));
            draw_text(std::to_string(r.bytes.size()) + "B", font_mono_small,
                      D2D1::RectF(rr.right - 46, rr.top, rr.right - 4, rr.bottom), hex_color(0x64748B),
                      DWRITE_TEXT_ALIGNMENT_TRAILING);
            app.packet_row_hits.push_back({rr, static_cast<int>(r.seq), ""});
            ry += row_h;
        }

        // ---- Decode + hex dump of the selected frame (or newest) ----
        float dt = list_bottom + 10.0f;
        D2D1_RECT_F detail = D2D1::RectF(x + 8, dt, x + w - 8, y + h - 8);
        fill_round_rect(detail, 6.0f, hex_color(0x0B111E));
        draw_round_rect(detail, 6.0f, hex_color(0x1E293B), 1.0f);

        const RawFrameRecord* f = app.selected_frame();
        bool following_newest = false;
        if (!f && !rows.empty()) { f = rows.back(); following_newest = true; }
        if (!f) return;

        std::string title = "FRAME #" + std::to_string(f->seq) + "  @ " + f->time +
                            (following_newest ? "   (newest - click a row to pin)" : "   (pinned - click again to unpin)");
        draw_text(title, font_small, D2D1::RectF(detail.left + 10, dt + 4, detail.right - 10, dt + 22), hex_color(0x94A3B8));

        float ly = dt + 24.0f;
        const float lh = 16.0f;
        for (const auto& line : f->details) {
            if (ly + lh > detail.bottom - 4) break;
            uint32_t c = (line.rfind("DECRYPTED", 0) == 0) ? 0x34D399
                       : (line.rfind("    hop", 0) == 0) ? 0xC084FC
                       : 0xE2E8F0;
            draw_text(line, font_mono_small, D2D1::RectF(detail.left + 12, ly, detail.right - 12, ly + lh), hex_color(c));
            ly += lh;
        }
        ly += 6.0f;
        if (ly + lh < detail.bottom - 4) {
            draw_text("HEX DUMP (" + std::to_string(f->bytes.size()) + " bytes)", font_small,
                      D2D1::RectF(detail.left + 12, ly, detail.right - 12, ly + lh), hex_color(0x64748B));
            ly += lh + 2.0f;
        }
        for (const auto& line : hex_dump_lines(f->bytes)) {
            if (ly + lh > detail.bottom - 4) break;
            draw_text(line, font_mono_small, D2D1::RectF(detail.left + 12, ly, detail.right - 12, ly + lh), hex_color(0x38BDF8));
            ly += lh;
        }
    }

    void render_adverts_view(MeshGuiApp& app, float x, float y, float w, float h) {
        D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h);
        fill_round_rect(card, 10.0f, hex_color(0x0F172A));
        draw_round_rect(card, 10.0f, hex_color(0x1E293B), 1.2f);

        std::map<std::string, bool> uniq;
        size_t rf_count = 0;
        for (const auto& a : app.adverts) { uniq[a.key_prefix] = true; if (a.has_rf) ++rf_count; }
        char hdr[160];
        std::snprintf(hdr, sizeof(hdr), "LIVE ADVERT LOG   %zu adverts  |  %zu unique stations  |  %zu heard over-the-air",
                      app.adverts.size(), uniq.size(), rf_count);
        draw_text(hdr, font_small, D2D1::RectF(x + 14, y + 6, x + w - 200, y + 26), hex_color(0xFBBF24));
        float bx = x + w - 10.0f - 88.0f - 60.0f - 4.0f;
        bx += pill(app, bx, y + 4, "Export CSV", false, 0x10B981, BTN_EXPORT_ADVERTS, 22.0f) + 4.0f;
        pill(app, bx, y + 4, "Clear", false, 0xEF4444, BTN_ADV_CLEAR, 22.0f);

        // Column layout
        float c_time = x + 14, c_name = x + 104, c_type = x + 270, c_key = x + 360, c_sig = x + 460, c_gps = x + 580,
              c_path = x + 740;
        float hy = y + 32.0f;
        uint32_t hc = 0x64748B;
        draw_text("TIME", font_small, D2D1::RectF(c_time, hy, c_name, hy + 18), hex_color(hc));
        draw_text("STATION", font_small, D2D1::RectF(c_name, hy, c_type, hy + 18), hex_color(hc));
        draw_text("TYPE", font_small, D2D1::RectF(c_type, hy, c_key, hy + 18), hex_color(hc));
        draw_text("KEY", font_small, D2D1::RectF(c_key, hy, c_sig, hy + 18), hex_color(hc));
        draw_text("SIGNAL / HOPS", font_small, D2D1::RectF(c_sig, hy, c_gps, hy + 18), hex_color(hc));
        draw_text("GPS", font_small, D2D1::RectF(c_gps, hy, c_path, hy + 18), hex_color(hc));
        draw_text("PATH / SOURCE", font_small, D2D1::RectF(c_path, hy, x + w - 10, hy + 18), hex_color(hc));

        float top = hy + 22.0f;
        const float row_h = 22.0f;
        int visible = std::max(1, static_cast<int>((y + h - 8 - top) / row_h));
        int total = static_cast<int>(app.adverts.size());
        app.adverts_scroll = std::clamp(app.adverts_scroll, 0, std::max(0, total - visible));
        if (total == 0) {
            draw_text("Waiting for adverts... (stations beacon every few minutes - repeaters, rooms and companions all show up here)",
                      font_ui, D2D1::RectF(x + 20, top + 6, x + w - 20, top + 30), hex_color(0x64748B));
        }
        float ry = top;
        for (int i = total - 1 - app.adverts_scroll; i >= 0 && ry + row_h <= y + h - 6; --i) {
            const AdvertRecord& a = app.adverts[static_cast<size_t>(i)];
            D2D1_RECT_F rr = D2D1::RectF(x + 8, ry, x + w - 8, ry + row_h - 2);
            bool hov = pt_in_rect(app.mouse_pt, rr);
            fill_round_rect(rr, 4.0f, hov ? hex_color(0x1E293B) : (i % 2 ? hex_color(0x0B111E) : hex_color(0x0F172A)));
            uint32_t tc = (a.contact_type == 2) ? 0x38BDF8 : (a.contact_type == 3) ? 0xC084FC : 0x34D399;
            draw_text(a.time.substr(0, 8), font_mono_small, D2D1::RectF(c_time, ry, c_name, ry + row_h - 2), hex_color(0x64748B));
            draw_text(a.name, font_ui_bold, D2D1::RectF(c_name, ry, c_type - 6, ry + row_h - 2), hex_color(tc));
            draw_text(contact_type_name(a.contact_type), font_small, D2D1::RectF(c_type, ry, c_key, ry + row_h - 2), hex_color(tc));
            draw_text(a.key_prefix, font_mono_small, D2D1::RectF(c_key, ry, c_sig, ry + row_h - 2), hex_color(0x94A3B8));
            char sig[48];
            if (a.has_rf) std::snprintf(sig, sizeof(sig), "%ddBm %+.1fdB %uh", a.rssi, a.snr, a.hops);
            else std::snprintf(sig, sizeof(sig), "-");
            draw_text(sig, font_mono_small, D2D1::RectF(c_sig, ry, c_gps, ry + row_h - 2),
                      a.has_rf ? hex_color(0x22D3EE) : hex_color(0x475569));
            char gps[48] = "-";
            if (a.lat_e6 || a.lon_e6) std::snprintf(gps, sizeof(gps), "%.4f, %.4f", a.lat_e6 / 1e6, a.lon_e6 / 1e6);
            draw_text(gps, font_mono_small, D2D1::RectF(c_gps, ry, c_path, ry + row_h - 2), hex_color(0xCBD5E1));
            std::string path = a.path.empty() ? a.source : (a.path + "   [" + a.source + "]");
            draw_text(path, font_mono_small, D2D1::RectF(c_path, ry, rr.right - 6, ry + row_h - 2), hex_color(0x94A3B8));
            app.station_hits.push_back({rr, 0, a.name});
            ry += row_h;
        }
    }

    void render_paths_view(MeshGuiApp& app, float x, float y, float w, float h) {
        D2D1_RECT_F card = D2D1::RectF(x, y, x + w, y + h);
        fill_round_rect(card, 10.0f, hex_color(0x0F172A));
        draw_round_rect(card, 10.0f, hex_color(0x1E293B), 1.2f);

        char hdr[160];
        std::snprintf(hdr, sizeof(hdr), "RELAY PATH TRACER   %zu traces  |  %zu distinct relays seen",
                      app.path_traces.size(), app.relay_stats.size());
        draw_text(hdr, font_small, D2D1::RectF(x + 14, y + 6, x + w - 90, y + 26), hex_color(0xC084FC));
        pill(app, x + w - 70.0f, y + 4, "Clear", false, 0xEF4444, BTN_PATH_CLEAR, 22.0f);

        bool two_col = w >= 720.0f;
        float list_right = two_col ? x + w * 0.62f : x + w - 8;
        float stats_left = two_col ? list_right + 8 : x + 8;
        float list_bottom = two_col ? y + h - 8 : y + 32 + (h - 40) * 0.58f;

        // ---- Trace list (newest first) ----
        float top = y + 32.0f;
        const float row_h = 40.0f;
        int visible = std::max(1, static_cast<int>((list_bottom - top) / row_h));
        int total = static_cast<int>(app.path_traces.size());
        app.paths_scroll = std::clamp(app.paths_scroll, 0, std::max(0, total - visible));
        if (total == 0) {
            draw_text("Every over-the-air packet's relay path lands here: origin > repeaters > you.", font_ui,
                      D2D1::RectF(x + 20, top + 6, list_right - 10, top + 30), hex_color(0x64748B));
        }
        float ry = top;
        for (int i = total - 1 - app.paths_scroll; i >= 0 && ry + row_h <= list_bottom; --i) {
            const PathRecord& p = app.path_traces[static_cast<size_t>(i)];
            D2D1_RECT_F rr = D2D1::RectF(x + 8, ry, list_right, ry + row_h - 3);
            fill_round_rect(rr, 5.0f, pt_in_rect(app.mouse_pt, rr) ? hex_color(0x1E293B) : hex_color(0x131C2E));
            draw_text(p.time.substr(0, 8) + "  " + p.what, font_ui_bold,
                      D2D1::RectF(rr.left + 8, rr.top + 2, rr.right - 150, rr.top + 19), hex_color(0xE2E8F0));
            char sig[64];
            std::snprintf(sig, sizeof(sig), "%s  %ddBm %+.1fdB", p.route.c_str(), p.rssi, p.snr);
            draw_text(sig, font_mono_small, D2D1::RectF(rr.right - 230, rr.top + 2, rr.right - 8, rr.top + 19),
                      hex_color(0x22D3EE), DWRITE_TEXT_ALIGNMENT_TRAILING);

            std::string chain = p.origin.empty() ? "?" : p.origin;
            if (p.hops.empty()) chain += "  ==direct==>  ME";
            else {
                for (const auto& hop : p.hops) chain += "  >  " + (hop.resolved ? hop.label : ("[" + hop.label + "]"));
                chain += "  >  ME";
            }
            draw_text(chain, font_mono_small, D2D1::RectF(rr.left + 8, rr.top + 19, rr.right - 8, rr.bottom - 2),
                      p.hops.empty() ? hex_color(0x34D399) : hex_color(0xC084FC));
            ry += row_h;
        }

        // ---- Relay statistics ----
        float sy = two_col ? top : list_bottom + 8;
        float sbottom = y + h - 8;
        float sw = (two_col ? (x + w - 8) : (x + w - 8)) - stats_left;
        float half_h = (sbottom - sy) * 0.5f;

        auto draw_stats = [&](const char* title, bool by_last, float top_y, float bottom_y, uint32_t accent) {
            D2D1_RECT_F box = D2D1::RectF(stats_left, top_y, stats_left + sw, bottom_y - 4);
            fill_round_rect(box, 6.0f, hex_color(0x0B111E));
            draw_round_rect(box, 6.0f, hex_color(0x1E293B), 1.0f);
            draw_text(title, font_small, D2D1::RectF(box.left + 10, top_y + 4, box.right - 10, top_y + 22), hex_color(accent));
            auto stats = app.sorted_relays(by_last);
            size_t maxv = 1;
            for (const auto& s : stats) maxv = std::max(maxv, by_last ? s.last_hop_count : s.relay_count);
            float ly = top_y + 26.0f;
            for (const auto& s : stats) {
                size_t v = by_last ? s.last_hop_count : s.relay_count;
                if (v == 0) continue;
                if (ly + 20.0f > box.bottom - 4) break;
                float bar_w = (box.right - box.left - 20.0f) * (static_cast<float>(v) / static_cast<float>(maxv));
                fill_round_rect(D2D1::RectF(box.left + 10, ly + 2, box.left + 10 + bar_w, ly + 17), 3.0f, hex_color(accent, 0.18f));
                draw_text(s.label, font_ui, D2D1::RectF(box.left + 16, ly, box.right - 110, ly + 19), hex_color(0xE2E8F0));
                char val[48];
                if (by_last) std::snprintf(val, sizeof(val), "%zu  best %+.1fdB", v, s.best_snr);
                else std::snprintf(val, sizeof(val), "%zu pkts", v);
                draw_text(val, font_mono_small, D2D1::RectF(box.right - 130, ly, box.right - 12, ly + 19), hex_color(accent),
                          DWRITE_TEXT_ALIGNMENT_TRAILING);
                ly += 21.0f;
            }
        };
        draw_stats("BUSIEST RELAYS (any hop)", false, sy, sy + half_h, 0xA855F7);
        draw_stats("YOUR DIRECT NEIGHBORS (final hop into this radio)", true, sy + half_h, sbottom, 0x10B981);
    }

    void render_serial_dropdown(MeshGuiApp& app, float x, float y) {
        float w = 380.0f;
        float row_h = 30.0f;
        size_t count = app.cached_ports.size() + 1 + (app.transport_connected ? 1 : 0);
        float h = 36.0f + count * row_h;

        D2D1_RECT_F menu = D2D1::RectF(x, y, x + w, y + h);
        fill_round_rect(menu, 8.0f, hex_color(0x0F172A));
        draw_round_rect(menu, 8.0f, hex_color(0x00E5FF), 1.6f);

        draw_text("SELECT MESHCORE USB SERIAL PORT", font_small,
                  D2D1::RectF(x + 12, y + 6, x + w - 12, y + 28), hex_color(0x00E5FF));

        float ry = y + 30.0f;
        D2D1_RECT_F auto_r = D2D1::RectF(x + 8, ry, x + w - 8, ry + row_h - 2);
        if (pt_in_rect(app.mouse_pt, auto_r)) fill_round_rect(auto_r, 5.0f, hex_color(0x1E293B));
        draw_text("Auto-Detect MeshCore Companion USB Serial", font_ui_bold,
                  D2D1::RectF(auto_r.left + 8, auto_r.top, auto_r.right - 8, auto_r.bottom), hex_color(0x10B981));
        app.serial_menu_hits.push_back({auto_r, 0, ""});
        ry += row_h;

        for (const auto& p : app.cached_ports) {
            D2D1_RECT_F pr = D2D1::RectF(x + 8, ry, x + w - 8, ry + row_h - 2);
            if (pt_in_rect(app.mouse_pt, pr)) fill_round_rect(pr, 5.0f, hex_color(0x1E293B));
            draw_text(p.port_name + " — " + p.friendly_name, p.is_meshcore_candidate ? font_ui_bold : font_ui,
                      D2D1::RectF(pr.left + 8, pr.top, pr.right - 8, pr.bottom),
                      p.is_meshcore_candidate ? hex_color(0x38BDF8) : hex_color(0xF1F5F9));
            app.serial_menu_hits.push_back({pr, 1, p.port_name});
            ry += row_h;
        }

        if (app.transport_connected) {
            D2D1_RECT_F dr = D2D1::RectF(x + 8, ry, x + w - 8, ry + row_h - 2);
            if (pt_in_rect(app.mouse_pt, dr)) fill_round_rect(dr, 5.0f, hex_color(0xEF4444, 0.25f));
            draw_text("Disconnect current radio", font_ui_bold,
                      D2D1::RectF(dr.left + 8, dr.top, dr.right - 8, dr.bottom), hex_color(0xF87171));
            app.serial_menu_hits.push_back({dr, 2, ""});
        }
    }

    void render_ble_dropdown(MeshGuiApp& app, float x, float y) {
        float w = 440.0f;
        float row_h = 30.0f;
        size_t dev_rows = std::max<size_t>(1, std::min<size_t>(10, app.cached_ble_devices.size()));
        size_t count = dev_rows + 1 + (app.transport_connected ? 1 : 0);
        float h = 38.0f + count * row_h;

        D2D1_RECT_F menu = D2D1::RectF(x, y, x + w, y + h);
        fill_round_rect(menu, 8.0f, hex_color(0x0F172A));
        draw_round_rect(menu, 8.0f, hex_color(0x38BDF8), 1.6f);

        draw_text("BLUETOOTH LE (NUS) — MESHCORE COMPANION SCANNER", font_small,
                  D2D1::RectF(x + 12, y + 6, x + w - 12, y + 28), hex_color(0x38BDF8));

        float ry = y + 30.0f;
        D2D1_RECT_F auto_r = D2D1::RectF(x + 8, ry, x + w - 8, ry + row_h - 2);
        if (pt_in_rect(app.mouse_pt, auto_r)) fill_round_rect(auto_r, 5.0f, hex_color(0x1E293B));
        draw_text("⟳ Auto-Scan & Connect MeshCore BLE NUS Node", font_ui_bold,
                  D2D1::RectF(auto_r.left + 8, auto_r.top, auto_r.right - 8, auto_r.bottom), hex_color(0x10B981));
        app.ble_menu_hits.push_back({auto_r, 0, ""});
        ry += row_h;

        if (app.cached_ble_devices.empty()) {
            D2D1_RECT_F sr = D2D1::RectF(x + 8, ry, x + w - 8, ry + row_h - 2);
            draw_text("Scanning Bluetooth LE for MeshCore nodes...", font_ui,
                      D2D1::RectF(sr.left + 8, sr.top, sr.right - 8, sr.bottom), hex_color(0x94A3B8));
            ry += row_h;
        } else {
            size_t shown = 0;
            for (const auto& d : app.cached_ble_devices) {
                if (shown++ >= 10) break;
                D2D1_RECT_F pr = D2D1::RectF(x + 8, ry, x + w - 8, ry + row_h - 2);
                if (pt_in_rect(app.mouse_pt, pr)) fill_round_rect(pr, 5.0f, hex_color(0x1E293B));
                std::string lbl = d.name + "  (" + std::to_string(d.rssi) + " dBm)" +
                                  (d.is_meshcore_candidate ? "  [MeshCore NUS ★]" : "");
                draw_text(lbl, d.is_meshcore_candidate ? font_ui_bold : font_ui,
                          D2D1::RectF(pr.left + 8, pr.top, pr.right - 8, pr.bottom),
                          d.is_meshcore_candidate ? hex_color(0x38BDF8) : hex_color(0xF1F5F9));
                app.ble_menu_hits.push_back({pr, 1, d.identifier});
                ry += row_h;
            }
        }

        if (app.transport_connected) {
            D2D1_RECT_F dr = D2D1::RectF(x + 8, ry, x + w - 8, ry + row_h - 2);
            if (pt_in_rect(app.mouse_pt, dr)) fill_round_rect(dr, 5.0f, hex_color(0xEF4444, 0.25f));
            draw_text("Disconnect current radio", font_ui_bold,
                      D2D1::RectF(dr.left + 8, dr.top, dr.right - 8, dr.bottom), hex_color(0xF87171));
            app.ble_menu_hits.push_back({dr, 2, ""});
        }
    }
};

// ============================================================================
// Global Window Context & Win32 Event Procedure
// ============================================================================
static MeshGuiApp* g_app = nullptr;
static D2DRenderer* g_renderer = nullptr;

static void handle_left_click(HWND hwnd, POINT pt) {
    if (!g_app) return;

    // 1. If Serial Dropdown Modal is open, check its items first
    if (g_app->serial_dropdown_open) {
        for (const auto& hit : g_app->serial_menu_hits) {
            if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
                g_app->serial_dropdown_open = false;
                if (hit.id == 2) {
                    g_app->transport.disconnect();
                    g_app->connected_port.clear();
                    g_app->log_public_system("[TRANSPORT] Radio disconnected by operator.");
                    return;
                }
                std::string port = hit.payload;
                if (port.empty()) {
                    for (const auto& p : g_app->cached_ports) {
                        if (p.is_meshcore_candidate) { port = p.port_name; break; }
                    }
                }
                g_app->connected_port = port;
                g_app->log_public_system(port.empty() ? "Auto-detecting MeshCore radio..."
                                                      : ("Connecting to MeshCore radio on " + port + " @ 115200 baud..."));
                g_app->transport.connect_serial(port);
                return;
            }
        }
        g_app->serial_dropdown_open = false;
    }

    // 1b. If BLE NUS Dropdown Modal is open, check its items first
    if (g_app->ble_dropdown_open) {
        for (const auto& hit : g_app->ble_menu_hits) {
            if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
                g_app->ble_dropdown_open = false;
                if (hit.id == 2) {
                    g_app->transport.disconnect();
                    g_app->connected_port.clear();
                    g_app->log_public_system("[TRANSPORT] BLE Radio disconnected by operator.");
                    return;
                }
                std::string target_ble = hit.payload;
                g_app->connected_port = "BLE";
                g_app->log_public_system(target_ble.empty()
                                             ? "[BLE] Scanning Bluetooth LE for MeshCore Companion nodes (NUS)..."
                                             : ("[BLE] Connecting to Bluetooth LE peripheral " + target_ble + "..."));
                g_app->transport.connect_ble(target_ble);
                return;
            }
        }
        g_app->ble_dropdown_open = false;
    }

    // 2. Check Tab Close ('x') buttons
    for (const auto& hit : g_app->tab_close_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            g_app->close_tab(static_cast<size_t>(hit.id));
            return;
        }
    }

    // 3. Check Top Horizontal Tabs
    for (const auto& hit : g_app->tab_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            g_app->select_tab(static_cast<size_t>(hit.id));
            g_app->active_input = ActiveInputTarget::ChatComposer;
            return;
        }
    }

    // 4. Check Left Sidebar Joined Tabs
    for (const auto& hit : g_app->joined_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            g_app->select_tab(static_cast<size_t>(hit.id));
            g_app->active_input = ActiveInputTarget::ChatComposer;
            return;
        }
    }

    // 5. Check Left Sidebar Discovered #Channels (1-click Join & Sync to Radio!)
    for (const auto& hit : g_app->discovered_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            g_app->join_channel(hit.payload);
            g_app->active_input = ActiveInputTarget::ChatComposer;
            return;
        }
    }

    // 6. Check Clickable #Hashtag Pills inside Chat Messages
    for (const auto& hit : g_app->chat_hashtag_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            g_app->join_channel(hit.payload);
            return;
        }
    }

    // 6b. Check Room Participant / Nicklist rows (single-click/tap replies @[user], double-click opens @DM tab!)
    for (const auto& hit : g_app->nicklist_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            static std::string last_nick_user;
            static uint64_t last_nick_ms = 0;
            uint64_t now_ms = GetTickCount64();
            if (_stricmp(last_nick_user.c_str(), hit.payload.c_str()) == 0 && (now_ms - last_nick_ms) <= 650) {
                last_nick_user.clear();
                last_nick_ms = 0;
                g_app->clear_reply_target();
                g_app->open_dm_tab(hit.payload, false);
                g_app->active_input = ActiveInputTarget::ChatComposer;
                return;
            }
            last_nick_user = hit.payload;
            last_nick_ms = now_ms;
            g_app->selected_station = hit.payload;
            g_app->reply_to_user(hit.payload);
            g_app->active_input = ActiveInputTarget::ChatComposer;
            return;
        }
    }

    // 7. Check Heard Station / Mesh System Cards (single-click selects; double-click opens @DM tab!)
    for (const auto& hit : g_app->station_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            static std::string last_st_user;
            static uint64_t last_st_ms = 0;
            uint64_t now_ms = GetTickCount64();
            if (_stricmp(last_st_user.c_str(), hit.payload.c_str()) == 0 && (now_ms - last_st_ms) <= 650) {
                last_st_user.clear();
                last_st_ms = 0;
                g_app->clear_reply_target();
                g_app->open_dm_tab(hit.payload, false);
                g_app->active_input = ActiveInputTarget::ChatComposer;
                return;
            }
            last_st_user = hit.payload;
            last_st_ms = now_ms;
            g_app->selected_station = hit.payload;
            g_app->active_input = ActiveInputTarget::ChatComposer;
            return;
        }
    }

    // 8. Check Right Sidebar Hardware Slot Matrix (click to open channel tab)
    for (const auto& hit : g_app->slot_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            if (!hit.payload.empty()) {
                g_app->join_channel(hit.payload);
            }
            return;
        }
    }

    // 8b. Packet Inspector rows (click pins a frame for hex dump/decode; click again to unpin)
    for (const auto& hit : g_app->packet_row_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            uint64_t seq = static_cast<uint64_t>(hit.id);
            g_app->selected_frame_seq = (g_app->selected_frame_seq == seq) ? 0 : seq;
            return;
        }
    }

    // 9. Check Top/Bottom Interactive Buttons
    for (const auto& hit : g_app->button_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            switch (hit.id) {
                case BTN_USB_SERIAL:
                    g_app->cached_ports = detect_serial_ports();
                    g_app->ble_dropdown_open = false;
                    g_app->serial_dropdown_open = !g_app->serial_dropdown_open;
                    break;
                case BTN_BLE_NUS:
                    g_app->serial_dropdown_open = false;
                    g_app->ble_dropdown_open = !g_app->ble_dropdown_open;
                    if (!g_app->transport_connected || g_app->transport_label.find("BLE") == std::string::npos) {
                        g_app->connected_port = "BLE";
                        g_app->log_public_system("[BLE] Scanning Bluetooth LE for MeshCore Companion nodes (NUS)...");
                        g_app->transport.connect_ble("");
                    }
                    break;
                case BTN_RF_SIM:
                    g_app->transport.toggle_simulator();
                    break;
                case BTN_ADD_TAG:
                    g_app->active_input = ActiveInputTarget::ChatComposer;
                    g_app->chat_input = "/addword #";
                    g_app->chat_cursor = g_app->chat_input.size();
                    break;
                case BTN_CALLSIGN:
                    g_app->active_input = ActiveInputTarget::CallsignEditor;
                    break;
                case BTN_COPY_PSK:
                    copy_text_to_clipboard(hwnd, hit.payload);
                    g_app->copy_psk_feedback_until = now_epoch_secs() + 2;
                    break;
                case BTN_SNIFFER_MODE_LIVE:
                    g_app->show_all_rainbow_in_sniffer = false;
                    g_app->sniffer_scroll = 0;
                    break;
                case BTN_SNIFFER_MODE_ALL:
                    g_app->show_all_rainbow_in_sniffer = true;
                    g_app->sniffer_scroll = 0;
                    break;
                case BTN_SNIFFER_FILTER_BOX:
                    g_app->active_input = ActiveInputTarget::SnifferFilter;
                    break;
                case BTN_CHAT_INPUT_BOX:
                    g_app->active_input = ActiveInputTarget::ChatComposer;
                    break;
                case BTN_TRANSMIT_TX:
                    g_app->submit_chat_input();
                    break;
                case BTN_QUICK_EMCOMM:
                case BTN_QUICK_MESHCORE:
                case BTN_QUICK_WEATHER:
                case BTN_QUICK_HAMRADIO:
                case BTN_QUICK_GENERAL:
                    g_app->join_channel(hit.payload);
                    break;
                case BTN_QUICK_SLOTS:
                    g_app->execute_slash_command("/slots");
                    break;
                case BTN_CLEAR_REPLY:
                    g_app->clear_reply_target();
                    break;
                case BTN_TOGGLE_THEME: {
                    g_app->light_mode = !g_app->light_mode;
                    g_app->map_style = g_app->light_mode ? 1 : 0;
                    g_app->state_dirty = true;
#ifdef _WIN32
                    BOOL dark_title = g_app->light_mode ? FALSE : TRUE;
                    DwmSetWindowAttribute(hwnd, 20, &dark_title, sizeof(dark_title));
#elif defined(__APPLE__) && !defined(MESHCORE_IOS)
                    if (hwnd && hwnd->ns_window) {
                        id app_name = mac_ns_str(g_app->light_mode ? "NSAppearanceNameAqua" : "NSAppearanceNameDarkAqua");
                        id appearance = mac_cls_send<id>("NSAppearance", sel_registerName("appearanceNamed:"), app_name);
                        if (appearance) {
                            mac_msg_send<void>(hwnd->ns_window, sel_registerName("setAppearance:"), appearance);
                        }
                    }
#endif
                    break;
                }
                case BTN_RESET_LAYOUT:
                    g_app->init_default_module_layout();
                    g_app->state_dirty = true;
                    break;
                case BTN_VIEW_CHAT:      g_app->center_view = CenterView::Chat; break;
                case BTN_VIEW_PACKETS:   g_app->center_view = CenterView::Packets; break;
                case BTN_VIEW_ADVERTS:   g_app->center_view = CenterView::Adverts; break;
                case BTN_VIEW_PATHS:     g_app->center_view = CenterView::Paths; break;
                case BTN_EXPORT_NODES:   g_app->export_nodes_csv(); break;
                case BTN_EXPORT_KML:     g_app->export_kml(); break;
                case BTN_EXPORT_PACKETS: g_app->export_packets_csv(); break;
                case BTN_EXPORT_ADVERTS: g_app->export_adverts_csv(); break;
                case BTN_EXPORT_ALL:     g_app->export_all(); break;
                case BTN_PKT_FILTER_ALL:  g_app->packet_filter = PacketFilter::All; g_app->packets_scroll = 0; break;
                case BTN_PKT_FILTER_RF:   g_app->packet_filter = PacketFilter::RfOnly; g_app->packets_scroll = 0; break;
                case BTN_PKT_FILTER_COMP: g_app->packet_filter = PacketFilter::Companion; g_app->packets_scroll = 0; break;
                case BTN_PKT_FILTER_TX:   g_app->packet_filter = PacketFilter::TxOnly; g_app->packets_scroll = 0; break;
                case BTN_PKT_FOLLOW:
                    g_app->packets_scroll = 0;
                    g_app->selected_frame_seq = 0;
                    break;
                case BTN_PKT_CLEAR:
                    g_app->raw_frames.clear();
                    g_app->selected_frame_seq = 0;
                    g_app->packets_scroll = 0;
                    break;
                case BTN_PKT_COPY_HEX: {
                    const RawFrameRecord* f = g_app->selected_frame();
                    if (!f && !g_app->raw_frames.empty()) f = &g_app->raw_frames.back();
                    if (f) copy_text_to_clipboard(hwnd, hex_string(f->bytes.data(), f->bytes.size()));
                    break;
                }
                case BTN_ADV_CLEAR:
                    g_app->adverts.clear();
                    g_app->adverts_scroll = 0;
                    break;
                case BTN_PATH_CLEAR:
                    g_app->path_traces.clear();
                    g_app->relay_stats.clear();
                    g_app->paths_scroll = 0;
                    break;
                case BTN_VIEW_MAP:       g_app->center_view = CenterView::Map; break;
                case BTN_MAP_FIT:        g_app->map_fit(); break;
                case BTN_MAP_REPEATERS:  g_app->map_repeaters_only = !g_app->map_repeaters_only; break;
                case BTN_MAP_LINKS:      g_app->map_show_links = !g_app->map_show_links; break;
                case BTN_MAP_STYLE:      g_app->map_style = 1 - g_app->map_style; break;
                case BTN_MAP_CLOSE_CARD: g_app->map_selected_key.clear(); break;
                case BTN_MAP_ZOOM_IN:
                case BTN_MAP_ZOOM_OUT: {
                    const auto& r = g_app->map_rect;
                    POINT c{static_cast<LONG>((r.left + r.right) / 2), static_cast<LONG>((r.top + r.bottom) / 2)};
                    g_app->map_zoom_at(c, hit.id == BTN_MAP_ZOOM_IN ? 1 : -1);
                    break;
                }
                case BTN_MAP_CENTER:
                    if (const auto* nd = g_app->find_node_by_key_hex(g_app->map_selected_key)) {
                        if (nd->lat_e6 || nd->lon_e6) {
                            g_app->map_nx = lon_to_nx(nd->lon_e6 / 1e6);
                            g_app->map_ny = lat_to_ny(nd->lat_e6 / 1e6);
                            g_app->map_zoom = std::max(g_app->map_zoom, 12);
                        }
                    }
                    break;
                case BTN_MAP_MESSAGE:
                    if (const auto* nd = g_app->find_node_by_key_hex(g_app->map_selected_key)) {
                        std::string nm = nd->name;
                        g_app->open_dm_tab(nm, false);
                        g_app->active_input = ActiveInputTarget::ChatComposer;
                    }
                    break;
                case BTN_MAP_TRACE:
                    if (const auto* nd = g_app->find_node_by_key_hex(g_app->map_selected_key)) {
                        g_app->trace_to_node(nd->name);
                    }
                    break;
                case BTN_MAP_RESET_PATH:
                    if (const auto* nd = g_app->find_node_by_key_hex(g_app->map_selected_key)) {
                        g_app->reset_node_path(nd->name);
                    }
                    break;
                case BTN_MAP_RSTATUS:
                    if (const auto* nd = g_app->find_node_by_key_hex(g_app->map_selected_key)) {
                        g_app->repeater_status_req(nd->name);
                    }
                    break;
                case BTN_MAP_RVER:
                    if (const auto* nd = g_app->find_node_by_key_hex(g_app->map_selected_key)) {
                        g_app->repeater_cli_cmd(nd->name, "ver");
                    }
                    break;
                case BTN_MAP_RNEIGHBORS:
                    if (const auto* nd = g_app->find_node_by_key_hex(g_app->map_selected_key)) {
                        g_app->repeater_cli_cmd(nd->name, "neighbors");
                    }
                    break;
                case BTN_SEND_ADVERT:
                    g_app->send_self_advert_beacon(true);
                    break;
                case BTN_PKT_RECORD:
                    g_app->toggle_session_recording();
                    break;
                case BTN_PKT_REPLAY:
                    g_app->replay_session_file();
                    break;
                case BTN_ATTACH_IMAGE:
                    g_app->attach_image_from_picker(hwnd);
                    break;
                case BTN_CONFIRM_SEND_IMAGE:
                    g_app->confirm_send_pending_image();
                    break;
                case BTN_CANCEL_SEND_IMAGE:
                    g_app->cancel_pending_image();
                    break;
                case BTN_MOB_TAB_CHAT:     g_app->mobile_tab = 0; break;
                case BTN_MOB_TAB_NODES:    g_app->mobile_tab = 1; break;
                case BTN_MOB_TAB_SNIFFER:  g_app->mobile_tab = 2; break;
                case BTN_MOB_TAB_CHANNELS: g_app->mobile_tab = 3; break;
                case BTN_MOB_TAB_RADIO:    g_app->mobile_tab = 4; break;
                case BTN_TOGGLE_NICKLIST: {
                    bool cur_open = (g_app->show_room_nicklist_override >= 0);
                    g_app->show_room_nicklist_override = cur_open ? -1 : 1;
                    break;
                }
                case BTN_NICKLIST_FILTER_BOX:
                    g_app->active_input = ActiveInputTarget::NicklistFilter;
                    break;
                case BTN_NICKLIST_FILTER_CLEAR:
                    g_app->nicklist_filter.clear();
                    g_app->nicklist_scroll = 0;
                    g_app->active_input = ActiveInputTarget::NicklistFilter;
                    break;
            }
            return;
        }
    }

    g_app->active_input = ActiveInputTarget::ChatComposer;
}

static void handle_left_double_click(HWND, POINT pt) {
    if (!g_app) return;

    // 0a. Double-clicking any participant in the Room Nicklist opens a direct @NodeName DM tab!
    for (const auto& hit : g_app->nicklist_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            g_app->clear_reply_target();
            g_app->open_dm_tab(hit.payload, false);
            g_app->active_input = ActiveInputTarget::ChatComposer;
            return;
        }
    }

    // 0. Double-clicking a node pin on the Live Map opens a @NodeName Direct Message tab
    if (g_app->center_view == CenterView::Map && D2DRenderer::pt_in_rect(pt, g_app->map_rect) &&
        !D2DRenderer::pt_in_rect(pt, g_app->map_card_rect)) {
        for (auto it = g_app->map_pin_hits.rbegin(); it != g_app->map_pin_hits.rend(); ++it) {
            if (D2DRenderer::pt_in_rect(pt, it->rect)) {
                if (const auto* nd = g_app->find_node_by_key_hex(it->payload)) {
                    std::string nm = nd->name;
                    g_app->open_dm_tab(nm, false);
                    g_app->active_input = ActiveInputTarget::ChatComposer;
                }
                return;
            }
        }
        return;
    }

    // 1. Double-clicking any user/node card in the Right Sidebar (Public Mesh System / Heard Stations)
    for (const auto& hit : g_app->station_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            g_app->clear_reply_target();
            g_app->open_dm_tab(hit.payload, false);
            g_app->active_input = ActiveInputTarget::ChatComposer;
            return;
        }
    }

    // 2. Double-clicking any user message / <Sender> badge / live beacon in the Center Chat View
    for (const auto& hit : g_app->chat_user_hits) {
        if (D2DRenderer::pt_in_rect(pt, hit.rect)) {
            if (g_app->active_tab_idx < g_app->tabs.size() &&
                g_app->tabs[g_app->active_tab_idx].name == MeshGuiApp::ALL_HEARD_TAB) {
                g_app->clear_reply_target();
                g_app->open_dm_tab(hit.payload, false);
                g_app->active_input = ActiveInputTarget::ChatComposer;
            } else {
                g_app->reply_to_user(hit.payload);
            }
            return;
        }
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            if (g_renderer) {
                g_renderer->resize(LOWORD(lParam), HIWORD(lParam));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_SETCURSOR:
            if (g_app && LOWORD(lParam) == HTCLIENT) {
                if (g_app->dragging_splitter == DraggingSplitter::ColumnWidth) {
                    SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
                for (const auto& vs : g_app->vert_splitter_hits) {
                    if (D2DRenderer::pt_in_rect(g_app->mouse_pt, vs.rect)) {
                        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                        return TRUE;
                    }
                }
                if (g_app->dragging_splitter == DraggingSplitter::ModuleHeight) {
                    SetCursor(LoadCursor(nullptr, IDC_SIZENS));
                    return TRUE;
                }
                for (const auto& hs : g_app->horiz_splitter_hits) {
                    if (D2DRenderer::pt_in_rect(g_app->mouse_pt, hs.rect)) {
                        SetCursor(LoadCursor(nullptr, IDC_SIZENS));
                        return TRUE;
                    }
                }
                if (g_app->module_drag_active || g_app->map_dragging) {
                    SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                    return TRUE;
                }
                for (const auto& mh : g_app->module_header_hits) {
                    if (D2DRenderer::pt_in_rect(g_app->mouse_pt, mh.rect)) {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }
                if (g_app->center_view == CenterView::Map) {
                    for (const auto& h : g_app->map_pin_hits) {
                        if (D2DRenderer::pt_in_rect(g_app->mouse_pt, h.rect) &&
                            !D2DRenderer::pt_in_rect(g_app->mouse_pt, g_app->map_card_rect)) {
                            SetCursor(LoadCursor(nullptr, IDC_HAND));
                            return TRUE;
                        }
                    }
                }
            }
            break;

        case WM_TIMER:
            if (g_app) {
                g_app->tick();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEMOVE:
            if (g_app) {
                g_app->mouse_pt.x = GET_X_LPARAM(lParam);
                g_app->mouse_pt.y = GET_Y_LPARAM(lParam);

                if (g_app->dragging_module >= 0 && !g_app->module_drag_active) {
                    int dx = std::abs(g_app->mouse_pt.x - g_app->drag_start_mouse.x);
                    int dy = std::abs(g_app->mouse_pt.y - g_app->drag_start_mouse.y);
                    if (dx + dy >= 5) {
                        g_app->module_drag_active = true;
                    }
                }

                if (g_app->dragging_splitter != DraggingSplitter::None) {
                    RECT cr;
                    GetClientRect(hwnd, &cr);
                    float total_h = static_cast<float>(cr.bottom - cr.top - 84);
                    if (total_h < 100.0f) total_h = 100.0f;

                    if (g_app->dragging_splitter == DraggingSplitter::ColumnWidth &&
                        g_app->active_splitter_col >= 0 && g_app->active_splitter_col < 4) {
                        float delta = static_cast<float>(g_app->mouse_pt.x - g_app->drag_start_mouse.x);
                        if (g_app->active_splitter_invert) delta = -delta;
                        g_app->module_cols[g_app->active_splitter_col].width =
                            std::clamp(g_app->drag_start_val + delta, 190.0f, 520.0f);
                        g_app->state_dirty = true;
                    } else if (g_app->dragging_splitter == DraggingSplitter::ModuleHeight &&
                               g_app->active_splitter_col >= 0 && g_app->active_splitter_col < 4) {
                        auto& col = g_app->module_cols[g_app->active_splitter_col];
                        int r = g_app->active_splitter_row;
                        if (r >= 0 && r + 1 < static_cast<int>(col.weights.size())) {
                            float delta_w = static_cast<float>(g_app->mouse_pt.y - g_app->drag_start_mouse.y) / total_h;
                            float sum_pair = g_app->drag_start_val + g_app->drag_start_val2;
                            float w0 = std::clamp(g_app->drag_start_val + delta_w, 0.12f, std::max(0.12f, sum_pair - 0.12f));
                            col.weights[r] = w0;
                            col.weights[r + 1] = std::max(0.12f, sum_pair - w0);
                            g_app->state_dirty = true;
                        }
                    }
                }
                if (g_app->map_dragging) {
                    double ws = 256.0 * std::ldexp(1.0, g_app->map_zoom);
                    int dx = g_app->mouse_pt.x - g_app->map_drag_start.x;
                    int dy = g_app->mouse_pt.y - g_app->map_drag_start.y;
                    if (std::abs(dx) + std::abs(dy) > 3) g_app->map_drag_moved = true;
                    g_app->map_nx = g_app->map_drag_nx - dx / ws;
                    g_app->map_nx -= std::floor(g_app->map_nx);
                    g_app->map_ny = std::clamp(g_app->map_drag_ny - dy / ws, 0.0, 1.0);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (g_app) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (!g_app->serial_dropdown_open && !g_app->ble_dropdown_open) {
                    for (const auto& vs : g_app->vert_splitter_hits) {
                        if (D2DRenderer::pt_in_rect(pt, vs.rect)) {
                            g_app->dragging_splitter = DraggingSplitter::ColumnWidth;
                            g_app->active_splitter_col = vs.col_idx;
                            g_app->active_splitter_invert = vs.invert_delta;
                            g_app->drag_start_mouse = pt;
                            g_app->drag_start_val = g_app->module_cols[vs.col_idx].width;
                            SetCapture(hwnd);
                            return 0;
                        }
                    }
                    for (const auto& hs : g_app->horiz_splitter_hits) {
                        if (D2DRenderer::pt_in_rect(pt, hs.rect)) {
                            auto& col = g_app->module_cols[hs.col_idx];
                            g_app->ensure_column_weights(col);
                            if (hs.row_idx >= 0 && hs.row_idx + 1 < static_cast<int>(col.weights.size())) {
                                g_app->dragging_splitter = DraggingSplitter::ModuleHeight;
                                g_app->active_splitter_col = hs.col_idx;
                                g_app->active_splitter_row = hs.row_idx;
                                g_app->drag_start_mouse = pt;
                                g_app->drag_start_val = col.weights[hs.row_idx];
                                g_app->drag_start_val2 = col.weights[hs.row_idx + 1];
                                SetCapture(hwnd);
                                return 0;
                            }
                        }
                    }
                    for (const auto& mh : g_app->module_header_hits) {
                        if (D2DRenderer::pt_in_rect(pt, mh.rect)) {
                            g_app->dragging_module = mh.id;
                            g_app->module_drag_active = false;
                            g_app->drop_target_valid = false;
                            g_app->drag_start_mouse = pt;
                            SetCapture(hwnd);
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        }
                    }
                }
                // Live Map: pins select, empty space pans (buttons + info card fall through to normal clicks)
                if (g_app->center_view == CenterView::Map && !g_app->serial_dropdown_open && !g_app->ble_dropdown_open &&
                    D2DRenderer::pt_in_rect(pt, g_app->map_rect) &&
                    !D2DRenderer::pt_in_rect(pt, g_app->map_card_rect)) {
                    bool on_button = false;
                    for (const auto& b : g_app->button_hits)
                        if (D2DRenderer::pt_in_rect(pt, b.rect)) { on_button = true; break; }
                    if (!on_button) {
                        g_app->active_input = ActiveInputTarget::ChatComposer;
                        for (auto it = g_app->map_pin_hits.rbegin(); it != g_app->map_pin_hits.rend(); ++it) {
                            if (D2DRenderer::pt_in_rect(pt, it->rect)) {
                                g_app->map_selected_key = it->payload;
                                InvalidateRect(hwnd, nullptr, FALSE);
                                return 0;
                            }
                        }
                        g_app->map_dragging = true;
                        g_app->map_drag_moved = false;
                        g_app->map_drag_start = pt;
                        g_app->map_drag_nx = g_app->map_nx;
                        g_app->map_drag_ny = g_app->map_ny;
                        SetCapture(hwnd);
                        return 0;
                    }
                }
                handle_left_click(hwnd, pt);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (g_app && g_app->dragging_module >= 0) {
                if (g_app->module_drag_active && g_app->drop_target_valid) {
                    g_app->apply_module_drop(g_app->dragging_module, g_app->drop_target_col, g_app->drop_target_row);
                }
                g_app->dragging_module = -1;
                g_app->module_drag_active = false;
                g_app->drop_target_valid = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_app && g_app->map_dragging) {
                g_app->map_dragging = false;
                if (!g_app->map_drag_moved) g_app->map_selected_key.clear();
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_app && g_app->dragging_splitter != DraggingSplitter::None) {
                g_app->dragging_splitter = DraggingSplitter::None;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;

        case WM_LBUTTONDBLCLK:
            if (g_app) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                handle_left_double_click(hwnd, pt);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (g_app) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                int steps = (delta > 0) ? 3 : -3;
                if (g_app->center_view == CenterView::Map &&
                    D2DRenderer::pt_in_rect(g_app->mouse_pt, g_app->map_rect)) {
                    g_app->map_zoom_at(g_app->mouse_pt, delta > 0 ? 1 : -1);
                } else if (D2DRenderer::pt_in_rect(g_app->mouse_pt, g_app->sniffer_view_rect)) {
                    g_app->sniffer_scroll = std::max(0, g_app->sniffer_scroll - steps);
                } else if (D2DRenderer::pt_in_rect(g_app->mouse_pt, g_app->nick_view_rect)) {
                    g_app->nicklist_scroll = std::max(0, g_app->nicklist_scroll - steps);
                } else if (D2DRenderer::pt_in_rect(g_app->mouse_pt, g_app->nodes_view_rect)) {
                    int max_nd = std::max(0, static_cast<int>(g_app->mesh_nodes.size()) - 5);
                    g_app->nodes_scroll = std::clamp(g_app->nodes_scroll - steps, 0, max_nd);
                } else if (g_app->center_view != CenterView::Chat &&
                           D2DRenderer::pt_in_rect(g_app->mouse_pt, g_app->tool_view_rect)) {
                    if (g_app->center_view == CenterView::Packets)
                        g_app->packets_scroll = std::max(0, g_app->packets_scroll + steps);
                    else if (g_app->center_view == CenterView::Adverts)
                        g_app->adverts_scroll = std::max(0, g_app->adverts_scroll - steps);
                    else if (g_app->center_view == CenterView::Paths)
                        g_app->paths_scroll = std::max(0, g_app->paths_scroll - steps);
                } else {
                    auto& tab = g_app->tabs[g_app->active_tab_idx];
                    int max_sc = std::max(0, static_cast<int>(tab.messages.size()) - 1);
                    tab.scroll_offset = std::clamp(tab.scroll_offset + steps, 0, max_sc);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_KEYDOWN:
            if (g_app) {
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (ctrl && wParam == 'F') {
                    g_app->active_input = ActiveInputTarget::NicklistFilter;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (ctrl && wParam == 'V') {
                    std::string clip = get_clipboard_text(hwnd);
                    std::string* dst = &g_app->chat_input;
                    if (g_app->active_input == ActiveInputTarget::SnifferFilter) dst = &g_app->sniffer_filter;
                    else if (g_app->active_input == ActiveInputTarget::NicklistFilter) dst = &g_app->nicklist_filter;
                    for (char c : clip) {
                        if (c >= 0x20 && c != 0x7F) {
                            dst->push_back(c);
                        }
                    }
                    g_app->chat_cursor = g_app->chat_input.size();
                    if (g_app->active_input == ActiveInputTarget::NicklistFilter) g_app->nicklist_scroll = 0;
                    if (g_app->active_input == ActiveInputTarget::SnifferFilter) g_app->sniffer_scroll = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (wParam == VK_TAB) {
                    size_t next = (g_app->active_tab_idx + 1) % g_app->tabs.size();
                    g_app->select_tab(next);
                } else if (wParam == VK_PRIOR) { // PageUp
                    auto& tab = g_app->tabs[g_app->active_tab_idx];
                    tab.scroll_offset = std::min<int>(tab.scroll_offset + 8,
                                                      std::max(0, static_cast<int>(tab.messages.size()) - 1));
                } else if (wParam == VK_NEXT) { // PageDown
                    auto& tab = g_app->tabs[g_app->active_tab_idx];
                    tab.scroll_offset = std::max(0, tab.scroll_offset - 8);
                } else if (wParam == VK_ESCAPE) {
                    g_app->serial_dropdown_open = false;
                    if (g_app->pending_image_ready) {
                        g_app->cancel_pending_image();
                    } else if (g_app->active_input == ActiveInputTarget::SnifferFilter) {
                        g_app->sniffer_filter.clear();
                    } else if (g_app->active_input == ActiveInputTarget::NicklistFilter) {
                        g_app->nicklist_filter.clear();
                        g_app->nicklist_scroll = 0;
                    } else if (!g_app->replying_to_user.empty()) {
                        g_app->clear_reply_target();
                    }
                    g_app->active_input = ActiveInputTarget::ChatComposer;
                } else if (wParam == VK_RETURN) {
                    if (g_app->active_input == ActiveInputTarget::ChatComposer) {
                        g_app->submit_chat_input();
                    } else {
                        g_app->active_input = ActiveInputTarget::ChatComposer;
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_CHAR:
            if (g_app) {
                wchar_t wc = static_cast<wchar_t>(wParam);
                if (wc == L'\b') {
                    if (g_app->active_input == ActiveInputTarget::ChatComposer) {
                        if (!g_app->chat_input.empty()) g_app->chat_input.pop_back();
                        if (g_app->chat_input.empty()) g_app->replying_to_user.clear();
                    } else if (g_app->active_input == ActiveInputTarget::SnifferFilter) {
                        if (!g_app->sniffer_filter.empty()) g_app->sniffer_filter.pop_back();
                    } else if (g_app->active_input == ActiveInputTarget::NicklistFilter) {
                        if (!g_app->nicklist_filter.empty()) g_app->nicklist_filter.pop_back();
                        g_app->nicklist_scroll = 0;
                    } else if (g_app->active_input == ActiveInputTarget::CallsignEditor) {
                        if (!g_app->callsign.empty()) g_app->callsign.pop_back();
                    }
                } else if (wc >= 0x20 && wc < 0x7F) {
                    char c = static_cast<char>(wc);
                    if (g_app->active_input == ActiveInputTarget::ChatComposer) {
                        g_app->chat_input.push_back(c);
                    } else if (g_app->active_input == ActiveInputTarget::SnifferFilter) {
                        g_app->sniffer_filter.push_back(c);
                        g_app->sniffer_scroll = 0;
                    } else if (g_app->active_input == ActiveInputTarget::NicklistFilter) {
                        g_app->nicklist_filter.push_back(c);
                        g_app->nicklist_scroll = 0;
                    } else if (g_app->active_input == ActiveInputTarget::CallsignEditor) {
                        if (g_app->callsign.size() < 14 && c != ' ' && c != ':') {
                            g_app->callsign.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                        }
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (g_renderer && g_app) {
                g_renderer->render(hwnd, ps.hdc, *g_app);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_PRINTCLIENT: {
            if (g_renderer && g_app && wParam) {
                g_renderer->render(hwnd, reinterpret_cast<HDC>(wParam), *g_app);
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_DESTROY:
            if (g_app) g_app->save_state();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    std::ostream dbg(nullptr); // startup trace disabled (point at an ofstream to debug)
    dbg << "[STARTUP] WinMain entered" << std::endl;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT hr_co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    dbg << "[STARTUP] CoInitializeEx result: 0x" << std::hex << hr_co << std::dec << std::endl;

    if (!hInstance) hInstance = GetModuleHandleW(nullptr);

    dbg << "[STARTUP] Initializing MeshGuiApp..." << std::endl;
    MeshGuiApp app;
    dbg << "[STARTUP] Initializing D2DRenderer..." << std::endl;
    D2DRenderer renderer;
    g_app = &app;
    g_renderer = &renderer;

    HICON hIconBig = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    HICON hIconSmall = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!hIconBig) hIconBig = LoadIcon(nullptr, IDI_APPLICATION);
    if (!hIconSmall) hIconSmall = hIconBig;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = hIconBig;
    wc.hIconSm       = hIconSmall;
    wc.lpszClassName = L"MeshCorePlusPlusGuiWindow";
    ATOM atom = RegisterClassExW(&wc);
    if (!atom) {
        DWORD err = GetLastError();
        dbg << "[STARTUP] RegisterClassExW: " << err << std::endl;
    } else {
        dbg << "[STARTUP] RegisterClassExW success" << std::endl;
    }

    dbg << "[STARTUP] Calling CreateWindowExW..." << std::endl;
    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"MeshCore++",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80, 60, 1460, 880,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        DWORD err = GetLastError();
        dbg << "[ERROR] CreateWindowExW failed: " << err << std::endl;
        return 1;
    }
    dbg << "[STARTUP] Window created successfully: " << hwnd << std::endl;

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIconBig));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIconSmall));

    // Sync Windows 11 Titlebar theme (DWMWA_USE_IMMERSIVE_DARK_MODE = 20)
    BOOL dark_mode = app.light_mode ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark_mode, sizeof(dark_mode));

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    dbg << "[STARTUP] ShowWindow completed. Entering message loop..." << std::endl;
    dbg.flush();

    // 30 FPS UI + RF telemetry timer
    SetTimer(hwnd, 1, 33, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_app = nullptr;
    g_renderer = nullptr;
    return 0;
}
#elif defined(__APPLE__)
#ifndef MESHCORE_IOS
// Embed Info.plist with NSBluetoothAlwaysUsageDescription directly into Mach-O (__TEXT,__info_plist)
// on macOS only. (On iOS, embedding __TEXT,__info_plist causes amfid to reject Sideloadly/AltStore
// re-signed bundles because the provisioning profile rewrites Info.plist's CFBundleIdentifier.)
__attribute__((used, section("__TEXT,__info_plist")))
static const char kEmbeddedMacInfoPlist[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>CFBundleIdentifier</key>\n"
    "  <string>io.meshcore.plusplus</string>\n"
    "  <key>CFBundleName</key>\n"
    "  <string>MeshCore++</string>\n"
    "  <key>NSBluetoothAlwaysUsageDescription</key>\n"
    "  <string>MeshCore++ uses Bluetooth Low Energy (Nordic UART Service) to scan and connect to MeshCore Companion LoRa mesh nodes.</string>\n"
    "  <key>NSBluetoothPeripheralUsageDescription</key>\n"
    "  <string>MeshCore++ uses Bluetooth Low Energy (Nordic UART Service) to communicate with MeshCore Companion LoRa mesh nodes.</string>\n"
    "</dict>\n"
    "</plist>\n";
#endif

bool g_mac_cmd_down = false;
static MacWindowState g_mac_win;

template <typename Ret, typename... Args>
static inline Ret msg_send(id obj, SEL sel, Args... args) {
    return mac_msg_send<Ret>(obj, sel, args...);
}
template <typename Ret, typename... Args>
static inline Ret cls_send(const char* cls_name, SEL sel, Args... args) {
    return mac_cls_send<Ret>(cls_name, sel, args...);
}

static id ns_str(const char* utf8) {
    return mac_ns_str(utf8);
}

#ifdef MESHCORE_IOS
static id g_ios_tf = nullptr;
static bool g_ios_keyboard_visible = false;
static int g_ios_top_inset = 54;
static CGPoint g_ios_touch_start{0.0, 0.0};
static CGFloat g_ios_last_drag_y = 0.0;
static bool g_ios_dragging_scroll = false;
static bool g_ios_ble_autostarted = false;

static void ios_ensure_app_initialized() {
    if (!g_app) {
        g_app = new MeshGuiApp();
    }
    if (!g_renderer) {
        g_renderer = new D2DRenderer();
    }
    g_ios_on_image_picked = [](const std::wstring& wpath) {
        if (g_app && !wpath.empty()) {
            g_app->attach_image_from_path(wpath);
        }
    };
    g_ios_on_image_bits_picked = [](const uint8_t* bits180, const char* label, const char* err) {
        if (!g_app) return;
        if (bits180) {
            std::memcpy(g_app->pending_image_bits.data(), bits180, MONO_IMG_BYTES);
            g_app->pending_image_name = label ? label : "ios_photo.png";
            g_app->pending_image_ready = true;
            g_app->log_console(std::string("[IMAGE ENCODER] Dithered '") + g_app->pending_image_name +
                               "' -> 48x30 1-bit Floyd-Steinberg (180B payload / 181B opcode 0x49). Auto-transmitting...");
            g_app->confirm_send_pending_image();
        } else if (err) {
            g_app->log_active_tab(std::string("[IMAGE ENCODER ERROR] ") + err);
        }
        if (g_mac_win.ns_view) {
            msg_send<void>(g_mac_win.ns_view, sel_registerName("setNeedsDisplay"));
        }
    };
}

static void ios_sync_tf_from_app() {
    if (!g_ios_tf || !g_app) return;
    const std::string* src = &g_app->chat_input;
    if (g_app->active_input == ActiveInputTarget::SnifferFilter) src = &g_app->sniffer_filter;
    else if (g_app->active_input == ActiveInputTarget::NicklistFilter) src = &g_app->nicklist_filter;
    else if (g_app->active_input == ActiveInputTarget::CallsignEditor) src = &g_app->callsign;
    msg_send<void>(g_ios_tf, sel_registerName("setText:"), ns_str(src->c_str()));
}

static CGPoint ios_touch_pt(id self, id touches, long* out_taps = nullptr) {
    id touch = msg_send<id>(touches, sel_registerName("anyObject"));
    if (!touch) return CGPointMake(0.0, 0.0);
    if (out_taps) {
        *out_taps = msg_send<long>(touch, sel_registerName("tapCount"));
    }
    CGPoint pt = msg_send<CGPoint>(touch, sel_registerName("locationInView:"), self);
    pt.y -= static_cast<CGFloat>(g_ios_top_inset);
    return pt;
}

static void ios_view_draw_rect(id self, SEL, CGRect) {
    ios_ensure_app_initialized();
    CGRect bounds = msg_send<CGRect>(self, sel_registerName("bounds"));
    int raw_w = static_cast<int>(bounds.size.width);
    int raw_h = static_cast<int>(bounds.size.height);
    g_ios_top_inset = (raw_h > raw_w && raw_h >= 720) ? 54 : 24;
    int bot_inset = g_ios_keyboard_visible ? 285 : ((raw_h > raw_w && raw_h >= 720) ? 24 : 0);
    int new_w = std::max(240, raw_w);
    int new_h = std::max(280, raw_h - g_ios_top_inset - bot_inset);

    if (new_w != g_mac_win.width || new_h != g_mac_win.height) {
        g_mac_win.width = new_w;
        g_mac_win.height = new_h;
        WndProc(&g_mac_win, WM_SIZE, 0, MAKELPARAM_MAC(new_w, new_h));
    }
    CGContextRef cg = UIGraphicsGetCurrentContext();
    if (!cg) return;

    // Fill status bar + home indicator areas with theme background
    CGContextSaveGState(cg);
    if (g_app && g_app->light_mode) {
        CGContextSetRGBFillColor(cg, 0.945, 0.961, 0.976, 1.0);
    } else {
        CGContextSetRGBFillColor(cg, 0.035, 0.051, 0.086, 1.0);
    }
    CGContextFillRect(cg, bounds);
    CGContextTranslateCTM(cg, 0.0, static_cast<CGFloat>(g_ios_top_inset));

    g_mac_win.active_cg = cg;
    WndProc(&g_mac_win, WM_PAINT, 0, 0);
    g_mac_win.active_cg = nullptr;
    CGContextRestoreGState(cg);
}

static void ios_touches_began(id self, SEL, id touches, id) {
    long taps = 1;
    CGPoint pt = ios_touch_pt(self, touches, &taps);
    g_ios_touch_start = pt;
    g_ios_last_drag_y = pt.y;
    g_ios_dragging_scroll = false;

    LPARAM lp = MAKELPARAM_MAC(static_cast<int>(pt.x), static_cast<int>(pt.y));
    WndProc(&g_mac_win, WM_MOUSEMOVE, 0, lp);
    if (taps >= 2) {
        WndProc(&g_mac_win, WM_LBUTTONDBLCLK, 0, lp);
        msg_send<void>(self, sel_registerName("setNeedsDisplay"));
        return;
    }
    if (g_app && g_app->mobile_tab == 0 && g_app->center_view == CenterView::Map &&
        D2DRenderer::pt_in_rect(POINT{static_cast<LONG>(pt.x), static_cast<LONG>(pt.y)}, g_app->map_rect)) {
        WndProc(&g_mac_win, WM_LBUTTONDOWN, 0, lp);
    }
    msg_send<void>(self, sel_registerName("setNeedsDisplay"));
}

static void ios_touches_moved(id self, SEL, id touches, id) {
    CGPoint pt = ios_touch_pt(self, touches, nullptr);
    LPARAM lp = MAKELPARAM_MAC(static_cast<int>(pt.x), static_cast<int>(pt.y));
    if (g_app && g_app->map_dragging) {
        WndProc(&g_mac_win, WM_MOUSEMOVE, 0, lp);
        msg_send<void>(self, sel_registerName("setNeedsDisplay"));
        return;
    }
    CGFloat dy = pt.y - g_ios_last_drag_y;
    if (!g_ios_dragging_scroll && std::abs(pt.y - g_ios_touch_start.y) > 12.0) {
        g_ios_dragging_scroll = true;
    }
    if (g_ios_dragging_scroll && std::abs(dy) >= 18.0) {
        g_ios_last_drag_y = pt.y;
        int delta = (dy > 0.0) ? 120 : -120;
        WPARAM wp = static_cast<WPARAM>(static_cast<uint16_t>(static_cast<int16_t>(delta))) << 16;
        WndProc(&g_mac_win, WM_MOUSEWHEEL, wp, lp);
        msg_send<void>(self, sel_registerName("setNeedsDisplay"));
    }
}

static void ios_touches_ended(id self, SEL, id touches, id) {
    CGPoint pt = ios_touch_pt(self, touches, nullptr);
    LPARAM lp = MAKELPARAM_MAC(static_cast<int>(pt.x), static_cast<int>(pt.y));
    POINT win_pt{static_cast<LONG>(pt.x), static_cast<LONG>(pt.y)};

    if (g_app && g_app->map_dragging) {
        WndProc(&g_mac_win, WM_LBUTTONUP, 0, lp);
        msg_send<void>(self, sel_registerName("setNeedsDisplay"));
        return;
    }

    if (!g_ios_dragging_scroll) {
        bool hit_text_box = false;
        bool hit_tx_btn = false;
        bool hit_attach_btn = false;
        if (g_app) {
            for (const auto& b : g_app->button_hits) {
                if (D2DRenderer::pt_in_rect(win_pt, b.rect)) {
                    if (b.id == BTN_CHAT_INPUT_BOX || b.id == BTN_ADD_TAG ||
                        b.id == BTN_SNIFFER_FILTER_BOX || b.id == BTN_NICKLIST_FILTER_BOX ||
                        b.id == BTN_CALLSIGN) {
                        hit_text_box = true;
                    } else if (b.id == BTN_TRANSMIT_TX) {
                        hit_tx_btn = true;
                    } else if (b.id == BTN_ATTACH_IMAGE) {
                        hit_attach_btn = true;
                    }
                    break;
                }
            }
        }

        if (hit_attach_btn && g_ios_keyboard_visible && g_ios_tf) {
            g_ios_keyboard_visible = false;
            msg_send<ObjcBool>(g_ios_tf, sel_registerName("resignFirstResponder"));
        }

        WndProc(&g_mac_win, WM_LBUTTONDOWN, 0, lp);
        WndProc(&g_mac_win, WM_LBUTTONUP, 0, lp);

        if (hit_text_box && g_ios_tf) {
            ios_sync_tf_from_app();
            g_ios_keyboard_visible = true;
            msg_send<ObjcBool>(g_ios_tf, sel_registerName("becomeFirstResponder"));
        } else if (hit_tx_btn && g_ios_tf) {
            ios_sync_tf_from_app();
            g_ios_keyboard_visible = false;
            msg_send<ObjcBool>(g_ios_tf, sel_registerName("resignFirstResponder"));
        } else if (g_ios_keyboard_visible && g_ios_tf) {
            g_ios_keyboard_visible = false;
            msg_send<ObjcBool>(g_ios_tf, sel_registerName("resignFirstResponder"));
        }
    }
    g_ios_dragging_scroll = false;
    msg_send<void>(self, sel_registerName("setNeedsDisplay"));
}

static void ios_on_text_changed(id self, SEL, id sender) {
    if (!g_app || !sender) return;
    id txt_obj = msg_send<id>(sender, sel_registerName("text"));
    std::string val = mac_ns_to_std_str(txt_obj);
    if (g_app->active_input == ActiveInputTarget::ChatComposer) {
        g_app->chat_input = val;
        g_app->chat_cursor = g_app->chat_input.size();
    } else if (g_app->active_input == ActiveInputTarget::SnifferFilter) {
        g_app->sniffer_filter = val;
        g_app->sniffer_scroll = 0;
    } else if (g_app->active_input == ActiveInputTarget::NicklistFilter) {
        g_app->nicklist_filter = val;
        g_app->nicklist_scroll = 0;
    } else if (g_app->active_input == ActiveInputTarget::CallsignEditor) {
        std::string clean;
        for (unsigned char c : val) {
            if (clean.size() < 14 && c >= 0x21 && c < 0x7F && c != ':') {
                clean.push_back(static_cast<char>(std::toupper(c)));
            }
        }
        g_app->callsign = clean;
    }
    msg_send<void>(self, sel_registerName("setNeedsDisplay"));
}

static void ios_on_text_return(id self, SEL, id sender) {
    if (g_app) {
        WndProc(&g_mac_win, WM_KEYDOWN, VK_RETURN, 0);
        ios_sync_tf_from_app();
    }
    g_ios_keyboard_visible = false;
    if (sender) {
        msg_send<ObjcBool>(sender, sel_registerName("resignFirstResponder"));
    }
    msg_send<void>(self, sel_registerName("setNeedsDisplay"));
}

static void ios_view_on_tick(id self, SEL, id) {
    if (!g_ios_ble_autostarted && g_app) {
        g_ios_ble_autostarted = true;
        g_app->connected_port = "BLE";
        g_app->log_public_system("[iOS BLE] Scanning Bluetooth LE for MeshCore Companion nodes (NUS)...");
        g_app->transport.connect_ble("");
    }
    WndProc(&g_mac_win, WM_TIMER, 1, 0);
    msg_send<void>(self, sel_registerName("setNeedsDisplay"));
}

static void ios_ensure_classes_registered();

static void ios_setup_root_window(id window_scene) {
    ios_ensure_classes_registered();
    ios_ensure_app_initialized();
    if (g_mac_win.ns_window && g_mac_win.ns_view) {
        if (window_scene) {
            msg_send<void>(g_mac_win.ns_window, sel_registerName("setWindowScene:"), window_scene);
        }
        msg_send<void>(g_mac_win.ns_window, sel_registerName("makeKeyAndVisible"));
        return;
    }

    CGRect bounds = CGRectMake(0.0, 0.0, 393.0, 852.0);
    if (window_scene) {
        id coord = msg_send<id>(window_scene, sel_registerName("coordinateSpace"));
        if (coord) bounds = msg_send<CGRect>(coord, sel_registerName("bounds"));
    } else {
        id screen = cls_send<id>("UIScreen", sel_registerName("mainScreen"));
        if (screen) bounds = msg_send<CGRect>(screen, sel_registerName("bounds"));
    }
    if (bounds.size.width < 200.0 || bounds.size.height < 200.0) {
        bounds = CGRectMake(0.0, 0.0, 393.0, 852.0);
    }

    id win_alloc = cls_send<id>("UIWindow", sel_registerName("alloc"));
    id window = nullptr;
    if (window_scene) {
        window = msg_send<id>(win_alloc, sel_registerName("initWithWindowScene:"), window_scene);
        if (window) msg_send<void>(window, sel_registerName("setFrame:"), bounds);
    } else {
        window = msg_send<id>(win_alloc, sel_registerName("initWithFrame:"), bounds);
    }
    if (window) CFRetain(window);

    id vc_alloc = cls_send<id>("UIViewController", sel_registerName("alloc"));
    id vc = msg_send<id>(vc_alloc, sel_registerName("init"));
    if (vc) CFRetain(vc);

    Class view_cls = objc_getClass("MeshCoreIOSView");
    id view_alloc = msg_send<id>(reinterpret_cast<id>(view_cls), sel_registerName("alloc"));
    id view = msg_send<id>(view_alloc, sel_registerName("initWithFrame:"), bounds);
    if (view) {
        CFRetain(view);
        // UIViewAutoresizingFlexibleWidth (2) | UIViewAutoresizingFlexibleHeight (16) = 18
        msg_send<void>(view, sel_registerName("setAutoresizingMask:"), static_cast<unsigned long>(18));
        msg_send<void>(view, sel_registerName("setMultipleTouchEnabled:"), static_cast<ObjcBool>(1));
    }

    // Off-screen UITextField to bridge native iOS keyboard events + Return key
    id tf_alloc = cls_send<id>("UITextField", sel_registerName("alloc"));
    g_ios_tf = msg_send<id>(tf_alloc, sel_registerName("initWithFrame:"), CGRectMake(-2000.0, -2000.0, 120.0, 32.0));
    if (g_ios_tf) {
        CFRetain(g_ios_tf);
        // UIKeyboardAppearanceDark = 1, UIReturnKeySend = 7, UITextAutocorrectionTypeNo = 1
        msg_send<void>(g_ios_tf, sel_registerName("setKeyboardAppearance:"), static_cast<long>(1));
        msg_send<void>(g_ios_tf, sel_registerName("setReturnKeyType:"), static_cast<long>(7));
        msg_send<void>(g_ios_tf, sel_registerName("setAutocorrectionType:"), static_cast<long>(1));
        // UIControlEventEditingChanged = (1 << 17), UIControlEventEditingDidEndOnExit = (1 << 19)
        msg_send<void>(g_ios_tf, sel_registerName("addTarget:action:forControlEvents:"),
                       view, sel_registerName("onTextChanged:"), static_cast<unsigned long>(1UL << 17));
        msg_send<void>(g_ios_tf, sel_registerName("addTarget:action:forControlEvents:"),
                       view, sel_registerName("onTextReturn:"), static_cast<unsigned long>(1UL << 19));
        msg_send<void>(view, sel_registerName("addSubview:"), g_ios_tf);
    }

    g_mac_win.width = static_cast<int>(bounds.size.width);
    g_mac_win.height = std::max(280, static_cast<int>(bounds.size.height) - g_ios_top_inset - 24);
    g_mac_win.ns_window = window;
    g_mac_win.ns_view = view;
    g_mac_win.ios_vc = vc;
    g_ios_root_vc = vc;

    msg_send<void>(vc, sel_registerName("setView:"), view);
    msg_send<void>(window, sel_registerName("setRootViewController:"), vc);
    msg_send<void>(window, sel_registerName("makeKeyAndVisible"));

    // 30 FPS UI + RF telemetry timer (33ms)
    cls_send<id>("NSTimer",
                 sel_registerName("scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:"),
                 0.033, view, sel_registerName("onTick:"), static_cast<id>(nullptr), static_cast<ObjcBool>(1));
}

static id ios_delegate_get_window(id, SEL) {
    return g_mac_win.ns_window;
}

static void ios_delegate_set_window(id, SEL, id win) {
    if (win && win != g_mac_win.ns_window) {
        CFRetain(win);
        if (g_mac_win.ns_window) CFRelease(g_mac_win.ns_window);
        g_mac_win.ns_window = win;
    }
}

static void ios_deferred_classic_window_check(id, SEL, id) {
    // Fallback for older non-scene iOS runtimes if scene:willConnectToSession:options: was not invoked
    if (!g_mac_win.ns_window) {
        ios_setup_root_window(nullptr);
    }
}

static ObjcBool ios_app_did_finish_launching(id self, SEL, id, id) {
    ios_ensure_classes_registered();
    ios_ensure_app_initialized();
    // Schedule a 50ms check: on iOS 13-27+ with UIApplicationSceneManifest, scene:willConnectToSession:options:
    // creates the UIWindow with the UIWindowScene immediately after didFinishLaunchingWithOptions returns.
    // Creating a scene-less UIWindow inside didFinishLaunchingWithOptions on iOS 26/27 triggers a UIKit scene assertion!
    cls_send<id>("NSTimer",
                 sel_registerName("scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:"),
                 0.05, self, sel_registerName("onDeferredWindowCheck:"), static_cast<id>(nullptr), static_cast<ObjcBool>(0));
    return static_cast<ObjcBool>(1);
}

static void ios_scene_will_connect(id, SEL, id scene, id, id) {
    ios_setup_root_window(scene);
}

static id ios_config_for_scene_session(id, SEL, id, id session, id) {
    id role = session ? msg_send<id>(session, sel_registerName("role")) : ns_str("UIWindowSceneSessionRoleApplication");
    id cfg_alloc = cls_send<id>("UISceneConfiguration", sel_registerName("alloc"));
    id cfg = msg_send<id>(cfg_alloc, sel_registerName("initWithName:sessionRole:"), ns_str("Default Configuration"), role);
    if (cfg) {
        Class ws_cls = objc_getClass("UIWindowScene");
        if (ws_cls) {
            msg_send<void>(cfg, sel_registerName("setSceneClass:"), reinterpret_cast<id>(ws_cls));
        }
        msg_send<void>(cfg, sel_registerName("setDelegateClass:"), reinterpret_cast<id>(objc_getClass("MeshCoreIOSAppDelegate")));
    }
    return cfg;
}

static ObjcBool ios_delegate_conforms_to_protocol(id, SEL, id) {
    return static_cast<ObjcBool>(1);
}

static void ios_ensure_classes_registered() {
    static bool s_registered = false;
    if (s_registered) return;
    s_registered = true;

    Class ui_view = objc_getClass("UIView");
    if (ui_view && !objc_getClass("MeshCoreIOSView")) {
        Class view_cls = objc_allocateClassPair(ui_view, "MeshCoreIOSView", 0);
        if (view_cls) {
            class_addMethod(view_cls, sel_registerName("drawRect:"), reinterpret_cast<IMP>(ios_view_draw_rect), "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
            class_addMethod(view_cls, sel_registerName("touchesBegan:withEvent:"), reinterpret_cast<IMP>(ios_touches_began), "v@:@@");
            class_addMethod(view_cls, sel_registerName("touchesMoved:withEvent:"), reinterpret_cast<IMP>(ios_touches_moved), "v@:@@");
            class_addMethod(view_cls, sel_registerName("touchesEnded:withEvent:"), reinterpret_cast<IMP>(ios_touches_ended), "v@:@@");
            class_addMethod(view_cls, sel_registerName("touchesCancelled:withEvent:"), reinterpret_cast<IMP>(ios_touches_ended), "v@:@@");
            class_addMethod(view_cls, sel_registerName("onTextChanged:"), reinterpret_cast<IMP>(ios_on_text_changed), "v@:@");
            class_addMethod(view_cls, sel_registerName("onTextReturn:"), reinterpret_cast<IMP>(ios_on_text_return), "v@:@");
            class_addMethod(view_cls, sel_registerName("onTick:"), reinterpret_cast<IMP>(ios_view_on_tick), "v@:@");
            objc_registerClassPair(view_cls);
        }
    }

    if (!objc_getClass("MeshCoreIOSAppDelegate")) {
        Class super_del = objc_getClass("UIResponder");
        if (!super_del) super_del = objc_getClass("NSObject");
        Class del_cls = objc_allocateClassPair(super_del, "MeshCoreIOSAppDelegate", 0);
        if (del_cls) {
            const char* protos[] = {"UIApplicationDelegate", "UIWindowSceneDelegate", "UISceneDelegate"};
            for (const char* pname : protos) {
                if (Protocol* p = objc_getProtocol(pname)) {
                    class_addProtocol(del_cls, p);
                }
            }
            class_addMethod(del_cls, sel_registerName("conformsToProtocol:"),
                            reinterpret_cast<IMP>(ios_delegate_conforms_to_protocol), "B@:@");
            class_addMethod(del_cls, sel_registerName("window"), reinterpret_cast<IMP>(ios_delegate_get_window), "@@:");
            class_addMethod(del_cls, sel_registerName("setWindow:"), reinterpret_cast<IMP>(ios_delegate_set_window), "v@:@");
            class_addMethod(del_cls, sel_registerName("application:didFinishLaunchingWithOptions:"),
                            reinterpret_cast<IMP>(ios_app_did_finish_launching), "B@:@@");
            class_addMethod(del_cls, sel_registerName("onDeferredWindowCheck:"),
                            reinterpret_cast<IMP>(ios_deferred_classic_window_check), "v@:@");
            class_addMethod(del_cls, sel_registerName("application:configurationForConnectingSceneSession:options:"),
                            reinterpret_cast<IMP>(ios_config_for_scene_session), "@@:@@@");
            class_addMethod(del_cls, sel_registerName("scene:willConnectToSession:options:"),
                            reinterpret_cast<IMP>(ios_scene_will_connect), "v@:@@@");
            objc_registerClassPair(del_cls);
        }
    }
}

__attribute__((visibility("default")))
int main(int argc, char** argv) {
    ios_ensure_classes_registered();
    return UIApplicationMain(argc, argv, nullptr, ns_str("MeshCoreIOSAppDelegate"));
}
#else
static CGPoint event_pt_in_view(id self, id event) {
    CGPoint win_pt = msg_send<CGPoint>(event, sel_registerName("locationInWindow"));
    return msg_send<CGPoint>(self, sel_registerName("convertPoint:fromView:"), win_pt, static_cast<id>(nullptr));
}

static ObjcBool mac_view_is_flipped(id, SEL) { return static_cast<ObjcBool>(1); }
static ObjcBool mac_view_accepts_first_responder(id, SEL) { return static_cast<ObjcBool>(1); }

static void mac_view_draw_rect(id self, SEL, CGRect) {
    CGRect bounds = msg_send<CGRect>(self, sel_registerName("bounds"));
    int new_w = static_cast<int>(bounds.size.width);
    int new_h = static_cast<int>(bounds.size.height);
    if (new_w > 0 && new_h > 0 && (new_w != g_mac_win.width || new_h != g_mac_win.height)) {
        g_mac_win.width = new_w;
        g_mac_win.height = new_h;
        WndProc(&g_mac_win, WM_SIZE, 0, MAKELPARAM_MAC(new_w, new_h));
    }
    id gfx_ctx = cls_send<id>("NSGraphicsContext", sel_registerName("currentContext"));
    if (!gfx_ctx) return;
    CGContextRef cg = msg_send<CGContextRef>(gfx_ctx, sel_registerName("CGContext"));
    if (!cg) return;
    g_mac_win.active_cg = cg;
    WndProc(&g_mac_win, WM_PAINT, 0, 0);
    g_mac_win.active_cg = nullptr;
}

static void mac_flush_display(id view) {
    if (g_mac_win.needs_display) {
        g_mac_win.needs_display = false;
        msg_send<void>(view, sel_registerName("setNeedsDisplay:"), static_cast<ObjcBool>(1));
    }
}

static void mac_view_mouse_down(id self, SEL, id event) {
    if (g_mac_win.ns_window) {
        msg_send<ObjcBool>(g_mac_win.ns_window, sel_registerName("makeFirstResponder:"), self);
    }
    CGPoint pt = event_pt_in_view(self, event);
    LPARAM lp = MAKELPARAM_MAC(static_cast<int>(pt.x), static_cast<int>(pt.y));
    long clicks = msg_send<long>(event, sel_registerName("clickCount"));
    if (clicks >= 2) {
        WndProc(&g_mac_win, WM_LBUTTONDBLCLK, 0, lp);
    } else {
        WndProc(&g_mac_win, WM_LBUTTONDOWN, 0, lp);
    }
    mac_flush_display(self);
}

static void mac_view_mouse_up(id self, SEL, id event) {
    CGPoint pt = event_pt_in_view(self, event);
    LPARAM lp = MAKELPARAM_MAC(static_cast<int>(pt.x), static_cast<int>(pt.y));
    WndProc(&g_mac_win, WM_LBUTTONUP, 0, lp);
    mac_flush_display(self);
}

static void mac_view_mouse_moved(id self, SEL, id event) {
    CGPoint pt = event_pt_in_view(self, event);
    LPARAM lp = MAKELPARAM_MAC(static_cast<int>(pt.x), static_cast<int>(pt.y));
    WndProc(&g_mac_win, WM_MOUSEMOVE, 0, lp);
    mac_flush_display(self);
}

static void mac_view_scroll_wheel(id self, SEL, id event) {
    CGPoint pt = event_pt_in_view(self, event);
    LPARAM lp = MAKELPARAM_MAC(static_cast<int>(pt.x), static_cast<int>(pt.y));
    WndProc(&g_mac_win, WM_MOUSEMOVE, 0, lp);
    CGFloat dy = msg_send<CGFloat>(event, sel_registerName("scrollingDeltaY"));
    int delta = (dy > 0.0) ? 120 : (dy < 0.0) ? -120 : 0;
    if (delta != 0) {
        WPARAM wp = static_cast<WPARAM>(static_cast<uint16_t>(static_cast<int16_t>(delta))) << 16;
        WndProc(&g_mac_win, WM_MOUSEWHEEL, wp, lp);
        mac_flush_display(self);
    }
}

static void mac_view_key_down(id self, SEL, id event) {
    unsigned long flags = msg_send<unsigned long>(event, sel_registerName("modifierFlags"));
    // NSEventModifierFlagCommand (1 << 20) or NSEventModifierFlagControl (1 << 18)
    g_mac_cmd_down = ((flags & ((1UL << 20) | (1UL << 18))) != 0);
    unsigned short key_code = msg_send<unsigned short>(event, sel_registerName("keyCode"));

    if (g_mac_cmd_down) {
        if (key_code == 3) { // Cmd+F -> Focus Room Users Search Box
            WndProc(&g_mac_win, WM_KEYDOWN, 'F', 0);
            g_mac_cmd_down = false;
            mac_flush_display(self);
            return;
        }
        if (key_code == 8) { // Cmd+C
            WndProc(&g_mac_win, WM_KEYDOWN, 'C', 0);
            g_mac_cmd_down = false;
            mac_flush_display(self);
            return;
        }
        if (key_code == 9) { // Cmd+V
            WndProc(&g_mac_win, WM_KEYDOWN, 'V', 0);
            g_mac_cmd_down = false;
            mac_flush_display(self);
            return;
        }
        if (key_code == 40) { // Cmd+K
            WndProc(&g_mac_win, WM_KEYDOWN, 'K', 0);
            g_mac_cmd_down = false;
            mac_flush_display(self);
            return;
        }
        if (key_code == 12) { // Cmd+Q
            WndProc(&g_mac_win, WM_DESTROY, 0, 0);
            id app = cls_send<id>("NSApplication", sel_registerName("sharedApplication"));
            msg_send<void>(app, sel_registerName("terminate:"), static_cast<id>(nullptr));
            return;
        }
    }

    if (key_code == 36 || key_code == 76) { // Return / Enter
        WndProc(&g_mac_win, WM_KEYDOWN, VK_RETURN, 0);
    } else if (key_code == 53) { // Escape
        WndProc(&g_mac_win, WM_KEYDOWN, VK_ESCAPE, 0);
    } else if (key_code == 48) { // Tab
        WndProc(&g_mac_win, WM_KEYDOWN, VK_TAB, 0);
    } else if (key_code == 116) { // PageUp
        WndProc(&g_mac_win, WM_KEYDOWN, VK_PRIOR, 0);
    } else if (key_code == 121) { // PageDown
        WndProc(&g_mac_win, WM_KEYDOWN, VK_NEXT, 0);
    } else if (key_code == 51 || key_code == 117) { // Backspace (51) or Forward Delete (117)
        WndProc(&g_mac_win, WM_CHAR, static_cast<WPARAM>('\b'), 0);
    } else {
        id chars = msg_send<id>(event, sel_registerName("characters"));
        if (chars) {
            const char* utf8 = msg_send<const char*>(chars, sel_registerName("UTF8String"));
            if (utf8) {
                for (const char* p = utf8; *p; ++p) {
                    unsigned char uc = static_cast<unsigned char>(*p);
                    if (uc >= 0x20 && uc < 0x7F) {
                        WndProc(&g_mac_win, WM_CHAR, static_cast<WPARAM>(uc), 0);
                    }
                }
            }
        }
    }
    g_mac_cmd_down = false;
    mac_flush_display(self);
}

static void mac_view_on_tick(id self, SEL, id) {
    WndProc(&g_mac_win, WM_TIMER, 1, 0);
    msg_send<void>(self, sel_registerName("setNeedsDisplay:"), static_cast<ObjcBool>(1));
}

static void mac_win_will_close(id, SEL, id) {
    WndProc(&g_mac_win, WM_DESTROY, 0, 0);
    id app = cls_send<id>("NSApplication", sel_registerName("sharedApplication"));
    msg_send<void>(app, sel_registerName("terminate:"), static_cast<id>(nullptr));
}

// Ensures that on macOS Sonoma+ (both Intel x86_64 and Apple Silicon arm64), the running app bundle
// or binary has a genuine native Apple `LC_CODE_SIGNATURE` generated by `/usr/bin/codesign` so that
// `tccd` / `bluetoothd` honors the user's System Settings -> Privacy & Security -> Bluetooth toggle.
static void mac_ensure_native_codesign(char** argv) {
    if (std::getenv("MESHCORE_SIGNED_EXEC") != nullptr) return;

    char exe_buf[4096]{};
    uint32_t exe_sz = sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &exe_sz) != 0) return;

    char real_buf[4096]{};
    const char* exe_path = realpath(exe_buf, real_buf) ? real_buf : exe_buf;
    std::filesystem::path ep(exe_path);

    std::error_code ec;
    auto fsize = std::filesystem::file_size(ep, ec);
    if (ec) return;

    // Determine whether we are running inside MeshCore++.app/Contents/MacOS/MeshCore++
    std::filesystem::path sign_target = ep;
    bool is_app_bundle = false;
    if (ep.parent_path().filename() == "MacOS" &&
        ep.parent_path().parent_path().filename() == "Contents" &&
        ep.parent_path().parent_path().parent_path().extension() == ".app") {
        sign_target = ep.parent_path().parent_path().parent_path();
        is_app_bundle = true;
    }

    std::filesystem::path data_dir = std::filesystem::path(from_wide(exe_dir()));
    std::filesystem::create_directories(data_dir, ec);

    // Store marker inside the .app bundle itself (Contents/.codesigned_v6_<size>) so every freshly
    // downloaded/unzipped MeshCore++.app gets signed on first launch, and never re-resets TCC after!
    std::filesystem::path marker_dir = is_app_bundle ? (sign_target / "Contents") : data_dir;
    std::filesystem::path marker = marker_dir / (".codesigned_v6_" + std::to_string(fsize));
    if (std::filesystem::exists(marker, ec)) return;

    std::string target_str = sign_target.string();
    std::string cmd = "/usr/bin/xattr -cr \"" + target_str + "\" >/dev/null 2>&1; "
                      "/usr/bin/codesign --force --deep --identifier io.meshcore.plusplus --sign - \"" + target_str + "\" >/dev/null 2>&1; "
                      "/usr/bin/tccutil reset Bluetooth io.meshcore.plusplus >/dev/null 2>&1; "
                      "/usr/bin/tccutil reset Bluetooth com.meshcore.plusplus >/dev/null 2>&1";
    std::system(cmd.c_str());

    // Record the new post-codesign file size inside the bundle (and fallback data_dir) so subsequent launches
    // start immediately without resetting the user's granted Bluetooth TCC permission!
    auto new_fsize = std::filesystem::file_size(ep, ec);
    {
        std::ofstream mf(marker_dir / (".codesigned_v6_" + std::to_string(new_fsize ? new_fsize : fsize)));
        mf << "signed\n";
    }
    {
        std::ofstream mf2(marker);
        mf2 << "signed\n";
    }

    setenv("MESHCORE_SIGNED_EXEC", "1", 1);
    execv(exe_path, argv);
}

int main(int, char** argv) {
    mac_ensure_native_codesign(argv);

    MeshGuiApp app;
    D2DRenderer renderer;
    g_app = &app;
    g_renderer = &renderer;

    id ns_app = cls_send<id>("NSApplication", sel_registerName("sharedApplication"));
    // NSApplicationActivationPolicyRegular = 0
    msg_send<ObjcBool>(ns_app, sel_registerName("setActivationPolicy:"), static_cast<long>(0));

    Class view_cls = objc_allocateClassPair(objc_getClass("NSView"), "MeshCoreView", 0);
    class_addMethod(view_cls, sel_registerName("isFlipped"), reinterpret_cast<IMP>(mac_view_is_flipped), "B@:");
    class_addMethod(view_cls, sel_registerName("acceptsFirstResponder"), reinterpret_cast<IMP>(mac_view_accepts_first_responder), "B@:");
    class_addMethod(view_cls, sel_registerName("drawRect:"), reinterpret_cast<IMP>(mac_view_draw_rect), "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    class_addMethod(view_cls, sel_registerName("mouseDown:"), reinterpret_cast<IMP>(mac_view_mouse_down), "v@:@");
    class_addMethod(view_cls, sel_registerName("mouseUp:"), reinterpret_cast<IMP>(mac_view_mouse_up), "v@:@");
    class_addMethod(view_cls, sel_registerName("mouseMoved:"), reinterpret_cast<IMP>(mac_view_mouse_moved), "v@:@");
    class_addMethod(view_cls, sel_registerName("mouseDragged:"), reinterpret_cast<IMP>(mac_view_mouse_moved), "v@:@");
    class_addMethod(view_cls, sel_registerName("scrollWheel:"), reinterpret_cast<IMP>(mac_view_scroll_wheel), "v@:@");
    class_addMethod(view_cls, sel_registerName("keyDown:"), reinterpret_cast<IMP>(mac_view_key_down), "v@:@");
    class_addMethod(view_cls, sel_registerName("onTick:"), reinterpret_cast<IMP>(mac_view_on_tick), "v@:@");
    class_addMethod(view_cls, sel_registerName("windowWillClose:"), reinterpret_cast<IMP>(mac_win_will_close), "v@:@");
    objc_registerClassPair(view_cls);

    CGRect frame = CGRectMake(80.0, 80.0, 1460.0, 880.0);
    // NSWindowStyleMaskTitled(1) | Closable(2) | Miniaturizable(4) | Resizable(8) = 15
    unsigned long style_mask = 15UL;
    id window_alloc = cls_send<id>("NSWindow", sel_registerName("alloc"));
    id window = msg_send<id>(window_alloc,
                             sel_registerName("initWithContentRect:styleMask:backing:defer:"),
                             frame, style_mask, static_cast<unsigned long>(2), static_cast<ObjcBool>(0));

    msg_send<void>(window, sel_registerName("setTitle:"), ns_str("MeshCore++"));
    msg_send<void>(window, sel_registerName("setAcceptsMouseMovedEvents:"), static_cast<ObjcBool>(1));

    id appearance = cls_send<id>("NSAppearance", sel_registerName("appearanceNamed:"),
                                 ns_str(app.light_mode ? "NSAppearanceNameAqua" : "NSAppearanceNameDarkAqua"));
    if (appearance) {
        msg_send<void>(window, sel_registerName("setAppearance:"), appearance);
    }

    id view_alloc = msg_send<id>(reinterpret_cast<id>(view_cls), sel_registerName("alloc"));
    id view = msg_send<id>(view_alloc, sel_registerName("initWithFrame:"), frame);

    msg_send<void>(window, sel_registerName("setContentView:"), view);
    msg_send<void>(window, sel_registerName("setDelegate:"), view);
    msg_send<ObjcBool>(window, sel_registerName("makeFirstResponder:"), view);
    msg_send<void>(window, sel_registerName("center"));
    msg_send<void>(window, sel_registerName("makeKeyAndOrderFront:"), static_cast<id>(nullptr));
    msg_send<void>(ns_app, sel_registerName("activateIgnoringOtherApps:"), static_cast<ObjcBool>(1));

    g_mac_win.ns_window = window;
    g_mac_win.ns_view = view;

    // 30 FPS UI + RF telemetry timer (33ms)
    cls_send<id>("NSTimer",
                 sel_registerName("scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:"),
                 0.033, view, sel_registerName("onTick:"), static_cast<id>(nullptr), static_cast<ObjcBool>(1));

    msg_send<void>(ns_app, sel_registerName("run"));

    g_app = nullptr;
    g_renderer = nullptr;
    return 0;
}
#endif // MESHCORE_IOS
#endif


