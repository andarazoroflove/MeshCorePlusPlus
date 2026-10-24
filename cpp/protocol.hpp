#pragma once

#include "crypto.hpp"
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace meshirc {

constexpr uint8_t FRAME_OUTBOUND_MAGIC = 0x3C; // '<' (Host -> Radio)
constexpr uint8_t FRAME_INBOUND_MAGIC  = 0x3E; // '>' (Radio -> Host)
constexpr size_t  MAX_FRAME_PAYLOAD    = 512;

// Outbound Companion Commands (MeshCore Companion Protocol v3 / v1.17+)
constexpr uint8_t CMD_APP_START            = 0x01;
constexpr uint8_t CMD_SEND_TXT_MSG         = 0x02;
constexpr uint8_t CMD_SEND_CHANNEL_TXT_MSG = 0x03;
constexpr uint8_t CMD_GET_CONTACTS         = 0x04;
constexpr uint8_t CMD_SET_DEVICE_TIME      = 0x06;
constexpr uint8_t CMD_SEND_SELF_ADVERT     = 0x07;
constexpr uint8_t CMD_SYNC_NEXT_MESSAGE    = 0x0A;
constexpr uint8_t CMD_RESET_PATH           = 0x0D;
constexpr uint8_t CMD_GET_BAT              = 0x14;
constexpr uint8_t CMD_DEVICE_QUERY         = 0x16;
constexpr uint8_t CMD_SEND_LOGIN           = 0x1A;
constexpr uint8_t CMD_SEND_STATUS_REQ      = 0x1B;
constexpr uint8_t CMD_GET_CHANNEL          = 0x1F;
constexpr uint8_t CMD_SET_CHANNEL          = 0x20;
constexpr uint8_t CMD_SEND_TRACE_PATH      = 0x24;
constexpr uint8_t CMD_SEND_TELEMETRY_REQ   = 0x27;

constexpr uint8_t TXT_TYPE_PLAIN           = 0x00;
constexpr uint8_t TXT_TYPE_CLI_DATA        = 0x01;
constexpr uint8_t TXT_TYPE_SIGNED_PLAIN    = 0x02;

// Inbound Companion Responses & Push Codes
constexpr uint8_t RESP_CODE_OK               = 0x00;
constexpr uint8_t RESP_CODE_ERR              = 0x01;
constexpr uint8_t RESP_CONTACTS_START        = 0x02;
constexpr uint8_t RESP_CONTACT               = 0x03;
constexpr uint8_t RESP_END_OF_CONTACTS       = 0x04;
constexpr uint8_t RESP_SELF_INFO             = 0x05;
constexpr uint8_t RESP_SENT                  = 0x06;
constexpr uint8_t RESP_CONTACT_MSG_RECV      = 0x07;
constexpr uint8_t RESP_CHANNEL_MSG_RECV      = 0x08;
constexpr uint8_t RESP_NO_MORE_MESSAGES      = 0x0A;
constexpr uint8_t RESP_BATTERY               = 0x0C;
constexpr uint8_t RESP_DEVICE_INFO           = 0x0D;
constexpr uint8_t RESP_CONTACT_MSG_RECV_V3   = 0x10;
constexpr uint8_t RESP_CHANNEL_MSG_RECV_V3   = 0x11;
constexpr uint8_t RESP_CHANNEL_INFO          = 0x12;

constexpr uint8_t PUSH_ADVERT                = 0x80;
constexpr uint8_t PUSH_PATH_UPDATED          = 0x81;
constexpr uint8_t PUSH_SEND_CONFIRMED        = 0x82;
constexpr uint8_t PUSH_MSG_WAITING           = 0x83;
constexpr uint8_t PUSH_RAW_DATA              = 0x84;
constexpr uint8_t PUSH_LOGIN_SUCCESS         = 0x85;
constexpr uint8_t PUSH_LOGIN_FAIL            = 0x86;
constexpr uint8_t PUSH_STATUS_RESPONSE       = 0x87;
constexpr uint8_t PUSH_LOG_RX_DATA           = 0x88;
constexpr uint8_t PUSH_TRACE_DATA            = 0x89;
constexpr uint8_t PUSH_NEW_ADVERT            = 0x8A;
constexpr uint8_t PUSH_TELEMETRY_RESPONSE    = 0x8B;

constexpr size_t NUM_HW_CHANNEL_SLOTS = 8;

inline std::vector<uint8_t> encode_serial_frame(const std::vector<uint8_t>& payload) {
    uint16_t len = static_cast<uint16_t>(payload.size());
    std::vector<uint8_t> out;
    out.reserve(3 + payload.size());
    out.push_back(FRAME_OUTBOUND_MAGIC);
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

inline std::vector<uint8_t> build_cmd_app_start(const std::string& app_name = "MeshCore++") {
    // MeshCore v1.17+: [0x01, 0x03 (app_ver), 6 reserved spaces, app_name...]
    std::vector<uint8_t> p = {CMD_APP_START, 0x03, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20};
    p.insert(p.end(), app_name.begin(), app_name.end());
    return p;
}

inline std::vector<uint8_t> build_cmd_device_query() {
    return {CMD_DEVICE_QUERY, 0x03};
}

inline std::vector<uint8_t> build_cmd_get_bat() {
    return {CMD_GET_BAT};
}

inline std::vector<uint8_t> build_cmd_get_contacts() {
    return {CMD_GET_CONTACTS};
}

inline std::vector<uint8_t> build_cmd_sync_next_message() {
    return {CMD_SYNC_NEXT_MESSAGE};
}

inline std::vector<uint8_t> build_cmd_set_device_time(uint32_t epoch_secs) {
    return {
        CMD_SET_DEVICE_TIME,
        static_cast<uint8_t>(epoch_secs & 0xFF),
        static_cast<uint8_t>((epoch_secs >> 8) & 0xFF),
        static_cast<uint8_t>((epoch_secs >> 16) & 0xFF),
        static_cast<uint8_t>((epoch_secs >> 24) & 0xFF)
    };
}

inline std::vector<uint8_t> build_cmd_send_self_advert(bool flood = true) {
    return {CMD_SEND_SELF_ADVERT, static_cast<uint8_t>(flood ? 0x01 : 0x00)};
}

inline std::vector<uint8_t> build_cmd_get_channel(uint8_t slot_idx) {
    return {CMD_GET_CHANNEL, slot_idx};
}

inline std::vector<uint8_t> build_cmd_set_channel(uint8_t slot_idx,
                                                  const std::string& channel_name,
                                                  const std::array<uint8_t, 16>& psk) {
    std::vector<uint8_t> p(2 + 32 + 16, 0);
    p[0] = CMD_SET_CHANNEL;
    p[1] = slot_idx;
    size_t copy_len = std::min<size_t>(channel_name.size(), 31);
    std::memcpy(p.data() + 2, channel_name.data(), copy_len);
    std::memcpy(p.data() + 34, psk.data(), 16);
    return p;
}

inline std::vector<uint8_t> build_cmd_send_channel_txt_msg(uint8_t slot_idx,
                                                           uint32_t timestamp,
                                                           const std::string& text) {
    std::vector<uint8_t> p;
    p.reserve(7 + text.size());
    p.push_back(CMD_SEND_CHANNEL_TXT_MSG);
    p.push_back(0x00); // TXT_TYPE_PLAIN
    p.push_back(slot_idx);
    p.push_back(static_cast<uint8_t>(timestamp & 0xFF));
    p.push_back(static_cast<uint8_t>((timestamp >> 8) & 0xFF));
    p.push_back(static_cast<uint8_t>((timestamp >> 16) & 0xFF));
    p.push_back(static_cast<uint8_t>((timestamp >> 24) & 0xFF));
    p.insert(p.end(), text.begin(), text.end());
    return p;
}

// Builds the exact 181-byte single-packet RF payload: [Byte 0 = 0x49 ('I'), Bytes 1..180 = packed bitmap]
inline std::vector<uint8_t> build_raw_mono_img_packet(const std::array<uint8_t, MONO_IMG_BYTES>& bits) {
    std::vector<uint8_t> p(MONO_IMG_RF_BYTES);
    p[0] = PAYLOAD_TYPE_MONO_IMG;
    std::memcpy(p.data() + 1, bits.data(), MONO_IMG_BYTES);
    return p;
}

// Wraps the single-packet 0x49 ('I!') payload into a channel dispatch command preserving routing headers
inline std::vector<uint8_t> build_cmd_send_channel_mono_img(uint8_t slot_idx,
                                                            uint32_t timestamp,
                                                            const std::array<uint8_t, MONO_IMG_BYTES>& bits) {
    std::string wire = encode_mono_img_companion_wire(bits);
    std::vector<uint8_t> p;
    p.reserve(7 + wire.size());
    p.push_back(CMD_SEND_CHANNEL_TXT_MSG);
    p.push_back(TXT_TYPE_PLAIN); // 0x00 so stock MeshCore v1.17+ companion firmware accepts & broadcasts over RF
    p.push_back(slot_idx);
    p.push_back(static_cast<uint8_t>(timestamp & 0xFF));
    p.push_back(static_cast<uint8_t>((timestamp >> 8) & 0xFF));
    p.push_back(static_cast<uint8_t>((timestamp >> 16) & 0xFF));
    p.push_back(static_cast<uint8_t>((timestamp >> 24) & 0xFF));
    p.insert(p.end(), wire.begin(), wire.end()); // Byte 0 of RF payload = 0x49 ('I'), Byte 1 = '!'
    return p;
}

inline std::vector<uint8_t> build_cmd_send_txt_msg(const std::array<uint8_t, 6>& pubkey_prefix,
                                                   uint32_t timestamp,
                                                   const std::string& text,
                                                   uint8_t txt_type = TXT_TYPE_PLAIN,
                                                   uint8_t attempt = 0) {
    std::vector<uint8_t> p;
    p.reserve(13 + text.size());
    p.push_back(CMD_SEND_TXT_MSG);
    p.push_back(txt_type);
    p.push_back(attempt & 0x03);
    p.push_back(static_cast<uint8_t>(timestamp & 0xFF));
    p.push_back(static_cast<uint8_t>((timestamp >> 8) & 0xFF));
    p.push_back(static_cast<uint8_t>((timestamp >> 16) & 0xFF));
    p.push_back(static_cast<uint8_t>((timestamp >> 24) & 0xFF));
    p.insert(p.end(), pubkey_prefix.begin(), pubkey_prefix.end());
    p.insert(p.end(), text.begin(), text.end());
    return p;
}

inline std::vector<uint8_t> build_cmd_send_login(const std::array<uint8_t, 32>& pub_key,
                                                 const std::string& password) {
    std::vector<uint8_t> p;
    p.reserve(33 + password.size());
    p.push_back(CMD_SEND_LOGIN);
    p.insert(p.end(), pub_key.begin(), pub_key.end());
    size_t len = std::min<size_t>(password.size(), 15);
    p.insert(p.end(), password.begin(), password.begin() + len);
    return p;
}

inline std::vector<uint8_t> build_cmd_send_status_req(const std::array<uint8_t, 32>& pub_key) {
    std::vector<uint8_t> p;
    p.reserve(33);
    p.push_back(CMD_SEND_STATUS_REQ);
    p.insert(p.end(), pub_key.begin(), pub_key.end());
    return p;
}

inline std::vector<uint8_t> build_cmd_send_telemetry_req(const std::array<uint8_t, 32>& pub_key) {
    std::vector<uint8_t> p = {CMD_SEND_TELEMETRY_REQ, 0x00, 0x00, 0x00};
    p.insert(p.end(), pub_key.begin(), pub_key.end());
    return p;
}

inline std::vector<uint8_t> build_cmd_reset_path(const std::array<uint8_t, 32>& pub_key) {
    std::vector<uint8_t> p;
    p.reserve(33);
    p.push_back(CMD_RESET_PATH);
    p.insert(p.end(), pub_key.begin(), pub_key.end());
    return p;
}

inline std::vector<uint8_t> build_cmd_send_trace_path(uint32_t tag,
                                                      const std::vector<uint8_t>& path_hashes,
                                                      uint32_t auth_code = 0) {
    std::vector<uint8_t> p;
    p.reserve(10 + path_hashes.size());
    p.push_back(CMD_SEND_TRACE_PATH);
    p.push_back(static_cast<uint8_t>(tag & 0xFF));
    p.push_back(static_cast<uint8_t>((tag >> 8) & 0xFF));
    p.push_back(static_cast<uint8_t>((tag >> 16) & 0xFF));
    p.push_back(static_cast<uint8_t>((tag >> 24) & 0xFF));
    p.push_back(static_cast<uint8_t>(auth_code & 0xFF));
    p.push_back(static_cast<uint8_t>((auth_code >> 8) & 0xFF));
    p.push_back(static_cast<uint8_t>((auth_code >> 16) & 0xFF));
    p.push_back(static_cast<uint8_t>((auth_code >> 24) & 0xFF));
    p.push_back(0x00); // flags
    p.insert(p.end(), path_hashes.begin(), path_hashes.end());
    return p;
}

inline uint8_t battery_mv_to_percent(uint16_t mv) {
    if (mv <= 3200) return 0;
    if (mv >= 4200) return 100;
    return static_cast<uint8_t>(((mv - 3200) * 100) / 1000);
}

enum class RadioPacketKind {
    Ok,
    Error,
    SelfInfo,
    DeviceInfo,
    Battery,
    ChannelInfo,
    ContactsStart,
    ContactInfo,
    ContactsEnd,
    MsgWaiting,
    NoMoreMessages,
    ChannelMsgRecv,
    DirectMsgRecv,
    MsgSent,
    SendConfirmed,
    LoginResult,
    StatusResponse,
    TelemetryResponse,
    TraceData,
    RawSniffedGroupFrame,
    LiveAdvert,
    RawRfActivity,
    Unknown
};

struct DecodedRadioPacket {
    RadioPacketKind kind = RadioPacketKind::Unknown;
    uint8_t raw_code = 0;
    uint8_t error_code = 0;
    std::string node_name;
    std::string model;
    std::string firmware;
    std::string build_date;
    uint32_t freq_khz = 0;
    uint32_t bw_hz = 0;
    uint8_t sf = 0;
    uint8_t cr = 0;
    uint8_t tx_power_dbm = 0;
    uint16_t max_contacts = 0;
    uint8_t max_channels = 0;
    uint16_t battery_mv = 0;
    uint8_t battery_pct = 0;
    uint8_t slot_idx = 0;
    std::string channel_name;
    std::array<uint8_t, 16> psk{};
    std::array<uint8_t, 32> pub_key{};
    uint8_t contact_type = 0; // 1=Chat, 2=Repeater, 3=Room, 4=Sensor
    int8_t out_path_len = -1;
    int32_t lat_e6 = 0;
    int32_t lon_e6 = 0;
    uint32_t total_contacts = 0;
    uint8_t hops = 0;
    uint8_t hop_hash_size = 1;
    std::vector<uint8_t> hop_bytes;
    std::vector<float> trace_snrs; // per-hop SNR values for PUSH_TRACE_DATA
    uint8_t lora_payload_type = 0;
    size_t lora_raw_len = 0;
    float snr = 0.0f;
    int16_t rssi = -80;
    uint32_t timestamp = 0;
    uint8_t txt_type = 0;
    bool sent_flood = false;
    uint32_t ack_tag = 0;
    uint32_t round_trip_ms = 0;
    bool login_ok = false;
    uint8_t login_permissions = 0;
    std::string sender;
    std::string text;
    bool is_mono_img = false;
    std::array<uint8_t, MONO_IMG_BYTES> mono_img_bytes{};
    std::vector<uint8_t> encrypted_frame;
    std::vector<uint8_t> raw;      // full companion payload as received (for hex inspector)
    uint8_t route_type = 0xFF;     // LoRa route type for PUSH_LOG_RX_DATA (0xFF = n/a)
};

inline std::string read_cstr_slice(const uint8_t* bytes, size_t max_len) {
    size_t n = 0;
    while (n < max_len && bytes[n] != 0) ++n;
    return trim_str(std::string(reinterpret_cast<const char*>(bytes), n));
}

inline void split_sender_and_text(const std::string& raw, std::string& sender, std::string& text) {
    size_t pos = raw.find(": ");
    if (pos != std::string::npos && pos > 0 && pos <= 64) {
        sender = trim_str(raw.substr(0, pos));
        text = trim_str(raw.substr(pos + 2));
    } else {
        sender = "Station";
        text = trim_str(raw);
    }
}

// Extracts a 1-bit tactical bitmap (opcode 0x49) from a channel message body if present.
// Returns: 1 = extracted valid 180-byte mono image, -1 = recognized 0x49 opcode but truncated/corrupt (ignore), 0 = normal text
inline int try_extract_channel_mono_img(const uint8_t* data,
                                        size_t rem_len,
                                        uint8_t txt_type,
                                        std::string& out_sender,
                                        std::array<uint8_t, MONO_IMG_BYTES>& out_bits) {
    if (rem_len == 0) return (txt_type == PAYLOAD_TYPE_MONO_IMG) ? -1 : 0;

    // Case 1a: Direct companion-safe [0x49 ('I'), '!', 120_Base91_chars]
    if (rem_len >= 2 && data[0] == PAYLOAD_TYPE_MONO_IMG && data[1] == '!') {
        if (rem_len >= MONO_IMG_WIRE_LEN && decode_mono_img_companion_wire(data, rem_len, out_bits)) {
            out_sender = "Station";
            return 1;
        }
        return -1;
    }

    // Case 1b: Direct raw [0x49, 180_bytes]
    if (data[0] == PAYLOAD_TYPE_MONO_IMG && rem_len >= MONO_IMG_RF_BYTES) {
        out_sender = "Station";
        std::memcpy(out_bits.data(), data + 1, MONO_IMG_BYTES);
        return 1;
    }

    // Case 2: ["Sender: ", 0x49, ...] (also supports UTF-8 callsigns and optional 4B signed plain prefix)
    auto scan_with_offset = [&](size_t start_off) -> int {
        if (start_off + 4 >= rem_len) return 0;
        size_t max_scan = std::min<size_t>(rem_len - start_off, 70);
        size_t colon_pos = std::string::npos;
        for (size_t i = 0; i + 1 < max_scan; ++i) {
            uint8_t c = data[start_off + i];
            if (c == ':' && data[start_off + i + 1] == ' ') {
                colon_pos = i;
                break;
            }
            if (c < 0x20 || c == 0x7F) break;
        }
        if (colon_pos == std::string::npos || colon_pos == 0) return 0;

        size_t body_off = start_off + colon_pos + 2;
        if (body_off + 2 <= rem_len && data[body_off] == PAYLOAD_TYPE_MONO_IMG && data[body_off + 1] == '!') {
            if (body_off + MONO_IMG_WIRE_LEN <= rem_len &&
                decode_mono_img_companion_wire(data + body_off, rem_len - body_off, out_bits)) {
                out_sender = trim_str(std::string(reinterpret_cast<const char*>(data + start_off), colon_pos));
                if (out_sender.empty()) out_sender = "Station";
                return 1;
            }
            return -1;
        }
        if (body_off + MONO_IMG_RF_BYTES <= rem_len && data[body_off] == PAYLOAD_TYPE_MONO_IMG) {
            out_sender = trim_str(std::string(reinterpret_cast<const char*>(data + start_off), colon_pos));
            if (out_sender.empty()) out_sender = "Station";
            std::memcpy(out_bits.data(), data + body_off + 1, MONO_IMG_BYTES);
            return 1;
        }
        if (txt_type == PAYLOAD_TYPE_MONO_IMG && body_off + MONO_IMG_BYTES <= rem_len) {
            out_sender = trim_str(std::string(reinterpret_cast<const char*>(data + start_off), colon_pos));
            if (out_sender.empty()) out_sender = "Station";
            std::memcpy(out_bits.data(), data + body_off, MONO_IMG_BYTES);
            return 1;
        }
        return 0;
    };

    if (int rc = scan_with_offset(0); rc != 0) return rc;
    if (((txt_type & 0x0F) == TXT_TYPE_SIGNED_PLAIN || ((txt_type >> 2) & 0x0F) == TXT_TYPE_SIGNED_PLAIN)) {
        if (int rc = scan_with_offset(4); rc != 0) return rc;
    }

    // Case 3: txt_type == 0x49 and raw 180 bytes directly at data[0..179]
    if (txt_type == PAYLOAD_TYPE_MONO_IMG) {
        if (rem_len >= MONO_IMG_BYTES) {
            out_sender = "Station";
            std::memcpy(out_bits.data(), data, MONO_IMG_BYTES);
            return 1;
        }
        return -1; // Truncated 0x49 frame: gracefully ignore
    }

    return 0;
}

inline uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline int32_t read_i32_le(const uint8_t* p) {
    return static_cast<int32_t>(read_u32_le(p));
}

inline std::optional<DecodedRadioPacket> parse_companion_payload(const std::vector<uint8_t>& payload) {
    if (payload.empty()) return std::nullopt;
    DecodedRadioPacket pkt;
    uint8_t code = payload[0];
    pkt.raw_code = code;
    pkt.raw = payload;

    // Direct 181-byte single-packet PAYLOAD_TYPE_MONO_IMG (0x49) frame: [0x49, 180_bytes]
    if (code == PAYLOAD_TYPE_MONO_IMG) {
        if (payload.size() >= MONO_IMG_RF_BYTES) {
            pkt.kind = RadioPacketKind::ChannelMsgRecv;
            pkt.slot_idx = 0;
            pkt.txt_type = PAYLOAD_TYPE_MONO_IMG;
            pkt.sender = "Station";
            pkt.text = "[1-Bit Tactical Bitmap 48x30]";
            pkt.is_mono_img = true;
            pkt.snr = 8.0f;
            pkt.rssi = -78;
            std::memcpy(pkt.mono_img_bytes.data(), payload.data() + 1, MONO_IMG_BYTES);
            return pkt;
        }
        // Gracefully ignore truncated 0x49 frame without printing corrupted binary characters
        pkt.kind = RadioPacketKind::Unknown;
        return pkt;
    }

    switch (code) {
        case RESP_CODE_OK:
            pkt.kind = RadioPacketKind::Ok;
            return pkt;
        case RESP_CODE_ERR:
            pkt.kind = RadioPacketKind::Error;
            pkt.error_code = payload.size() > 1 ? payload[1] : 0xFF;
            return pkt;
        case RESP_SENT:
            if (payload.size() >= 10) {
                pkt.kind = RadioPacketKind::MsgSent;
                pkt.sent_flood = (payload[1] != 0);
                pkt.ack_tag = read_u32_le(payload.data() + 2);
                pkt.round_trip_ms = read_u32_le(payload.data() + 6);
                return pkt;
            }
            break;
        case PUSH_SEND_CONFIRMED:
            if (payload.size() >= 9) {
                pkt.kind = RadioPacketKind::SendConfirmed;
                pkt.ack_tag = read_u32_le(payload.data() + 1);
                pkt.round_trip_ms = read_u32_le(payload.data() + 5);
                return pkt;
            }
            break;
        case RESP_NO_MORE_MESSAGES:
            pkt.kind = RadioPacketKind::NoMoreMessages;
            return pkt;
        case PUSH_MSG_WAITING:
            pkt.kind = RadioPacketKind::MsgWaiting;
            return pkt;
        case RESP_BATTERY:
        case CMD_GET_BAT:
            if (payload.size() >= 3) {
                pkt.kind = RadioPacketKind::Battery;
                pkt.battery_mv = static_cast<uint16_t>(payload[1]) |
                                 (static_cast<uint16_t>(payload[2]) << 8);
                pkt.battery_pct = battery_mv_to_percent(pkt.battery_mv);
                return pkt;
            }
            break;
        case RESP_SELF_INFO:
            // MeshCore v1.17 RESP_CODE_SELF_INFO (0x05):
            // [0]=0x05, [1]=adv_type, [2]=tx_power, [3]=max_tx_power, [4..36]=pub_key(32B),
            // [36..40]=lat, [40..44]=lon, [44..48]=opts, [48..52]=freq_khz, [52..56]=bw_hz,
            // [56]=sf, [57]=cr, [58..]=node_name
            pkt.kind = RadioPacketKind::SelfInfo;
            if (payload.size() >= 58) {
                pkt.tx_power_dbm = payload[2];
                std::memcpy(pkt.pub_key.data(), payload.data() + 4, 32);
                pkt.lat_e6 = read_i32_le(payload.data() + 36);
                pkt.lon_e6 = read_i32_le(payload.data() + 40);
                pkt.freq_khz = read_u32_le(payload.data() + 48);
                pkt.bw_hz = read_u32_le(payload.data() + 52);
                pkt.sf = payload[56];
                pkt.cr = payload[57];
                if (payload.size() > 58) {
                    pkt.node_name = read_cstr_slice(payload.data() + 58, payload.size() - 58);
                }
            } else if (payload.size() > 1) {
                pkt.node_name = read_cstr_slice(payload.data() + 1, payload.size() - 1);
            }
            if (pkt.node_name.empty()) pkt.node_name = "MeshCore Node";
            return pkt;

        case RESP_DEVICE_INFO: {
            // MeshCore v1.17 RESP_CODE_DEVICE_INFO (0x0D):
            // [0]=0x0D, [1]=fw_ver_code, [2]=max_contacts/2, [3]=max_channels, [4..8]=ble_pin,
            // [8..20]=build_date(12B), [20..60]=board_model(40B), [60..80]=fw_version(20B)
            pkt.kind = RadioPacketKind::DeviceInfo;
            if (payload.size() >= 4) {
                pkt.max_contacts = static_cast<uint16_t>(payload[2]) * 2;
                pkt.max_channels = payload[3];
            }
            if (payload.size() >= 20) {
                pkt.build_date = read_cstr_slice(payload.data() + 8, std::min<size_t>(12, payload.size() - 8));
            }
            if (payload.size() >= 60) {
                pkt.model = read_cstr_slice(payload.data() + 20, 40);
                pkt.firmware = read_cstr_slice(payload.data() + 60, payload.size() - 60);
            } else if (payload.size() > 20) {
                pkt.model = read_cstr_slice(payload.data() + 20, payload.size() - 20);
                pkt.firmware = "v1.x";
            } else {
                pkt.model = "MeshCore Radio";
                pkt.firmware = "v1.x";
            }
            if (pkt.model.empty()) pkt.model = "MeshCore Radio";
            if (pkt.firmware.empty()) pkt.firmware = "v1.17";
            return pkt;
        }

        case RESP_CONTACTS_START:
            if (payload.size() >= 5) {
                pkt.kind = RadioPacketKind::ContactsStart;
                pkt.total_contacts = read_u32_le(payload.data() + 1);
                return pkt;
            }
            break;

        case RESP_CONTACT:
        case PUSH_ADVERT:
        case PUSH_NEW_ADVERT:
            // 148-byte Contact record:
            // [0]=code, [1..33]=pub_key(32B), [33]=type, [34]=flags, [35]=out_path_len(int8),
            // [36..100]=out_path(64B), [100..132]=adv_name(32B), [132..136]=last_advert_ts,
            // [136..140]=lat_e6, [140..144]=lon_e6, [144..148]=lastmod
            if (payload.size() >= 144) {
                pkt.kind = RadioPacketKind::ContactInfo;
                std::memcpy(pkt.pub_key.data(), payload.data() + 1, 32);
                pkt.contact_type = payload[33];
                pkt.out_path_len = static_cast<int8_t>(payload[35]);
                pkt.node_name = read_cstr_slice(payload.data() + 100, 32);
                pkt.timestamp = read_u32_le(payload.data() + 132);
                pkt.lat_e6 = read_i32_le(payload.data() + 136);
                pkt.lon_e6 = read_i32_le(payload.data() + 140);
                return pkt;
            }
            break;

        case RESP_END_OF_CONTACTS:
            pkt.kind = RadioPacketKind::ContactsEnd;
            return pkt;

        case RESP_CHANNEL_INFO:
            if (payload.size() >= 50) {
                pkt.kind = RadioPacketKind::ChannelInfo;
                pkt.slot_idx = payload[1];
                pkt.channel_name = read_cstr_slice(payload.data() + 2, 32);
                std::memcpy(pkt.psk.data(), payload.data() + 34, 16);
                return pkt;
            }
            break;

        case RESP_CHANNEL_MSG_RECV:
            if (payload.size() >= 8) {
                pkt.slot_idx = payload[1];
                pkt.hops = (payload[2] == 0xFF) ? 0 : (payload[2] & 0x3F);
                pkt.txt_type = payload[3];
                pkt.timestamp = read_u32_le(payload.data() + 4);
                pkt.snr = 7.0f;
                pkt.rssi = -80;
                int img_rc = try_extract_channel_mono_img(payload.data() + 8, payload.size() - 8,
                                                          pkt.txt_type, pkt.sender, pkt.mono_img_bytes);
                if (img_rc == 1) {
                    pkt.kind = RadioPacketKind::ChannelMsgRecv;
                    pkt.is_mono_img = true;
                    pkt.text = "[1-Bit Tactical Bitmap 48x30]";
                    return pkt;
                } else if (img_rc < 0) {
                    pkt.kind = RadioPacketKind::Unknown;
                    return pkt;
                }
                pkt.kind = RadioPacketKind::ChannelMsgRecv;
                std::string raw_txt = read_cstr_slice(payload.data() + 8, payload.size() - 8);
                if (((pkt.txt_type & 0x0F) == TXT_TYPE_SIGNED_PLAIN || ((pkt.txt_type >> 2) & 0x0F) == TXT_TYPE_SIGNED_PLAIN) &&
                    payload.size() > 12 && raw_txt.find(": ") == std::string::npos) {
                    std::string alt_txt = read_cstr_slice(payload.data() + 12, payload.size() - 12);
                    if (alt_txt.find(": ") != std::string::npos) raw_txt = alt_txt;
                }
                split_sender_and_text(raw_txt, pkt.sender, pkt.text);
                return pkt;
            }
            break;

        case RESP_CHANNEL_MSG_RECV_V3:
            if (payload.size() >= 11) {
                pkt.snr = static_cast<float>(static_cast<int8_t>(payload[1])) / 4.0f;
                pkt.rssi = (payload[2] != 0) ? static_cast<int16_t>(static_cast<int8_t>(payload[2])) : -82;
                pkt.slot_idx = payload[4];
                pkt.hops = (payload[5] == 0xFF) ? 0 : (payload[5] & 0x3F);
                pkt.txt_type = payload[6];
                pkt.timestamp = read_u32_le(payload.data() + 7);
                int img_rc = try_extract_channel_mono_img(payload.data() + 11, payload.size() - 11,
                                                          pkt.txt_type, pkt.sender, pkt.mono_img_bytes);
                if (img_rc == 1) {
                    pkt.kind = RadioPacketKind::ChannelMsgRecv;
                    pkt.is_mono_img = true;
                    pkt.text = "[1-Bit Tactical Bitmap 48x30]";
                    return pkt;
                } else if (img_rc < 0) {
                    pkt.kind = RadioPacketKind::Unknown;
                    return pkt;
                }
                pkt.kind = RadioPacketKind::ChannelMsgRecv;
                std::string raw_txt = read_cstr_slice(payload.data() + 11, payload.size() - 11);
                if (((pkt.txt_type & 0x0F) == TXT_TYPE_SIGNED_PLAIN || ((pkt.txt_type >> 2) & 0x0F) == TXT_TYPE_SIGNED_PLAIN) &&
                    payload.size() > 15 && raw_txt.find(": ") == std::string::npos) {
                    std::string alt_txt = read_cstr_slice(payload.data() + 15, payload.size() - 15);
                    if (alt_txt.find(": ") != std::string::npos) raw_txt = alt_txt;
                }
                split_sender_and_text(raw_txt, pkt.sender, pkt.text);
                return pkt;
            }
            break;

        case RESP_CONTACT_MSG_RECV:
            if (payload.size() >= 13) {
                pkt.kind = RadioPacketKind::DirectMsgRecv;
                std::memcpy(pkt.pub_key.data(), payload.data() + 1, 6);
                pkt.hops = (payload[7] == 0xFF) ? 0 : (payload[7] & 0x3F);
                pkt.txt_type = payload[8];
                pkt.timestamp = read_u32_le(payload.data() + 9);
                pkt.text = read_cstr_slice(payload.data() + 13, payload.size() - 13);
                return pkt;
            }
            break;

        case RESP_CONTACT_MSG_RECV_V3:
            if (payload.size() >= 16) {
                pkt.kind = RadioPacketKind::DirectMsgRecv;
                pkt.snr = static_cast<float>(static_cast<int8_t>(payload[1])) / 4.0f;
                pkt.rssi = (payload[2] != 0) ? static_cast<int16_t>(static_cast<int8_t>(payload[2])) : -78;
                std::memcpy(pkt.pub_key.data(), payload.data() + 4, 6);
                pkt.hops = (payload[10] == 0xFF) ? 0 : (payload[10] & 0x3F);
                pkt.txt_type = payload[11];
                pkt.timestamp = read_u32_le(payload.data() + 12);
                pkt.text = read_cstr_slice(payload.data() + 16, payload.size() - 16);
                return pkt;
            }
            break;

        case PUSH_LOGIN_SUCCESS:
            if (payload.size() >= 8) {
                pkt.kind = RadioPacketKind::LoginResult;
                pkt.login_ok = true;
                pkt.login_permissions = payload[1];
                std::memcpy(pkt.pub_key.data(), payload.data() + 2, 6);
                return pkt;
            }
            break;

        case PUSH_LOGIN_FAIL:
            pkt.kind = RadioPacketKind::LoginResult;
            pkt.login_ok = false;
            if (payload.size() >= 8) {
                std::memcpy(pkt.pub_key.data(), payload.data() + 2, 6);
            }
            return pkt;

        case PUSH_STATUS_RESPONSE:
            if (payload.size() >= 8) {
                pkt.kind = RadioPacketKind::StatusResponse;
                std::memcpy(pkt.pub_key.data(), payload.data() + 2, 6);
                pkt.encrypted_frame.assign(payload.begin() + 8, payload.end());
                return pkt;
            }
            break;

        case PUSH_TELEMETRY_RESPONSE:
            if (payload.size() >= 8) {
                pkt.kind = RadioPacketKind::TelemetryResponse;
                std::memcpy(pkt.pub_key.data(), payload.data() + 2, 6);
                pkt.encrypted_frame.assign(payload.begin() + 8, payload.end());
                return pkt;
            }
            break;

        case PUSH_TRACE_DATA:
            // [0]=0x89, [1]=reserved, [2]=path_len, [3]=flags, [4..8]=tag, [8..12]=auth,
            // [12..12+path_len]=path_hashes, [12+path_len..12+2*path_len+1]=path_snrs (int8 * 4)
            if (payload.size() >= 12) {
                pkt.kind = RadioPacketKind::TraceData;
                uint8_t plen = payload[2];
                pkt.hops = plen;
                pkt.ack_tag = read_u32_le(payload.data() + 4);
                size_t h_off = 12;
                if (h_off + plen <= payload.size()) {
                    pkt.hop_bytes.assign(payload.begin() + h_off, payload.begin() + h_off + plen);
                    size_t s_off = h_off + plen;
                    for (size_t i = s_off; i < payload.size(); ++i) {
                        float s = static_cast<float>(static_cast<int8_t>(payload[i])) / 4.0f;
                        pkt.trace_snrs.push_back(s);
                    }
                    if (!pkt.trace_snrs.empty()) {
                        pkt.snr = pkt.trace_snrs.back();
                    }
                }
                return pkt;
            }
            break;

        case PUSH_RAW_DATA:
            // Simulator or direct raw payload: [0x84, snr_x4, rssi, hops, encrypted_frame...]
            if (payload.size() >= 5) {
                pkt.kind = RadioPacketKind::RawSniffedGroupFrame;
                pkt.snr = static_cast<float>(static_cast<int8_t>(payload[1])) / 4.0f;
                pkt.rssi = static_cast<int16_t>(static_cast<int8_t>(payload[2]));
                pkt.hops = payload[3];
                pkt.encrypted_frame.assign(payload.begin() + 4, payload.end());
                return pkt;
            }
            break;

        case PUSH_LOG_RX_DATA: {
            // Real MeshCore Companion v1.17+ Over-The-Air LoRa frame:
            // [0]=0x88, [1]=snr_x4(int8), [2]=rssi(int8), [3..]=raw LoRa packet
            if (payload.size() < 6) break;
            pkt.snr = static_cast<float>(static_cast<int8_t>(payload[1])) / 4.0f;
            pkt.rssi = static_cast<int16_t>(static_cast<int8_t>(payload[2]));

            const uint8_t* raw = payload.data() + 3;
            size_t raw_len = payload.size() - 3;
            pkt.lora_raw_len = raw_len;

            // Direct 181-byte [0x49, 180_bytes] over-the-air frame
            if (raw_len == MONO_IMG_RF_BYTES && raw[0] == PAYLOAD_TYPE_MONO_IMG) {
                pkt.kind = RadioPacketKind::RawSniffedGroupFrame;
                pkt.lora_payload_type = PAYLOAD_TYPE_MONO_IMG;
                pkt.encrypted_frame.assign(raw, raw + raw_len);
                return pkt;
            }

            uint8_t header = raw[0];
            uint8_t route_type = header & 0x03;
            pkt.route_type = route_type;
            uint8_t payload_type = (header >> 2) & 0x0F;
            pkt.lora_payload_type = payload_type;

            size_t idx = 1;
            if (route_type == 0x00 || route_type == 0x03) {
                // ROUTE_TYPE_TRANSPORT_FLOOD or ROUTE_TYPE_TRANSPORT_DIRECT has 4-byte transport code
                if (raw_len < idx + 5) break;
                idx += 4;
            }
            if (idx >= raw_len) break;

            uint8_t path_byte = raw[idx++];
            uint8_t hash_size = static_cast<uint8_t>((path_byte >> 6) + 1);
            uint8_t hop_count = static_cast<uint8_t>(path_byte & 0x3F);
            size_t path_bytes_len = static_cast<size_t>(hash_size) * hop_count;

            if (hash_size == 4 || path_bytes_len > 64 || idx + path_bytes_len > raw_len) {
                // Fallback for legacy 1-byte path_len if needed
                if (path_byte <= 64 && idx + path_byte <= raw_len) {
                    hash_size = 1;
                    hop_count = path_byte;
                    path_bytes_len = hop_count;
                }
            }
            if (idx + path_bytes_len <= raw_len) {
                pkt.hops = hop_count;
                pkt.hop_hash_size = hash_size;
                pkt.hop_bytes.assign(raw + idx, raw + idx + path_bytes_len);
                idx += path_bytes_len;
            }

            if ((payload_type == 0x05 || payload_type == 0x06 || payload_type == 0x0F) && idx + 17 <= raw_len) {
                // PAYLOAD_TYPE_GRP_TXT (0x05) / GRP_DATA (0x06) / RAW_CUSTOM (0x0F)
                pkt.kind = RadioPacketKind::RawSniffedGroupFrame;
                pkt.encrypted_frame.assign(raw + idx, raw + raw_len);
                return pkt;
            } else if (idx + MONO_IMG_RF_BYTES <= raw_len && raw[idx] == PAYLOAD_TYPE_MONO_IMG) {
                pkt.kind = RadioPacketKind::RawSniffedGroupFrame;
                pkt.lora_payload_type = PAYLOAD_TYPE_MONO_IMG;
                pkt.encrypted_frame.assign(raw + idx, raw + idx + MONO_IMG_RF_BYTES);
                return pkt;
            } else if (payload_type == 0x04 && idx + 101 <= raw_len) {
                // PAYLOAD_TYPE_ADVERT (0x04): [pub_key(32B), ts(4B), sig(64B), app_data...]
                pkt.kind = RadioPacketKind::LiveAdvert;
                std::memcpy(pkt.pub_key.data(), raw + idx, 32);
                pkt.timestamp = read_u32_le(raw + idx + 32);
                size_t aidx = idx + 100;
                uint8_t flags = raw[aidx++];
                pkt.contact_type = flags & 0x0F;
                if ((flags & 0x10) && aidx + 8 <= raw_len) {
                    pkt.lat_e6 = read_i32_le(raw + aidx);
                    pkt.lon_e6 = read_i32_le(raw + aidx + 4);
                    aidx += 8;
                }
                if ((flags & 0x20) && aidx + 2 <= raw_len) aidx += 2;
                if ((flags & 0x40) && aidx + 2 <= raw_len) aidx += 2;
                if ((flags & 0x80) && aidx < raw_len) {
                    pkt.node_name = read_cstr_slice(raw + aidx, raw_len - aidx);
                }
                return pkt;
            } else {
                pkt.kind = RadioPacketKind::RawRfActivity;
                return pkt;
            }
        }

        default:
            break;
    }

    pkt.kind = RadioPacketKind::Unknown;
    return pkt;
}

// Stateful streaming frame parser for 0x3E ('>') + u16 LE length + payload
class FrameParser {
public:
    static bool is_valid_inbound_code(uint8_t c) noexcept {
        return c <= 0x1D || c == PAYLOAD_TYPE_MONO_IMG || (c >= 0x80 && c <= 0x92);
    }

    std::vector<std::vector<uint8_t>> feed(const uint8_t* data, size_t len) {
        auto now = std::chrono::steady_clock::now();
        if (!buffer_.empty() &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_feed_time_).count() > 250) {
            // At 115200 baud, a valid frame completes in <25ms; any partial residue after 250ms of silence is stale
            buffer_.clear();
        }
        last_feed_time_ = now;
        buffer_.insert(buffer_.end(), data, data + len);
        std::vector<std::vector<uint8_t>> frames;

        while (true) {
            auto it = std::find(buffer_.begin(), buffer_.end(), FRAME_INBOUND_MAGIC);
            if (it == buffer_.end()) {
                buffer_.clear();
                break;
            }
            if (it != buffer_.begin()) {
                buffer_.erase(buffer_.begin(), it);
            }
            if (buffer_.size() < 3) break;

            uint16_t frame_len = static_cast<uint16_t>(buffer_[1]) |
                                 (static_cast<uint16_t>(buffer_[2]) << 8);
            // Companion frames are 1..300 bytes (buffer_[2] <= 0x01)
            if (frame_len == 0 || frame_len > 300) {
                buffer_.erase(buffer_.begin());
                continue;
            }
            if (buffer_.size() >= 4 && !is_valid_inbound_code(buffer_[3])) {
                buffer_.erase(buffer_.begin());
                continue;
            }
            if (buffer_.size() < 3 + frame_len) break;

            std::vector<uint8_t> payload(buffer_.begin() + 3, buffer_.begin() + 3 + frame_len);
            buffer_.erase(buffer_.begin(), buffer_.begin() + 3 + frame_len);
            frames.push_back(std::move(payload));
        }
        return frames;
    }

private:
    std::vector<uint8_t> buffer_;
    std::chrono::steady_clock::time_point last_feed_time_{std::chrono::steady_clock::now()};
};

// 8-Slot Hardware Channel LRU Manager
struct HardwareSlot {
    uint8_t slot_idx = 0;
    std::string channel_name; // empty if unassigned
    std::array<uint8_t, 16> psk{};
    uint8_t channel_hash = 0;
    uint64_t last_used_tick = 0;
    bool synced = false;
};

class HardwareSlotTable {
public:
    std::array<HardwareSlot, NUM_HW_CHANNEL_SLOTS> slots{};
    uint64_t tick_counter = 1;

    HardwareSlotTable() {
        slots[0].slot_idx = 0;
        slots[0].channel_name = PUBLIC_CHANNEL_NAME;
        slots[0].psk = PUBLIC_CHANNEL_PSK;
        slots[0].channel_hash = compute_channel_hash(PUBLIC_CHANNEL_PSK);
        slots[0].last_used_tick = UINT64_MAX;
        slots[0].synced = true;

        for (size_t i = 1; i < NUM_HW_CHANNEL_SLOTS; ++i) {
            slots[i].slot_idx = static_cast<uint8_t>(i);
            slots[i].channel_name.clear();
            slots[i].channel_hash = 0;
            slots[i].last_used_tick = 0;
            slots[i].synced = false;
        }
    }

    void mark_non_public_unsynced() {
        for (size_t i = 1; i < NUM_HW_CHANNEL_SLOTS; ++i) {
            slots[i].synced = false;
        }
    }

    std::optional<uint8_t> slot_for_channel(const std::string& raw_name) const {
        std::string trimmed = trim_str(raw_name);
        std::string norm = normalize_channel_name(raw_name);
        for (const auto& s : slots) {
            if (!s.channel_name.empty() &&
                (s.channel_name == trimmed || normalize_channel_name(s.channel_name) == norm)) {
                return s.slot_idx;
            }
        }
        return std::nullopt;
    }

    std::string channel_for_slot(uint8_t idx) const {
        if (idx < NUM_HW_CHANNEL_SLOTS && !slots[idx].channel_name.empty()) {
            return slots[idx].channel_name;
        }
        return (idx == 0) ? PUBLIC_CHANNEL_NAME : ("#slot" + std::to_string(idx));
    }

    // Allocates or updates LRU slot 1..7 (or returns 0 for Public).
    // Sets *out_needs_sync = true only when the hardware slot must be programmed via CMD_SET_CHANNEL.
    uint8_t allocate_or_touch(const std::string& raw_name,
                              std::array<uint8_t, 16>& out_psk,
                              std::string& out_evicted,
                              bool* out_needs_sync = nullptr) {
        ++tick_counter;
        out_evicted.clear();
        std::string norm = normalize_channel_name(raw_name);
        if (norm == PUBLIC_CHANNEL_NAME) {
            out_psk = slots[0].psk;
            if (out_needs_sync) *out_needs_sync = false;
            return 0;
        }
        std::string exact = trim_str(raw_name);
        if (!exact.empty() && exact[0] != '#') exact = "#" + exact;
        if (exact.empty()) exact = norm;
        out_psk = derive_psk(exact);
        uint8_t hash = compute_channel_hash(out_psk);

        for (size_t i = 1; i < NUM_HW_CHANNEL_SLOTS; ++i) {
            if (!slots[i].channel_name.empty() &&
                (slots[i].channel_name == exact || normalize_channel_name(slots[i].channel_name) == norm)) {
                bool already_synced = slots[i].synced && (slots[i].psk == out_psk);
                out_psk = slots[i].psk;
                slots[i].last_used_tick = tick_counter;
                slots[i].synced = true;
                if (out_needs_sync) *out_needs_sync = !already_synced;
                return static_cast<uint8_t>(i);
            }
        }

        size_t best_idx = 1;
        uint64_t best_key = UINT64_MAX;
        for (size_t i = 1; i < NUM_HW_CHANNEL_SLOTS; ++i) {
            uint64_t key = slots[i].channel_name.empty() ? 0 : slots[i].last_used_tick;
            if (key < best_key) {
                best_key = key;
                best_idx = i;
            }
        }

        out_evicted = slots[best_idx].channel_name;
        slots[best_idx].channel_name = exact;
        slots[best_idx].psk = out_psk;
        slots[best_idx].channel_hash = hash;
        slots[best_idx].last_used_tick = tick_counter;
        slots[best_idx].synced = true;
        if (out_needs_sync) *out_needs_sync = true;
        return static_cast<uint8_t>(best_idx);
    }

    void update_from_radio(uint8_t idx, const std::string& name, const std::array<uint8_t, 16>& psk) {
        if (idx >= NUM_HW_CHANNEL_SLOTS) return;
        ++tick_counter;
        std::string trimmed = trim_str(name);
        if (trimmed.empty()) {
            slots[idx].channel_name.clear();
            slots[idx].psk = {};
            slots[idx].channel_hash = 0;
            slots[idx].last_used_tick = 0;
            slots[idx].synced = false;
            return;
        }
        slots[idx].channel_name = (normalize_channel_name(trimmed) == PUBLIC_CHANNEL_NAME)
                                      ? PUBLIC_CHANNEL_NAME
                                      : trimmed;
        slots[idx].psk = psk;
        slots[idx].channel_hash = compute_channel_hash(psk);
        slots[idx].last_used_tick = (idx == 0) ? UINT64_MAX : tick_counter;
        slots[idx].synced = true;
    }
};

} // namespace meshirc
