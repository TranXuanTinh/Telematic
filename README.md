# Automotive Telematics Control Unit (TCU) Platform (Yocto Linux & C++17)

Production-architected **Dual-Compute Domain Telematics Control Unit (TCU)** engineered for **Qualcomm SA515M (Yocto Linux)** paired with an **Automotive Companion MCU (NXP S32K144 / STM32G4)**, featuring full hardware-free verification via **Yocto `qemuarm64`** and **Host PC Mock HAL + Virtual MCU Simulation**.

---

## 🚗 Core Telematics Capabilities

1. **Emergency Call (`tcu-ecalld` — CEN EN 15722 / UN ECE R144)**
   * Real-time (`SCHED_FIFO`) preemption of cellular modem and cabin voice audio (`< 100 ms` from crash/SOS to `112` dial).
   * Standard **140-byte Minimum Set of Data (MSD)** encoding (VIN, propulsion, WGS84 fix + `N-1`/`N-2` trail, crash $\Delta V$).
   * Backup Battery (**BUB**) autonomous operation upon 12V (`KL30`) battery severance + 60-minute PSAP auto-answer callback standby.

2. **Full Automotive OTA (`tcu-otad` — ISO 24089 / UN ECE R156)**
   * **Dual-Domain Atomic Updates**: **RAUC A/B Dual-Bank RootFS** updates for Yocto Linux + **Companion MCU Dual-Bank Firmware DFU** over CRC-protected SPI.
   * Resumable HTTPS `Range` (`HTTP 206`) downloads, X.509 / Ed25519 signature & anti-rollback verification, Vehicle Safety Preconditions (`Speed == 0`, `PARK`, `IGN == OFF`, `KL30 >= 12.2V`), and post-boot BIST verification with automatic slot rollback.

3. **Stolen Vehicle Tracking (`tcu-svtd`)**
   * Automatic arming on vehicle park/lock with **Haversine / Polygon Geofencing**, 6-axis IMU tow/tilt detection, and instant **BUB failover** on main 12V wire cut.
   * Silent covert tracking (5s high-rate GNSS + Serving/Neighbor Cell-ID triangulation), encrypted SQLite store-and-forward in cellular dead-zones, and **Dual-Gated Zero-Speed Safe Immobilizer Inhibit**.

4. **Test Service & Diagnostics (`tcu-diagd` + `tcu-ctl` — ISO 14229 UDS)**
   * Startup and 30-second periodic **Built-In Self-Test (BIST)** across MCU SPI link, Modem/SIM, active GNSS antenna current, Audio Codec loopback, Flash wear, and BUB SoC/SoH.
   * UDS Server (`0x10`, `0x22`, `0x2E`, `0x19`, `0x14`, `0x31` including non-112 **Test eCall Routine**) + persistent DTC Freeze-Frame store + `tcu-ctl` HIL/SIL fault injection CLI.

---

## 🏗️ Architectural Overview

```text
                        [OEM Cloud / OTA CDN / SOC / PSAP 112]
                                          ▲
                         4G LTE / 5G VoLTE / HTTPS / MQTT mTLS 1.3
                                          ▼
+===================================================================================+
| QUALCOMM SA515M SoC (Yocto Linux — Multi-Process Isolated User Space)             |
|                                                                                   |
|  Tier 1 (Feature Daemons):  [tcu-ecalld]  [tcu-otad]  [tcu-svtd]  [tcu-diagd]     |
|                                   ▲            ▲           ▲           ▲          |
|                     Intra-SoC IPC: AF_UNIX SOCK_SEQPACKET (libtcu-ipc) / D-Bus    |
|                                   ▼            ▼           ▼           ▼          |
|  Tier 2 (Platform Daemons): [tcu-platformd (Modem/GNSS/Audio/Cloud)] [tcu-mcu-ipcd]|
|                                                ▲                         ▲        |
|  Tier 3 (Pure Virtual HAL): [Sa515m Concrete HALs]  <--or-->  [Host/QEMU Mock HAL]|
+==========================================================================|========+
                                   Inter-Processor IPC: SPI (10MHz + IRQ) / UART
                                   Binary Framed Protocol + CRC-16-CCITT
+==========================================================================v========+
| COMPANION VEHICLE MCU (NXP S32K144 / STM32G4 — or Virtual tcu-mcu-sim)            |
|  Power Master (< 0.8mA Sleep) | CAN-FD + Selective Wake | IMU | BUB | Ext Watchdog|
+==========================================================================|========+
                                   CAN 2.0B / CAN-FD (ISO 15765-2 ISO-TP)
                                          ▼
                  [Vehicle Gateway | Airbag SRS | BCM | Immobilizer | OBD-II]
```

---

## 📚 Complete Documentation Suite

| Document | Language | Scope |
|---|---|---|
| **[TCU Architecture Overview](docs/design/tcu_architecture_overview.md)** | Tiếng Việt | Complete synthesis of SA515M + MCU dual-domain design, Power FSM, 3 IPC tiers & feature mapping |
| **[System Requirements (SysRS)](docs/system_requirements/README.md)** | English | Hardware specs (SA515M, S32K144, BUB, Audio, IMU), sleep current budgets, regulatory standards |
| **[Software Requirements (SRS)](docs/software_requirements/README.md)** | English | Yocto Scarthgap/Kirkstone layers, C++17 dependencies, detailed functional specs (`FR-ECALL`, `FR-OTA`, `FR-SVT`, `FR-DIAG`) |
| **[System Design (SDD)](docs/design/system_design.md)** | English | Multi-process 3-layer architecture, E2E sequence diagrams, A/B GPT flash layout, RAM/CPU budgets |
| **[High-Level Design (HLD)](docs/design/high_level_design.md)** | English | 6 daemon contracts, `libtcu-ipc` Pub/Sub topics & RPCs, safety priority preemption rules |
| **[Software Design (SWDD)](docs/design/software_design.md)** | English | Modular C++17 directory layout, constructor DI, Pure Virtual HALs, Mock + QEMU verification |
| **[Low-Level Design (LLD)](docs/design/low_level_design.md)** | English | SPI `CRC-16-CCITT` protocol, 140-byte EN 15722 MSD, OTA/SVT FSMs, UDS DIDs/DTCs, SQLite WAL schema |
| **[Multi-Process & Threading Model](docs/design/threading_model.md)** | Tiếng Việt | 14 POSIX threads across 6 daemons, `SCHED_FIFO` real-time scheduling, dual-tier watchdog, sleep sync |
| **[System Operational Workflows](docs/design/system_workflow.md)** | Tiếng Việt | Step-by-step millisecond workflows & SLA resource matrices across all 5 operational scenarios |
| **[Setup & Verification Guide](docs/SETUP_GUIDE.md)** | Tiếng Việt | Step-by-step Host PC Mock testing, Yocto `qemuarm64` + RAUC build, and SA515M porting guide |

---

## 📁 Repository Structure

> **Project Status**: **Phase 1 & Phase 2 Complete — Full C++17 Multi-Daemon Implementation, Yocto Layer & Automated Test Suite**.

```text
├── docs/                            # Complete SysRS, SRS, SDD, HLD, SWDD, LLD, Workflow & Setup specs
├── config/
│   ├── tcu_config.yaml              # Master TCU configuration (Modem, GNSS, Cloud, eCall, OTA, SVT)
│   └── uds_dids.yaml                # ISO 14229 Diagnostic DIDs, Routine IDs & BIST thresholds
├── include/
│   ├── common/                      # Shared domain types, CRC-16/32, EN 15722 MSD codec, Haversine math
│   ├── ipc/                         # libtcu-ipc: Sequenced UDS Pub/Sub & RPC client/broker
│   ├── hal/                         # Pure virtual HAL contracts (IMcuIpc, IModem, IGnss, IAudio, IOtaBoot, ICrypto, ICloud, IStorage, IWatchdog)
│   ├── service/                     # Platform-independent domain services
│   └── app/                         # State machines & orchestrators for the 6 TCU daemons
├── src/
│   ├── common/ & ipc/ & service/    # Shared libraries
│   ├── hal/{sa515m,mock}/           # Concrete SA515M Linux HALs & Host/QEMU Mock HALs
│   ├── daemons/                     # tcu-mcu-ipcd, tcu-platformd, tcu-ecalld, tcu-otad, tcu-svtd, tcu-diagd
│   └── tools/                       # tcu-ctl (CLI fault injector & UDS client) & tcu-mcu-sim (Virtual MCU)
├── scripts/
│   └── run_mock_cluster.sh          # One-command Host PC 7-process simulation cluster launcher
├── tests/                           # Unit & multi-daemon integration test suites (6 CTest targets)
├── systemd/                         # Hardened non-root systemd service units & watchdog configs
├── yocto/meta-tcu-telematics/       # Custom Yocto layer (tcu-suite_1.0.bb, RAUC config, tcu-telematics-image.bb)
└── CMakeLists.txt                   # Unified CMake build definition
```

---

## 🚀 Quickstart Build & Verification Commands

### 1. Host PC Hardware-Free Simulation (`MockHAL` + `tcu-mcu-sim`)
```bash
mkdir -p build && cd build
cmake .. -G Ninja -DUSE_MOCK_HAL=ON -DBUILD_TESTS=ON
cmake --build .
ctest --output-on-failure
```

### 2. Yocto `qemuarm64` Dual-Bank Image Build
```bash
source poky/oe-init-build-env build-qemuarm64
# Add meta-oe, meta-networking, meta-python, meta-rauc, and meta-tcu-telematics
bitbake tcu-telematics-image
runqemu qemuarm64 tcu-telematics-image nographic slirp
```
