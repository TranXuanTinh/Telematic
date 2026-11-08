# 02. Embedded Linux, ARM Platforms & RTOS Cho Hệ Thống TCU

---

## 1. Tại Sao TCU Hiện Đại Dùng Kiến Trúc Lai: Embedded Linux (Cortex-A) + RTOS (Cortex-M/R)?

### Bản chất kỹ thuật
Hầu hết các TCU công nghiệp (như Qualcomm SA515M/SA525M, NXP i.MX8 + S32K144, Renesas R-Car + RH850) tách phần cứng thành **2 miền tính toán độc lập**:

| Tiêu chí | Application Processor (AP — ARM Cortex-A) | Companion MCU / Safety Island (ARM Cortex-M / Cortex-R) |
| :--- | :--- | :--- |
| **Hệ điều hành** | **Embedded Linux** (Yocto / Buildroot) | **RTOS** (FreeRTOS, ThreadX, AUTOSAR OS / OSEK) |
| **Thời gian khởi động** | `2 – 8 giây` (ROM $\rightarrow$ U-Boot $\rightarrow$ Kernel $\rightarrow$ `systemd` + Modem) | **`< 20 – 50 ms`** (nhận được khung CAN Wakeup đầu tiên ngay lập tức!) |
| **Dòng tiêu thụ khi ngủ** | Tắt nguồn hoàn toàn qua PMIC (`0 mA`) hoặc Suspend-to-RAM | Chế độ `STOP/VLPS` cực thấp (**`< 100 – 500 µA`**) trực CAN Transceiver & IMU |
| **Tính tất định (Determinism)** | Soft Real-Time (bị ảnh hưởng bởi MMU page table, cache miss, kernel IRQ) | **Hard Real-Time** (thời gian đáp ứng ngắt micro-giây xác định 100%, không dùng MMU ảo) |
| **Nhiệm vụ chính trên TCU** | Stack 4G/5G, TCP/IP, TLS/OpenSSL, MQTT/HTTPS, SOME/IP, SQLite, eCall Voice/MSD, quản lý gói OTA lớn | Quản lý nguồn PMIC, thu thập CAN/J1939 thời gian thực, bắt xung nổ túi khí SRS PWM, giám sát nghiêng xe IMU, khóa đề an toàn & Watchdog ngoài |

### 🗣️ Sample English Answer (40s)
> *"Automotive TCUs split responsibilities between an ARM Cortex-A Linux SoC and an ARM Cortex-M or Cortex-R companion MCU running an RTOS or Classic AUTOSAR. Linux gives us rich networking, TLS security, file systems, and A/B update stacks, while the MCU boots in tens of milliseconds, draws microamps during ignition-off sleep, handles deterministic CAN/J1939 wakeup and airbag crash interrupts, and supervises the Linux SoC with an external hardware watchdog. Clear SPI framing, sequence checks, and independent reset recovery at that boundary are critical."*

---

## 2. So Sánh Chuyên Sâu: RTOS vs. Embedded Linux (Câu Hỏi Cốt Lõi Trong JD)

| Khía cạnh | RTOS (FreeRTOS / AUTOSAR OS trên Cortex-M/R) | Embedded Linux (Yocto trên Cortex-A) |
| :--- | :--- | :--- |
| **Không gian bộ nhớ** | Không gian địa chỉ phẳng (Physical Address); dùng **MPU** (Memory Protection Unit) chia tối đa 8–16 vùng bảo vệ nếu cần Safety | **MMU** (Memory Management Unit) ánh xạ địa chỉ ảo riêng biệt cho từng Process; lỗi con trỏ chỉ gây `SIGSEGV` tiến trình đó, không làm sập cả hệ thống |
| **Bộ lập lịch (Scheduler)** | Preemptive Priority-based cố định: Task sẵn sàng có độ ưu tiên cao nhất **luôn luôn** chạy ngay lập tức | Mặc định **CFS** (`SCHED_OTHER` — công bằng thông lượng); cần cấu hình tường minh **`SCHED_FIFO` / `SCHED_RR`** ( và bản vá `PREEMPT_RT` nếu cần độ trễ dưới trăm $\mu s$) |
| **Ngữ cảnh Ngắt (ISR)** | ISR chạy trực tiếp trên phần cứng NVIC/GIC; chỉ làm việc tối thiểu rồi đánh thức Task qua `xQueueSendFromISR` / `EventGroup` | Tách **Top-Half** (hard IRQ cực ngắn tắt ngắt cục bộ) và **Bottom-Half** (`softirq`, `tasklet`, hoặc **Threaded IRQ** trong kernel) rồi đẩy lên User-space qua file descriptor |
| **Liên lạc nội bộ** | Queue, Ring Buffer, Semaphore, Mutex, Event Flags trong cùng RAM | Unix Domain Socket, Shared Memory (`shm_open`), Pipe, Message Queue, Netlink |

---

## 3. Lập Trình Hệ Thống Linux (Linux System Programming) Trên TCU

### 3.1. I/O Multiplexing với `epoll` (Trái tim của Event Loop)
* Tại sao dùng `epoll` thay cho `select` / `poll` trong `src/ipc/uds_broker.cpp`?
  * `select` giới hạn `FD_SETSIZE = 1024` và cả `select`/`poll` đều tốn độ phức tạp $O(N)$ copy mảng `fd` giữa User-space và Kernel mỗi lần chờ.
  * `epoll` (`epoll_create1`, `epoll_ctl`, `epoll_wait`) lưu danh sách theo dõi ngay trong cấu trúc cây đỏ-đen của Kernel và trả về đúng danh sách các `fd` thực sự có sự kiện với chi phí $O(1)$ theo số sự kiện hoạt động.
  * **Level-Triggered (LT) vs. Edge-Triggered (ET)**: Mặc định LT an toàn và dễ code đúng hơn; nếu dùng `EPOLLET`, bắt buộc dùng non-blocking `fd` và đọc vòng lặp cho tới khi nhận `EAGAIN` / `EWOULDBLOCK`, nếu không dữ liệu còn sót sẽ không bao giờ đánh thức `epoll_wait` nữa!

### 3.2. Giao Tiếp Mạng Xe Qua Linux `SocketCAN`
Nếu SoC Linux nối trực tiếp CAN Controller (hoặc qua SPI-to-CAN như MCP2518FD), Linux cung cấp giao diện socket chuẩn **`PF_CAN`** thay vì driver ký tự `/dev/canX` độc quyền:
* **Classical CAN**: `socket(PF_CAN, SOCK_RAW, CAN_RAW)` đọc/ghi `struct can_frame` (16 bytes, tối đa 8 bytes data).
* **CAN-FD**: Bật `setsockopt(..., CAN_RAW_FD_FRAMES, &enable, sizeof(enable))` và dùng `struct canfd_frame` (72 bytes, tối đa 64 bytes data, cờ `CANFD_BRS` cho Bit Rate Switch).
* **Lọc phần cứng/nhân (`CAN_RAW_FILTER`)**: Luôn cài bộ lọc ID trong kernel bằng `setsockopt(CAN_RAW_FILTER)` để tiến trình ứng dụng không bị đánh thức hàng nghìn lần mỗi giây bởi các khung CAN không liên quan!
* **ISO-TP & J1939 trong Kernel**: Nhân Linux hiện đại tích hợp sẵn `CAN_ISOTP` (`net/can/isotp.c`) và `CAN_J1939` (`net/can/j1939/`), cho phép tầng C++ gửi/nhận nguyên gói PDU 4095 bytes mà kernel tự lo chia khung `FF/FC/CF` với độ chính xác timer cấp kernel!

### 3.3. Lập Lịch Thời Gian Thực (`SCHED_FIFO`) & Bài Toán Đảo Ngược Độ Ưu Tiên (Priority Inversion)
* Trong `src/daemons/ecalld/main.cpp` và `systemd/tcu-ecalld.service`, `tcu-ecalld` được nâng lên `SCHED_FIFO` mức ưu tiên `80` (yêu cầu quyền `CAP_SYS_NICE`, không cần chạy full `root`!).
* **Nguy cơ Priority Inversion kinh điển (như vụ tàu Mars Pathfinder)**:
  1. Luồng thấp $T_L$ (`tcu-otad` / logging) giữ một `mutex` dùng chung.
  2. Luồng cao $T_H$ (`tcu-ecalld`, `SCHED_FIFO 80`) cần `mutex` đó nên bị block chờ $T_L$.
  3. Luồng trung bình $T_M$ (nén file / tải dữ liệu, không cần `mutex` kia) chiếm CPU của $T_L$, làm $T_L$ không bao giờ chạy xong để nhả khóa $\rightarrow$ **$T_H$ bị treo gián tiếp bởi $T_M$!**
* **Cách phòng chống chuẩn**:
  1. Thiết kế không chia sẻ khóa giữa luồng khẩn cấp `P0` và luồng nền (tách queue/worker riêng).
  2. Trên POSIX/RTOS, khởi tạo mutex với thuộc tính **Priority Inheritance (`PTHREAD_PRIO_INHERIT`)** để khi $T_H$ chờ khóa của $T_L$, kernel tạm thời nâng độ ưu tiên của $T_L$ lên ngang bằng $T_H$ cho đến khi nhả khóa.
  3. Tránh để luồng `SCHED_FIFO` rơi vào vòng lặp vô hạn chiếm 100% CPU (kernel có cơ chế bảo vệ `sched_rt_runtime_us` mặc định 950ms/1000ms).

### 🗣️ Sample English Answer (35s)
> *"Assigning `SCHED_FIFO` priority to an emergency thread only helps if it cannot be blocked by lower-priority work. To prevent priority inversion, I avoid sharing locks or synchronous dispatchers between real-time paths and background tasks like logging or OTA downloads. Where a shared mutex is unavoidable across real-time threads, I enable `PTHREAD_PRIO_INHERIT`, keep critical sections bounded in memory without I/O, and lock pages with `mlockall` to avoid major page faults."*

---

## 4. Bảo Mật & Quản Lý Vòng Đời Dịch Vụ Với `systemd`

Khi triển khai trên Yocto Linux (`systemd/*.service`), hội đồng phỏng vấn rất đánh giá cao kỹ sư nắm vững các cơ chế hardening thực tế:
1. **`Type=notify` & `WatchdogSec=15`**: Daemon chỉ báo `READY=1` qua `NOTIFY_SOCKET` *sau khi* đã mở xong DB, bind socket IPC và kết nối phần cứng; định kỳ gửi `WATCHDOG=1` nhưng **chỉ khi vòng lặp nghiệp vụ chính thực sự khỏe mạnh** (không ping mù quáng trong một thread timer độc lập!).
2. **Least Privilege (Nguyên tắc tối thiểu đặc quyền)**:
   * Không chạy daemon ứng dụng dưới quyền `root`! Tạo user hệ thống `tcu` và cấp quyền qua Linux Capabilities (`AmbientCapabilities=CAP_SYS_NICE CAP_NET_ADMIN`) + nhóm thiết bị (`dialout`, `spi`, `audio`).
   * Cô lập filesystem bằng `ProtectSystem=strict`, `ProtectHome=true`, `PrivateTmp=true`, `NoNewPrivileges=true`, chỉ mở ghi đúng `ReadWritePaths=/run/tcu /data`.
   * **Lưu ý quan trọng khi review repo**: Không dùng nhầm cờ `--runtime-dir /run/tcu` để ghi đè cả đường dẫn cơ sở dữ liệu SQLite bền vững! Socket tạm nằm ở `/run/tcu` (`tmpfs` trên RAM), còn hàng đợi Store-and-Forward, mã lỗi DTC và OTA journal bắt buộc nằm trên phân vùng flash bền vững `/data`!

