#include <jni.h>
#include <string>
#include <vector>
#include <array>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <ctime>
#include <algorithm>

#include "crypto.hpp"
#include "protocol.hpp"
#include "image_encoder.hpp"

using namespace meshirc;

namespace {

struct AndroidMeshEngine {
    std::mutex mtx;
    RainbowTable rainbow;
    HardwareSlotTable hw_slots;
    std::string callsign = "MobileOp";

    AndroidMeshEngine() {
        std::array<uint8_t, 16> psk{};
        std::string evicted;
        hw_slots.allocate_or_touch("#meshcore", psk, evicted);
        hw_slots.allocate_or_touch("#emcomm", psk, evicted);
        hw_slots.allocate_or_touch("#weather", psk, evicted);
        hw_slots.allocate_or_touch("#general", psk, evicted);
    }
};

static AndroidMeshEngine& engine() {
    static AndroidMeshEngine inst;
    return inst;
}

static uint32_t current_epoch_u32() {
    return static_cast<uint32_t>(std::time(nullptr));
}

static std::string jstr_to_utf8(JNIEnv* env, jstring jstr) {
    if (!jstr) return "";
    const char* chars = env->GetStringUTFChars(jstr, nullptr);
    if (!chars) return "";
    std::string out(chars);
    env->ReleaseStringUTFChars(jstr, chars);
    return out;
}

static jbyteArray vec_to_jbytes(JNIEnv* env, const std::vector<uint8_t>& v) {
    jbyteArray arr = env->NewByteArray(static_cast<jsize>(v.size()));
    if (arr && !v.empty()) {
        env->SetByteArrayRegion(arr, 0, static_cast<jsize>(v.size()),
                                reinterpret_cast<const jbyte*>(v.data()));
    }
    return arr;
}

static std::string clean_tab_field(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\t' || c == '\r' || c == '\n') out.push_back(' ');
        else out.push_back(c);
    }
    return out;
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_com_meshcore_plusplus_MeshCoreActivity_nativeInit(JNIEnv* env, jobject /*thiz*/, jstring jCallsign) {
    auto& eng = engine();
    std::lock_guard<std::mutex> lk(eng.mtx);
    std::string cs = jstr_to_utf8(env, jCallsign);
    if (!cs.empty()) eng.callsign = cs;
    return static_cast<jint>(eng.rainbow.total_candidates());
}

JNIEXPORT jstring JNICALL
Java_com_meshcore_plusplus_MeshCoreActivity_nativeAddRainbowChannel(JNIEnv* env, jobject /*thiz*/, jstring jChan) {
    auto& eng = engine();
    std::lock_guard<std::mutex> lk(eng.mtx);
    std::string name = jstr_to_utf8(env, jChan);
    auto cand = eng.rainbow.add_channel(name);
    std::ostringstream oss;
    oss << cand.name << "\t0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
        << static_cast<int>(cand.channel_hash) << "\t" << psk_to_hex(cand.psk);
    return env->NewStringUTF(oss.str().c_str());
}

JNIEXPORT jbyteArray JNICALL
Java_com_meshcore_plusplus_MeshCoreActivity_nativeBuildInitCommand(JNIEnv* env, jobject /*thiz*/, jint step) {
    std::vector<uint8_t> raw;
    switch (step) {
        case 0: raw = build_cmd_device_query(); break;
        case 1: raw = build_cmd_app_start("MeshCore++"); break;
        case 2: raw = build_cmd_get_bat(); break;
        case 3: raw = build_cmd_get_contacts(); break;
        case 4: raw = build_cmd_get_bat(); break;
        case 5: raw = build_cmd_sync_next_message(); break;
        case 6: raw = build_cmd_get_channel(0); break;
        default:
            if (step >= 10 && step < 18) {
                raw = build_cmd_get_channel(static_cast<uint8_t>(step - 10));
            }
            break;
    }
    return vec_to_jbytes(env, raw);
}

JNIEXPORT jbyteArray JNICALL
Java_com_meshcore_plusplus_MeshCoreActivity_nativeBuildSelfAdvert(JNIEnv* env, jobject /*thiz*/, jboolean flood) {
    auto raw = build_cmd_send_self_advert(flood != JNI_FALSE);
    return vec_to_jbytes(env, raw);
}

JNIEXPORT jbyteArray JNICALL
Java_com_meshcore_plusplus_MeshCoreActivity_nativeSyncChannelSlot(JNIEnv* env, jobject /*thiz*/, jstring jChan) {
    auto& eng = engine();
    std::lock_guard<std::mutex> lk(eng.mtx);
    std::string ch = jstr_to_utf8(env, jChan);
    eng.rainbow.add_channel(ch);
    std::array<uint8_t, 16> psk{};
    std::string evicted;
    bool needs_sync = false;
    uint8_t slot_idx = eng.hw_slots.allocate_or_touch(ch, psk, evicted, &needs_sync);
    if (normalize_channel_name(ch) == PUBLIC_CHANNEL_NAME) {
        return env->NewByteArray(0);
    }
    auto raw = build_cmd_set_channel(slot_idx, ch, psk);
    return vec_to_jbytes(env, raw);
}

JNIEXPORT jobjectArray JNICALL
Java_com_meshcore_plusplus_MeshCoreActivity_nativeBuildChannelTextFrames(
    JNIEnv* env, jobject /*thiz*/, jstring jChan, jstring jCallsign, jstring jText) {
    auto& eng = engine();
    std::lock_guard<std::mutex> lk(eng.mtx);
    std::string ch = jstr_to_utf8(env, jChan);
    std::string cs = jstr_to_utf8(env, jCallsign);
    std::string txt = jstr_to_utf8(env, jText);
    if (!cs.empty()) eng.callsign = cs;

    std::array<uint8_t, 16> psk{};
    std::string evicted;
    bool needs_sync = false;
    uint8_t slot_idx = eng.hw_slots.allocate_or_touch(ch, psk, evicted, &needs_sync);

    std::vector<std::vector<uint8_t>> frames;
    if (normalize_channel_name(ch) != PUBLIC_CHANNEL_NAME) {
        frames.push_back(build_cmd_set_channel(slot_idx, ch, psk));
    }
    frames.push_back(build_cmd_send_channel_txt_msg(slot_idx, current_epoch_u32(), txt));

    jclass byteArrCls = env->FindClass("[B");
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(frames.size()), byteArrCls, nullptr);
    for (size_t i = 0; i < frames.size(); ++i) {
        jbyteArray jb = vec_to_jbytes(env, frames[i]);
        env->SetObjectArrayElement(out, static_cast<jsize>(i), jb);
        env->DeleteLocalRef(jb);
    }
    return out;
}

// Encodes an ARGB_8888 pixel array into a 48x30 1-bit dithered bitmap (180 bytes) and returns
// a byte[][] where:
//   element[0] = 180-byte normalized 1-bit bitmap (for local 5x Nearest-Neighbor display)
//   element[1] = optional CMD_SET_CHANNEL (0x20) frame (or empty if Public slot 0)
//   element[2] = CMD_SEND_CHANNEL_TXT_MSG (0x03) single-packet 0x49 wire frame
JNIEXPORT jobjectArray JNICALL
Java_com_meshcore_plusplus_MeshCoreActivity_nativeEncodeAndBuildImageFrames(
    JNIEnv* env, jobject /*thiz*/, jintArray jArgbPixels, jint width, jint height, jstring jChan) {
    if (!jArgbPixels || width <= 0 || height <= 0) return nullptr;
    jsize len = env->GetArrayLength(jArgbPixels);
    if (len < width * height) return nullptr;

    std::vector<jint> argb(static_cast<size_t>(width) * static_cast<size_t>(height));
    env->GetIntArrayRegion(jArgbPixels, 0, width * height, argb.data());

    std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (size_t i = 0; i < argb.size(); ++i) {
        uint32_t px = static_cast<uint32_t>(argb[i]);
        rgba[i * 4 + 0] = static_cast<uint8_t>((px >> 16) & 0xFF); // R
        rgba[i * 4 + 1] = static_cast<uint8_t>((px >> 8) & 0xFF);  // G
        rgba[i * 4 + 2] = static_cast<uint8_t>(px & 0xFF);         // B
        rgba[i * 4 + 3] = static_cast<uint8_t>((px >> 24) & 0xFF); // A
    }

    std::array<uint8_t, MONO_IMG_BYTES> raw_bits{};
    encode_rgba_to_mono_bitmap(rgba.data(), width, height, width * 4, false, raw_bits);

    // Encode through single-packet 0x49 companion wire codec (Mode 1 2x2 sub-block / Mode 0 raw)
    std::string wire_str = encode_mono_img_companion_wire(raw_bits);
    std::array<uint8_t, MONO_IMG_BYTES> wire_bits = raw_bits;
    decode_mono_img_companion_wire(reinterpret_cast<const uint8_t*>(wire_str.data()), wire_str.size(), wire_bits);

    auto& eng = engine();
    std::lock_guard<std::mutex> lk(eng.mtx);
    std::string ch = jstr_to_utf8(env, jChan);
    std::array<uint8_t, 16> psk{};
    std::string evicted;
    bool needs_sync = false;
    uint8_t slot_idx = eng.hw_slots.allocate_or_touch(ch, psk, evicted, &needs_sync);

    std::vector<uint8_t> bmp_vec(wire_bits.begin(), wire_bits.end());
    std::vector<uint8_t> sync_frame;
    if (normalize_channel_name(ch) != PUBLIC_CHANNEL_NAME) {
        sync_frame = build_cmd_set_channel(slot_idx, ch, psk);
    }
    std::vector<uint8_t> img_frame = build_cmd_send_channel_mono_img(slot_idx, current_epoch_u32(), wire_bits);

    jclass byteArrCls = env->FindClass("[B");
    jobjectArray out = env->NewObjectArray(3, byteArrCls, nullptr);
    jbyteArray jb0 = vec_to_jbytes(env, bmp_vec);
    jbyteArray jb1 = vec_to_jbytes(env, sync_frame);
    jbyteArray jb2 = vec_to_jbytes(env, img_frame);
    env->SetObjectArrayElement(out, 0, jb0);
    env->SetObjectArrayElement(out, 1, jb1);
    env->SetObjectArrayElement(out, 2, jb2);
    env->DeleteLocalRef(jb0);
    env->DeleteLocalRef(jb1);
    env->DeleteLocalRef(jb2);
    return out;
}

JNIEXPORT jintArray JNICALL
Java_com_meshcore_plusplus_MeshCoreActivity_nativeUnpackMonoBitmapToArgb(
    JNIEnv* env, jobject /*thiz*/, jbyteArray jMono180, jboolean lightMode) {
    if (!jMono180 || env->GetArrayLength(jMono180) < static_cast<jsize>(MONO_IMG_BYTES)) {
        return nullptr;
    }
    std::array<uint8_t, MONO_IMG_BYTES> packed{};
    env->GetByteArrayRegion(jMono180, 0, static_cast<jsize>(MONO_IMG_BYTES),
                            reinterpret_cast<jbyte*>(packed.data()));

    uint32_t fg = (lightMode != JNI_FALSE) ? 0xFF0F172Au : 0xFF38BDF8u; // Tactical cyan on dark, slate on light
    uint32_t bg = (lightMode != JNI_FALSE) ? 0xFFF8FAFCu : 0xFF060A12u;
    uint8_t raw_px[MONO_IMG_PIXELS]{};
    unpack_mono_bitmap(packed, raw_px);
    std::array<uint32_t, MONO_IMG_PIXELS> pixels{};
    for (size_t i = 0; i < MONO_IMG_PIXELS; ++i) {
        pixels[i] = raw_px[i] ? fg : bg;
    }

    jintArray out = env->NewIntArray(static_cast<jsize>(MONO_IMG_PIXELS));
    if (out) {
        env->SetIntArrayRegion(out, 0, static_cast<jsize>(MONO_IMG_PIXELS),
                               reinterpret_cast<const jint*>(pixels.data()));
    }
    return out;
}

// Decodes an incoming BLE NUS companion frame and returns a tab-separated event string
JNIEXPORT jstring JNICALL
Java_com_meshcore_plusplus_MeshCoreActivity_nativeDecodeBleFrame(
    JNIEnv* env, jobject /*thiz*/, jbyteArray jRawFrame) {
    if (!jRawFrame) return env->NewStringUTF("");
    jsize len = env->GetArrayLength(jRawFrame);
    if (len <= 0) return env->NewStringUTF("");

    std::vector<uint8_t> raw(static_cast<size_t>(len));
    env->GetByteArrayRegion(jRawFrame, 0, len, reinterpret_cast<jbyte*>(raw.data()));

    // Strip optional 0x3E ('>') serial framing header if present
    if (raw.size() >= 4 && raw[0] == FRAME_INBOUND_MAGIC) {
        uint16_t flen = static_cast<uint16_t>(raw[1]) | (static_cast<uint16_t>(raw[2]) << 8);
        if (raw.size() >= 3u + flen) {
            raw.assign(raw.begin() + 3, raw.begin() + 3 + flen);
        }
    }

    auto opt_pkt = parse_companion_payload(raw);
    if (!opt_pkt) return env->NewStringUTF("");
    const DecodedRadioPacket& pkt = *opt_pkt;

    auto& eng = engine();
    std::lock_guard<std::mutex> lk(eng.mtx);

    std::ostringstream oss;
    switch (pkt.kind) {
        case RadioPacketKind::ChannelMsgRecv: {
            std::string ch = eng.hw_slots.channel_for_slot(pkt.slot_idx);
            std::string img_hex;
            if (pkt.is_mono_img) {
                img_hex = bytes_to_hex(pkt.mono_img_bytes.data(), MONO_IMG_BYTES);
            } else {
                for (const auto& tag : extract_hashtags(pkt.text)) {
                    eng.rainbow.add_channel(tag);
                }
            }
            oss << "CHAT\t" << clean_tab_field(ch) << "\t"
                << clean_tab_field(pkt.sender) << "\t"
                << clean_tab_field(pkt.text) << "\t"
                << pkt.rssi << "\t" << std::fixed << std::setprecision(1) << pkt.snr << "\t"
                << static_cast<int>(pkt.hops) << "\t" << img_hex;
            break;
        }
        case RadioPacketKind::RawSniffedGroupFrame: {
            if (auto sn = eng.rainbow.sniff_frame(pkt.encrypted_frame)) {
                std::string img_hex;
                if (sn->is_mono_img) {
                    img_hex = bytes_to_hex(sn->mono_img_bytes.data(), MONO_IMG_BYTES);
                } else {
                    for (const auto& tag : extract_hashtags(sn->text)) {
                        eng.rainbow.add_channel(tag);
                    }
                }
                oss << "CHAT\t" << clean_tab_field(sn->channel_name) << "\t"
                    << clean_tab_field(sn->sender) << "\t"
                    << clean_tab_field(sn->text) << "\t"
                    << pkt.rssi << "\t" << std::fixed << std::setprecision(1) << pkt.snr << "\t"
                    << static_cast<int>(pkt.hops) << "\t" << img_hex;
            }
            break;
        }
        case RadioPacketKind::DirectMsgRecv: {
            std::string khex = bytes_to_hex(pkt.pub_key.data(), 6);
            oss << "CHAT\t@" << khex << "\t"
                << khex << "\t"
                << clean_tab_field(pkt.text) << "\t"
                << pkt.rssi << "\t" << std::fixed << std::setprecision(1) << pkt.snr << "\t"
                << static_cast<int>(pkt.hops) << "\t";
            break;
        }
        case RadioPacketKind::LiveAdvert:
        case RadioPacketKind::ContactInfo: {
            std::string khex = bytes_to_hex(pkt.pub_key.data(), 6);
            std::string nm = pkt.node_name.empty() ? khex : pkt.node_name;
            oss << "NODE\t" << clean_tab_field(nm) << "\t" << khex << "\t"
                << static_cast<int>(pkt.contact_type) << "\t"
                << static_cast<int>(pkt.hops) << "\t"
                << pkt.rssi << "\t" << std::fixed << std::setprecision(1) << pkt.snr << "\t"
                << pkt.lat_e6 << "\t" << pkt.lon_e6;
            break;
        }
        case RadioPacketKind::SelfInfo: {
            oss << "SELF\t" << clean_tab_field(pkt.node_name) << "\t"
                << pkt.freq_khz << "\t" << pkt.bw_hz << "\t"
                << static_cast<int>(pkt.sf) << "\t" << static_cast<int>(pkt.cr) << "\t"
                << static_cast<int>(pkt.tx_power_dbm);
            break;
        }
        case RadioPacketKind::DeviceInfo: {
            oss << "DEVICE\t" << clean_tab_field(pkt.model) << "\t" << clean_tab_field(pkt.firmware);
            break;
        }
        case RadioPacketKind::Battery: {
            oss << "BATTERY\t" << pkt.battery_mv << "\t" << static_cast<int>(pkt.battery_pct);
            break;
        }
        case RadioPacketKind::ChannelInfo: {
            if (!pkt.channel_name.empty()) {
                eng.hw_slots.update_from_radio(pkt.slot_idx, pkt.channel_name, pkt.psk);
                if (pkt.channel_name[0] == '#') {
                    eng.rainbow.add_channel(pkt.channel_name);
                }
            }
            oss << "SLOT\t" << static_cast<int>(pkt.slot_idx) << "\t" << clean_tab_field(pkt.channel_name);
            break;
        }
        default:
            break;
    }
    return env->NewStringUTF(oss.str().c_str());
}

} // extern "C"

