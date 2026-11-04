# Software Requirements Specification (SRS)
## Project: Automotive Telematics Control Unit (TCU) on Yocto Linux

---

## 1. Operating System & Yocto Linux BSP Stack

The SA515M software platform is built from source using the **Yocto Project / OpenEmbedded** build system to achieve deterministic boot time (`< 8 seconds` to eCall readiness), minimal attack surface, read-only `squashfs`/`ext4` root filesystems, and atomic A/B updates.

### 1.1 Yocto Release & Layer Architecture

| Layer | Repository / Source | Target Branch | Purpose in TCU Image |
|---|---|---|---|
| **poky** | `git://git.yoctoproject.org/poky` | `scarthgap` (5.0 LTS) / `kirkstone` (4.0 LTS) | BitBake engine, OpenEmbedded-Core, `aarch64` / `arm` cross-toolchain, `systemd` |
| **meta-qti-bsp** *(Production)* | Qualcomm Proprietary CAF / QTI SDK | OEM BSP Release | SA515M Linux kernel, QMI modem IPC drivers, TrustZone/QTEE, NAND/eMMC flash layout |
| **poky/meta-yocto-bsp** *(Emulation)* | Included in `poky` | `scarthgap` / `kirkstone` | Reference `qemuarm64` kernel (`linux-yocto`), VirtIO storage, serial, network & virtual CAN |
| **meta-openembedded** (`meta-oe`, `meta-networking`, `meta-python`) | `https://github.com/openembedded/meta-openembedded.git` | `scarthgap` / `kirkstone` | `sqlite3`, `yaml-cpp`, `spdlog`, `nlohmann-json`, `paho-mqtt-c`, `paho-mqtt-cpp`, `can-utils`, `modemmanager` |
| **meta-rauc** | `https://github.com/rauc/meta-rauc.git` | `scarthgap` / `kirkstone` | **RAUC** A/B Dual-Bank RootFS atomic update client, X.509 bundle verification, U-Boot/GRUB/ABL integration |
| **meta-security** *(Optional Hardening)* | `git://git.yoctoproject.org/meta-security` | `scarthgap` / `kirkstone` | `dm-verity` read-only rootfs integrity, TPM/HSM utilities, AppArmor/SELinux profiles |
| **meta-tcu-telematics** | Local Repository (`yocto/meta-tcu-telematics`) | `main` | Custom recipes for all TCU C++17 daemons, `libtcu-ipc`, `tcu-ctl`, `tcu-mcu-sim`, systemd targets, and `tcu-telematics-image.bb` |

---

## 2. Core C++17 Libraries & Middleware Dependencies

All TCU daemons and shared libraries are written in **ISO C++17** with strict RAII memory management and zero unhandled exceptions across IPC boundaries.

| Component / Library | Min Version | License | Architectural Tier | Role in TCU Platform |
|---|---|---|---|---|
| **ISO C++17 Toolchain** | GCC 11+ | GPLv3 w/ Runtime Exc. | All Tiers | `std::optional`, `std::variant`, `std::string_view`, `std::filesystem`, `<chrono>` |
| **POSIX Sockets & `epoll`** | Linux Kernel 5.15+ | GPLv2 | IPC (`libtcu-ipc`) | Non-blocking `AF_UNIX` (`SOCK_SEQPACKET`) intra-SoC message bus & MCU PTY/SPI polling |
| **OpenSSL (`libcrypto`/`libssl`)**| 3.0+ | Apache 2.0 | HAL / Service | mTLS 1.3 cloud authentication, SHA-256 hashing, Ed25519 / RSA-2048 OTA bundle signature verification |
| **RAUC (`rauc` D-Bus / CLI)** | 1.8+ | LGPL 2.1 | HAL (`IOtaBootHAL`) | Atomic A/B RootFS slot installation, boot-good confirmation, automatic fallback |
| **Eclipse Paho MQTT C/C++** | 1.3.12+ | EPL 2.0 | HAL (`ICloudHAL`) | Persistent mTLS MQTT v5.0 telemetry & remote command client |
| **libcurl** | 8.0+ | MIT/curl | HAL (`ICloudHAL`) | Resumable HTTPS `Range` (`HTTP 206`) OTA bundle downloads with TLS pinning |
| **ModemManager / libqmi** | 1.18+ | GPLv2 / LGPLv2 | HAL (`IModemHAL`) | Cellular bearer setup, signal telemetry, emergency call dialing (`112`) |
| **ALSA (`libasound2`)** | 1.2+ | LGPL 2.1 | HAL (`IAudioHAL`) | PCM voice routing between cellular modem, cabin mic, speaker, and diagnostic loopback |
| **SQLite3** | 3.38+ | Public Domain | HAL (`IStorageHAL`) | WAL-mode persistent ring-buffer for offline SVT breadcrumbs, DTC store, and OTA journal |
| **nlohmann/json** | 3.11+ | MIT | Service / IPC | Cloud MQTT JSON payloads and `tcu-ctl` diagnostic CLI output |
| **yaml-cpp** | 0.7+ | MIT | Service | Schema-validated configuration loading (`tcu_config.yaml`, `uds_dids.yaml`) |
| **spdlog** | 1.11+ | MIT | Common | Structured asynchronous logging with `systemd-journald` and ring-file sinks |

---

## 3. Functional Software Requirements

### 3.1 Inter-Processor & Intra-SoC Communication (`FR-IPC`)
* **FR-IPC-01**: `tcu-mcu-ipcd` shall maintain a full-duplex binary framed link over SPI/UART to the Companion MCU with `CRC-16-CCITT` integrity checking, monotonic `SeqNum`, and automatic retransmission (`<= 3` retries, `50 ms` timeout).
* **FR-IPC-02**: Intra-SoC communication between Linux daemons shall use `libtcu-ipc` over `AF_UNIX` `SOCK_SEQPACKET` sockets (`/run/tcu/ipc.sock`) supporting both asynchronous **Pub/Sub topics** and synchronous/async **RPC request-response** with timeout enforcement.
* **FR-IPC-03**: `tcu-mcu-ipcd` shall synchronize power state transitions (`STARTUP`, `ACTIVE`, `POST_RUN`, `PRE_SLEEP`, `SLEEP`) between the MCU and all Linux daemons, guaranteeing SQLite WAL flush prior to Linux suspend/power-off.

### 3.2 Emergency Call Service — `tcu-ecalld` (`FR-ECALL`)
* **FR-ECALL-01 (Triggers)**: `tcu-ecalld` shall trigger an emergency session within **`<= 100 ms`** upon receiving either an automatic severe crash event (`CRASH_AIRBAG_DEPLOYED` from MCU CAN/PWM + IMU confirmation) or a manual SOS button press (`>= 2000 ms` hold).
* **FR-ECALL-02 (Preemption)**: Upon eCall activation, `tcu-ecalld` shall preempt any ongoing OTA download/MCU flash, non-emergency audio playback, or background cloud uploads, and lock TCU power state in `ACTIVE` (preventing sleep).
* **FR-ECALL-03 (MSD Encoding)**: `tcu-ecalld` shall construct a standard **140-byte Minimum Set of Data (MSD)** compliant with **CEN EN 15722** containing message identifier, activation mode (auto/manual), test call flag, vehicle VIN (ISO 3779), propulsion type, UTC timestamp, last valid GNSS fix + `N-1` / `N-2` delta coordinates, vehicle direction (`0..179` in `2°` steps), and optional crash severity metadata.
* **FR-ECALL-04 (Call & Voice Handoff)**: `tcu-ecalld` shall dial emergency PSAP (`112` for live emergency, or configured test number when `test_call = true`), transmit the MSD with acknowledgement retry, unmute cabin microphone/speaker via `IAudioHAL` for bidirectional PSAP voice communication, and remain in **PSAP Callback Standby** for `60 minutes` (configurable) after PSAP call termination.
* **FR-ECALL-05 (BUB Operation)**: If main 12V (`KL30`) is lost during a crash, `tcu-ecalld` shall complete the full eCall session on the Backup Battery (BUB).

### 3.3 Full Automotive OTA Service — `tcu-otad` (`FR-OTA`)
* **FR-OTA-01 (Campaign & Resumable Download)**: `tcu-otad` shall receive OTA campaign notifications via MQTT or periodic HTTPS polling and download update bundles using HTTPS `Range` requests with SHA-256 chunk resume across ignition cycles.
* **FR-OTA-02 (Cryptographic Verification)**: `tcu-otad` shall reject any OTA bundle whose manifest signature (Ed25519 / RSA-2048 X.509 certificate chain), target hardware compatibility (`hw_compat`), anti-rollback version (`version > current_min_version`), or payload SHA-256 digest fails verification.
* **FR-OTA-03 (Vehicle Safety Preconditions)**: Installation/activation shall be blocked unless: `eCall == IDLE`, `VehicleSpeed == 0 km/h`, `Gear == PARK`, `Ignition == OFF`, and `BatteryVoltage >= 12.2V` (`BUB SoC >= 50%`).
* **FR-OTA-04 (A/B Linux System Update)**: `tcu-otad` shall invoke RAUC (`IOtaBootHAL`) to write the verified rootfs/kernel image to the inactive slot (`slot.rootfs.0` $\leftrightarrow$ `slot.rootfs.1`), switch boot order, reboot, and confirm `mark-good` **only after** `tcu-diagd` passes post-boot BIST within `90 seconds`; otherwise bootloader watchdogs shall automatically roll back to the previous working slot.
* **FR-OTA-05 (Companion MCU Firmware DFU)**: When a bundle contains an MCU firmware image, `tcu-otad` shall stream CRC-protected chunks via `tcu-mcu-ipcd` to the MCU's inactive flash bank, verify full-bank CRC32/signature, trigger atomic bootloader swap, and roll back MCU firmware if post-reset handshake fails within `5 seconds`.

### 3.4 Stolen Vehicle Tracking Service — `tcu-svtd` (`FR-SVT`)
* **FR-SVT-01 (Auto-Arming & Geofence)**: `tcu-svtd` shall automatically transition to `ARMED` upon `Ignition == OFF` and `Doors == LOCKED`, locking the parked GNSS anchor coordinate $(\phi_0, \lambda_0)$ with configurable circular/polygon geofence radius (default `100 m`).
* **FR-SVT-02 (Theft & Tamper Detection)**: While `ARMED`, `tcu-svtd` shall enter `STOLEN_ACTIVE` upon: (a) unauthorized towing/tilt (`IMU tilt > 3.0°` or sustained motion without authorized key/immobilizer unlock), (b) geofence breach (`Haversine distance > 100 m` with `AuthorizedDriver == false`), (c) main 12V battery cut (`KL30 < 6.0V` triggering BUB switchover), or (d) authenticated `SVT_ACTIVATE` command from OEM Cloud SOC.
* **FR-SVT-03 (Covert Tracking & Store-and-Forward)**: In `STOLEN_ACTIVE`, TCU shall suppress all cabin indicators (silent/stealth mode), publish encrypted GNSS + Cellular serving/neighbor Cell-ID telemetry every `5 seconds` (moving) or `60 seconds` (stationary on BUB), and buffer up to `50,000` encrypted location breadcrumbs in SQLite when cellular coverage is unavailable.
* **FR-SVT-04 (Safe Immobilizer Inhibit)**: Upon receiving a cryptographically signed `IMMOBILIZE_ENGAGE` command from OEM Cloud SOC, `tcu-svtd` shall **never** command fuel/ignition cut while `VehicleSpeed > 0 km/h`; instead it shall queue `IMMOBILIZE_PENDING` and send `SET_STARTER_INHIBIT` to BCM via MCU **only when** `VehicleSpeed == 0 km/h` and `Ignition == OFF` for `>= 3 seconds`.

### 3.5 Test Service & Diagnostics — `tcu-diagd` (`FR-DIAG`)
* **FR-DIAG-01 (Startup & Periodic BIST)**: `tcu-diagd` shall execute startup BIST within `5 seconds` of boot and periodic health checks every `30 seconds` covering MCU IPC latency, Modem/SIM readiness, GNSS antenna health, eCall Audio Codec loopback, Flash free space, A/B slot status, CPU temperature, and BUB SoC/SoH. If any eCall-critical check fails, `tcu-diagd` shall command the MCU to illuminate the cabin eCall Red Fault LED and log a persistent DTC.
* **FR-DIAG-02 (UDS ISO 14229 Handler)**: `tcu-diagd` shall process UDS requests bridged from CAN (`0x7E0` / `0x767`) or local socket: `0x10` Session Control, `0x22`/`0x2E` Read/Write DID, `0x19`/`0x14` Read/Clear DTC Information, and `0x31` Routine Control (including **Routine `0x0201`: Trigger Test eCall**, **Routine `0x0202`: Audio Loopback Self-Test**, **Routine `0x0203`: SVT Tamper Self-Test**).
* **FR-DIAG-03 (CLI Fault Injection & HIL/SIL Harness)**: The `tcu-ctl` CLI tool shall allow authorized developers/CI pipelines (disabled in locked production fused builds unless in `EOL_EXTENDED_SESSION`) to inspect live daemon states and inject deterministic synthetic faults/events (simulated airbag crash, SOS press, 12V battery cut, GNSS spoofing/movement, OTA campaign trigger).

---

## 4. Non-Functional Requirements (`NFR`)

* **NFR-PERF-01 (Cold Boot to eCall Ready)**: `< 8.0 seconds` from `KL15` / `Crash Wake` to `tcu-ecalld` operational readiness.
* **NFR-PERF-02 (Memory Footprint)**: Total RSS across all 6 TCU Linux daemons shall not exceed **`120 MB RAM`**, leaving ample headroom on a `512 MB` SA515M system.
* **NFR-REL-01 (Dual-Level Watchdog Supervision)**: Every Linux daemon shall send keepalives (`WatchdogSec=15`) to `systemd`, and `tcu-mcu-ipcd` shall send hardware watchdog heartbeats every `2 seconds` to the Companion MCU (`timeout = 10 seconds`).
* **NFR-SEC-01 (Least Privilege & Transport Security)**: Feature daemons shall run under dedicated non-root Linux users/groups (`tcu-ecall`, `tcu-ota`, `tcu-svt`, `tcu-diag`) with `ProtectSystem=strict` systemd sandboxing; all cloud traffic shall enforce **TLS 1.3** with mutual certificate authentication.
