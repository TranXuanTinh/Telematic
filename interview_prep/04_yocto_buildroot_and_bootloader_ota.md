# 04. Yocto Project, Buildroot, ARM Bootloader & Dual-Bank OTA

---

## 1. Yocto Project vs. Buildroot (Câu Hỏi So Sánh Cực Phổ Biến)

| Tiêu chí | Yocto Project / OpenEmbedded (Poky) | Buildroot |
| :--- | :--- | :--- |
| **Triết lý xây dựng** | Xây dựng một **bản phân phối Linux tùy biến hoàn chỉnh (Custom Distro)** dựa trên các gói (`.ipk` / `.rpm` / `.deb`) và hệ thống phân lớp (**Layers**). | Xây dựng trực tiếp một **Root Filesystem Image nguyên khối** thông qua hệ thống cấu hình **Kconfig (`make menuconfig`)** và Makefiles. |
| **Quản lý mở rộng & BSP Hãng Chip** | Tách biệt sạch sẽ qua các layer (`meta-qti-bsp`, `meta-nxp`, `meta-rauc`, `meta-tcu-telematics`). Dùng `.bbappend` sửa đổi gói gốc mà không đụng vào mã nguồn Poky. | Dùng `BR2_EXTERNAL` tree; đơn giản, dễ hiểu nhanh nhưng kém linh hoạt hơn khi phối hợp hàng chục layer từ nhiều nhà cung cấp Tier-1 / Silicon Vendor. |
| **Tốc độ Build & Tái sử dụng** | Lần đầu build rất nặng (1–3 giờ), nhưng có **`sstate-cache` (Shared State Cache)** và `downloads` cache cực mạnh cho đội ngũ lớn & CI/CD. | Cực nhẹ và nhanh (15–30 phút cho image tối giản), rất hợp cho board bring-up nhanh, initramfs hoặc hệ thống nhúng nhỏ gọn. |
| **Ứng dụng thực tế Automotive** | **Tiêu chuẩn vàng cho Production TCU / IVI / Telematics** (Qualcomm SA515M, NXP i.MX8, Renesas, TI) nhờ quản lý license/SBOM, vá CVE bảo mật và BSP chính hãng. | Thường dùng cho **Recovery Initramfs**, hệ thống nhúng chuyên dụng nhỏ, hoặc giai đoạn đánh giá phần cứng sớm. |

---

## 2. Kiến Thức Cốt Lõi Yocto Project Cần Nắm Vững (Tham Chiếu `yocto/meta-tcu-telematics/`)

### 2.1. Chuỗi Task của BitBake Cho Một Recipe (`tcu-suite_1.0.bb`)
Khi chạy `bitbake tcu-suite`, BitBake thực thi đồ thị phụ thuộc theo thứ tự chuẩn:
1. `do_fetch` $\rightarrow$ tải mã nguồn từ `SRC_URI` (Git commit hash cố định, tarball kèm `sha256sum`) về `DL_DIR`.
2. `do_unpack` $\rightarrow$ giải nén vào `${WORKDIR}`.
3. `do_patch` $\rightarrow$ áp các bản vá `.patch` theo thứ tự.
4. `do_configure` $\rightarrow$ chạy `cmake` (thông qua `inherit cmake` và `EXTRA_OECMAKE`) với cross-toolchain.
5. `do_compile` $\rightarrow$ biên dịch mã nguồn (`ninja` / `make`).
6. `do_install` $\rightarrow$ cài đặt binary, config, systemd service vào thư mục giả lập gốc `${D}` (ví dụ `${D}${bindir}/tcu-ecalld`, `${D}${systemd_system_unitdir}/tcu-ecalld.service`).
7. `do_package` & `do_package_write_*` $\rightarrow$ chia nhỏ từ `${D}` vào các gói con (`${PN}`, `${PN}-dbg`, `${PN}-dev`, `${PN}-doc`) theo biến `FILES:${PN}`.
8. `do_rootfs` (ở image recipe `tcu-telematics-image.bb`) $\rightarrow$ ghép tất cả gói trong `IMAGE_INSTALL` thành ảnh hệ thống và chạy `wic` tạo bảng phân vùng GPT (`.wic`).

### 2.2. Các câu hỏi bẫy về Yocto hay gặp trong phỏng vấn
* **Phân biệt `DEPENDS` vs. `RDEPENDS:${PN}`**:
  * `DEPENDS`: Phụ thuộc **lúc biên dịch (Build-time)** — cần header và thư viện liên kết trong `recipe-sysroot` (ví dụ `openssl sqlite3 yaml-cpp spdlog curl`).
  * `RDEPENDS:${PN}`: Phụ thuộc **lúc chạy trên board (Runtime)** — BitBake bắt buộc cài gói đó vào RootFS cùng với gói của bạn (ví dụ `rauc can-utils sqlite3`).
* **Làm sao sửa cấu hình Kernel, Device Tree hoặc cấu hình gói có sẵn (như RAUC) mà không sửa layer gốc?**
  * Tạo file **`.bbappend`** trong layer riêng `meta-tcu-telematics` (ví dụ `recipes-core/rauc/rauc-conf.bbappend` dùng `FILESEXTRAPATHS:prepend := "${THISDIR}/files:"` để ghi đè `system.conf`, hoặc `linux-yocto_%.bbappend` kèm config fragment `.cfg` và bản vá Device Tree `.dts` cho SPI0, CAN controller, GPIO SOS/Airbag).
* **Cross-compilation SDK cho đội phát triển C++**:
  * Chạy `bitbake tcu-telematics-image -c populate_sdk` tạo bộ cài đặt Toolchain độc lập (chứa `aarch64-poky-linux-g++`, sysroot đầy đủ thư viện OpenSSL/SQLite/yaml-cpp và `OECMAKE_SOURCEPATH` environment script) để lập trình viên build CMake cực nhanh trên PC mà không cần cài toàn bộ Yocto.

---

## 3. ARM Bootloader, Chuỗi Khởi Động Tin Cậy (Secure Boot) & Bố Cục Flash

### 3.1. Chuỗi khởi động nhiều giai đoạn trên ARM SoC (Multi-Stage Boot Sequence)
```text
[Power-On Reset]
      │
      ▼
1. Boot ROM (Immutable mask ROM bên trong SoC)
   - Khởi tạo xung nhịp cơ bản, đọc chân Boot Mode / eFuse
   - Xác thực chữ ký số mật mã (RSA/ECDSA/Ed25519) của Bootloader cấp 1 bằng khóa Public Key Hash đốt trong OTP/eFuse!
      │
      ▼
2. SPL / Secondary Bootloader (U-Boot SPL hoặc Qualcomm SBL / ARM Trusted Firmware TF-A BL2/BL31)
   - Chạy trong SRAM nội bộ (vì DDR RAM chưa được huấn luyện!)
   - Khởi tạo PMIC và cấu hình **DDR Controller**, khởi tạo môi trường bảo mật TrustZone (OP-TEE)
   - Tải và xác thực chữ ký của U-Boot Proper vào DDR RAM
      │
      ▼
3. U-Boot Proper (Main Bootloader)
   - Đọc biến môi trường bền vững (`boot_order`, `boot_A_left`, `boot_B_left`)
   - Quyết định khởi động **Slot A** hay **Slot B** (A/B Dual-Bank Logic)
   - Tải ảnh **FIT Image (`kernel + initramfs + Device Tree .dtb`)** đã ký số, kiểm tra chữ ký FIT, kích hoạt Hardware Watchdog!
      │
      ▼
4. Linux Kernel + `dm-verity` Read-Only RootFS + `systemd`
   - Kernel mount RootFS chỉ đọc (SquashFS/ext4 được bảo vệ toàn vẹn từng khối bởi cây băm Merkle `dm-verity`)
   - Khởi chạy `/sbin/init` (`systemd`) -> mount phân vùng `/factory` (RO) và `/data` (RW) -> bật các daemon TCU!
```

### 3.2. Secure Boot khác Signed OTA ở điểm nào?
* **Signed OTA**: Kiểm tra quyền phát hành và tính toàn vẹn của gói cập nhật **tại thời điểm tải và cài đặt** trước khi ghi xuống ổ nhớ.
* **Secure Boot + `dm-verity`**: Kiểm tra toàn bộ chuỗi thực thi **mỗi lần cấp nguồn khởi động** (ngăn chặn kẻ tấn công tháo chip eMMC/NAND ra nạp trộm firmware độc hại ngoại tuyến!). Hai cơ chế này bổ trợ cho nhau và cùng neo vào **Hardware Root of Trust (OTP/eFuse + HSM)**.

---

## 4. Thiết Kế Dual-Bank A/B OTA + Companion MCU DFU (Chuẩn UN ECE R156)

### 4.1. Cơ chế `bootcount` & `mark-good` giữa U-Boot và RAUC
Tham chiếu trực tiếp file [`system.conf`](../yocto/meta-tcu-telematics/recipes-core/rauc/files/system.conf) và [`tcu-dualbank-ab.wks`](../yocto/meta-tcu-telematics/wic/tcu-dualbank-ab.wks):
1. Đang chạy ở **Slot A (`rootfs.0`)**. `tcu-otad` tải gói `.raucb` (qua HTTPS `Range` có khả năng resume khi đứt mạng/tắt máy), xác thực chữ ký X.509/Ed25519 và kiểm tra **Anti-Rollback Version**.
2. Kiểm tra **Điều kiện An toàn Xe (Vehicle Safety Preconditions)** với dữ liệu **tươi (fresh)** từ CAN/MCU:
   * Xe du lịch: `Speed == 0`, `Gear == PARK`, `Ignition == OFF`, `KL30 >= ngưỡng an toàn`, `eCall == IDLE`.
   * Xe tải / Máy công trình (CV/OHW): Áp dụng profile riêng (`Wheel/Ground Speed == 0`, `Engine RPM == 0`, `PTO == Inactive`, `Parking Brake / Hydraulic Lock == Engaged`, nguồn `24V >= ngưỡng 24V`).
3. Ghi gói cập nhật trực tiếp vào phân vùng không hoạt động **Slot B (`rootfs.1` + `boot_b`)**; Slot A đang chạy hoàn toàn không bị đụng chạm.
4. Cấu hình U-Boot cho phép **Trial Boot** vào Slot B với bộ đếm giới hạn `BOOT_B_LEFT = 3` và bật **Hardware Watchdog** trước khi gọi lệnh `reboot`.
5. **Hai tầng tự phục hồi (Two-Tier Rollback)**:
   * **Lỗi mức thấp (Kernel Panic / RootFS hỏng không lên được User-space)**: Hardware Watchdog reset máy $\rightarrow$ U-Boot trừ `BOOT_B_LEFT` về `0` $\rightarrow$ tự động chuyển lại về **Slot A**!
   * **Lỗi mức ứng dụng (Boot lên được Linux nhưng mất kết nối MCU/Modem/CAN)**: `tcu-diagd` chạy **Post-Boot BIST** thất bại $\rightarrow$ không gọi `rauc status mark-good` mà chủ động kích hoạt rollback về **Slot A**! Chỉ khi BIST đạt toàn bộ tiêu chí mới commit `mark-good`.

### 4.2. Thách thức cập nhật kép (Linux SoC + Companion MCU) & Mất nguồn đột ngột
* **Câu hỏi phản biện cấp Senior**: *"Cập nhật Linux A/B cộng với nạp MCU qua SPI có tự động nguyên tử (atomic) không?"*
* **Câu trả lời chuẩn xác**: **Không tự động nguyên tử!** Nếu nạp xong MCU v1.9 rồi sụt nguồn trước khi Linux kịp chuyển sang Slot B v2.4, hệ thống sẽ tỉnh dậy ở trạng thái lệch phiên bản (**Linux v2.3 + MCU v1.9**).
* **Cách giải quyết thực tế**:
  1. Thiết kế giao thức SPI có đánh phiên bản tương thích ngược tối thiểu $N-1$, hoặc khai báo rõ ma trận tương thích trong Manifest.
  2. Companion MCU dùng **Dual-Bank Flash nội bộ** (hoặc Bootloader ROM/Flash riêng không bao giờ bị ghi đè) để nếu đứt nguồn ngay giữa lúc truyền các chunk 512-byte qua SPI, MCU Bootloader thấy CRC32 bank phụ chưa hợp lệ sẽ giữ nguyên bank chính đang chạy!
  3. Ghi nhật ký tiến trình bền vững (`fsync` xuống `/data`) tại từng mốc chuyển trạng thái để khi có điện lại, `tcu-otad` biết chính xác cần tiếp tục hay hoàn tác MCU về ảnh firmware đi kèm trong Slot A.

### 🗣️ Sample English Answer (40s)
> *"For reliable TCU updates, I combine hardware secure boot with an A/B slot scheme such as RAUC integrated with U-Boot boot counters. We authenticate the manifest first, check hardware compatibility and monotonic anti-rollback counters, stream to the inactive partition, and verify fresh vehicle-safe state—adapting conditions for passenger cars versus 24-volt off-highway machines without a PARK gear. U-Boot allows a bounded trial boot guarded by a hardware watchdog, and user space only marks the slot good after post-boot BIST confirms MCU, CAN, and modem health. Because Linux plus companion-MCU updates aren't inherently atomic, I also version the SPI protocol and test power cuts at every state transition."*

