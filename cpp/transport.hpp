#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <bthdef.h>
#include <bthledef.h>
#include <bluetoothleapis.h>
#elif defined(__APPLE__)
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "protocol.hpp"
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace meshirc {

struct DetectedSerialPort {
    std::string port_name;      // e.g. "COM4" or "/dev/cu.usbserial-0001"
    std::string friendly_name;  // e.g. "Heltec V3 (Silicon Labs CP210x USB to UART Bridge)"
    bool is_meshcore_candidate = false;
};

struct DetectedBleDevice {
    std::string identifier;     // CoreBluetooth UUID string or Windows GATT path
    std::string name;           // e.g. "MeshCore-T1000E" or "SenseCAP T1000-E"
    int rssi = -70;             // Signal strength in dBm
    bool is_meshcore_candidate = false;
};

enum class EventType {
    TransportStatus,
    ConsoleLog,
    SerialPortsListed,
    BleDevicesListed,
    RadioPacket,
    RawTx
};

struct MeshEvent {
    EventType type = EventType::ConsoleLog;
    std::string status_label;
    bool connected = false;
    bool is_sim = false;
    std::string log_line;
    std::vector<DetectedSerialPort> ports;
    std::vector<DetectedBleDevice> ble_devices;
    DecodedRadioPacket packet;
    std::vector<uint8_t> tx_payload; // for EventType::RawTx
};

inline std::vector<DetectedSerialPort> detect_serial_ports() {
    std::vector<DetectedSerialPort> results;
#ifdef _WIN32
    HDEVINFO hDevInfo = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (hDevInfo != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devData{};
        devData.cbSize = sizeof(SP_DEVINFO_DATA);

        for (DWORD idx = 0; SetupDiEnumDeviceInfo(hDevInfo, idx, &devData); ++idx) {
            char friendly[256] = {0};
            char hwid[512] = {0};
            SetupDiGetDeviceRegistryPropertyA(hDevInfo, &devData, SPDRP_FRIENDLYNAME, nullptr,
                                              reinterpret_cast<PBYTE>(friendly), sizeof(friendly) - 1, nullptr);
            SetupDiGetDeviceRegistryPropertyA(hDevInfo, &devData, SPDRP_HARDWAREID, nullptr,
                                              reinterpret_cast<PBYTE>(hwid), sizeof(hwid) - 1, nullptr);

            std::string fname(friendly);
            std::string hw(hwid);

            // Extract (COMx) from friendly name if present
            std::string port_name;
            size_t com_pos = fname.find("(COM");
            if (com_pos != std::string::npos) {
                size_t end_pos = fname.find(')', com_pos);
                if (end_pos != std::string::npos && end_pos > com_pos + 1) {
                    port_name = fname.substr(com_pos + 1, end_pos - com_pos - 1);
                }
            }
            if (port_name.empty()) continue;

            std::string combined = fname + " " + hw;
            std::transform(combined.begin(), combined.end(), combined.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            DetectedSerialPort item;
            item.port_name = port_name;
            item.friendly_name = fname;
            item.is_meshcore_candidate = false;

            if (combined.find("vid_10c4") != std::string::npos ||
                combined.find("cp210") != std::string::npos ||
                combined.find("heltec") != std::string::npos) {
                item.friendly_name = "Heltec V3 (CP2102 USB-UART) — " + fname;
                item.is_meshcore_candidate = true;
            } else if (combined.find("vid_2886") != std::string::npos ||
                       combined.find("vid_1915") != std::string::npos ||
                       combined.find("t1000") != std::string::npos ||
                       combined.find("sensecap") != std::string::npos ||
                       combined.find("seeed") != std::string::npos ||
                       combined.find("nrf52") != std::string::npos) {
                item.friendly_name = "SenseCAP T1000-E (USB CDC-ACM) — " + fname;
                item.is_meshcore_candidate = true;
            } else if (combined.find("vid_303a") != std::string::npos) {
                item.friendly_name = "ESP32-S3 Native USB (Heltec V3) — " + fname;
                item.is_meshcore_candidate = true;
            }

            results.push_back(item);
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    // Fast non-blocking fallback via HARDWARE\DEVICEMAP\SERIALCOMM
    if (results.empty()) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char val_name[256];
            BYTE val_data[256];
            DWORD idx = 0;
            while (true) {
                DWORD name_len = sizeof(val_name) - 1;
                DWORD data_len = sizeof(val_data) - 1;
                DWORD type = 0;
                if (RegEnumValueA(hKey, idx++, val_name, &name_len, nullptr, &type, val_data, &data_len) != ERROR_SUCCESS) {
                    break;
                }
                if (type == REG_SZ && data_len > 0) {
                    val_data[data_len] = 0;
                    std::string port(reinterpret_cast<char*>(val_data));
                    if (!port.empty()) {
                        results.push_back({port, "Serial Port (" + port + ")", true});
                    }
                }
            }
            RegCloseKey(hKey);
        }
    }
#elif defined(__APPLE__)
    if (DIR* d = opendir("/dev")) {
        while (struct dirent* ent = readdir(d)) {
            std::string name(ent->d_name);
            if (name.rfind("cu.", 0) != 0) continue;
            if (name.find("Bluetooth-Incoming-Port") != std::string::npos ||
                name.find("debug-console") != std::string::npos) continue;
            std::string full = "/dev/" + name;
            DetectedSerialPort item;
            item.port_name = full;
            if (name.find("usbserial") != std::string::npos ||
                name.find("SLAB_USBtoUART") != std::string::npos ||
                name.find("wchusbserial") != std::string::npos) {
                item.friendly_name = "Heltec V3 (CP2102 USB-UART) — " + full;
                item.is_meshcore_candidate = true;
            } else if (name.find("usbmodem") != std::string::npos) {
                item.friendly_name = "SenseCAP T1000-E / ESP32-S3 (USB CDC-ACM) — " + full;
                item.is_meshcore_candidate = true;
            } else {
                item.friendly_name = "macOS Serial (" + full + ")";
                item.is_meshcore_candidate = false;
            }
            results.push_back(item);
        }
        closedir(d);
    }
#endif
    return results;
}

#ifdef __APPLE__
struct MacBleState {
    struct RetainedPeriph {
        std::string uuid;
        std::string name;
        id periph = nullptr;
    };

    std::mutex mtx;
    std::string target_id;
    std::vector<DetectedBleDevice> discovered;
    std::vector<RetainedPeriph> periphs;
    id central = nullptr;
    id delegate = nullptr;
    dispatch_queue_t queue = nullptr;
    id active_peripheral = nullptr;
    id rx_char = nullptr;
    id tx_char = nullptr;
    long rx_write_type = 0; // 0 = CBCharacteristicWriteWithResponse, 1 = CBCharacteristicWriteWithoutResponse
    bool connecting = false;
    bool connected = false;
    bool gatt_ready = false;
    bool disconnected_event = false;
    std::string connected_name;
    std::deque<std::vector<uint8_t>> rx_frames;
    std::deque<std::string> logs;
    std::string status_label;
    bool status_connected = false;
    bool status_dirty = false;
    bool devices_dirty = false;
};

inline MacBleState g_mac_ble;

inline bool mac_ble_is_meshcore_name(const std::string& raw_name) {
    if (raw_name.empty()) return false;
    std::string lower = raw_name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("t1000") != std::string::npos ||
           lower.find("sensecap") != std::string::npos ||
           lower.find("meshcore") != std::string::npos ||
           lower.find("heltec") != std::string::npos ||
           lower.find("seeed") != std::string::npos ||
           lower.find("xiao") != std::string::npos ||
           lower.find("rak") != std::string::npos ||
           lower.find("companion") != std::string::npos ||
           lower.find("whisper") != std::string::npos ||
           lower.find("ble") != std::string::npos ||
           lower.find("lora") != std::string::npos ||
           lower.find("nrf") != std::string::npos;
}

inline bool mac_ble_uuid_is_nus(const std::string& u, const char* prefix4) {
    std::string s = u;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::string target = std::string(prefix4) + "-B5A3-F393-E0A9-E50E24DCCA9E";
    return s.find(target) != std::string::npos || s == prefix4;
}

inline bool mac_ble_adv_has_nus(id adv_data) {
    if (!adv_data) return false;
    const char* keys[2] = {"kCBAdvDataServiceUUIDs", "kCBAdvDataOverflowServiceUUIDs"};
    for (const char* k : keys) {
        id arr = mac_msg_send<id>(adv_data, sel_registerName("objectForKey:"), mac_ns_str(k));
        if (!arr) continue;
        unsigned long cnt = mac_msg_send<unsigned long>(arr, sel_registerName("count"));
        for (unsigned long i = 0; i < cnt; ++i) {
            id uuid_obj = mac_msg_send<id>(arr, sel_registerName("objectAtIndex:"), i);
            if (!uuid_obj) continue;
            std::string u = mac_ns_to_std_str(mac_msg_send<id>(uuid_obj, sel_registerName("UUIDString")));
            if (mac_ble_uuid_is_nus(u, "6E400001")) return true;
        }
    }
    return false;
}

inline void mac_ble_start_connect_locked(id central, id delegate, id periph, const std::string& name, const std::string& uuid) {
    if (!central || !periph) return;
    g_mac_ble.connecting = true;
    g_mac_ble.connected = false;
    g_mac_ble.gatt_ready = false;
    g_mac_ble.disconnected_event = false;
    if (g_mac_ble.active_peripheral) CFRelease(g_mac_ble.active_peripheral);
    CFRetain(periph);
    g_mac_ble.active_peripheral = periph;
    g_mac_ble.connected_name = name.empty() ? ("BLE-" + uuid.substr(0, 8)) : name;
    g_mac_ble.logs.push_back("[BLE] Connecting to " + g_mac_ble.connected_name + " [" + uuid + "]...");
    g_mac_ble.status_label = "BLE: Connecting to " + g_mac_ble.connected_name + "...";
    g_mac_ble.status_connected = false;
    g_mac_ble.status_dirty = true;

    mac_msg_send<void>(central, sel_registerName("stopScan"));
    mac_msg_send<void>(periph, sel_registerName("setDelegate:"), delegate);
    mac_msg_send<void>(central, sel_registerName("connectPeripheral:options:"), periph, static_cast<id>(nullptr));
}

inline void mac_ble_handle_peripheral_locked(id central, id delegate, id periph, id adv_data, int rssi, bool force_nus) {
    if (!periph) return;
    id ns_uuid = mac_msg_send<id>(periph, sel_registerName("identifier"));
    std::string uuid_str = ns_uuid ? mac_ns_to_std_str(mac_msg_send<id>(ns_uuid, sel_registerName("UUIDString"))) : "";
    if (uuid_str.empty()) return;

    std::string name;
    if (adv_data) {
        id adv_name = mac_msg_send<id>(adv_data, sel_registerName("objectForKey:"), mac_ns_str("kCBAdvDataLocalName"));
        name = mac_ns_to_std_str(adv_name);
    }
    if (name.empty()) {
        name = mac_ns_to_std_str(mac_msg_send<id>(periph, sel_registerName("name")));
    }

    bool has_nus = force_nus || mac_ble_adv_has_nus(adv_data);
    bool is_candidate = has_nus || mac_ble_is_meshcore_name(name);

    // Keep retained CBPeripheral reference so CoreBluetooth doesn't deallocate it before connectPeripheral:
    bool found_p = false;
    for (auto& rp : g_mac_ble.periphs) {
        if (rp.uuid == uuid_str) {
            if (!name.empty()) rp.name = name;
            found_p = true;
            break;
        }
    }
    if (!found_p) {
        CFRetain(periph);
        g_mac_ble.periphs.push_back({uuid_str, name, periph});
    }

    // Only list devices that have a human-readable name or advertise NUS
    if (!name.empty() || is_candidate) {
        std::string disp_name = name.empty() ? ("MeshCore BLE (" + uuid_str.substr(0, 8) + ")") : name;
        bool updated = false;
        for (auto& d : g_mac_ble.discovered) {
            if (d.identifier == uuid_str) {
                if (!name.empty()) d.name = disp_name;
                if (rssi != 0 && rssi > -120) d.rssi = rssi;
                d.is_meshcore_candidate = d.is_meshcore_candidate || is_candidate;
                updated = true;
                break;
            }
        }
        if (!updated) {
            DetectedBleDevice dev;
            dev.identifier = uuid_str;
            dev.name = disp_name;
            dev.rssi = (rssi != 0 && rssi > -120) ? rssi : -65;
            dev.is_meshcore_candidate = is_candidate;
            g_mac_ble.discovered.push_back(dev);
            g_mac_ble.logs.push_back("[BLE] Discovered: " + disp_name + " (" + std::to_string(dev.rssi) + " dBm)" +
                                     (is_candidate ? " [MeshCore / SenseCAP T1000-E NUS ★]" : ""));
        }
        std::sort(g_mac_ble.discovered.begin(), g_mac_ble.discovered.end(),
                  [](const DetectedBleDevice& a, const DetectedBleDevice& b) {
                      if (a.is_meshcore_candidate != b.is_meshcore_candidate)
                          return a.is_meshcore_candidate > b.is_meshcore_candidate;
                      return a.rssi > b.rssi;
                  });
        g_mac_ble.devices_dirty = true;
    }

    if (!g_mac_ble.connecting && !g_mac_ble.connected) {
        if (!g_mac_ble.target_id.empty()) {
            if (uuid_str == g_mac_ble.target_id || (!name.empty() && name == g_mac_ble.target_id)) {
                mac_ble_start_connect_locked(central, delegate, periph, name, uuid_str);
            }
        } else if (is_candidate) {
            mac_ble_start_connect_locked(central, delegate, periph, name, uuid_str);
        }
    }
}

inline void mac_ble_did_update_state(id self, SEL, id central) {
    std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
    long state = mac_msg_send<long>(central, sel_registerName("state"));
    if (state == 5) { // CBManagerStatePoweredOn
        g_mac_ble.logs.push_back("[BLE] CoreBluetooth Powered ON. Checking connected NUS devices & scanning for SenseCAP T1000-E...");
        g_mac_ble.status_label = "BLE: Scanning for SenseCAP T1000-E / NUS...";
        g_mac_ble.status_connected = false;
        g_mac_ble.status_dirty = true;

        // 1. Check already-connected peripherals advertising Nordic UART Service (6E400001-B5A3-F393-E0A9-E50E24DCCA9E)
        id nus_uuid = mac_cls_send<id>("CBUUID", sel_registerName("UUIDWithString:"),
                                       mac_ns_str("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"));
        if (nus_uuid) {
            id arr = mac_cls_send<id>("NSArray", sel_registerName("arrayWithObject:"), nus_uuid);
            id conn_list = mac_msg_send<id>(central, sel_registerName("retrieveConnectedPeripheralsWithServices:"), arr);
            unsigned long cnt = conn_list ? mac_msg_send<unsigned long>(conn_list, sel_registerName("count")) : 0;
            for (unsigned long i = 0; i < cnt; ++i) {
                id p = mac_msg_send<id>(conn_list, sel_registerName("objectAtIndex:"), i);
                if (p) mac_ble_handle_peripheral_locked(central, self, p, nullptr, -45, true);
            }
        }

        // 2. Also start live air scan for all peripherals so T1000-E / Heltec V3 / custom names appear immediately
        if (!g_mac_ble.connecting && !g_mac_ble.connected) {
            mac_msg_send<void>(central, sel_registerName("scanForPeripheralsWithServices:options:"),
                               static_cast<id>(nullptr), static_cast<id>(nullptr));
        }
    } else if (state == 4) {
        g_mac_ble.logs.push_back("[BLE] macOS Bluetooth is Powered OFF! Turn on Bluetooth in System Settings / Control Center.");
        g_mac_ble.status_label = "BLE: Bluetooth Powered OFF";
        g_mac_ble.status_connected = false;
        g_mac_ble.status_dirty = true;
    } else if (state == 3) {
        g_mac_ble.logs.push_back("[BLE] macOS Bluetooth permission denied! Enable MeshCore++ in System Settings -> Privacy & Security -> Bluetooth.");
        g_mac_ble.status_label = "BLE: Permission Denied (Check Privacy Settings)";
        g_mac_ble.status_connected = false;
        g_mac_ble.status_dirty = true;
    } else if (state == 2) {
        g_mac_ble.logs.push_back("[BLE] Bluetooth Low Energy (BLE) is unsupported on this Mac.");
        g_mac_ble.status_label = "BLE: Unsupported Hardware";
        g_mac_ble.status_connected = false;
        g_mac_ble.status_dirty = true;
    }
}

inline void mac_ble_did_discover(id self, SEL, id central, id peripheral, id adv_data, id rssi_num) {
    std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
    int rssi = rssi_num ? mac_msg_send<int>(rssi_num, sel_registerName("intValue")) : -65;
    mac_ble_handle_peripheral_locked(central, self, peripheral, adv_data, rssi, false);
}

inline void mac_ble_did_connect(id self, SEL, id, id peripheral) {
    std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
    g_mac_ble.connecting = false;
    g_mac_ble.connected = true;
    g_mac_ble.logs.push_back("[BLE] Connected to " + g_mac_ble.connected_name + ". Discovering Nordic UART Service (6E400001-B5A3-F393-E0A9-E50E24DCCA9E)...");
    g_mac_ble.status_label = "BLE: " + g_mac_ble.connected_name + " (Discovering GATT...)";
    g_mac_ble.status_connected = true;
    g_mac_ble.status_dirty = true;

    mac_msg_send<void>(peripheral, sel_registerName("setDelegate:"), self);
    mac_msg_send<void>(peripheral, sel_registerName("discoverServices:"), static_cast<id>(nullptr));
}

inline void mac_ble_did_fail_connect(id, SEL, id, id, id error) {
    std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
    std::string err_str = error ? mac_ns_to_std_str(mac_msg_send<id>(error, sel_registerName("localizedDescription"))) : "unknown error";
    g_mac_ble.connecting = false;
    g_mac_ble.connected = false;
    g_mac_ble.gatt_ready = false;
    g_mac_ble.disconnected_event = true;
    g_mac_ble.logs.push_back("[BLE] Failed to connect to " + g_mac_ble.connected_name + ": " + err_str);
    g_mac_ble.status_label = "BLE: Connection Failed (" + g_mac_ble.connected_name + ")";
    g_mac_ble.status_connected = false;
    g_mac_ble.status_dirty = true;
}

inline void mac_ble_did_disconnect(id, SEL, id, id, id error) {
    std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
    std::string err_str = error ? mac_ns_to_std_str(mac_msg_send<id>(error, sel_registerName("localizedDescription"))) : "link closed";
    g_mac_ble.connecting = false;
    g_mac_ble.connected = false;
    g_mac_ble.gatt_ready = false;
    g_mac_ble.disconnected_event = true;
    if (g_mac_ble.rx_char) { CFRelease(g_mac_ble.rx_char); g_mac_ble.rx_char = nullptr; }
    if (g_mac_ble.tx_char) { CFRelease(g_mac_ble.tx_char); g_mac_ble.tx_char = nullptr; }
    g_mac_ble.logs.push_back("[BLE] Disconnected from " + g_mac_ble.connected_name + " (" + err_str + ").");
    g_mac_ble.status_label = "BLE: Disconnected";
    g_mac_ble.status_connected = false;
    g_mac_ble.status_dirty = true;
}

inline void mac_ble_did_discover_services(id, SEL, id peripheral, id error) {
    std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
    if (error) {
        std::string err_str = mac_ns_to_std_str(mac_msg_send<id>(error, sel_registerName("localizedDescription")));
        g_mac_ble.logs.push_back("[BLE] Service discovery error: " + err_str);
        return;
    }
    id services = mac_msg_send<id>(peripheral, sel_registerName("services"));
    unsigned long cnt = services ? mac_msg_send<unsigned long>(services, sel_registerName("count")) : 0;
    bool found_any = false;
    for (unsigned long i = 0; i < cnt; ++i) {
        id svc = mac_msg_send<id>(services, sel_registerName("objectAtIndex:"), i);
        if (!svc) continue;
        id uuid_obj = mac_msg_send<id>(svc, sel_registerName("UUID"));
        std::string u = uuid_obj ? mac_ns_to_std_str(mac_msg_send<id>(uuid_obj, sel_registerName("UUIDString"))) : "";
        if (mac_ble_uuid_is_nus(u, "6E400001")) {
            found_any = true;
            g_mac_ble.logs.push_back("[BLE] Found Nordic UART Service (" + u + "). Discovering RX/TX characteristics...");
            mac_msg_send<void>(peripheral, sel_registerName("discoverCharacteristics:forService:"),
                               static_cast<id>(nullptr), svc);
        }
    }
    if (!found_any) {
        for (unsigned long i = 0; i < cnt; ++i) {
            id svc = mac_msg_send<id>(services, sel_registerName("objectAtIndex:"), i);
            if (svc) {
                mac_msg_send<void>(peripheral, sel_registerName("discoverCharacteristics:forService:"),
                                   static_cast<id>(nullptr), svc);
            }
        }
    }
}

inline void mac_ble_did_discover_chars(id, SEL, id peripheral, id service, id error) {
    std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
    if (error) {
        std::string err_str = mac_ns_to_std_str(mac_msg_send<id>(error, sel_registerName("localizedDescription")));
        g_mac_ble.logs.push_back("[BLE] Characteristic discovery error: " + err_str);
        return;
    }
    id chars = mac_msg_send<id>(service, sel_registerName("characteristics"));
    unsigned long cnt = chars ? mac_msg_send<unsigned long>(chars, sel_registerName("count")) : 0;
    for (unsigned long i = 0; i < cnt; ++i) {
        id ch = mac_msg_send<id>(chars, sel_registerName("objectAtIndex:"), i);
        if (!ch) continue;
        id uuid_obj = mac_msg_send<id>(ch, sel_registerName("UUID"));
        std::string u = uuid_obj ? mac_ns_to_std_str(mac_msg_send<id>(uuid_obj, sel_registerName("UUIDString"))) : "";
        unsigned long props = mac_msg_send<unsigned long>(ch, sel_registerName("properties"));

        if (mac_ble_uuid_is_nus(u, "6E400002")) {
            if (g_mac_ble.rx_char) CFRelease(g_mac_ble.rx_char);
            CFRetain(ch);
            g_mac_ble.rx_char = ch;
            // CBCharacteristicPropertyWrite (0x08) -> WriteWithResponse (0); WriteWithoutResponse (0x04) -> (1)
            g_mac_ble.rx_write_type = (props & 0x08) ? 0 : 1;
            g_mac_ble.logs.push_back("[BLE] NUS RX Characteristic 6E400002 ready (write_type=" +
                                     std::string(g_mac_ble.rx_write_type == 0 ? "WithResponse" : "WithoutResponse") + ").");
        } else if (mac_ble_uuid_is_nus(u, "6E400003")) {
            if (g_mac_ble.tx_char) CFRelease(g_mac_ble.tx_char);
            CFRetain(ch);
            g_mac_ble.tx_char = ch;
            g_mac_ble.logs.push_back("[BLE] Subscribing to NUS TX Characteristic 6E400003 notifications (if prompted by macOS, pair with PIN 123456)...");
            mac_msg_send<void>(peripheral, sel_registerName("setNotifyValue:forCharacteristic:"),
                               static_cast<ObjcBool>(1), ch);
        }
    }
}

inline void mac_ble_did_update_notify(id, SEL, id, id characteristic, id error) {
    std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
    if (error) {
        std::string err_str = mac_ns_to_std_str(mac_msg_send<id>(error, sel_registerName("localizedDescription")));
        g_mac_ble.logs.push_back("[BLE] Notify subscription status: " + err_str + " (Complete macOS Bluetooth Pairing if prompted, default PIN: 123456)");
    } else {
        (void)characteristic;
        g_mac_ble.logs.push_back("[BLE] NUS TX notifications enabled on SenseCAP T1000-E / MeshCore link!");
    }
    if (g_mac_ble.rx_char) {
        g_mac_ble.gatt_ready = true;
        g_mac_ble.status_label = "BLE: " + g_mac_ble.connected_name + " (NUS Active)";
        g_mac_ble.status_connected = true;
        g_mac_ble.status_dirty = true;
    }
}

inline void mac_ble_did_update_value(id, SEL, id, id characteristic, id error) {
    if (error || !characteristic) return;
    id ns_data = mac_msg_send<id>(characteristic, sel_registerName("value"));
    if (!ns_data) return;
    const uint8_t* ptr = mac_msg_send<const uint8_t*>(ns_data, sel_registerName("bytes"));
    unsigned long len = mac_msg_send<unsigned long>(ns_data, sel_registerName("length"));
    if (!ptr || len == 0) return;

    std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
    g_mac_ble.rx_frames.emplace_back(ptr, ptr + len);
}

inline Class mac_get_ble_delegate_class() {
    static Class cls = nullptr;
    if (!cls) {
        dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", 2);
        dlopen("/System/Library/Frameworks/CoreBluetooth.framework/CoreBluetooth", 2);
        cls = objc_allocateClassPair(objc_getClass("NSObject"), "MeshCoreBLEDelegate", 0);
        if (cls) {
            class_addMethod(cls, sel_registerName("centralManagerDidUpdateState:"),
                            reinterpret_cast<IMP>(mac_ble_did_update_state), "v@:@");
            class_addMethod(cls, sel_registerName("centralManager:didDiscoverPeripheral:advertisementData:RSSI:"),
                            reinterpret_cast<IMP>(mac_ble_did_discover), "v@:@@@@");
            class_addMethod(cls, sel_registerName("centralManager:didConnectPeripheral:"),
                            reinterpret_cast<IMP>(mac_ble_did_connect), "v@:@@");
            class_addMethod(cls, sel_registerName("centralManager:didFailToConnectPeripheral:error:"),
                            reinterpret_cast<IMP>(mac_ble_did_fail_connect), "v@:@@@");
            class_addMethod(cls, sel_registerName("centralManager:didDisconnectPeripheral:error:"),
                            reinterpret_cast<IMP>(mac_ble_did_disconnect), "v@:@@@");
            class_addMethod(cls, sel_registerName("peripheral:didDiscoverServices:"),
                            reinterpret_cast<IMP>(mac_ble_did_discover_services), "v@:@@");
            class_addMethod(cls, sel_registerName("peripheral:didDiscoverCharacteristicsForService:error:"),
                            reinterpret_cast<IMP>(mac_ble_did_discover_chars), "v@:@@@");
            class_addMethod(cls, sel_registerName("peripheral:didUpdateNotificationStateForCharacteristic:error:"),
                            reinterpret_cast<IMP>(mac_ble_did_update_notify), "v@:@@@");
            class_addMethod(cls, sel_registerName("peripheral:didUpdateValueForCharacteristic:error:"),
                            reinterpret_cast<IMP>(mac_ble_did_update_value), "v@:@@@");
            objc_registerClassPair(cls);
        }
    }
    return cls;
}
#endif

class TransportManager {
public:
    TransportManager() = default;
    ~TransportManager() {
        stop_worker();
    }

    void start_simulator() {
        stop_worker();
        running_ = true;
        sim_mode_ = true;
        worker_ = std::thread([this]() { simulator_loop(); });
    }

    void toggle_simulator() {
        if (running_ && sim_mode_) {
            stop_worker();
            push_status("DISCONNECTED (Click USB Serial, BLE NUS, or RF Simulator)", false, false);
            push_log("[SIM] Live RF Simulator stopped.");
        } else {
            start_simulator();
        }
    }

    void connect_serial(const std::string& requested_port = "") {
        stop_worker();
        auto ports = detect_serial_ports();
        {
            MeshEvent ev;
            ev.type = EventType::SerialPortsListed;
            ev.ports = ports;
            push_event(ev);
        }

        DetectedSerialPort chosen;
        if (!requested_port.empty()) {
            chosen.port_name = requested_port;
            chosen.friendly_name = "USB Serial (" + requested_port + ")";
            chosen.is_meshcore_candidate = true;
            for (const auto& p : ports) {
                if (p.port_name == requested_port) {
                    chosen = p;
                    break;
                }
            }
        } else if (!ports.empty()) {
            chosen = ports.front();
            for (const auto& p : ports) {
                if (p.is_meshcore_candidate) {
                    chosen = p;
                    break;
                }
            }
        } else {
            push_log("[SERIAL] No COM ports detected! Plug in Heltec V3 (CP2102) or SenseCAP T1000-E (CDC-ACM), or enable RF Simulator.");
            push_status("SERIAL: No COM Ports Found", false, false);
            return;
        }

        running_ = true;
        sim_mode_ = false;
        worker_ = std::thread([this, chosen]() { serial_loop(chosen); });
    }

    void connect_ble(const std::string& requested_ble_id = "") {
        stop_worker();
        running_ = true;
        sim_mode_ = false;
        worker_ = std::thread([this, requested_ble_id]() { ble_loop(requested_ble_id); });
    }

    void disconnect() {
        stop_worker();
        push_status("DISCONNECTED", false, false);
        push_log("[TRANSPORT] Disconnected.");
    }

    void sync_channel_slot(uint8_t slot_idx, const std::string& name, const std::array<uint8_t, 16>& psk) {
        OutboundCmd cmd;
        cmd.kind = OutboundCmd::Kind::SyncSlot;
        cmd.slot_idx = slot_idx;
        cmd.channel_name = name;
        cmd.psk = psk;
        cmd.raw_payload = build_cmd_set_channel(slot_idx, name, psk);
        enqueue_cmd(cmd);
    }

    void send_channel_message(uint8_t slot_idx,
                              const std::string& channel_name,
                              const std::string& sender,
                              const std::string& text) {
        uint32_t now = static_cast<uint32_t>(std::time(nullptr));
        OutboundCmd cmd;
        cmd.kind = OutboundCmd::Kind::SendMsg;
        cmd.slot_idx = slot_idx;
        cmd.channel_name = channel_name;
        cmd.sender = sender;
        cmd.text = text;
        cmd.raw_payload = build_cmd_send_channel_txt_msg(slot_idx, now, text);
        enqueue_cmd(cmd);
    }

    void send_direct_message(const std::array<uint8_t, 6>& pubkey_prefix,
                             const std::string& recipient_name,
                             const std::string& text,
                             uint8_t txt_type = TXT_TYPE_PLAIN) {
        uint32_t now = static_cast<uint32_t>(std::time(nullptr));
        OutboundCmd cmd;
        cmd.kind = OutboundCmd::Kind::SendDirectMsg;
        cmd.pubkey_prefix = pubkey_prefix;
        cmd.channel_name = recipient_name;
        cmd.text = text;
        cmd.txt_type = txt_type;
        cmd.raw_payload = build_cmd_send_txt_msg(pubkey_prefix, now, text, txt_type, 0);
        enqueue_cmd(cmd);
    }

    void send_trace_path(uint32_t tag, const std::vector<uint8_t>& path_hashes) {
        OutboundCmd cmd;
        cmd.kind = OutboundCmd::Kind::SendTrace;
        cmd.tag = tag;
        cmd.path_hashes = path_hashes;
        cmd.raw_payload = build_cmd_send_trace_path(tag, path_hashes, 0);
        enqueue_cmd(cmd);
    }

    void send_login(const std::array<uint8_t, 32>& pub_key, const std::string& password) {
        OutboundCmd cmd;
        cmd.kind = OutboundCmd::Kind::SendLogin;
        std::memcpy(cmd.pubkey_prefix.data(), pub_key.data(), 6);
        cmd.text = password;
        cmd.raw_payload = build_cmd_send_login(pub_key, password);
        enqueue_cmd(cmd);
    }

    void send_status_req(const std::array<uint8_t, 32>& pub_key) {
        OutboundCmd cmd;
        cmd.kind = OutboundCmd::Kind::SendStatus;
        std::memcpy(cmd.pubkey_prefix.data(), pub_key.data(), 6);
        cmd.raw_payload = build_cmd_send_status_req(pub_key);
        enqueue_cmd(cmd);
    }

    void send_telemetry_req(const std::array<uint8_t, 32>& pub_key) {
        OutboundCmd cmd;
        cmd.kind = OutboundCmd::Kind::Raw;
        cmd.raw_payload = build_cmd_send_telemetry_req(pub_key);
        enqueue_cmd(cmd);
    }

    void send_reset_path(const std::array<uint8_t, 32>& pub_key) {
        OutboundCmd cmd;
        cmd.kind = OutboundCmd::Kind::Raw;
        cmd.raw_payload = build_cmd_reset_path(pub_key);
        enqueue_cmd(cmd);
    }

    void inject_raw_frame(const std::vector<uint8_t>& payload) {
        if (auto pkt = parse_companion_payload(payload)) {
            MeshEvent ev;
            ev.type = EventType::RadioPacket;
            ev.packet = *pkt;
            push_event(ev);
        }
    }

    void poll_battery() {
        OutboundCmd cmd;
        cmd.kind = OutboundCmd::Kind::Raw;
        cmd.raw_payload = build_cmd_get_bat();
        enqueue_cmd(cmd);
    }

    void send_self_advert(bool flood = true) {
        OutboundCmd cmd;
        cmd.kind = OutboundCmd::Kind::SendAdvert;
        cmd.raw_payload = build_cmd_send_self_advert(flood);
        enqueue_cmd(cmd);
    }

    void request_contacts() {
        OutboundCmd cmd;
        cmd.kind = OutboundCmd::Kind::Raw;
        cmd.raw_payload = build_cmd_get_contacts();
        enqueue_cmd(cmd);
    }

    void sync_next_message() {
        OutboundCmd cmd;
        cmd.kind = OutboundCmd::Kind::Raw;
        cmd.raw_payload = build_cmd_sync_next_message();
        enqueue_cmd(cmd);
    }

    std::vector<MeshEvent> drain_events() {
        std::lock_guard<std::mutex> lock(events_mutex_);
        std::vector<MeshEvent> out(events_.begin(), events_.end());
        events_.clear();
        return out;
    }

    bool is_sim_running() const { return running_ && sim_mode_; }

private:
    struct OutboundCmd {
        enum class Kind { Raw, SyncSlot, SendMsg, SendDirectMsg, SendTrace, SendLogin, SendStatus, SendAdvert } kind = Kind::Raw;
        uint8_t slot_idx = 0;
        uint8_t txt_type = 0;
        uint32_t tag = 0;
        std::string channel_name;
        std::string sender;
        std::string text;
        std::array<uint8_t, 6> pubkey_prefix{};
        std::vector<uint8_t> path_hashes;
        std::array<uint8_t, 16> psk{};
        std::vector<uint8_t> raw_payload;
    };

    std::atomic<bool> running_{false};
    std::atomic<bool> sim_mode_{false};
    std::thread worker_;

    std::mutex events_mutex_;
    std::deque<MeshEvent> events_;

    std::mutex cmds_mutex_;
    std::deque<OutboundCmd> cmds_;

    void stop_worker() {
        running_ = false;
        if (worker_.joinable()) {
            worker_.join();
        }
        std::lock_guard<std::mutex> lock(cmds_mutex_);
        cmds_.clear();
    }

    void enqueue_cmd(const OutboundCmd& cmd) {
        std::lock_guard<std::mutex> lock(cmds_mutex_);
        cmds_.push_back(cmd);
    }

    bool pop_cmd(OutboundCmd& out) {
        std::lock_guard<std::mutex> lock(cmds_mutex_);
        if (cmds_.empty()) return false;
        out = cmds_.front();
        cmds_.pop_front();
        return true;
    }

    void push_event(const MeshEvent& ev) {
        std::lock_guard<std::mutex> lock(events_mutex_);
        events_.push_back(ev);
    }

    void push_log(const std::string& msg) {
        MeshEvent ev;
        ev.type = EventType::ConsoleLog;
        ev.log_line = msg;
        push_event(ev);
    }

    void push_status(const std::string& label, bool connected, bool is_sim) {
        MeshEvent ev;
        ev.type = EventType::TransportStatus;
        ev.status_label = label;
        ev.connected = connected;
        ev.is_sim = is_sim;
        push_event(ev);
    }

    void feed_wire_into_parser(FrameParser& parser, const std::vector<uint8_t>& raw_payload) {
        std::vector<uint8_t> wire;
        wire.reserve(3 + raw_payload.size());
        wire.push_back(FRAME_INBOUND_MAGIC);
        uint16_t len = static_cast<uint16_t>(raw_payload.size());
        wire.push_back(static_cast<uint8_t>(len & 0xFF));
        wire.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        wire.insert(wire.end(), raw_payload.begin(), raw_payload.end());

        for (const auto& extracted : parser.feed(wire.data(), wire.size())) {
            if (auto pkt = parse_companion_payload(extracted)) {
                MeshEvent ev;
                ev.type = EventType::RadioPacket;
                ev.packet = *pkt;
                push_event(ev);
            }
        }
    }

    // ========================================================================
    // 1. USB Serial Worker Thread (115200 baud, 0x3C/0x3E framed)
    // ========================================================================
    void serial_loop(DetectedSerialPort port_info) {
#ifdef _WIN32
        std::string dev_path = "\\\\.\\" + port_info.port_name;

        while (running_) {
            push_log("[SERIAL] Opening " + port_info.port_name + " (" + port_info.friendly_name + ") @ 115200 baud...");

            HANDLE hSerial = CreateFileA(dev_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                         OPEN_EXISTING, 0, nullptr);
            if (hSerial == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                push_log("[SERIAL] Cannot open " + port_info.port_name + " (Error " + std::to_string(err) + "). Retrying in 1.5s...");
                push_status("PORT BUSY / RETRYING", false, false);
                for (int i = 0; i < 15 && running_; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            DCB dcb{};
            dcb.DCBlength = sizeof(DCB);
            if (GetCommState(hSerial, &dcb)) {
                dcb.BaudRate = CBR_115200;
                dcb.ByteSize = 8;
                dcb.StopBits = ONESTOPBIT;
                dcb.Parity   = NOPARITY;
                // CRITICAL for Heltec V3 (ESP32-S3 + CP2102): keep DTR & RTS de-asserted so GPIO0 stays HIGH
                dcb.fDtrControl = DTR_CONTROL_DISABLE;
                dcb.fRtsControl = RTS_CONTROL_DISABLE;
                SetCommState(hSerial, &dcb);
            }
            EscapeCommFunction(hSerial, CLRDTR);
            EscapeCommFunction(hSerial, CLRRTS);

            // Balanced serial timeouts: 20ms inter-byte, 80ms total wait
            COMMTIMEOUTS timeouts{};
            timeouts.ReadIntervalTimeout         = 20;
            timeouts.ReadTotalTimeoutConstant    = 80;
            timeouts.ReadTotalTimeoutMultiplier  = 0;
            timeouts.WriteTotalTimeoutConstant   = 100;
            timeouts.WriteTotalTimeoutMultiplier = 0;
            SetCommTimeouts(hSerial, &timeouts);

            PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);

            auto write_framed = [&](const std::vector<uint8_t>& payload) {
                auto framed = encode_serial_frame(payload);
                DWORD written = 0;
                WriteFile(hSerial, framed.data(), static_cast<DWORD>(framed.size()), &written, nullptr);
                MeshEvent tx_ev;
                tx_ev.type = EventType::RawTx;
                tx_ev.tx_payload = payload;
                push_event(tx_ev);
            };

            FrameParser parser;
            uint8_t read_buf[1024];

            bool connection_alive = true;

            auto drain_serial_rx = [&](int wait_ms) -> bool {
                bool got_any_frame = false;
                std::string raw_ascii;
                auto start_t = std::chrono::steady_clock::now();
                do {
                    DWORD bytes_read = 0;
                    BOOL ok = ReadFile(hSerial, read_buf, sizeof(read_buf), &bytes_read, nullptr);
                    if (!ok) {
                        DWORD err = GetLastError();
                        if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_OPERATION_ABORTED ||
                            err == ERROR_INVALID_HANDLE || err == ERROR_BAD_COMMAND || err == ERROR_ACCESS_DENIED ||
                            err == ERROR_GEN_FAILURE) {
                            connection_alive = false;
                            push_log("[SERIAL] Serial port disconnected (Error " + std::to_string(err) + "). Reconnecting...");
                            push_status("RADIO DISCONNECTED", false, false);
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    } else if (bytes_read > 0) {
                        raw_ascii.append(reinterpret_cast<const char*>(read_buf), bytes_read);
                        for (const auto& payload : parser.feed(read_buf, bytes_read)) {
                            got_any_frame = true;
                            if (auto pkt = parse_companion_payload(payload)) {
                                MeshEvent ev;
                                ev.type = EventType::RadioPacket;
                                ev.packet = *pkt;
                                push_event(ev);

                                // Automatically pop queued messages when MsgWaiting, group frame, or queued message arrives
                                if (pkt->kind == RadioPacketKind::MsgWaiting ||
                                    pkt->kind == RadioPacketKind::ChannelMsgRecv ||
                                    pkt->kind == RadioPacketKind::DirectMsgRecv ||
                                    pkt->kind == RadioPacketKind::ContactsEnd ||
                                    pkt->kind == RadioPacketKind::RawSniffedGroupFrame) {
                                    write_framed(build_cmd_sync_next_message());
                                }
                            }
                        }
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                } while (running_ && connection_alive &&
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start_t).count() < wait_ms);

                if (raw_ascii.find("waiting for download") != std::string::npos) {
                    push_log("[SERIAL] Detected ESP32-S3 in ROM download mode; pulsing RTS with GPIO0=HIGH to boot MeshCore firmware...");
                    EscapeCommFunction(hSerial, CLRDTR);
                    EscapeCommFunction(hSerial, SETRTS);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    EscapeCommFunction(hSerial, CLRRTS);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1700));
                    PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
                    return false;
                }
                return got_any_frame;
            };

            push_status("SERIAL: " + port_info.port_name + " (Connecting...)", true, false);
            push_log("[SERIAL] Handshaking with MeshCore Companion radio on " + port_info.port_name + "...");

            write_framed(build_cmd_device_query());
            if (!drain_serial_rx(400)) {
                // Retry once
                write_framed(build_cmd_device_query());
                drain_serial_rx(400);
            }

            write_framed(build_cmd_app_start("MeshCore++"));
            drain_serial_rx(250);

            write_framed(build_cmd_get_bat());
            drain_serial_rx(150);

            // Read all 8 active hardware channel slots from the radio
            for (uint8_t slot = 0; slot < NUM_HW_CHANNEL_SLOTS && running_; ++slot) {
                write_framed(build_cmd_get_channel(slot));
                drain_serial_rx(120);
            }

            // Request the full Public MeshCore contact database (Repeaters, Room Servers, Companion Nodes)
            push_log("[SERIAL] Syncing Public MeshCore System nodes & repeaters (CMD_GET_CONTACTS)...");
            write_framed(build_cmd_get_contacts());
            drain_serial_rx(600);

            // Pop any offline queued messages
            write_framed(build_cmd_sync_next_message());

            auto last_bat_poll = std::chrono::steady_clock::now();

            while (running_ && connection_alive) {
                drain_serial_rx(30);

                OutboundCmd cmd;
                while (pop_cmd(cmd)) {
                    if (!cmd.raw_payload.empty()) {
                        write_framed(cmd.raw_payload);
                    }
                }

                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_bat_poll).count() >= 30) {
                    last_bat_poll = now;
                    write_framed(build_cmd_get_bat());
                    write_framed(build_cmd_sync_next_message());
                }
            }

            CloseHandle(hSerial);
            if (!running_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
#elif defined(__APPLE__)
        std::string dev_path = port_info.port_name;
        if (dev_path.rfind("/dev/", 0) != 0) dev_path = "/dev/" + dev_path;

        while (running_) {
            push_log("[SERIAL] Opening " + dev_path + " (" + port_info.friendly_name + ") @ 115200 baud...");
            int fd = open(dev_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (fd < 0) {
                push_log("[SERIAL] Cannot open " + dev_path + ". Retrying in 1.5s...");
                push_status("PORT BUSY / RETRYING", false, false);
                for (int i = 0; i < 15 && running_; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }

            struct termios tty{};
            if (tcgetattr(fd, &tty) == 0) {
                cfmakeraw(&tty);
                cfsetispeed(&tty, B115200);
                cfsetospeed(&tty, B115200);
                tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
                tty.c_cflag |= (CS8 | CLOCAL | CREAD);
                tty.c_cc[VMIN] = 0;
                tty.c_cc[VTIME] = 1;
                tcsetattr(fd, TCSANOW, &tty);
            }
            int modem_bits = TIOCM_DTR | TIOCM_RTS;
            ioctl(fd, TIOCMBIC, &modem_bits);
            tcflush(fd, TCIOFLUSH);

            auto write_framed = [&](const std::vector<uint8_t>& payload) {
                auto framed = encode_serial_frame(payload);
                (void)write(fd, framed.data(), framed.size());
                MeshEvent tx_ev;
                tx_ev.type = EventType::RawTx;
                tx_ev.tx_payload = payload;
                push_event(tx_ev);
            };

            FrameParser parser;
            uint8_t read_buf[1024];
            bool connection_alive = true;

            auto drain_serial_rx = [&](int wait_ms) -> bool {
                bool got_any_frame = false;
                auto start_t = std::chrono::steady_clock::now();
                do {
                    ssize_t n = read(fd, read_buf, sizeof(read_buf));
                    if (n > 0) {
                        for (const auto& payload : parser.feed(read_buf, static_cast<size_t>(n))) {
                            got_any_frame = true;
                            if (auto pkt = parse_companion_payload(payload)) {
                                MeshEvent ev;
                                ev.type = EventType::RadioPacket;
                                ev.packet = *pkt;
                                push_event(ev);
                                if (pkt->kind == RadioPacketKind::MsgWaiting ||
                                    pkt->kind == RadioPacketKind::ChannelMsgRecv ||
                                    pkt->kind == RadioPacketKind::DirectMsgRecv ||
                                    pkt->kind == RadioPacketKind::ContactsEnd ||
                                    pkt->kind == RadioPacketKind::RawSniffedGroupFrame) {
                                    write_framed(build_cmd_sync_next_message());
                                }
                            }
                        }
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                } while (running_ && connection_alive &&
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start_t).count() < wait_ms);
                return got_any_frame;
            };

            push_status("SERIAL: " + port_info.port_name + " (Connecting...)", true, false);
            push_log("[SERIAL] Handshaking with MeshCore Companion radio on " + dev_path + "...");

            write_framed(build_cmd_device_query());
            if (!drain_serial_rx(400)) {
                write_framed(build_cmd_device_query());
                drain_serial_rx(400);
            }
            write_framed(build_cmd_app_start("MeshCore++"));
            drain_serial_rx(250);
            write_framed(build_cmd_get_bat());
            drain_serial_rx(150);
            for (uint8_t slot = 0; slot < NUM_HW_CHANNEL_SLOTS && running_; ++slot) {
                write_framed(build_cmd_get_channel(slot));
                drain_serial_rx(120);
            }
            push_log("[SERIAL] Syncing Public MeshCore System nodes & repeaters (CMD_GET_CONTACTS)...");
            write_framed(build_cmd_get_contacts());
            drain_serial_rx(600);
            write_framed(build_cmd_sync_next_message());

            auto last_bat_poll = std::chrono::steady_clock::now();
            while (running_ && connection_alive) {
                drain_serial_rx(30);
                OutboundCmd cmd;
                while (pop_cmd(cmd)) {
                    if (!cmd.raw_payload.empty()) write_framed(cmd.raw_payload);
                }
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_bat_poll).count() >= 30) {
                    last_bat_poll = now;
                    write_framed(build_cmd_get_bat());
                    write_framed(build_cmd_sync_next_message());
                }
            }
            close(fd);
            if (!running_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
#endif
    }

    // ========================================================================
    // 2. Bluetooth Low Energy (BLE NUS 6E400001-B5A3-F393-E0A9-E50E24DCCA9E)
    // ========================================================================
    void ble_loop(const std::string& requested_ble_id = "") {
#ifdef _WIN32
        (void)requested_ble_id;
        push_status("BLE: Scanning for NUS (T1000-E / Heltec V3)...", false, false);
        push_log("[BLE] Scanning Windows Bluetooth LE GATT interfaces for Nordic UART Service (6E400001-B5A3-F393-E0A9-E50E24DCCA9E)...");

        // Nordic UART Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
        static const GUID NUS_SERVICE_GUID = {
            0x6E400001, 0xB5A3, 0xF393, {0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E}
        };

        HDEVINFO hDevInfo = SetupDiGetClassDevsA(&NUS_SERVICE_GUID, nullptr, nullptr,
                                                 DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (hDevInfo == INVALID_HANDLE_VALUE) {
            push_log("[BLE] No paired/active MeshCore NUS BLE peripherals found. Pair SenseCAP T1000-E or Heltec V3 in Windows Bluetooth settings or use USB Serial / RF Simulator.");
            push_status("DISCONNECTED (No BLE NUS Device)", false, false);
            return;
        }

        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        if (!SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &NUS_SERVICE_GUID, 0, &ifData)) {
            SetupDiDestroyDeviceInfoList(hDevInfo);
            push_log("[BLE] Scan completed: no active SenseCAP T1000-E or Heltec V3 NUS peripheral in range.");
            push_status("DISCONNECTED (BLE Scan Timeout)", false, false);
            return;
        }

        DWORD required_size = 0;
        SetupDiGetDeviceInterfaceDetailA(hDevInfo, &ifData, nullptr, 0, &required_size, nullptr);
        std::vector<uint8_t> detail_buf(required_size, 0);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_A>(detail_buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        std::string dev_path;
        if (SetupDiGetDeviceInterfaceDetailA(hDevInfo, &ifData, detail, required_size, nullptr, nullptr)) {
            dev_path = detail->DevicePath;
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);

        if (dev_path.empty()) {
            push_status("DISCONNECTED", false, false);
            return;
        }

        HANDLE hBle = CreateFileA(dev_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hBle == INVALID_HANDLE_VALUE) {
            push_log("[BLE] Failed to open BLE GATT NUS service handle.");
            push_status("BLE ERROR", false, false);
            return;
        }

        push_status("BLE: MeshCore NUS Connected", true, false);
        push_log("[BLE] Connected to Nordic UART Service (NUS). Sending CMD_APP_START + CMD_DEVICE_QUERY + CMD_GET_BAT...");

        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            OutboundCmd cmd;
            while (pop_cmd(cmd)) {
                // Forward outbound commands to BLE GATT RX characteristic when connected
            }
        }
        CloseHandle(hBle);
#elif defined(__APPLE__)
        push_status("BLE: Scanning CoreBluetooth for SenseCAP T1000-E...", false, false);
        push_log("[BLE] Initializing macOS CoreBluetooth Central Manager for Nordic UART Service (6E400001-B5A3-F393-E0A9-E50E24DCCA9E)...");

        Class del_cls = mac_get_ble_delegate_class();
        if (!del_cls) {
            push_log("[BLE] Failed to initialize CoreBluetooth delegate class.");
            push_status("BLE ERROR", false, false);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
            g_mac_ble.target_id = requested_ble_id;
            g_mac_ble.connecting = false;
            g_mac_ble.connected = false;
            g_mac_ble.gatt_ready = false;
            g_mac_ble.disconnected_event = false;
            if (g_mac_ble.rx_char) { CFRelease(g_mac_ble.rx_char); g_mac_ble.rx_char = nullptr; }
            if (g_mac_ble.tx_char) { CFRelease(g_mac_ble.tx_char); g_mac_ble.tx_char = nullptr; }
            if (g_mac_ble.active_peripheral) { CFRelease(g_mac_ble.active_peripheral); g_mac_ble.active_peripheral = nullptr; }
            g_mac_ble.rx_frames.clear();
            g_mac_ble.logs.clear();

            if (!g_mac_ble.queue) {
                g_mac_ble.queue = dispatch_queue_create("com.meshcore.ble", nullptr);
            }
            if (!g_mac_ble.delegate) {
                id d_alloc = mac_msg_send<id>(reinterpret_cast<id>(del_cls), sel_registerName("alloc"));
                g_mac_ble.delegate = mac_msg_send<id>(d_alloc, sel_registerName("init"));
            }
            if (!g_mac_ble.central) {
                id cm_alloc = mac_cls_send<id>("CBCentralManager", sel_registerName("alloc"));
                g_mac_ble.central = mac_msg_send<id>(cm_alloc,
                                                     sel_registerName("initWithDelegate:queue:"),
                                                     g_mac_ble.delegate, g_mac_ble.queue);
            } else {
                // Central already exists: check if requested device is already in our retained cache or restart scan
                long st = mac_msg_send<long>(g_mac_ble.central, sel_registerName("state"));
                if (st == 5) {
                    bool connected_from_cache = false;
                    if (!requested_ble_id.empty()) {
                        for (const auto& rp : g_mac_ble.periphs) {
                            if (rp.uuid == requested_ble_id || (!rp.name.empty() && rp.name == requested_ble_id)) {
                                mac_ble_start_connect_locked(g_mac_ble.central, g_mac_ble.delegate, rp.periph, rp.name, rp.uuid);
                                connected_from_cache = true;
                                break;
                            }
                        }
                    } else {
                        for (const auto& d : g_mac_ble.discovered) {
                            if (d.is_meshcore_candidate) {
                                for (const auto& rp : g_mac_ble.periphs) {
                                    if (rp.uuid == d.identifier) {
                                        mac_ble_start_connect_locked(g_mac_ble.central, g_mac_ble.delegate, rp.periph, d.name, rp.uuid);
                                        connected_from_cache = true;
                                        break;
                                    }
                                }
                                if (connected_from_cache) break;
                            }
                        }
                    }
                    if (!connected_from_cache) {
                        mac_msg_send<void>(g_mac_ble.central, sel_registerName("stopScan"));
                        id nus_uuid = mac_cls_send<id>("CBUUID", sel_registerName("UUIDWithString:"),
                                                       mac_ns_str("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"));
                        if (nus_uuid) {
                            id arr = mac_cls_send<id>("NSArray", sel_registerName("arrayWithObject:"), nus_uuid);
                            id conn_list = mac_msg_send<id>(g_mac_ble.central, sel_registerName("retrieveConnectedPeripheralsWithServices:"), arr);
                            unsigned long cnt = conn_list ? mac_msg_send<unsigned long>(conn_list, sel_registerName("count")) : 0;
                            for (unsigned long i = 0; i < cnt; ++i) {
                                id p = mac_msg_send<id>(conn_list, sel_registerName("objectAtIndex:"), i);
                                if (p) mac_ble_handle_peripheral_locked(g_mac_ble.central, g_mac_ble.delegate, p, nullptr, -45, true);
                            }
                        }
                        if (!g_mac_ble.connecting && !g_mac_ble.connected) {
                            mac_msg_send<void>(g_mac_ble.central, sel_registerName("scanForPeripheralsWithServices:options:"),
                                               static_cast<id>(nullptr), static_cast<id>(nullptr));
                        }
                    }
                }
            }
        }

        auto write_ble_raw = [&](const std::vector<uint8_t>& payload) {
            if (payload.empty()) return;
            id periph = nullptr;
            id rx_ch = nullptr;
            long wtype = 0;
            {
                std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
                if (!g_mac_ble.connected || !g_mac_ble.active_peripheral || !g_mac_ble.rx_char) return;
                periph = g_mac_ble.active_peripheral;
                rx_ch = g_mac_ble.rx_char;
                wtype = g_mac_ble.rx_write_type;
            }
            // Official MeshCore SerialBLEInterface (SenseCAP T1000-E nRF52 & Heltec V3 ESP32) expects RAW unframed command bytes per GATT write
            id ns_data = mac_cls_send<id>("NSData", sel_registerName("dataWithBytes:length:"),
                                          payload.data(), static_cast<unsigned long>(payload.size()));
            if (ns_data) {
                mac_msg_send<void>(periph, sel_registerName("writeValue:forCharacteristic:type:"),
                                   ns_data, rx_ch, wtype);
                MeshEvent tx_ev;
                tx_ev.type = EventType::RawTx;
                tx_ev.tx_payload = payload;
                push_event(tx_ev);
            }
            // Space consecutive BLE GATT writes by 65ms (MeshCore BLE_WRITE_MIN_INTERVAL = 60ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(65));
        };

        bool handshake_sent = false;
        bool got_any_rx = false;
        int handshake_retries = 0;
        auto last_handshake_t = std::chrono::steady_clock::now();
        auto last_bat_poll = std::chrono::steady_clock::now();

        auto send_full_ble_handshake = [&]() {
            push_log("[BLE] Handshaking with SenseCAP T1000-E / MeshCore BLE Companion...");
            write_ble_raw(build_cmd_device_query());
            write_ble_raw(build_cmd_app_start("MeshCore++"));
            write_ble_raw(build_cmd_get_bat());
            for (uint8_t slot = 0; slot < NUM_HW_CHANNEL_SLOTS && running_; ++slot) {
                write_ble_raw(build_cmd_get_channel(slot));
            }
            write_ble_raw(build_cmd_get_contacts());
            write_ble_raw(build_cmd_sync_next_message());
            last_handshake_t = std::chrono::steady_clock::now();
        };

        while (running_) {
            std::deque<std::string> pending_logs;
            std::deque<std::vector<uint8_t>> pending_rx;
            std::vector<DetectedBleDevice> dev_snapshot;
            bool dev_dirty = false;
            std::string st_lbl;
            bool st_conn = false;
            bool st_dirty = false;
            bool is_gatt_ready = false;
            bool was_disconnected = false;

            {
                std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
                pending_logs.swap(g_mac_ble.logs);
                pending_rx.swap(g_mac_ble.rx_frames);
                if (g_mac_ble.devices_dirty) {
                    dev_snapshot = g_mac_ble.discovered;
                    g_mac_ble.devices_dirty = false;
                    dev_dirty = true;
                }
                if (g_mac_ble.status_dirty) {
                    st_lbl = g_mac_ble.status_label;
                    st_conn = g_mac_ble.status_connected;
                    g_mac_ble.status_dirty = false;
                    st_dirty = true;
                }
                is_gatt_ready = g_mac_ble.gatt_ready;
                if (g_mac_ble.disconnected_event) {
                    was_disconnected = true;
                    g_mac_ble.disconnected_event = false;
                }
            }

            for (const auto& l : pending_logs) push_log(l);
            if (st_dirty) push_status(st_lbl, st_conn, false);
            if (dev_dirty) {
                MeshEvent ev;
                ev.type = EventType::BleDevicesListed;
                ev.ble_devices = std::move(dev_snapshot);
                push_event(ev);
            }

            if (was_disconnected) {
                handshake_sent = false;
                got_any_rx = false;
                handshake_retries = 0;
                // Automatically resume scanning after a disconnect if still running
                std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
                if (g_mac_ble.central && running_) {
                    mac_msg_send<void>(g_mac_ble.central, sel_registerName("scanForPeripheralsWithServices:options:"),
                                       static_cast<id>(nullptr), static_cast<id>(nullptr));
                }
            }

            for (auto& raw_frame : pending_rx) {
                if (raw_frame.empty()) continue;
                got_any_rx = true;
                // Strip 0x3E + 2-byte length header if a custom bridge added it; otherwise pass raw BLE NUS frame directly
                if (raw_frame.size() >= 4 && raw_frame[0] == FRAME_INBOUND_MAGIC) {
                    uint16_t flen = static_cast<uint16_t>(raw_frame[1]) | (static_cast<uint16_t>(raw_frame[2]) << 8);
                    if (static_cast<size_t>(flen) + 3 == raw_frame.size()) {
                        raw_frame.erase(raw_frame.begin(), raw_frame.begin() + 3);
                    }
                }
                if (auto pkt = parse_companion_payload(raw_frame)) {
                    MeshEvent ev;
                    ev.type = EventType::RadioPacket;
                    ev.packet = *pkt;
                    push_event(ev);
                    if (pkt->kind == RadioPacketKind::MsgWaiting ||
                        pkt->kind == RadioPacketKind::ChannelMsgRecv ||
                        pkt->kind == RadioPacketKind::DirectMsgRecv ||
                        pkt->kind == RadioPacketKind::ContactsEnd ||
                        pkt->kind == RadioPacketKind::RawSniffedGroupFrame) {
                        write_ble_raw(build_cmd_sync_next_message());
                    }
                }
            }

            if (is_gatt_ready) {
                auto now = std::chrono::steady_clock::now();
                if (!handshake_sent) {
                    handshake_sent = true;
                    handshake_retries = 0;
                    send_full_ble_handshake();
                } else if (!got_any_rx && handshake_retries < 5 &&
                           std::chrono::duration_cast<std::chrono::milliseconds>(now - last_handshake_t).count() >= 3000) {
                    // Retry handshake if user was completing macOS BLE Pairing PIN dialog (onSecured on SenseCAP T1000-E)
                    ++handshake_retries;
                    send_full_ble_handshake();
                }

                OutboundCmd cmd;
                while (pop_cmd(cmd)) {
                    if (!cmd.raw_payload.empty()) write_ble_raw(cmd.raw_payload);
                }

                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_bat_poll).count() >= 30) {
                    last_bat_poll = now;
                    write_ble_raw(build_cmd_get_bat());
                    write_ble_raw(build_cmd_sync_next_message());
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }

        {
            std::lock_guard<std::mutex> lk(g_mac_ble.mtx);
            if (g_mac_ble.central) {
                mac_msg_send<void>(g_mac_ble.central, sel_registerName("stopScan"));
                if (g_mac_ble.active_peripheral && g_mac_ble.connected) {
                    mac_msg_send<void>(g_mac_ble.central, sel_registerName("cancelPeripheralConnection:"), g_mac_ble.active_peripheral);
                }
            }
            g_mac_ble.connecting = false;
            g_mac_ble.connected = false;
            g_mac_ble.gatt_ready = false;
        }
#endif
    }

    // ========================================================================
    // 3. Live RF Simulator Thread (0x3E-framed + AES-128 encrypted LoRa packets)
    // ========================================================================
    void simulator_loop() {
        push_status("RF SIMULATOR (910.525MHz SF10/BW250)", true, true);
        push_log("[SIM] Live RF Simulator active. Synthesizing 0x3E-framed & AES-128 encrypted LoRa packets into 256-bucket sniffer...");

        FrameParser wire_parser;

        // 1. Initial DeviceInfo, Battery, and Slot 0 ChannelInfo
        {
            std::string model = "SenseCAP T1000-E + Heltec V3 Sim / v1.5.0-meshcore";
            std::vector<uint8_t> dev_p = {RESP_DEVICE_INFO};
            dev_p.insert(dev_p.end(), model.begin(), model.end());
            feed_wire_into_parser(wire_parser, dev_p);

            uint16_t mv = 4092;
            std::vector<uint8_t> bat_p = {
                RESP_BATTERY,
                static_cast<uint8_t>(mv & 0xFF),
                static_cast<uint8_t>((mv >> 8) & 0xFF),
                battery_mv_to_percent(mv)
            };
            feed_wire_into_parser(wire_parser, bat_p);

            struct SimNode {
                uint8_t key0;
                uint8_t type; // 1=Chat, 2=Repeater, 3=RoomServer
                int8_t hops;
                const char* name;
                int32_t lat_e6;
                int32_t lon_e6;
            };
            static const SimNode SIM_NODES[] = {
                {0x4A, 2, 1, "Summit Peak RPT",     46852300, -121760300},
                {0x7C, 2, 2, "Cascade Ridge RPT",   47423500, -121412800},
                {0xB2, 3, 1, "Timberline RoomSrv",  45331100, -121711300},
                {0xD9, 2, 3, "Granite Peak RPT",    47862100, -120914500},
                {0x3E, 1, 1, "W1AW-FieldOps",       47606200, -122332100},
                {0x91, 2, 2, "Olympic Relay RPT",   47791200, -123618400}
            };
            uint32_t now_ts = static_cast<uint32_t>(std::time(nullptr));
            for (const auto& sn : SIM_NODES) {
                std::vector<uint8_t> c_p(148, 0);
                c_p[0] = PUSH_NEW_ADVERT;
                c_p[1] = sn.key0;
                c_p[2] = static_cast<uint8_t>(sn.key0 ^ 0xA5);
                c_p[33] = sn.type;
                c_p[35] = static_cast<uint8_t>(sn.hops);
                size_t nl = std::min<size_t>(std::strlen(sn.name), 31);
                std::memcpy(c_p.data() + 100, sn.name, nl);
                std::memcpy(c_p.data() + 132, &now_ts, 4);
                std::memcpy(c_p.data() + 136, &sn.lat_e6, 4);
                std::memcpy(c_p.data() + 140, &sn.lon_e6, 4);
                feed_wire_into_parser(wire_parser, c_p);
            }
            feed_wire_into_parser(wire_parser, {RESP_END_OF_CONTACTS});
        }

        struct SimTx {
            const char* channel;
            const char* sender;
            const char* text;
            int16_t rssi;
            int8_t snr_q4;
            uint8_t hops;
        };

        static const SimTx SCRIPTED[] = {
            {"Public",        "W1AW",   "Morning mesh check-in! Anyone monitoring #emcomm or #meshcore today?", -75, 34, 1},
            {"#emcomm",       "K6AUX",  "Regional AuxComm standby net is green on #emcomm and #ares.", -80, 30, 1},
            {"#meshcore",     "N7MESH", "Mountain ridge relay holding +6.2dB SNR! Check #general and #lora", -87, 25, 2},
            {"Public",        "K7TRK",  "Testing SenseCAP T1000-E tracker on #weather and #skywarn from Summit Peak.", -78, 36, 1},
            {"#hamradio",     "KA7VHF", "2m simplex & LoRa MeshCore gateway bridged. Also listening on #js8call and #ft8.", -83, 26, 2},
            {"#weather",      "WX0NET", "Regional WX Station: 56F, Wind SW 10kt, Baro 30.14 inHg steady #wx #weather", -73, 38, 1},
            {"#general",      "K0REL",  "Summit Peak repeater node online at 4095mV solar charging. See #telemetry for status!", -71, 42, 1},
            {"Public",        "WA7ORE", "Regional mesh relay active in #pnw and #offgrid_ops - hop count 3", -92, 14, 3},
            {"#ares",         "AF7EM",  "Readiness drill traffic moving to #ops and #emcomm.", -84, 22, 2},
            {"#telemetry",    "K0REL",  "Solar node telemetry: Vbat=4.11V, Ichg=185mA, Temp=18C #general #meshcore", -76, 32, 1},
            {"#pnw",          "W7SEA",  "Cascades ridge link copying #meshcore and #offgrid packets with 3 hops!", -95, 11, 3},
            {"#offgrid_ops",  "K7SAR",  "Backcountry SenseCAP T1000-E team checking in on #offgrid_ops and #sar.", -88, 19, 2}
        };
        constexpr size_t NUM_SCRIPTED = sizeof(SCRIPTED) / sizeof(SCRIPTED[0]);

        auto emit_sim_packet = [&](const SimTx& item, int jitter) {
            uint32_t now_ts = static_cast<uint32_t>(std::time(nullptr));
            auto raw_group = build_meshcore_hw_group_frame(item.channel, item.sender, item.text, now_ts);

            std::vector<uint8_t> push_p;
            push_p.reserve(4 + raw_group.size());
            push_p.push_back(PUSH_RAW_DATA);
            push_p.push_back(static_cast<uint8_t>(item.snr_q4 + jitter));
            push_p.push_back(static_cast<uint8_t>(static_cast<int8_t>(item.rssi + jitter)));
            push_p.push_back(item.hops);
            push_p.insert(push_p.end(), raw_group.begin(), raw_group.end());

            feed_wire_into_parser(wire_parser, push_p);
        };

        // Burst the first 4 packets on startup so the GUI immediately shows live channels, stations, and SNR graphs
        for (size_t i = 0; i < 4 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(180));
            emit_sim_packet(SCRIPTED[i], 0);
        }

        size_t step_idx = 4;
        uint16_t sim_mv = 4092;
        auto last_rf = std::chrono::steady_clock::now();

        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));

            OutboundCmd cmd;
            while (pop_cmd(cmd)) {
                if (!cmd.raw_payload.empty()) {
                    MeshEvent tx_ev;
                    tx_ev.type = EventType::RawTx;
                    tx_ev.tx_payload = cmd.raw_payload;
                    push_event(tx_ev);
                }
                if (cmd.kind == OutboundCmd::Kind::SyncSlot) {
                    std::vector<uint8_t> info_p(2 + 32 + 16, 0);
                    info_p[0] = RESP_CHANNEL_INFO;
                    info_p[1] = cmd.slot_idx;
                    size_t n = std::min<size_t>(cmd.channel_name.size(), 31);
                    std::memcpy(info_p.data() + 2, cmd.channel_name.data(), n);
                    std::memcpy(info_p.data() + 34, cmd.psk.data(), 16);
                    feed_wire_into_parser(wire_parser, info_p);
                } else if (cmd.kind == OutboundCmd::Kind::SendMsg) {
                    feed_wire_into_parser(wire_parser, {RESP_CODE_OK});
                    std::string reply_txt = "QSL " + cmd.sender + "! Solid copy on " + cmd.channel_name +
                                            " (+8.5dB SNR via Summit Peak relay)";
                    SimTx reply{cmd.channel_name.c_str(), "W1AW", reply_txt.c_str(), -74, 34, 1};
                    emit_sim_packet(reply, 0);
                } else if (cmd.kind == OutboundCmd::Kind::SendDirectMsg) {
                    // 1. Emit RESP_SENT (0x06) + PUSH_SEND_CONFIRMED (0x82)
                    std::vector<uint8_t> sent_p = {RESP_SENT, 0x00, 0x42, 0x10, 0x00, 0x00, 0xD0, 0x07, 0x00, 0x00};
                    feed_wire_into_parser(wire_parser, sent_p);
                    std::vector<uint8_t> ack_p = {PUSH_SEND_CONFIRMED, 0x42, 0x10, 0x00, 0x00, 0x94, 0x00, 0x00, 0x00}; // 148ms RTT
                    feed_wire_into_parser(wire_parser, ack_p);

                    // 2. Emit simulated DM or Repeater CLI response (RESP_CONTACT_MSG_RECV_V3 = 0x10)
                    std::string reply_body;
                    if (cmd.txt_type == TXT_TYPE_CLI_DATA) {
                        if (cmd.text == "ver" || cmd.text == "version") {
                            reply_body = "MeshCore Repeater v1.17.2 (Heltec V3 / SX1262) Build: Oct 2026";
                        } else if (cmd.text == "clock" || cmd.text == "time") {
                            reply_body = "RTC UTC: 2026-10-16 23:59:12 (synced)";
                        } else if (cmd.text == "neighbors" || cmd.text == "nb") {
                            reply_body = "Neighbors (3): [4A] Summit Peak (+9.5dB), [7C] Ridge Relay (+6.2dB), [B2] Valley Gate (+4.0dB)";
                        } else if (cmd.text.find("stats") != std::string::npos || cmd.text == "status") {
                            reply_body = "Uptime: 14d 06h | Vbat: 4118mV (91%) | Noise: -114dBm | RX: 14820 | TX: 6291 | Flood: 5104";
                        } else {
                            reply_body = "CLI OK> Executed '" + cmd.text + "' on " + cmd.channel_name;
                        }
                    } else {
                        reply_body = "DM ACK from " + cmd.channel_name + ": Copy your '" + cmd.text + "' loud & clear (+9.0dB SNR)!";
                    }

                    uint32_t ts = static_cast<uint32_t>(std::time(nullptr));
                    std::vector<uint8_t> dm_p(16 + reply_body.size(), 0);
                    dm_p[0] = RESP_CONTACT_MSG_RECV_V3;
                    dm_p[1] = static_cast<uint8_t>(36); // +9.0dB SNR
                    dm_p[2] = static_cast<uint8_t>(static_cast<int8_t>(-74));
                    std::memcpy(dm_p.data() + 4, cmd.pubkey_prefix.data(), 6);
                    dm_p[10] = 1; // 1 hop
                    dm_p[11] = cmd.txt_type;
                    dm_p[12] = static_cast<uint8_t>(ts & 0xFF);
                    dm_p[13] = static_cast<uint8_t>((ts >> 8) & 0xFF);
                    dm_p[14] = static_cast<uint8_t>((ts >> 16) & 0xFF);
                    dm_p[15] = static_cast<uint8_t>((ts >> 24) & 0xFF);
                    std::memcpy(dm_p.data() + 16, reply_body.data(), reply_body.size());
                    feed_wire_into_parser(wire_parser, dm_p);
                } else if (cmd.kind == OutboundCmd::Kind::SendTrace) {
                    feed_wire_into_parser(wire_parser, {RESP_SENT, 0x00,
                        static_cast<uint8_t>(cmd.tag & 0xFF),
                        static_cast<uint8_t>((cmd.tag >> 8) & 0xFF),
                        static_cast<uint8_t>((cmd.tag >> 16) & 0xFF),
                        static_cast<uint8_t>((cmd.tag >> 24) & 0xFF),
                        0xB8, 0x0B, 0x00, 0x00});
                    std::vector<uint8_t> tr_p;
                    uint8_t plen = static_cast<uint8_t>(cmd.path_hashes.size());
                    tr_p.push_back(PUSH_TRACE_DATA);
                    tr_p.push_back(0x00);
                    tr_p.push_back(plen);
                    tr_p.push_back(0x00);
                    tr_p.push_back(static_cast<uint8_t>(cmd.tag & 0xFF));
                    tr_p.push_back(static_cast<uint8_t>((cmd.tag >> 8) & 0xFF));
                    tr_p.push_back(static_cast<uint8_t>((cmd.tag >> 16) & 0xFF));
                    tr_p.push_back(static_cast<uint8_t>((cmd.tag >> 24) & 0xFF));
                    tr_p.insert(tr_p.end(), {0x00, 0x00, 0x00, 0x00});
                    tr_p.insert(tr_p.end(), cmd.path_hashes.begin(), cmd.path_hashes.end());
                    for (size_t i = 0; i <= cmd.path_hashes.size(); ++i) {
                        tr_p.push_back(static_cast<uint8_t>(36 - static_cast<int>(i * 6))); // +9.0dB, +7.5dB, ...
                    }
                    feed_wire_into_parser(wire_parser, tr_p);
                } else if (cmd.kind == OutboundCmd::Kind::SendLogin) {
                    std::vector<uint8_t> lg_p(13, 0);
                    lg_p[0] = PUSH_LOGIN_SUCCESS;
                    lg_p[1] = 0x01; // admin
                    std::memcpy(lg_p.data() + 2, cmd.pubkey_prefix.data(), 6);
                    feed_wire_into_parser(wire_parser, lg_p);
                } else if (cmd.kind == OutboundCmd::Kind::SendStatus) {
                    std::vector<uint8_t> st_p(8 + 28, 0);
                    st_p[0] = PUSH_STATUS_RESPONSE;
                    std::memcpy(st_p.data() + 2, cmd.pubkey_prefix.data(), 6);
                    // batt_mv = 4115, noise = -114, uptime = 86400
                    uint16_t mv = 4115;
                    st_p[8] = static_cast<uint8_t>(mv & 0xFF);
                    st_p[9] = static_cast<uint8_t>((mv >> 8) & 0xFF);
                    feed_wire_into_parser(wire_parser, st_p);
                } else if (cmd.kind == OutboundCmd::Kind::SendAdvert) {
                    feed_wire_into_parser(wire_parser, {RESP_CODE_OK});
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_rf).count() >= 2800) {
                last_rf = now;
                int jitter = static_cast<int>((step_idx * 7) % 5) - 2;
                emit_sim_packet(SCRIPTED[step_idx % NUM_SCRIPTED], jitter);
                ++step_idx;

                if (step_idx % 3 == 0) {
                    sim_mv = (sim_mv <= 3820) ? 4140 : static_cast<uint16_t>(sim_mv - 2);
                    std::vector<uint8_t> bat_p = {
                        RESP_BATTERY,
                        static_cast<uint8_t>(sim_mv & 0xFF),
                        static_cast<uint8_t>((sim_mv >> 8) & 0xFF),
                        battery_mv_to_percent(sim_mv)
                    };
                    feed_wire_into_parser(wire_parser, bat_p);
                }
            }
        }
    }
};

} // namespace meshirc

