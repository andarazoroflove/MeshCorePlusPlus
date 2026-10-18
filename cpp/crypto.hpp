#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace meshirc {

constexpr const char* PUBLIC_CHANNEL_NAME = "Public";
constexpr const char* PUBLIC_CHANNEL_PSK_HEX = "8b3387e9c5cdea6ac9e5edbaa115cd72";

constexpr std::array<uint8_t, 16> PUBLIC_CHANNEL_PSK = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72
};

// ============================================================================
// Pure C++20 FIPS 180-4 SHA-256 Implementation
// ============================================================================
class Sha256 {
public:
    static std::array<uint8_t, 32> digest(const uint8_t* data, size_t len) {
        static constexpr uint32_t K[64] = {
            0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
            0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
            0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
            0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
            0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
            0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
            0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
            0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
            0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
            0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
            0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
            0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
            0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
            0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
            0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
            0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
        };

        uint32_t h[8] = {
            0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
            0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL
        };

        uint64_t bit_len = static_cast<uint64_t>(len) * 8ULL;
        size_t padded_len = ((len + 9 + 63) / 64) * 64;
        std::vector<uint8_t> buf(padded_len, 0);
        if (len > 0) {
            std::memcpy(buf.data(), data, len);
        }
        buf[len] = 0x80;
        for (int i = 0; i < 8; ++i) {
            buf[padded_len - 1 - i] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF);
        }

        auto rotr = [](uint32_t x, uint32_t n) -> uint32_t {
            return (x >> n) | (x << (32 - n));
        };

        for (size_t offset = 0; offset < padded_len; offset += 64) {
            uint32_t w[64];
            for (int i = 0; i < 16; ++i) {
                size_t j = offset + i * 4;
                w[i] = (static_cast<uint32_t>(buf[j]) << 24) |
                       (static_cast<uint32_t>(buf[j + 1]) << 16) |
                       (static_cast<uint32_t>(buf[j + 2]) << 8) |
                       (static_cast<uint32_t>(buf[j + 3]));
            }
            for (int i = 16; i < 64; ++i) {
                uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }

            uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

            for (int i = 0; i < 64; ++i) {
                uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                uint32_t ch = (e & f) ^ ((~e) & g);
                uint32_t temp1 = hh + S1 + ch + K[i] + w[i];
                uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t temp2 = S0 + maj;

                hh = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }

            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
            h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
        }

        std::array<uint8_t, 32> out{};
        for (int i = 0; i < 8; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
            out[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
            out[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
            out[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFF);
        }
        return out;
    }

    static std::array<uint8_t, 32> digest_str(const std::string& s) {
        return digest(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
};

// ============================================================================
// Pure C++20 FIPS 197 AES-128 Block Cipher (Encrypt + Decrypt)
// ============================================================================
class Aes128 {
public:
    explicit Aes128(const std::array<uint8_t, 16>& key) {
        expand_key(key);
    }

    void encrypt_block(uint8_t block[16]) const {
        add_round_key(block, 0);
        for (int round = 1; round < 10; ++round) {
            sub_bytes(block);
            shift_rows(block);
            mix_columns(block);
            add_round_key(block, round);
        }
        sub_bytes(block);
        shift_rows(block);
        add_round_key(block, 10);
    }

    void decrypt_block(uint8_t block[16]) const {
        add_round_key(block, 10);
        for (int round = 9; round >= 1; --round) {
            inv_shift_rows(block);
            inv_sub_bytes(block);
            add_round_key(block, round);
            inv_mix_columns(block);
        }
        inv_shift_rows(block);
        inv_sub_bytes(block);
        add_round_key(block, 0);
    }

private:
    std::array<uint8_t, 176> round_keys_{};

    static constexpr uint8_t SBOX[256] = {
        0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
        0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
        0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
        0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
        0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
        0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
        0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
        0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
        0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
        0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
        0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
        0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
        0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
        0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
        0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
        0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
    };

    static constexpr uint8_t INV_SBOX[256] = {
        0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
        0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
        0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
        0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
        0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
        0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
        0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
        0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
        0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
        0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
        0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
        0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
        0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
        0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
        0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
        0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
    };

    static uint8_t gmul(uint8_t a, uint8_t b) {
        uint8_t p = 0;
        for (int i = 0; i < 8; ++i) {
            if (b & 1) p ^= a;
            uint8_t hi = a & 0x80;
            a <<= 1;
            if (hi) a ^= 0x1b;
            b >>= 1;
        }
        return p;
    }

    void expand_key(const std::array<uint8_t, 16>& key) {
        static constexpr uint8_t RCON[11] = {
            0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
        };
        std::memcpy(round_keys_.data(), key.data(), 16);
        for (int i = 4; i < 44; ++i) {
            uint8_t temp[4];
            for (int j = 0; j < 4; ++j) temp[j] = round_keys_[(i - 1) * 4 + j];
            if (i % 4 == 0) {
                uint8_t t = temp[0];
                temp[0] = SBOX[temp[1]] ^ RCON[i / 4];
                temp[1] = SBOX[temp[2]];
                temp[2] = SBOX[temp[3]];
                temp[3] = SBOX[t];
            }
            for (int j = 0; j < 4; ++j) {
                round_keys_[i * 4 + j] = round_keys_[(i - 4) * 4 + j] ^ temp[j];
            }
        }
    }

    void add_round_key(uint8_t state[16], int round) const {
        for (int i = 0; i < 16; ++i) state[i] ^= round_keys_[round * 16 + i];
    }

    static void sub_bytes(uint8_t state[16]) {
        for (int i = 0; i < 16; ++i) state[i] = SBOX[state[i]];
    }

    static void inv_sub_bytes(uint8_t state[16]) {
        for (int i = 0; i < 16; ++i) state[i] = INV_SBOX[state[i]];
    }

    static void shift_rows(uint8_t s[16]) {
        uint8_t t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
        t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
    }

    static void inv_shift_rows(uint8_t s[16]) {
        uint8_t t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
        t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
    }

    static void mix_columns(uint8_t s[16]) {
        for (int c = 0; c < 4; ++c) {
            int i = c * 4;
            uint8_t s0 = s[i], s1 = s[i + 1], s2 = s[i + 2], s3 = s[i + 3];
            s[i]     = gmul(s0, 2) ^ gmul(s1, 3) ^ s2 ^ s3;
            s[i + 1] = s0 ^ gmul(s1, 2) ^ gmul(s2, 3) ^ s3;
            s[i + 2] = s0 ^ s1 ^ gmul(s2, 2) ^ gmul(s3, 3);
            s[i + 3] = gmul(s0, 3) ^ s1 ^ s2 ^ gmul(s3, 2);
        }
    }

    static void inv_mix_columns(uint8_t s[16]) {
        for (int c = 0; c < 4; ++c) {
            int i = c * 4;
            uint8_t s0 = s[i], s1 = s[i + 1], s2 = s[i + 2], s3 = s[i + 3];
            s[i]     = gmul(s0, 0x0e) ^ gmul(s1, 0x0b) ^ gmul(s2, 0x0d) ^ gmul(s3, 0x09);
            s[i + 1] = gmul(s0, 0x09) ^ gmul(s1, 0x0e) ^ gmul(s2, 0x0b) ^ gmul(s3, 0x0d);
            s[i + 2] = gmul(s0, 0x0d) ^ gmul(s1, 0x09) ^ gmul(s2, 0x0e) ^ gmul(s3, 0x0b);
            s[i + 3] = gmul(s0, 0x0b) ^ gmul(s1, 0x0d) ^ gmul(s2, 0x09) ^ gmul(s3, 0x0e);
        }
    }
};

// ============================================================================
// Helper Utilities & Key Derivation
// ============================================================================
inline std::string bytes_to_hex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

inline std::string psk_to_hex(const std::array<uint8_t, 16>& psk) {
    return bytes_to_hex(psk.data(), psk.size());
}

inline std::string trim_str(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

inline std::string normalize_channel_name(const std::string& raw) {
    std::string t = trim_str(raw);
    if (t.empty()) return "#general";
    std::string lower = t;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "public" || lower == "#public") return PUBLIC_CHANNEL_NAME;
    if (lower[0] != '#') return "#" + lower;
    return lower;
}

inline std::array<uint8_t, 16> derive_psk(const std::string& channel_name) {
    std::string norm = normalize_channel_name(channel_name);
    if (norm == PUBLIC_CHANNEL_NAME) {
        return PUBLIC_CHANNEL_PSK;
    }
    std::string exact = trim_str(channel_name);
    if (!exact.empty() && exact[0] != '#') exact = "#" + exact;
    auto full = Sha256::digest_str(exact.empty() ? norm : exact);
    std::array<uint8_t, 16> psk{};
    std::memcpy(psk.data(), full.data(), 16);
    return psk;
}

inline uint8_t compute_channel_hash(const std::array<uint8_t, 16>& psk) {
    auto digest = Sha256::digest(psk.data(), psk.size());
    return digest[0];
}

inline std::vector<uint8_t> encrypt_aes128_blocks(const std::array<uint8_t, 16>& psk,
                                                  const uint8_t* plain,
                                                  size_t len) {
    Aes128 cipher(psk);
    size_t padded = (len == 0) ? 16 : ((len + 15) / 16) * 16;
    std::vector<uint8_t> out(padded, 0);
    if (len > 0) std::memcpy(out.data(), plain, len);
    for (size_t off = 0; off < padded; off += 16) {
        cipher.encrypt_block(&out[off]);
    }
    return out;
}

inline std::optional<std::vector<uint8_t>> decrypt_aes128_blocks(
    const std::array<uint8_t, 16>& psk,
    const uint8_t* cipher_bytes,
    size_t len) {
    if (len == 0 || (len % 16) != 0) return std::nullopt;
    Aes128 cipher(psk);
    std::vector<uint8_t> out(cipher_bytes, cipher_bytes + len);
    for (size_t off = 0; off < len; off += 16) {
        cipher.decrypt_block(&out[off]);
    }
    return out;
}

inline std::vector<uint8_t> build_meshcore_hw_group_frame(
    const std::string& channel_name,
    const std::string& sender,
    const std::string& message,
    uint32_t timestamp) {
    auto psk = derive_psk(channel_name);
    uint8_t hash = compute_channel_hash(psk);
    std::string formatted = trim_str(sender) + ": " + message;

    std::vector<uint8_t> plain(5 + formatted.size());
    plain[0] = static_cast<uint8_t>(timestamp & 0xFF);
    plain[1] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
    plain[2] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
    plain[3] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
    plain[4] = 0x00;
    std::memcpy(plain.data() + 5, formatted.data(), formatted.size());

    auto ct = encrypt_aes128_blocks(psk, plain.data(), plain.size());
    auto mac = Sha256::digest(ct.data(), ct.size());

    std::vector<uint8_t> frame;
    frame.reserve(3 + ct.size());
    frame.push_back(hash);
    frame.push_back(mac[0]);
    frame.push_back(mac[1]);
    frame.insert(frame.end(), ct.begin(), ct.end());
    return frame;
}

struct ChannelCandidate {
    std::string name;
    std::array<uint8_t, 16> psk;
    uint8_t channel_hash;
};

struct SniffedGroupMessage {
    std::string channel_name;
    uint8_t channel_hash;
    std::array<uint8_t, 16> psk;
    std::string sender;
    std::string text;
    uint32_t timestamp;
};

// Extract #hashtags matching #[a-zA-Z0-9_-]{2,32}
inline std::vector<std::string> extract_hashtags(const std::string& text) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '#') {
            if (i > 0) {
                unsigned char prev = static_cast<unsigned char>(text[i - 1]);
                if (std::isalnum(prev) || prev == '_') continue;
            }
            size_t j = i + 1;
            while (j < text.size()) {
                unsigned char c = static_cast<unsigned char>(text[j]);
                if (std::isalnum(c) || c == '_' || c == '-') {
                    ++j;
                } else {
                    break;
                }
            }
            size_t tag_len = j - (i + 1);
            if (tag_len >= 2 && tag_len <= 32) {
                std::string tag = text.substr(i, tag_len + 1);
                std::transform(tag.begin(), tag.end(), tag.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (seen.insert(tag).second) {
                    out.push_back(tag);
                }
            }
            i = j - 1;
        }
    }
    return out;
}

// ============================================================================
// 256-Bucket Rainbow Table Sniffer
// ============================================================================
class RainbowTable {
public:
    std::array<std::vector<ChannelCandidate>, 256> buckets;

    RainbowTable() {
        add_channel(PUBLIC_CHANNEL_NAME);
        // Also register SHA-256("#public") (Bucket 0x66) mapped to Public in case a station uses literal #public
        {
            auto full = Sha256::digest_str("#public");
            std::array<uint8_t, 16> pub_tag_psk{};
            std::memcpy(pub_tag_psk.data(), full.data(), 16);
            uint8_t ph = compute_channel_hash(pub_tag_psk);
            buckets[ph].push_back(ChannelCandidate{PUBLIC_CHANNEL_NAME, pub_tag_psk, ph});
        }

        static const char* BUILTIN_TAGS[] = {
            "#general", "#test", "#chat", "#mesh", "#weather", "#wx", "#bbs",
            "#hamradio", "#emcomm", "#ares", "#races", "#gmrs", "#repeater", "#skywarn",
            "#ft8", "#js8call", "#aprs", "#winlink", "#auxcomm", "#shares", "#telemetry",
            "#meshcore", "#lora", "#heltec", "#t1000e", "#sensecap", "#emergency", "#net",
            "#traffic", "#dx", "#vhf", "#uhf", "#hf", "#cw", "#ssb", "#digital", "#packet",
            "#offgrid", "#offgrid_ops", "#prepper", "#sar", "#cert", "#fema", "#redcross",
            "#comms", "#ops", "#hq", "#admin", "#dev", "#rust", "#cpp", "#linux", "#help",
            "#info", "#news", "#alerts", "#fire", "#smoke", "#road", "#mountain", "#summit",
            "#sota", "#pota", "#iota", "#bot", "#ping", "#echo", "#beacon", "#relay",
            "#overland", "#camping", "#hiking", "#backcountry", "#trail", "#4x4", "#rv",
            "#solar", "#battery", "#power", "#antenna", "#rf", "#scanner", "#sdr",
            "#frs", "#murs", "#cb", "#simplex", "#duplex", "#crossband", "#gateway",
            "#meshtastic", "#reticulum", "#atak", "#tak", "#cot", "#gps", "#tracker",
            "#room", "#lounge", "#random", "#tech", "#hardware", "#firmware", "#nodes",
            "#status", "#log", "#debug", "#alpha", "#beta", "#field", "#base", "#mobile",
            "#portable", "#rover", "#maritime", "#marine", "#aviation", "#wxnet",
            "#seattle", "#portland", "#spokane", "#boise", "#saltlake", "#denver",
            "#phoenix", "#lasvegas", "#reno", "#losangeles", "#sandiego", "#sanfrancisco",
            "#sacramento", "#austin", "#dallas", "#houston", "#sanantonio", "#chicago",
            "#detroit", "#minneapolis", "#stlouis", "#kansascity", "#omaha", "#milwaukee",
            "#atlanta", "#miami", "#orlando", "#tampa", "#charlotte", "#raleigh", "#nashville",
            "#newyork", "#boston", "#philadelphia", "#pittsburgh", "#washingtondc", "#baltimore",
            "#london", "#toronto", "#vancouver", "#montreal", "#calgary", "#sydney", "#melbourne",
            "#prep", "#Prep", "#preppers", "#survival", "#local", "#regional",
            "#tac", "#tac1", "#tac2", "#y2k", "#Y2K", "#SAR", "#Ops", "#ADMIN",
            "#pnw", "#southwest", "#midwest", "#northeast", "#southeast", "#rockies",
            "#alabama", "#alaska", "#arizona", "#arkansas", "#california", "#colorado",
            "#connecticut", "#delaware", "#florida", "#georgia", "#hawaii", "#illinois",
            "#indiana", "#iowa", "#kansas", "#kentucky", "#louisiana", "#maine", "#maryland",
            "#massachusetts", "#michigan", "#minnesota", "#mississippi", "#missouri",
            "#montana", "#nebraska", "#nevada", "#newhampshire", "#newjersey", "#newmexico",
            "#newyork", "#northcarolina", "#northdakota", "#ohio", "#oklahoma", "#oregon",
            "#pennsylvania", "#rhodeisland", "#southcarolina", "#southdakota", "#tennessee",
            "#texas", "#vermont", "#virginia", "#washington", "#westvirginia", "#wisconsin",
            "#wyoming",
            "#al", "#ak", "#az", "#ar", "#ca", "#co", "#ct", "#de", "#fl", "#ga",
            "#hi", "#id", "#il", "#in", "#ia", "#ks", "#ky", "#la", "#me", "#md",
            "#ma", "#mi", "#mn", "#ms", "#mo", "#mt", "#ne", "#nv", "#nh", "#nj",
            "#nm", "#ny", "#nc", "#nd", "#oh", "#ok", "#or", "#pa", "#ri", "#sc",
            "#sd", "#tn", "#tx", "#ut", "#vt", "#va", "#wa", "#wv", "#wi", "#wy"
        };

        for (const char* tag : BUILTIN_TAGS) {
            add_channel(tag);
        }
        for (int i = 0; i <= 99; ++i) {
            add_channel("#ch" + std::to_string(i));
        }
        load_wordlist("wordlist.txt");
    }

    ChannelCandidate add_channel(const std::string& raw_name) {
        std::string norm = normalize_channel_name(raw_name);
        if (norm == PUBLIC_CHANNEL_NAME) {
            auto psk = PUBLIC_CHANNEL_PSK;
            uint8_t h = compute_channel_hash(psk);
            ChannelCandidate cand{PUBLIC_CHANNEL_NAME, psk, h};
            if (known_names_.insert(PUBLIC_CHANNEL_NAME).second) {
                buckets[h].push_back(cand);
            }
            return cand;
        }
        auto norm_full = Sha256::digest_str(norm);
        std::array<uint8_t, 16> norm_psk{};
        std::memcpy(norm_psk.data(), norm_full.data(), 16);
        uint8_t h = compute_channel_hash(norm_psk);
        ChannelCandidate cand{norm, norm_psk, h};
        if (known_names_.insert(norm).second) {
            buckets[h].push_back(cand);
        }
        // If raw_name had uppercase characters (e.g. "#Y2K"), also register exact-case SHA-256 key
        std::string trimmed = trim_str(raw_name);
        if (!trimmed.empty()) {
            std::string exact = (trimmed[0] == '#') ? trimmed : ("#" + trimmed);
            if (exact != norm) {
                auto exact_full = Sha256::digest_str(exact);
                std::array<uint8_t, 16> exact_psk{};
                std::memcpy(exact_psk.data(), exact_full.data(), 16);
                uint8_t eh = compute_channel_hash(exact_psk);
                ChannelCandidate exact_cand{exact, exact_psk, eh};
                if (known_names_.insert(exact).second) {
                    buckets[eh].push_back(exact_cand);
                }
                return exact_cand;
            }
        }
        return cand;
    }

    ChannelCandidate add_channel_with_psk(const std::string& raw_name, const std::array<uint8_t, 16>& psk) {
        std::string trimmed = trim_str(raw_name);
        if (trimmed.empty()) trimmed = "#custom";
        if (normalize_channel_name(trimmed) == PUBLIC_CHANNEL_NAME) {
            trimmed = PUBLIC_CHANNEL_NAME;
        }
        uint8_t h = compute_channel_hash(psk);
        ChannelCandidate cand{trimmed, psk, h};
        if (known_names_.insert(trimmed).second) {
            buckets[h].push_back(cand);
        } else {
            for (auto& existing : buckets[h]) {
                if (existing.name == trimmed && existing.psk == psk) {
                    return existing;
                }
            }
            buckets[h].push_back(cand);
        }
        return cand;
    }

    bool contains(const std::string& raw_name) const {
        return known_names_.count(normalize_channel_name(raw_name)) > 0 ||
               known_names_.count(trim_str(raw_name)) > 0;
    }

    size_t total_candidates() const {
        return known_names_.size();
    }

    std::vector<ChannelCandidate> all_candidates() const {
        std::vector<ChannelCandidate> list;
        list.reserve(known_names_.size());
        std::unordered_set<std::string> seen;
        for (const auto& b : buckets) {
            for (const auto& c : b) {
                if (seen.insert(c.name).second) {
                    list.push_back(c);
                }
            }
        }
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
            return a.name < b.name;
        });
        return list;
    }

    void load_wordlist(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            std::string t = trim_str(line);
            if (t.empty() || t[0] == ';') continue;
            add_channel(t);
        }
    }

    ChannelCandidate add_and_persist_word(const std::string& raw_name, const std::string& path = "wordlist.txt") {
        std::string norm = normalize_channel_name(raw_name);
        bool is_new = !contains(norm);
        auto cand = add_channel(raw_name);
        if (is_new) {
            std::ofstream f(path, std::ios::app);
            if (f.is_open()) {
                f << cand.name << "\n";
            }
        }
        return cand;
    }

    std::optional<SniffedGroupMessage> sniff_frame(const std::vector<uint8_t>& frame) const {
        if (frame.size() < 17) return std::nullopt;
        if (auto res = sniff_slice(frame.data(), frame.size())) {
            return res;
        }
        // Fallback: if path length header was non-standard, check any 3 + 16*N suffix of frame
        for (size_t off = 1; off + 19 <= frame.size(); ++off) {
            size_t rem = frame.size() - off;
            if ((rem - 3) % 16 == 0) {
                if (auto res = sniff_slice(frame.data() + off, rem)) {
                    return res;
                }
            }
        }
        return std::nullopt;
    }

    std::optional<SniffedGroupMessage> sniff_slice(const uint8_t* ptr, size_t sz) const {
        if (sz < 17) return std::nullopt;
        uint8_t header_hash = ptr[0];
        const auto& bucket = buckets[header_hash];
        if (bucket.empty()) return std::nullopt;

        struct Slice { const uint8_t* ptr; size_t len; };
        std::vector<Slice> slices;
        if (sz >= 19 && (sz - 3) % 16 == 0) {
            slices.push_back({ptr + 3, sz - 3});
        }
        if ((sz - 1) % 16 == 0) {
            slices.push_back({ptr + 1, sz - 1});
        }

        for (const auto& cand : bucket) {
            for (const auto& s : slices) {
                auto dec = decrypt_aes128_blocks(cand.psk, s.ptr, s.len);
                if (!dec) continue;
                std::string sender, text;
                uint32_t ts = 0;
                if (parse_decrypted_plaintext(*dec, sender, text, ts)) {
                    return SniffedGroupMessage{
                        cand.name,
                        cand.channel_hash,
                        cand.psk,
                        sender,
                        text,
                        ts
                    };
                }
            }
        }
        return std::nullopt;
    }

private:
    std::unordered_set<std::string> known_names_;

    static bool is_valid_utf8_slice(const uint8_t* s, size_t n) {
        size_t i = 0;
        while (i < n) {
            uint8_t c = s[i];
            if (c < 0x80) {
                if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return false;
                if (c == 0x7F) return false;
                ++i;
            } else if ((c & 0xE0) == 0xC0) {
                if (i + 1 >= n || (s[i + 1] & 0xC0) != 0x80 || c < 0xC2) return false;
                i += 2;
            } else if ((c & 0xF0) == 0xE0) {
                if (i + 2 >= n || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80) return false;
                i += 3;
            } else if ((c & 0xF8) == 0xF0) {
                if (i + 3 >= n || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80 || (s[i + 3] & 0xC0) != 0x80 || c > 0xF4) return false;
                i += 4;
            } else {
                return false;
            }
        }
        return true;
    }

    static bool try_parse_sender_message(const uint8_t* data,
                                         size_t max_len,
                                         std::string& sender,
                                         std::string& message,
                                         bool allow_trailing_pad = false) {
        // MeshCore C-strings end at the FIRST null byte; bytes after 0x00 (or in the last AES block) may be uninitialized
        size_t len = 0;
        while (len < max_len && data[len] != 0) {
            uint8_t b = data[len];
            if ((b < 0x20 && b != '\t' && b != '\n' && b != '\r') || b == 0x7F) {
                // If unzeroed padding starts in the final 16-byte AES block, stop at the padding boundary
                if (allow_trailing_pad && len >= 4 && len + 16 >= max_len) {
                    break;
                }
                return false;
            }
            ++len;
        }
        // Trim any incomplete trailing UTF-8 continuation bytes at the block boundary
        if (allow_trailing_pad && len == max_len) {
            size_t back = 0;
            while (back < 3 && back < len && (data[len - 1 - back] & 0xC0) == 0x80) {
                ++back;
            }
            if (back < len && data[len - 1 - back] >= 0xC0) {
                uint8_t lead = data[len - 1 - back];
                size_t expected = ((lead & 0xE0) == 0xC0) ? 2 : ((lead & 0xF0) == 0xE0) ? 3 : 4;
                if (back + 1 < expected) {
                    len -= (back + 1);
                }
            }
        }
        if (len < 3) return false;
        if (!is_valid_utf8_slice(data, len)) return false;

        std::string s(reinterpret_cast<const char*>(data), len);
        size_t pos = s.find(": ");
        if (pos == std::string::npos || pos == 0 || pos > 64) return false;

        sender = trim_str(s.substr(0, pos));
        message = trim_str(s.substr(pos + 2));
        if (sender.empty() || message.empty()) return false;
        for (char c : sender) {
            if (static_cast<unsigned char>(c) < 0x20) return false;
        }
        return true;
    }

    static bool parse_decrypted_plaintext(const std::vector<uint8_t>& dec,
                                          std::string& sender,
                                          std::string& message,
                                          uint32_t& timestamp) {
        if (dec.size() > 6) {
            uint32_t ts = static_cast<uint32_t>(dec[0]) |
                          (static_cast<uint32_t>(dec[1]) << 8) |
                          (static_cast<uint32_t>(dec[2]) << 16) |
                          (static_cast<uint32_t>(dec[3]) << 24);
            bool plausible_ts = (ts >= 1577836800u && ts <= 2147483647u);
            uint8_t flags = dec[4];
            uint8_t txt_type_shifted = (flags >> 2) & 0x3F;
            uint8_t txt_type_low = flags & 0x0F;

            // Standard plain/CLI group text: [ts(4B), flags(1B), "Sender: message\0"]
            if ((txt_type_shifted <= 3 || txt_type_low <= 3) &&
                try_parse_sender_message(dec.data() + 5, dec.size() - 5, sender, message, plausible_ts)) {
                timestamp = ts;
                return true;
            }
            // Signed plain text (TXT_TYPE_SIGNED_PLAIN == 2): [ts(4B), flags(1B), pubkey_prefix(4B), "Sender: message\0"]
            if (dec.size() > 10 && (txt_type_shifted == 2 || txt_type_low == 2) &&
                try_parse_sender_message(dec.data() + 9, dec.size() - 9, sender, message, plausible_ts)) {
                timestamp = ts;
                return true;
            }
        }
        if (try_parse_sender_message(dec.data(), dec.size(), sender, message, false)) {
            timestamp = 0;
            return true;
        }
        return false;
    }
};

} // namespace meshirc

