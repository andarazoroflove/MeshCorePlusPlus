#include "crypto.hpp"
#include "protocol.hpp"
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

    std::cout << "[C++20 PASS] All SHA-256, AES-128, 256-Bucket Rainbow Sniffer, and Companion Protocol tests passed!\n";
    return 0;
}

