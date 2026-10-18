#include "crypto.hpp"
#include "protocol.hpp"
#include "image_encoder.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace meshirc;

    // 1. Verify Public PSK and #emcomm SHA-256 PSK derivation
    auto pub_psk = derive_psk("Public");
    assert(psk_to_hex(pub_psk) == PUBLIC_CHANNEL_PSK_HEX);

    auto emcomm_psk = derive_psk("#emcomm");
    assert(psk_to_hex(emcomm_psk) == "a9a49340642dfc4c562d7849b7c8a258");
    assert(compute_channel_hash(emcomm_psk) == 0x59);

    auto idaho_psk = derive_psk("#idaho");
    assert(psk_to_hex(idaho_psk) == "cf96bdc6a872fdc731258edd18e5d028");
    assert(compute_channel_hash(idaho_psk) == 0xD6);

    // 2. Verify 256-Bucket Rainbow Table & AES-128 Round-Trip Decryption
    RainbowTable rainbow;
    assert(rainbow.total_candidates() >= 300);

    auto hw_frame = build_meshcore_hw_group_frame(
        "#emcomm", "N0CALL", "Emergency net active on #meshcore and #ares", 1729000000);
    auto sniffed = rainbow.sniff_frame(hw_frame);
    assert(sniffed.has_value());
    assert(sniffed->channel_name == "#emcomm");
    assert(sniffed->sender == "N0CALL");
    assert(sniffed->text == "Emergency net active on #meshcore and #ares");
    assert(sniffed->timestamp == 1729000000);
    assert(!sniffed->is_mono_img);

    // 3. Verify 0x3E FrameParser + PUSH_RAW_DATA + Hashtag Extractor
    std::vector<uint8_t> push_p = {PUSH_RAW_DATA, 34, static_cast<uint8_t>(static_cast<int8_t>(-76)), 1};
    push_p.insert(push_p.end(), hw_frame.begin(), hw_frame.end());

    std::vector<uint8_t> wire = {FRAME_INBOUND_MAGIC, static_cast<uint8_t>(push_p.size() & 0xFF), 0};
    wire.insert(wire.end(), push_p.begin(), push_p.end());

    FrameParser parser;
    auto frames = parser.feed(wire.data(), wire.size());
    assert(frames.size() == 1);

    auto pkt = parse_companion_payload(frames[0]);
    assert(pkt.has_value());
    assert(pkt->kind == RadioPacketKind::RawSniffedGroupFrame);
    assert(pkt->rssi == -76);

    auto tags = extract_hashtags(sniffed->text);
    assert(tags.size() == 2);
    assert(tags[0] == "#meshcore");
    assert(tags[1] == "#ares");

    // 4. Verify 1-Bit Tactical Bitmap Bit-Packing (MSB-first, 1440 pixels <-> 180 bytes)
    uint8_t raw_pixels[MONO_IMG_PIXELS] = {};
    raw_pixels[0] = 1;                    // Pixel (0,0) -> Bit 7 of Byte 0 (0x80)
    raw_pixels[7] = 1;                    // Pixel (7,0) -> Bit 0 of Byte 0 (0x01)
    raw_pixels[MONO_IMG_PIXELS - 8] = 1;  // Pixel (40,29) -> Bit 7 of Byte 179 (0x80)
    raw_pixels[MONO_IMG_PIXELS - 1] = 1;  // Pixel (47,29) -> Bit 0 of Byte 179 (0x01)

    std::array<uint8_t, MONO_IMG_BYTES> packed_bits{};
    pack_mono_bitmap(raw_pixels, packed_bits);
    assert(packed_bits.size() == 180);
    assert(packed_bits[0] == 0x81);
    assert(packed_bits[179] == 0x81);

    uint8_t unpacked_pixels[MONO_IMG_PIXELS] = {};
    unpack_mono_bitmap(packed_bits, unpacked_pixels);
    for (size_t i = 0; i < MONO_IMG_PIXELS; ++i) {
        assert(unpacked_pixels[i] == raw_pixels[i]);
    }

    // 5. Verify 1.6:1 Center Crop + 48x30 Downscale + Contrast Stretch + Floyd-Steinberg Dithering
    std::vector<uint8_t> rgba(160 * 160 * 4, 0);
    for (size_t y = 0; y < 160; ++y) {
        for (size_t x = 0; x < 160; ++x) {
            uint8_t val = static_cast<uint8_t>((x * 255) / 159);
            size_t idx = (y * 160 + x) * 4;
            rgba[idx + 0] = val;
            rgba[idx + 1] = val;
            rgba[idx + 2] = val;
            rgba[idx + 3] = 255;
        }
    }
    std::array<uint8_t, MONO_IMG_BYTES> enc_bytes{};
    bool enc_ok = encode_rgba_to_mono_bitmap(rgba.data(), 160, 160, 160 * 4, false, enc_bytes);
    assert(enc_ok);
    uint8_t enc_pixels[MONO_IMG_PIXELS] = {};
    unpack_mono_bitmap(enc_bytes, enc_pixels);
    // Left column after 5th-95th percentile stretch & Floyd-Steinberg dithering should be 0 (black), right column 1 (white)
    assert(enc_pixels[0] == 0);
    assert(enc_pixels[MONO_IMG_W - 1] == 1);

    // 6. Verify 181-byte Single-Packet Framing (Opcode 0x49 + 180B) & Group Encryption/Decryption Round-Trip
    auto rf_frame = build_mono_img_rf_payload(enc_bytes);
    assert(rf_frame.size() == 181);
    assert(rf_frame[0] == PAYLOAD_TYPE_MONO_IMG);

    auto direct_pkt = parse_companion_payload(std::vector<uint8_t>(rf_frame.begin(), rf_frame.end()));
    assert(direct_pkt.has_value());
    assert(direct_pkt->kind == RadioPacketKind::ChannelMsgRecv);
    assert(direct_pkt->is_mono_img);
    assert(direct_pkt->mono_img_bytes == enc_bytes);

    auto enc_img_frame = build_meshcore_hw_group_mono_img_frame("Public", "K7MESH", enc_bytes, 1729000100);
    auto sniffed_img = rainbow.sniff_frame(enc_img_frame);
    assert(sniffed_img.has_value());
    assert(sniffed_img->channel_name == "Public");
    assert(sniffed_img->sender == "K7MESH");
    assert(sniffed_img->is_mono_img);
    assert(sniffed_img->mono_img_bytes == enc_bytes);

    // Verify graceful fallback on truncated 0x49 packet
    std::vector<uint8_t> truncated_0x49 = {PAYLOAD_TYPE_MONO_IMG, 0xAA, 0x55};
    auto trunc_pkt = parse_companion_payload(truncated_0x49);
    assert(trunc_pkt.has_value());
    assert(trunc_pkt->kind == RadioPacketKind::Unknown);

    std::cout << "[C++20 PASS] All SHA-256, AES-128, 256-Bucket Rainbow Sniffer, Companion Protocol, and 1-Bit Tactical Bitmap (0x49) tests passed!\n";
    return 0;
}


