# System Requirements Specification (SysRS)
## Project: Automotive Telematics Control Unit (TCU) on Yocto Linux (Qualcomm SA515M + Companion MCU)

---

## 1. System Overview & Target Environments

This System Requirements Specification defines the hardware, electrical, power, peripheral, safety, and build-host requirements for a Dual-Compute Domain **Automotive Telematics Control Unit (TCU)** supporting four core capabilities:
1. **Emergency Call (eCall / NG-eCall)** compliant with CEN EN 15722 and UN ECE R144.
2. **Full Automotive Over-The-Air (OTA) Updates** (Dual-Bank A/B Linux RootFS + Companion MCU Firmware DFU).
3. **Stolen Vehicle Tracking (SVT)** with Backup Battery (BUB) failover, IMU tamper/tow sensing, geofencing, and safe immobilizer inhibit.
4. **Test Service & Diagnostics** supporting Built-In Self-Test (BIST), UDS (ISO 14229), and automated Hardware/Software-in-the-Loop fault injection.

To support both industrial production design and hardware-free continuous verification, two execution targets are specified:
* **Production Target Hardware**: Qualcomm Snapdragon Automotive **SA515M SoC** + **NXP S32K144 (or STM32G4) Automotive Companion MCU**.
* **Emulation & Verification Target**: **Yocto `qemuarm64`** (dual-bank GPT storage + virtual network/serial/CAN) and **Host PC Native x86_64 Mock Mode** (`MockHAL` + `tcu-mcu-sim`).

---

## 2. Production Hardware Requirements

### 2.1 Domain 1 — Application & Connectivity SoC (Qualcomm SA515M)
* **Processor Core**: ARM® Cortex®-A7 application processor @ up to 1.5 GHz (32-bit ARMv7-A in native SA515M; modeled as 64-bit ARMv8-A under `qemuarm64` for portable Yocto verification).
* **Cellular Baseband Modem**:
  * Integrated 3GPP Release 15 LTE Cat-19 / 5G Sub-6 (variant-dependent) with 2G/3G CS fallback and **VoLTE / IMS Emergency Call** support.
  * Dual SIM architecture: Embedded Automotive **eUICC (eSIM, MFF2 AEC-Q100 Grade 2)** + optional diagnostic SIM interface.
* **GNSS Receiver**:
  * Integrated multi-constellation concurrent GNSS engine (**GPS, Galileo, GLONASS, BeiDou, QZSS**).
  * Update rate: configurable 1 Hz (normal) up to 10 Hz (active eCall / SVT pursuit).
  * Cold-start TTFF `< 30 s`, Hot-start TTFF `< 2 s`, horizontal accuracy `< 2.0 m` CEP.
  * Active GNSS antenna feed (3.3V LNA bias) with hardware open/short current detection circuit.
* **Memory & Non-Volatile Storage**:
  * **RAM**: Minimum **512 MB LPDDR4X** (1 GB recommended for concurrent OTA decompression and TLS buffers).
  * **Flash Storage**: Minimum **4 GB Automotive eMMC 5.1 (pSLC/MLC AEC-Q100)** or **NAND Flash with UBI**, partitioned into redundant **A/B RootFS slots**, A/B Kernel/Boot slots, factory calibration partition, and wear-leveled `/data` partition for offline telemetry and OTA staging.
* **Hardware Security**:
  * Hardware Root of Trust (Secure Boot), ARM TrustZone, hardware RNG, and secure key storage (HSM / QTEE) for X.509 mTLS device identity and OTA public verification keys.

### 2.2 Domain 2 — Companion Vehicle MCU (NXP S32K144 / STM32G4)
* **Core**: 32-bit ARM® Cortex®-M4F @ 80–112 MHz (AEC-Q100 Grade 1, `-40°C` to `+125°C`).
* **Memory**: Minimum **512 KB Dual-Bank On-Chip Flash** (supporting hardware A/B bank swap and rollback bootloader) + **64 KB SRAM** with ECC.
* **Inter-Processor Link (SA515M ↔ MCU)**:
  * **Primary High-Speed Bus**: **SPI** (Full-Duplex, 10 MHz clock, DMA-backed) + dedicated `MCU_TO_SOC_IRQ` and `SOC_TO_MCU_WAKE` GPIO lines.
  * **Secondary / Redundant Control Bus**: **UART** (921,600 bps, 8N1, RTS/CTS hardware flow control) for early boot handshake, wakeup reason reporting, and emergency fallback.
* **In-Vehicle Network Interfaces**:
  * **Primary Vehicle CAN**: **CAN-FD** (ISO 11898-1:2015, 500 kbps nominal / 2 Mbps data) connected to Vehicle Central Gateway, with ISO 11898-2 **Selective Wakeup** transceiver (e.g., TJA1145).
  * **Secondary Chassis/Safety CAN**: High-speed CAN 2.0B (500 kbps) connected to Airbag/SRS ECU and Body Control Module (BCM).
* **Discrete I/O & Sensors Supervised by MCU**:
  * **KL15 Ignition Input**: Opto-isolated/filtered digital wake input.
  * **Airbag Crash Input**: Dedicated hardware PWM / discrete crash pulse input (redundant to CAN crash message).
  * **eCall SOS Button & Status LED**: Debounced SOS push-button input + bi-color Green/Red readiness LED driver with open/short load diagnostics.
  * **6-Axis MEMS IMU (I2C/SPI)**: Automotive accelerometer + gyroscope (`±16g` high-g crash sensing + ultra-low-power wake-on-motion/tilt interrupt for unauthorized towing detection).
  * **External Hardware Window Watchdog**: Supervises MCU execution; MCU independently supervises SA515M heartbeat and drives `SOC_PMIC_RESET_N`.

### 2.3 Voice Audio Subsystem (eCall Requirement)
* **Audio Codec**: Automotive I2S/PCM codec (e.g., TI TLV320AIC3104-Q1) connected to SA515M digital audio interface.
* **Transducers**: Cabin MEMS microphone with acoustic echo cancellation (AEC) & noise suppression + dedicated 8Ω / 2W emergency speaker.
* **Diagnostic Loopback**: Hardware ADC sense or codec loopback path allowing `tcu-diagd` to verify microphone bias voltage and speaker impedance without audible disturbance.

### 2.4 Power Supply, Backup Battery (BUB) & Electrical Budget
* **Main Vehicle Supply (KL30)**: Nominal `12V DC` (`9.0V` to `16.0V` normal operation; survives ISO 16750-2 cranking voltage sag down to `6.0V` and load dump pulses).
* **Backup Battery (BUB)**:
  * Dedicated `3.6V – 7.2V` LiFePO4 or NiMH high-temperature automotive backup battery (`>= 1100 mAh`).
  * **Mandatory Autonomy**: Must sustain **at least 10 minutes of active eCall voice + GNSS** (UN R144 requirement) OR **>= 24 hours of covert Stolen Vehicle Tracking** after complete severing of the main KL30 12V vehicle battery.
* **Power Consumption Targets**:

| TCU Power State | SA515M State | MCU State | Target Current @ 12V | Wakeup Latency to CAN/SOS |
|---|---|---|---|---|
| **Active Run (Call / OTA)** | Active (Modem TX + CPU) | Run | `250 mA – 650 mA` | Immediate (`0 ms`) |
| **Active Idle (Telemetry)** | Connected Idle (DRX) | Run | `60 mA – 120 mA` | `< 10 ms` |
| **Standby / Listen (SVT Armed)**| Suspend-to-RAM (Modem Paging)| VLPS / Stop | `< 3.5 mA` | `< 150 ms` |
| **Deep Sleep (Parked > 72h)** | Powered Off (`PMIC OFF`) | Deep Stop + CAN/IMU Wake | **`< 0.8 mA` (`800 µA`)** | `< 50 ms` (MCU), `< 8 s` (Linux cold boot) |

---

## 3. Environmental & Regulatory Compliance Targets

* **Operating Temperature**: `-40°C` to `+85°C` ambient (AEC-Q100 Grade 2 module / Grade 1 MCU).
* **Emergency Call Standards**: CEN EN 15722 (MSD v2/v3), EN 16072, 3GPP TS 26.267 (in-band modem) / RFC 8147 (NG-eCall over IMS), UN ECE Regulation No. 144.
* **OTA & Cybersecurity Standards**: UN ECE R155 (CSMS), UN ECE R156 (SUMS), ISO/SAE 21434, ISO 24089.
* **Diagnostics Standards**: ISO 14229-1 (UDS), ISO 15765-2 (ISO-TP over CAN).

---

## 4. Host Development & Yocto Build Machine Requirements

### 4.1 For Fast Host Mock Development & Unit/Integration Testing
* **OS**: Ubuntu 22.04 LTS or Ubuntu 24.04 LTS (`x86_64`).
* **CPU / RAM / Disk**: 4+ cores, 8 GB RAM, 10 GB free disk space.
* **Toolchain**: GCC `11+` / Clang `14+` (C++17), CMake `3.16+`, SQLite3, OpenSSL, YAML-CPP, Paho MQTT, `iproute2` (`vcan` optional).

### 4.2 For Full Yocto Linux BSP + `qemuarm64` / SA515M Image Build
* **OS**: Ubuntu 22.04 LTS (`x86_64` native installation or WSL2/Docker build container).
* **CPU**: 8+ cores / 16 threads recommended.
* **RAM**: Minimum **32 GB RAM** (with 16 GB swap).
* **Disk Space**: Minimum **250 GB free NVMe SSD** (`ext4`) for `poky`, `downloads/`, `sstate-cache/`, and dual-bank `.wic` images.
* **Emulation Runtime**: `qemu-system-aarch64` (`runqemu qemuarm64 nographic slirp`) with virtual block device for RAUC A/B rootfs testing.
