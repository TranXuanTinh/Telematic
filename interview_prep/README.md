# 🎯 Bộ Tài Liệu Ôn Tập Phỏng Vấn: Senior Embedded Telematics Software Engineer
**Vị trí mục tiêu**: *Design, implement, and maintain embedded telematics software on TCU platforms (Embedded Linux / RTOS) for both Commercial Vehicle / Off-Highway (CV/OHW) and Passenger Car (PC) programs.*

---

## 🧭 1. Nguyên Tắc Trả Lời Cốt Lõi (Chiến Lược 3 Tầng Ngôn Ngữ)

Khi trả lời phỏng vấn cấp Senior, luôn giữ nhịp **25–45 giây** cho câu trả lời đầu tiên theo khung **4 bước**:
1. **Requirement / Constraint** (Bài toán & ràng buộc thực tế trên xe)
2. **Design / Implementation** (Giải pháp kiến trúc & kỹ thuật cụ thể)
3. **Failure Mode / Edge Case** (Chuyện gì xảy ra khi rớt mạng, sụt áp KL30, Bus-Off, race condition?)
4. **Verification** (Kiểm chứng bằng CANoe, HIL, Lauterbach, Unit/Integration Test như thế nào?)

**Tách bạch minh bạch 3 nguồn kinh nghiệm** (tuyệt đối không trộn lẫn prototype cá nhân với sản phẩm thương mại):
* **`"In my production work, I worked on..."`** $\rightarrow$ Kinh nghiệm thực chiến trong CV:
  * **FPT Software (07/2022 – 10/2025)**: *Function Owner / Technical Lead (nhóm 2 người)* cho **China eCall trên TCU** bằng **C++11/14** (từ phân tích yêu cầu, HLD/LLD, code, test đến bảo trì & làm việc trực tiếp với khách hàng); dự án **Thermal Systems** (công tác Hàn Quốc) và **ADAS Classic AUTOSAR (C)**.
  * **HCLTech (09/2026 – Nay)**: **Commercial Vehicle (ComVeh) Basic Software** trên Classic AUTOSAR — đọc **K-matrix**, cấu hình **CAN signal mapping**, kiểm chứng giao tiếp bằng **Vector CANoe** và **ECU WorkX**.
  * **Embedded Linux Edge**: Dự án Drone trên Linux (C++/Python, ROS, xử lý dữ liệu cảm biến đa tiến trình).
* **`"In my Telematic reference repository, I implemented..."`** $\rightarrow$ Kiến trúc mẫu trong repo này (6 Linux Daemons C++17, `AF_UNIX SOCK_SEQPACKET`, Pure Virtual HAL, Dual-Bank OTA workflow, SQLite WAL AES-256-GCM, Virtual MCU SPI CRC-16).
* **`"For a production CV/OHW and Passenger Car TCU, I would..."`** $\rightarrow$ Nhận diện thẳng thắn khoảng cách từ prototype lên sản phẩm thực tế (ví dụ: thay file-spool bằng Paho/mosquitto MQTT mTLS thật, chuẩn hóa ASN.1 UPER EN 15722 cho EU eCall, kiểm tra tính tươi mới `signal freshness` trước khi cho phép OTA/Immobilizer, bổ sung adapter J1939 `PGN/SPN/DM1` cho xe thương mại 24V).

---

## 🚛 2. Bảng Đối Chiếu Thiết Kế: Commercial Vehicle / Off-Highway (CV/OHW) vs. Passenger Car (PC)

Điểm ăn điểm lớn nhất của JD này là khả năng thiết kế **một nền tảng TCU dùng chung (Shared Core Platform)** nhưng cấu hình hóa được cho cả **Xe tải / Máy công trình (CV/OHW)** và **Xe du lịch (Passenger Car)**:

| Khía cạnh kỹ thuật | Commercial Vehicle & Off-Highway (CV/OHW) | Passenger Car (PC) | Cách thiết kế Kiến trúc TCU Dùng Chung |
| :--- | :--- | :--- | :--- |
| **Mạng xe & Giao thức** | **SAE J1939** (29-bit CAN ID, 250/500 kbps hoặc J1939-22 CAN-FD), OEM CAN, **ISOBUS (ISO 11783)** cho máy nông nghiệp, chuẩn **FMS** | **CAN / CAN-FD** theo K-matrix/DBC riêng của OEM, **Automotive Ethernet (SOME/IP, DoIP)**, LIN | Tách lớp `IVehicleBusAdapter` (`CanSignalAdapter`, `J1939Adapter`, `SomeIpAdapter`) xuất ra chuẩn nội bộ `NormalizedSignal {id, value, quality, timestamp_mono}` |
| **Chẩn đoán lỗi (DTC)** | **J1939 DM1** (lỗi đang kích hoạt), **DM2** (lỗi lịch sử), mã lỗi gồm **`SPN` + `FMI` + `OC`** truyền qua BAM / RTS-CTS | **ISO 14229 UDS** (`0x19`, `0x14`, `0x22`, `0x2E`, `0x31`) qua **ISO-TP (ISO 15765-2)** hoặc **DoIP**; mã DTC 3-byte + 1-byte Status Mask | Chuẩn hóa mô hình `FaultEvent` ở tầng biên để đẩy lên Cloud kèm ngữ cảnh ECU nguồn (`Source Address` hoặc `Physical CAN ID`) |
| **Nguồn điện & Môi trường** | Hệ điện **24V** (máy phát 28V, sụt áp đề máy sâu, tải cảm kháng lớn), rung xóc mạnh, bụi/nhiệt khắc nghiệt | Hệ điện **12V** (kèm 48V mild-hybrid hoặc HV trên EV); ràng buộc cực gắt về dòng rò khi đỗ lâu (`< 1–3 mA`) | Không hard-code ngưỡng áp (như `12.2V`); đưa vào **Vehicle Profile Config** do Companion MCU (RTOS) giám sát qua ADC |
| **Chu kỳ vận hành (Duty Cycle)** | Chạy ca dài 10–18h/ngày, nổ máy đứng yên chạy **PTO** (cẩu, trộn bê tông, thủy lực), vùng mỏ/rừng mất sóng dài ngày (72h+) | Chuyến đi ngắn đô thị, đỗ hầm chung cư, nhiều chu kỳ Wake/Sleep, yêu cầu phản hồi nhanh từ App người dùng (Remote Lock/HVAC) | Hàng đợi **Store-and-Forward** ưu tiên hóa theo dung lượng lớn cho CV/OHW; tối ưu **Selective Wake & Fast Sleep** cho PC |
| **Điều kiện An toàn (OTA / Actuation)** | Máy công trình có thể **không có số `PARK`** (dùng phanh tay thủy lực, khóa tay trang *Hydraulic Lockout*, `Engine RPM == 0`, `PTO == OFF`) | Kiểm tra `Gear == PARK`, `Wheel Speed == 0`, `EPB Engaged`, `Ignition == OFF`, `eCall == IDLE` | Tách biểu thức điều kiện an toàn thành **Policy Engine theo Vehicle Variant** kèm kiểm tra độ tươi dữ liệu (`Freshness / Validity`) |

---

## 📂 3. Cấu Trúc Thư Mục Ôn Tập (`interview_prep/`)

Hãy học lần lượt theo thứ tự ưu tiên hoặc tra cứu nhanh theo từng mảng kỹ thuật trong JD:

1. **[`01_cpp_embedded_and_coding.md`](01_cpp_embedded_and_coding.md)**
   * **C++11/14/17 cho Embedded Automotive**: RAII, Smart Pointers, Move Semantics, Memory Alignment/Padding, tránh Undefined Behavior & phân mảnh Heap, Pure Virtual HAL & Dependency Injection, phân tích 5 bài Live Coding trọng tâm.
2. **[`02_embedded_linux_arm_and_rtos.md`](02_embedded_linux_arm_and_rtos.md)**
   * **ARM Architecture, Embedded Linux & RTOS**: Kiến trúc 2 miền (ARM Cortex-A chạy Linux + ARM Cortex-M/R chạy RTOS/Classic AUTOSAR), User/Kernel space, SocketCAN, SPI/UART, `epoll`, `systemd` service & watchdog, lập lịch thời gian thực `SCHED_FIFO`, Priority Inversion (Priority Inheritance) và khác biệt cốt lõi RTOS vs. Linux.
3. **[`03_multithread_and_multiprocess_ipc.md`](03_multithread_and_multiprocess_ipc.md)**
   * **Multi-Thread & Multi-Process Concurrency**: Khi nào tách Process vs. Thread, so sánh các cơ chế IPC (`AF_UNIX SOCK_SEQPACKET`, Shared Memory, Pipe, D-Bus, SOME/IP), `std::mutex` vs `std::atomic`, chống Deadlock, và bài học thực tế từ lỗi Head-of-Line Blocking trong repo TCU.
4. **[`04_yocto_buildroot_and_bootloader_ota.md`](04_yocto_buildroot_and_bootloader_ota.md)**
   * **Yocto Project, Buildroot, Bootloader & Dual-Bank OTA**: BitBake workflow, Layers, `.bb`/`.bbappend`, WIC GPT layout, so sánh Yocto vs. Buildroot; chuỗi khởi động ARM (`ROM -> SPL/TF-A -> U-Boot -> Kernel -> RootFS`), Secure Boot, U-Boot `bootcount` + RAUC `mark-good`, nạp DFU cho MCU, xử lý mất nguồn giữa chừng & chuẩn UN R155/R156.
5. **[`05_cloud_connectivity_http_and_mqtt.md`](05_cloud_connectivity_http_and_mqtt.md)**
   * **HTTP/HTTPS, MQTT & Store-and-Forward**: Quy trình gỡ lỗi kết nối Modem 4G/5G từ SIM $\rightarrow$ APN $\rightarrow$ DNS $\rightarrow$ mTLS; khi nào dùng HTTPS (`Range` download) vs. MQTT (`QoS 0/1/2`, Keep-Alive, LWT, Persistent Session); tại sao `QoS 2` không thay thế được Idempotency; thiết kế SQLite WAL chống mất dữ liệu khi sụt nguồn & Exponential Backoff có Jitter.
6. **[`06_diagnostics_dtc_uds_j1939_cv_pc.md`](06_diagnostics_dtc_uds_j1939_cv_pc.md)**
   * **DTC, UDS (ISO 14229), ISO-TP & SAE J1939**: Khung truyền ISO-TP (`SF`, `FF`, `FC`, `CF`), các dịch vụ UDS trọng tâm trên TCU, vòng đời DTC chuẩn (Debounce, 8-bit Status Mask, Freeze-Frame, Aging/Healing), giải mã J1939 29-bit CAN ID (`PDU1` vs `PDU2`, `PGN`, `SPN`, `BAM` vs `RTS/CTS`, `DM1`/`DM2`).
7. **[`07_someip_and_automotive_ethernet.md`](07_someip_and_automotive_ethernet.md)**
   * **SOME/IP & Automotive Ethernet**: Kiến trúc hướng dịch vụ (SOA) trên xe hiện đại, định dạng header SOME/IP, `SOME/IP-SD` (Offer/Find/Subscribe), Methods (`Request/Response`, `Fire&Forget`), Events vs. Fields, Serialization & E2E Protection, tích hợp `vsomeip` trên TCU làm cầu nối CAN/Cloud $\leftrightarrow$ Ethernet.
8. **[`08_vector_canoe_jtag_lauterbach_debugging.md`](08_vector_canoe_jtag_lauterbach_debugging.md)**
   * **Công Cụ Thực Chiến Phòng Lab**: **Vector CANoe/CANalyzer** (검증 K-matrix/DBC, viết CAPL giả lập nút mạng & tiêm lỗi CRC/Timeout, Diagnostic Console), **Bus Analyzers & Oscilloscope** (chẩn đoán CAN Bus-Off, đo trở đầu cuối $60\,\Omega$, bắt khung SPI), **JTAG/SWD & Lauterbach Trace32** (đọc thanh ghi lỗi ARM `CFSR`/`HFSR`/`BFAR`, giải mã stack `MSP`/`PSP`, đặt Hardware Watchpoint bắt lỗi ghi đè bộ nhớ, viết script `.cmm`) và gỡ lỗi Linux (`gdbserver`, `perf`, `strace`, ASAN/TSAN).
9. **[`09_repo_walkthrough_and_senior_qa_120.md`](09_repo_walkthrough_and_senior_qa_120.md)**
   * **Tổng Hợp Chiến Thuật Bảo Vệ Repo, Khung STAR Cá Nhân & Bộ Câu Hỏi Tần Suất Cao Nhất**: Bảng ánh xạ trực tiếp từ code trong repo sang câu hỏi phản biện của interviewer, khung điền câu chuyện STAR từ dự án FPT eCall & HCLTech ComVeh, và checklist câu hỏi ngược lại cho hội đồng tuyển dụng.
10. **[`coding_practice/interview_coding_drills.cpp`](coding_practice/interview_coding_drills.cpp)**
    * File C++17 độc lập, biên dịch và chạy trực tiếp trên máy (`g++ -std=c++17 -O2 -Wall -Wextra -pthread interview_prep/coding_practice/interview_coding_drills.cpp -o /tmp/drills && /tmp/drills`), cài đặt trọn vẹn 5 bài tập coding thường gặp nhất trong vòng phỏng vấn kỹ thuật Telematics.

---

## 🗓️ 4. Lộ Trình Ôn Tập Nước Rút 5 Ngày Trước Phỏng Vấn

| Ngày | Tài liệu trọng tâm | Mục tiêu đầu ra (Tự nói thành tiếng không nhìn tài liệu) |
| :--- | :--- | :--- |
| **Ngày 1** | `README.md` + `09_repo_walkthrough...` + `01_cpp...` | Thuần thục bài giới thiệu 60s; trình bày rành mạch vai trò eCall FO (FPT) + ComVeh CANoe (HCLTech) + kiến trúc Repo; tự code lại 5 hàm trong `interview_coding_drills.cpp`. |
| **Ngày 2** | `02_embedded_linux...` + `03_multithread...` | Giải thích thuyết phục tại sao tách Linux SoC + RTOS MCU; phân biệt Process vs. Thread, Mutex vs. Atomic, Priority Inversion, và cách tránh nghẽn kênh khẩn cấp eCall. |
| **Ngày 3** | `04_yocto_buildroot...` + `05_cloud_connectivity...` | Nắm chắc luồng Yocto recipe $\rightarrow$ U-Boot/RAUC A/B OTA + rollback khi lỗi BIST; trình bày rõ MQTT QoS/Idempotency, HTTPS Range download và thiết kế Store-and-Forward chịu mất điện. |
| **Ngày 4** | `06_diagnostics_dtc...` + `07_someip...` | Tính nhẩm PGN từ 29-bit CAN ID trong 10 giây; giải thích ISO-TP, vòng đời DTC 8-bit status mask, khác biệt UDS vs. J1939 DM1, và luồng hoạt động của `SOME/IP-SD`. |
| **Ngày 5** | `08_vector_canoe...` + Mock Interview 45 phút | Trình bày quy trình debug thực tế với Vector CANoe (CAPL, Bus-Off), Logic Analyzer (SPI/CAN) và Lauterbach Trace32 (bắt HardFault/Memory corruption); hoàn thiện 4 câu chuyện STAR thật. |

