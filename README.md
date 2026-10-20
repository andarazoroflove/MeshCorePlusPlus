<div align="center">

<img src="releases/MeshCore++.app/Contents/Resources/AppIcon.png" alt="MeshCore++ Logo" width="128" height="128" />

# MeshCore++

**Native C++20 Tactical LoRa Mesh IRC Client, 256-Bucket Hashtag Channel Sniffer, 1-Bit Image Transceiver, Packet Inspector & Live Map**

[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20macOS%20(Universal)%20%7C%20iOS%20%7C%20Android-00E5FF?style=for-the-badge)](#downloads--installation)
[![Language](https://img.shields.io/badge/C%2B%2B-20-10B981?style=for-the-badge&logo=cplusplus)](#building-from-source)
[![Hardware](https://img.shields.io/badge/Radios-Any%20MeshCore%20Companion%20Node%20(USB%20%26%20BLE%20NUS)-FBBF24?style=for-the-badge)](#supported-transports--nodes)

</div>

---

**MeshCore++** is a zero-bloat, hardware-accelerated native client for **any MeshCore Companion Node** across **Windows 10/11**, **macOS Sonoma+ (Universal Binary)**, **iOS (iPhone & iPad)**, and **Android (Android 5.0 Lollipop through Android 15+)**. Built in pure **C++20** with Direct2D/DirectWrite on Windows, Cocoa/UIKit/CoreGraphics/CoreText/CoreBluetooth on macOS & iOS, and a native C++20 JNI engine on Android, it pairs an IRC-style multi-channel workstation with a real-time **256-Bucket SHA-256/AES-128 Hashtag Channel Sniffer**, **Single-Packet 48×30 1-Bit Image Transceiver (`0x49`)**, **Over-the-Air Packet Inspector**, **Repeater Path Tracer**, and **Live Tactical Mesh Map**.

---

## Screenshots

### 1. IRC Workstation & 256-Bucket Rainbow Hashtag Sniffer
![MeshCore++ Chat & 256-Bucket Sniffer](docs/screenshot_chat.png)

### 2. Live Tactical Mesh Map (OpenStreetMap + Repeater Topology)
![MeshCore++ Live Mesh Map](docs/screenshot_map.png)

### 3. Over-the-Air Packet Inspector & Hex Analyzer
![MeshCore++ Packet Inspector](docs/screenshot_packets.png)

---

## Key Features

- **256-Bucket SHA-256 / AES-128 Hashtag Channel Sniffer**
  - Passively intercepts raw over-the-air MeshCore group frames (`PUSH_RAW_DATA` `0x84` & `PUSH_LOG_RX_DATA` `0x88`) across all 256 1-byte channel hash buckets (`SHA256(PSK)[0]`).
  - Pre-computes a rainbow dictionary of 380+ common hashtag channels (`#emcomm`, `#meshcore`, `#weather`, `#hamradio`, `#general`, `#sar`, US states, major metro areas, `#ch0`–`#ch99`, plus custom `wordlist.txt` entries).
  - Automatically extracts new `#hashtag` channels mentioned inside decrypted chat messages and dynamically adds their derived keys (`SHA256("#tag")[0..16]`) to the live sniffer in real time.
  - **Promiscuous Decryption**: Even when you are only joined to `Public`, sniffed traffic on unjoined `#hashtag` channels is previewed inline with one-click **`+ Join #channel`** pills.

- **Single-Packet 48×30 1-Bit Monochrome Image Transceiver (`Opcode 0x49`)**
  - Click/tap **`[Img]`** next to the chat composer on Desktop, iOS, or Android to select any photo.
  - Automatically crops/fits with minimal clipping, applies 4×4 supersampled area downscaling, dynamic percentile contrast normalization, 3×3 unsharp masking, and serpentine Atkinson/Floyd-Steinberg error-diffusion dithering into a **180-byte (`48×30` 1-bit) bitmap**.
  - Compresses and transmits over LoRa RF in a **single MeshCore companion packet (`0x49`)** and renders inline at crisp **5× Nearest-Neighbor** scale (`240×150`).

- **Hardware Slot Synchronization (8-Slot LRU Table)**
  - Slot `0` is permanently pinned to the MeshCore `Public` channel (`8b3387e9c5cdea6ac9e5edbaa115cd72`).
  - Joining any `#hashtag` room automatically programs the derived 16-byte PSK into hardware slots `1..7` via `CMD_SET_CHANNEL` (`0x20`) with LRU eviction.

- **Universal Companion Node Connectivity**
  - **USB Serial (`115200 8N1`)**: Auto-detects any USB-UART or USB CDC-ACM MeshCore Companion node (`COMx` on Windows, `/dev/cu.usb*` via `IOKit` on macOS) with `0x3C` (host $\to$ radio) and `0x3E` (radio $\to$ host) binary framing.
  - **Bluetooth Low Energy (`BLE NUS`)**: Native **CoreBluetooth** (macOS & iOS), **WinRT BLE GATT** (Windows 10/11), and **Android BLE GATT** (Android 5.0–15+) client for the Nordic UART Service (`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`), supporting unframed GATT writes/notifications, automatic idle link wake-up, and BLE pairing with any MeshCore node.
  - **Built-in Live RF Simulator**: Synthesizes real AES-128 encrypted LoRa group frames, repeater beacons, battery telemetry, and CLI responses so you can test the entire UI without hardware attached.

- **Snooping & Diagnostics Toolkit**
  - **Packet Inspector**: Live capture of all Companion and Over-the-Air RF frames with filtering (`All`, `Over-the-air`, `Companion`, `TX`), full hex/ASCII dump, session recording/replay, and CSV export.
  - **Adverts & Node Directory**: Tracks Repeaters, Room Servers, Sensors, and Companion Stations with RSSI, SNR, hop count, and GPS coordinates. Double-click any node to open a direct message or remote Repeater CLI session.
  - **Path Tracer**: Visualizes multi-hop repeater routes (`4A => 7C => ME`), tracks relay statistics, and supports active route tracing (`/tracepath`).
  - **Live Mesh Map**: Interactive dark/light OpenStreetMap slippy map with automatic disk tile caching, repeater link overlays, and KML/CSV export.

---

## Supported Transports & Nodes

Works out of the box with **any LoRa radio running MeshCore Companion firmware** (nRF52840, ESP32-S3, RP2040, STM32, etc.):

| Transport | Protocol / Framing | Supported Platforms |
| :--- | :--- | :--- |
| **Bluetooth Low Energy (BLE NUS)** | Nordic UART Service (`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`) | Windows 10/11, macOS Sonoma+, iOS 15–27+, Android 5.0–15+ |
| **USB Serial (`115200 8N1`)** | USB CDC-ACM / CP210x / CH340 / CH9102 / FTDI (`0x3C` / `0x3E`) | Windows 10/11, macOS Sonoma+ |
| **Live RF Simulator** | Built-in AES-128 / 256-Bucket LoRa Frame Synthesizer | Windows, macOS, iOS |

---

## Downloads & Installation

Pre-built native packages for all four platforms are available on the [**GitHub Releases**](https://github.com/andarazoroflove/MeshCorePlusPlus/releases) page and in the [`releases/`](releases/) folder:

### 1. Windows 10 / 11 (`x86_64`)
- **Standalone Executable**: [`MeshCorePlusPlus-Windows-GUI.exe`](https://github.com/andarazoroflove/MeshCorePlusPlus/releases/download/v1.0.0/MeshCorePlusPlus-Windows-GUI.exe)
- Zero external runtime dependencies — statically linked C++20 / Direct2D / DirectWrite / WinHTTP binary. Simply download and run.

### 2. macOS Sonoma 14.0+ (`Universal Binary`: Apple Silicon `arm64` + Intel `x86_64`)
- **macOS Application Bundle (`.zip`)**: [`MeshCorePlusPlus-macOS-Sonoma.zip`](https://github.com/andarazoroflove/MeshCorePlusPlus/releases/download/v1.0.0/MeshCorePlusPlus-macOS-Sonoma.zip)
- **Standalone Universal Binary**: [`MeshCorePlusPlus-macOS-Sonoma-Universal`](https://github.com/andarazoroflove/MeshCorePlusPlus/releases/download/v1.0.0/MeshCorePlusPlus-macOS-Sonoma-Universal)
- **Quick Start on macOS**:
  1. Unzip `MeshCore++-macOS-Sonoma.zip` and launch `MeshCore++.app` (if Gatekeeper prompts on first launch, run `xattr -cr MeshCore++.app` or Right-Click $\to$ **Open**).
  2. Click **`USB Serial v`** or **`BLE NUS v`** in the top toolbar to connect to any MeshCore Companion node.

### 3. iOS 15.0 – 27.0+ (`arm64` iPhone & iPad `.ipa`)
- **Sideloadable iOS Package (`.ipa`)**: [`MeshCorePlusPlus-iOS.ipa`](https://github.com/andarazoroflove/MeshCorePlusPlus/releases/download/v1.0.0/MeshCorePlusPlus-iOS.ipa)
- **Quick Start on iOS**:
  1. Install `MeshCore++-iOS.ipa` using **Sideloadly**, **AltStore**, or **Xcode**.
  2. On first launch, trust your developer certificate under **Settings $\to$ General $\to$ VPN & Device Management** (and enable **Developer Mode** under **Settings $\to$ Privacy & Security** on iOS 16+).
  3. Grant Bluetooth permission when prompted to auto-scan and connect to your MeshCore Companion node over BLE NUS.

### 4. Android 5.0 Lollipop – Android 15+ (`armeabi-v7a`, `arm64-v8a`, `x86_64` `.apk`)
- **Universal Signed Android APK**: [`MeshCorePlusPlus-Android.apk`](https://github.com/andarazoroflove/MeshCorePlusPlus/releases/download/v1.0.0/MeshCorePlusPlus-Android.apk)
- Includes native C++20 JNI libraries (`libmeshcorepp.so`) for both 32-bit (`armeabi-v7a`) and 64-bit (`arm64-v8a`, `x86_64`) devices running **Android 5.0 (API 21)** through **Android 15+**.
- **Quick Start on Android**:
  1. Transfer and open `MeshCore++-Android.apk` on your Android phone/tablet, or install via ADB (`adb install -r MeshCore++-Android.apk`).
  2. Tap **`[BLE Scan]`** in the top bar (or open the **Radio** tab) to connect to your MeshCore Companion node.

---

## IRC & Tactical Slash Commands

Type any command into the bottom composer bar:

| Command | Description |
| :--- | :--- |
| `/join #channel` | Derive the SHA-256 PSK for `#channel`, sync it to a radio hardware slot (`1..7`), and open its room tab |
| `/part [#channel]` | Close the current or specified `#hashtag` channel tab |
| `/nick <CALLSIGN>` | Set your operator callsign (also editable via the top-right `OP: ... [Edit]` button) |
| `/dm <node> <text>` | Send an encrypted direct message to a discovered station |
| `/login <node> <pw>` | Authenticate as admin with a remote MeshCore Repeater or Room Server |
| `/cli <node> <cmd>` | Send a remote CLI command (`ver`, `clock`, `neighbors`, `stats`, `advert`) to a Repeater |
| `/status <node>` | Request live battery, noise floor, and uptime telemetry from a remote node |
| `/tracepath <hops>` | Send an active multi-hop RF trace packet across specified repeater prefixes or node names |
| `/advert [name]` | Broadcast a self-advertisement beacon over LoRa RF |
| `/slots` | Display the current 8-slot hardware channel table (`S0..S7`) |
| `/psk [#channel]` | Show the 16-byte hex PSK and 1-byte SHA-256 bucket hash for a channel |
| `/export` | Export discovered nodes, packets, adverts, and KML map pins to `./exports/` |

---

## Building from Source

MeshCore++ is header-organized in [`cpp/`](cpp/) and [`android/`](android/) with zero third-party library dependencies:

- [`cpp/main_gui.cpp`](cpp/main_gui.cpp) — Direct2D/DirectWrite & Cocoa/UIKit GUI workstation, 3-pane desktop & responsive mobile layout, Packet Inspector, Path Tracer, and Slippy Map engine
- [`cpp/crypto.hpp`](cpp/crypto.hpp) — Standalone C++20 SHA-256, HMAC-SHA256, AES-128-ECB/CBC, 256-Bucket Rainbow Table Sniffer, and `0x49` 1-bit image wire codec
- [`cpp/image_encoder.hpp`](cpp/image_encoder.hpp) — 48×30 1-bit monochrome image resizer, contrast normalizer, unsharp mask, and error-diffusion dither engine
- [`cpp/protocol.hpp`](cpp/protocol.hpp) — MeshCore Companion framing (`0x3C`/`0x3E`), over-the-air packet dissector, and 8-slot LRU manager
- [`cpp/transport.hpp`](cpp/transport.hpp) — Multi-threaded USB Serial, CoreBluetooth / WinRT BLE NUS GATT client, and Live RF Simulator
- [`cpp/macos_d2d_shim.hpp`](cpp/macos_d2d_shim.hpp) — Native Cocoa / UIKit / CoreGraphics / CoreText / ImageIO translation layer for macOS Sonoma+ and iOS 15+
- [`cpp/android_jni.cpp`](cpp/android_jni.cpp) & [`android/src/com/meshcore/plusplus/MeshCoreActivity.java`](android/src/com/meshcore/plusplus/MeshCoreActivity.java) — Android C++20 JNI bridge and BLE NUS mobile UI (`API 21+`)

### Compile on Windows (LLVM-MinGW / MinGW-w64)
```powershell
clang++ -O2 -std=c++20 -mwindows -Icpp cpp/main_gui.cpp cpp/resource.o -o releases/MeshCore++-Windows-GUI.exe `
  -ld2d1 -ldwrite -lwindowscodecs -lole32 -luuid -lwinhttp -lws2_32 -liphlpapi -ldwmapi -luxtheme `
  -lgdi32 -luser32 -lshell32 -lcomdlg32 -lsetupapi -static
```

### Compile on macOS Sonoma+ (Xcode Command Line Tools)
```bash
clang++ -O2 -std=c++20 -Icpp cpp/main_gui.cpp -o MeshCore++ \
  -framework AppKit -framework Foundation -framework CoreGraphics \
  -framework CoreText -framework CoreFoundation -framework ImageIO \
  -framework IOKit -framework CoreBluetooth -lobjc
```

### Run Cryptographic & Protocol Unit Tests
```bash
clang++ -O2 -std=c++20 -Icpp cpp/test_crypto.cpp -o test_crypto && ./test_crypto
```

---

## License

Released under the MIT License. Map tiles © [OpenStreetMap](https://www.openstreetmap.org/copyright) contributors.
