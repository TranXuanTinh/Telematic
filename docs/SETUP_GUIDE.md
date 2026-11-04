# 📖 Hướng Dẫn Cài Đặt, Build & Kiểm Thử Hệ Thống TCU (Setup & Verification Guide)
## Dự Án: Automotive Telematics Control Unit (Yocto Linux — Qualcomm SA515M + Companion MCU)

Tài liệu này hướng dẫn chi tiết từng bước thiết lập môi trường, biên dịch, xây dựng bản phân phối Yocto Linux và kiểm thử đầu-cuối cho cả 4 tính năng (**eCall**, **Full OTA**, **Stolen Vehicle Tracking**, **Test Service**) theo 3 kịch bản triển khai:
1. **Kịch bản 1 (Nhanh nhất cho Dev & CI)**: Biên dịch CMake và chạy toàn bộ 6 Linux Daemons + **Virtual MCU Simulator (`tcu-mcu-sim`)** + **Mock HAL** ngay trên **Host PC (Ubuntu x86_64)** không cần phần cứng nhúng.
2. **Kịch bản 2 (Kiểm thử tích hợp OS & A/B OTA)**: Build hệ điều hành nhúng hoàn chỉnh bằng **Yocto Project (`scarthgap` / `kirkstone`)** với `meta-tcu-telematics` + `meta-rauc` và khởi chạy trên máy ảo **`qemuarm64`**.
3. **Kịch bản 3 (Triển khai Production)**: Đấu nối phần cứng và tích hợp BSP lên bo mạch thực tế **Qualcomm SA515M + NXP S32K144 / STM32G4 MCU**.

---

## 🔌 PHẦN 1: Sơ Đồ Kết Nối Phần Cứng Tham Chiếu (SA515M + Companion MCU)

Đối với bo mạch TCU thực tế (hoặc bộ kit phát triển gắn rời SoC + MCU), các đường tín hiệu vật lý cần đảm bảo đúng thiết kế hai miền tính toán:

```text
                       +---------------------------------------------------+
                       |           TELEMATICS CONTROL UNIT (TCU)           |
                       |                                                   |
[Ăng-ten 4G/5G] <----->| RF Main/Div    Qualcomm SA515M SoC                |
[Ăng-ten GNSS]  ------>| RF GNSS (3.3V LNA Bias + Open/Short Sense)        |
[Mic / Loa Cabin]<---->| PCM / I2S Audio Codec (AEC + Loopback Test)       |
                       |                    ▲         ▲                    |
                       |      SPI0 (10MHz)  │         │ UART_HS + IRQ GPIO |
                       |                    ▼         ▼                    |
[Nguồn 12V KL30] ----->| Power / BUB    Companion MCU (S32K144 / STM32G4)  |
[Pin dự phòng BUB]<--->| Charger/ADC    (Power Master, IMU, Dual-Bank DFU) |
[Khóa điện KL15] ----->| Wake Input                   ▲                    |
[Nút SOS / LED]  <---->| GPIO / Diagnostics           │ CAN-FD / CAN 2.0B  |
[Xung Airbag SRS]----->| PWM Crash IRQ                ▼                    |
                       +------------------------------|--------------------+
                                                      │
                                      [Vehicle Gateway / BCM / OBD-II]
```

---

## 💻 PHẦN 2: Kịch Bản 1 — Build & Kiểm Thử Trên Host PC Với Mock HAL & Virtual MCU

Kiến trúc 3 tầng (`Daemons` $\to$ `Service` $\to$ `Pure Virtual HAL`) cho phép chạy chính xác 100% mã nguồn nghiệp vụ trên máy tính Linux cá nhân.

### 1. Cài đặt gói công cụ trên Ubuntu 22.04 / 24.04 LTS
```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake ninja-build pkg-config git \
    libssl-dev libsqlite3-dev libyaml-cpp-dev \
    nlohmann-json3-dev libspdlog-dev \
    libpaho-mqtt-dev libpaho-mqttpp-dev libcurl4-openssl-dev \
    can-utils iproute2
```

### 2. Biên dịch mã nguồn & Chạy bộ kiểm thử tự động (Unit + Integration Tests)
```bash
mkdir -p build && cd build

cmake .. \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DUSE_MOCK_HAL=ON \
    -DBUILD_TESTS=ON

cmake --build .
ctest --output-on-failure
```

### 3. Khởi chạy cụm giả lập TCU trên Host PC và Test 4 Tính Năng bằng `tcu-ctl`
Khi hoàn tất triển khai mã nguồn, cụm giả lập khởi chạy qua script hoặc từng tiến trình:
```bash
# Terminal 1: Khởi chạy Virtual Companion MCU + 6 Linux Daemons ở chế độ Mock
./tcu-mcu-sim --socket /tmp/tcu/mcu_spi.sock &
./tcu-mcu-ipcd --config ../config/tcu_config.yaml --mock &
./tcu-platformd --config ../config/tcu_config.yaml --mock &
./tcu-ecalld --config ../config/tcu_config.yaml --mock &
./tcu-svtd --config ../config/tcu_config.yaml --mock &
./tcu-otad --config ../config/tcu_config.yaml --mock &
./tcu-diagd --config ../config/tcu_config.yaml --mock &
```

Sử dụng công cụ **`tcu-ctl`** trên Terminal 2 để kiểm chứng từng tính năng:
```bash
# 1. [Test Service] Chạy tự kiểm tra BIST toàn hệ thống & đọc DID pin dự phòng qua UDS 0x22
./tcu-ctl diag run-bist
./tcu-ctl uds read-did 0xFD03

# 2. [eCall] Giả lập nổ túi khí ở ΔV = 48 km/h -> Kiểm tra gói 140-byte MSD & cuộc gọi 112
./tcu-ctl inject crash --airbag front --delta-v 48

# 3. [Stolen Vehicle Tracking] Đỗ xe khóa cửa -> Giả lập kẻ gian cắt bình 12V KL30
./tcu-ctl vehicle set --speed 0 --ign off --doors locked
./tcu-ctl inject tamper --kl30-cut
./tcu-ctl svt engage-immobilizer --signed-token demo.sig

# 4. [Full OTA] Kích hoạt gói cập nhật A/B Linux + MCU Firmware
./tcu-ctl ota trigger --manifest /tmp/demo_ota_2.4.0/manifest.json
```

---

## 📦 PHẦN 3: Kịch Bản 2 — Build Hệ Điều Hành Yocto Hoàn Chỉnh Cho `qemuarm64`

Kịch bản này kiểm chứng toàn bộ Yocto recipe, `systemd` service hardening, phân quyền người dùng không phải `root`, và cơ chế cập nhật phân vùng kép **RAUC A/B RootFS**.

### 1. Cài đặt gói phụ thuộc cho BitBake (Ubuntu 22.04 LTS)
```bash
sudo apt-get update
sudo apt-get install -y \
    gawk wget git diffstat unzip texinfo gcc build-essential \
    chrpath socat cpio python3 python3-pip python3-pexpect \
    xz-utils debianutils iputils-ping python3-git python3-jinja2 \
    python3-subunit zstd liblz4-tool file locales qemu-system-arm
sudo locale-gen en_US.UTF-8
```

### 2. Tải mã nguồn Yocto Scarthgap (5.0 LTS) & Các Layer Phụ Thuộc
```bash
mkdir -p ~/yocto-tcu && cd ~/yocto-tcu

# 1. Poky (Core Yocto 5.0 LTS)
git clone -b scarthgap git://git.yoctoproject.org/poky

# 2. Meta-OpenEmbedded (SQLite, YAML, Paho MQTT, spdlog, can-utils)
git clone -b scarthgap https://github.com/openembedded/meta-openembedded.git

# 3. Meta-RAUC (Quản lý cập nhật phân vùng kép A/B Atomic Update)
git clone -b scarthgap https://github.com/rauc/meta-rauc.git
```

### 3. Khởi tạo môi trường Build và Đăng ký Layer `meta-tcu-telematics`
```bash
cd ~/yocto-tcu
source poky/oe-init-build-env build-qemuarm64
```

Thêm các layer vào `conf/bblayers.conf`:
```bitbake
BBLAYERS ?= " \
  ${TOPDIR}/../poky/meta \
  ${TOPDIR}/../poky/meta-poky \
  ${TOPDIR}/../poky/meta-yocto-bsp \
  ${TOPDIR}/../meta-openembedded/meta-oe \
  ${TOPDIR}/../meta-openembedded/meta-networking \
  ${TOPDIR}/../meta-openembedded/meta-python \
  ${TOPDIR}/../meta-rauc \
  /media/tinhtran/01D85BC599D1D460/FreeLancer/Telematic/yocto/meta-tcu-telematics \
"
```

### 4. Cấu hình `conf/local.conf`
```bitbake
MACHINE = "qemuarm64"

# Sử dụng systemd làm trình quản lý tiến trình & watchdog chính
INIT_MANAGER = "systemd"

# Bật tính năng RAUC A/B Dual-Bank và định dạng ổ đĩa GPT .wic
DISTRO_FEATURES:append = " rauc systemd usrmerge"

# Tối ưu tốc độ biên dịch
BB_NUMBER_THREADS ?= "8"
PARALLEL_MAKE ?= "-j 8"
DL_DIR ?= "${TOPDIR}/../downloads"
SSTATE_DIR ?= "${TOPDIR}/../sstate-cache"
```

### 5. Build Image Hệ Điều Hành & Gói Cập Nhật OTA Bundle
```bash
# 1. Build image hệ điều hành TCU hoàn chỉnh
bitbake tcu-telematics-image

# 2. Build gói cập nhật .raucb có chữ ký X.509 để test tính năng OTA
bitbake tcu-ota-bundle
```

### 6. Khởi chạy Máy Ảo QEMU ARM64
```bash
runqemu qemuarm64 tcu-telematics-image nographic slirp
```

---

## 🚗 PHẦN 4: Kịch Bản 3 — Porting Sang Phần Cứng Production Qualcomm SA515M

Khi chuyển từ môi trường `qemuarm64` sang SDK chính thức của nhà sản xuất module SA515M (ví dụ Quectel AG55xQ / Telit FN980m / NAD OEM dựa trên `meta-qti-bsp`):
1. Thay `MACHINE = "sa515m-nad"` trong `conf/local.conf` và thêm layer `meta-qti-bsp` của nhà cung cấp SoC.
2. Trong `meta-tcu-telematics/recipes-tcu/tcu-suite/tcu-suite_1.0.bb`, đặt cờ CMake `-DUSE_MOCK_HAL=OFF -DENABLE_SA515M_HAL=ON` để liên kết trực tiếp với `libqmi` / `ModemManager`, `/dev/spidev0.0`, ALSA PCM voice interface và phân vùng MTD/UBI hoặc eMMC của SA515M.
3. Nạp chứng chỉ gốc (Production Root CA) của hệ thống quản lý khóa OEM vào phân vùng chỉ đọc `/factory/ota-ca.cert.pem` và cấu hình khóa khách hàng mTLS trong HSM/TrustZone.

---

## 🔍 PHẦN 5: Vận Hành & Gỡ Lỗi Trên Thiết Bị (Operations & Troubleshooting)

### 1. Kiểm tra trạng thái các Daemon TCU qua `systemd`
```bash
systemctl status tcu-mcu-ipcd tcu-platformd tcu-ecalld tcu-svtd tcu-otad tcu-diagd
journalctl -u "tcu-*" -f
```

### 2. Kiểm tra phân vùng A/B OTA hiện hành qua RAUC
```bash
rauc status
```

### 3. Truy vấn hàng đợi lưu vết ngoại tuyến & Mã lỗi DTC trong SQLite
```bash
sqlite3 /data/db/tcu_persistence.db "SELECT dtc_code, status_mask, occurrence_count FROM dtc_records;"
sqlite3 /data/db/tcu_persistence.db "SELECT priority, COUNT(*) FROM telemetry_queue GROUP BY priority;"
```

### 4. Thiết lập bus CAN ảo (`vcan0`) để bắt khung UDS / CAN
```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
candump vcan0
```
