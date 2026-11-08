# 03. Đa Luồng (Multi-Threading) & Đa Tiến Trình (Multi-Processing) Trên TCU

---

## 1. Khi Nào Tách Multi-Process, Khi Nào Dùng Multi-Thread?

### Bảng So Sánh Kiến Trúc Cho Phỏng Vấn Thiết Kế Hệ Thống

| Tiêu chí | Multi-Thread (Một tiến trình Monolithic nhiều luồng) | Multi-Process (Nhiều Daemon độc lập như trong Repo TCU) |
| :--- | :--- | :--- |
| **Cô lập lỗi (Fault Isolation)** | **Kém**: Một lỗi con trỏ null (`SIGSEGV`) ở module OTA/Parser JSON làm chết luôn cả cuộc gọi khẩn cấp eCall đang diễn ra! | **Rất mạnh**: Không gian địa chỉ ảo tách biệt qua MMU. Nếu `tcu-otad` hay `tcu-svtd` bị crash, `tcu-ecalld` và `tcu-mcu-ipcd` vẫn hoạt động bình thường; `systemd` tự khởi động lại đúng daemon bị lỗi. |
| **Bảo mật đặc quyền (Security Sandbox)** | Mọi luồng dùng chung UID, capabilities và quyền truy cập file/mạng của cả tiến trình. | Tách quyền chi tiết: chỉ `tcu-platformd` / `tcu-otad` được ra mạng ngoài; `tcu-ecalld` không mở port mạng, có `OOMScoreAdjust=-900`; chỉ `tcu-mcu-ipcd` chạm vào `/dev/spidev0.0`. |
| **Chi phí giao tiếp & RAM** | **Rất thấp**: Chia sẻ con trỏ/queue trực tiếp trong RAM (nanoseconds), tốn ít bộ nhớ. | **Cao hơn**: Tốn thêm vài MB RAM cho mỗi tiến trình, phải serialize/deserialize qua IPC (microseconds) và quản lý đồng bộ khởi động/tắt máy. |
| **Khuyến nghị thực tế cấp Senior** | Dùng **bên trong** một miền chức năng gắn kết chặt (ví dụ trong `IpcClient` tách luồng `rxLoop` đọc socket và `dispatchLoop` chạy callback). | Dùng để tách **các miền có mức độ an toàn/bảo mật khác nhau** (Safety-critical eCall vs. Untrusted Cloud/OTA vs. Hardware Bridge). Tránh chia quá vụn thành chục microservice gây nghẽn IPC! |

---

## 2. Lựa Chọn Cơ Chế IPC Trên Embedded Linux

Khi interviewer hỏi: *"Tại sao bạn chọn `AF_UNIX SOCK_SEQPACKET` thay vì TCP loopback, `SOCK_STREAM`, `SOCK_DGRAM`, Shared Memory hay D-Bus / SOME/IP?"*

1. **`AF_UNIX` + `SOCK_SEQPACKET`** (Đang dùng trong `libtcu-ipc`):
   * **Ưu điểm vượt trội**: Vừa có kết nối tin cậy, bảo toàn thứ tự và phát hiện đứt kết nối ngay lập tức (`EPOLLHUP`) như `SOCK_STREAM`, vừa **bảo toàn ranh giới từng bản tin nguyên tử (Message Boundaries)** như `SOCK_DGRAM` (mỗi lệnh `send` tương ứng đúng 1 lệnh `recv`, không bị dính gói như TCP stream!). Chạy hoàn toàn trong nhân Linux không qua ngăn xếp IP và đọc được danh tính tiến trình ngang hàng qua `SO_PEERCRED` (`pid`, `uid`, `gid`).
2. **POSIX Shared Memory (`shm_open` + `mmap`) + Semaphore/Eventfd**:
   * **Khi nào dùng**: Truyền dữ liệu băng thông lớn **Zero-Copy** (ví dụ luồng âm thanh PCM, camera frame ADAS, hoặc gói ảnh OTA lớn hàng chục MB thay vì đẩy hex qua JSON RPC!).
3. **D-Bus (`systemd-bus` / `libdbus`)**:
   * Phổ biến trên Linux để nói chuyện với `ModemManager`, `NetworkManager`, `RAUC` (`de.pengutronix.rauc.Installer`), `systemd`. Tiện lợi nhưng overhead phân tích bản tin cao, không phù hợp cho vòng lặp CAN tần số cao.
4. **SOME/IP (`vsomeip`)**:
   * Tiêu chuẩn AUTOSAR khi cần thống nhất giao tiếp **cả nội bộ ECU (Intra-ECU Unix socket) lẫn liên ECU qua mạng Automotive Ethernet (Inter-ECU UDP/TCP)** với Service Discovery tự động.

---

## 3. Đồng Bộ Hóa Đa Luồng Trong C++: `mutex`, `atomic`, `condition_variable` & `volatile`

### 3.1. Phân biệt rạch ròi `std::atomic`, `std::mutex` và `volatile` (Câu hỏi kinh điển!)
* **`volatile` KHÔNG HỀ có tác dụng đồng bộ đa luồng trong C++!**
  * `volatile` chỉ báo trình biên dịch không tối ưu hóa bỏ lệnh đọc/ghi thanh ghi phần cứng ánh xạ bộ nhớ (**MMIO register**) hoặc biến trong `sig_atomic_t` signal handler. Nó **không** chống xé dữ liệu (data tearing), **không** tạo rào chắn bộ nhớ (memory barrier) giữa các nhân ARM (vốn có mô hình bộ nhớ yếu — *weakly-ordered memory model*!), và gây **Data Race (UB)** nếu dùng thay `atomic`!
* **`std::atomic<T>`**:
  * Đảm bảo thao tác đọc-sửa-ghi không chia cắt kèm đồng bộ thứ tự bộ nhớ (`memory_order_seq_cst` mặc định, hoặc cặp `memory_order_acquire` / `memory_order_release`) cho **một biến đơn lẻ** (như cờ `running_`, bộ đếm thống kê `crc_errors_`, trạng thái đơn).
* **`std::mutex` + `std::condition_variable`**:
  * Bắt buộc dùng khi cần bảo vệ **bất biến của nhiều trường dữ liệu liên quan nhau** (ví dụ: cập nhật `state_`, `pending_crash_`, `sos_press_start_ms_` cùng lúc trong `EcallOrchestrator`, hoặc thao tác trên `std::deque`/`std::map`).
  * **Quy tắc dùng `condition_variable::wait`**: Luôn truyền **Predicate** `cv.wait(lk, [&]{ return !running_ || !queue_.empty(); })` để chống cả hai lỗi:
    1. **Spurious Wakeup** (hệ điều hành đánh thức giả dù không ai `notify`).
    2. **Lost Wakeup** (`notify` xảy ra trước khi luồng kia kịp vào `wait`).

---

## 4. Hai Bài Học Thiết Kế Thực Chiến Từ Repo `Telematic` (Điểm Cộng Lớn Khi Phản Biện!)

Nếu người phỏng vấn mở code của repo hoặc hỏi về các lỗi tiềm ẩn trong hệ thống đa luồng/đa tiến trình, hãy chủ động phân tích 2 tình huống cực đắt giá này:

### Tình huống A: Tách `rxLoop` và `dispatchLoop` để chống Nested RPC Deadlock (`src/ipc/ipc_client.cpp`)
* **Vấn đề**: Giả sử Daemon A nhận một RPC Request từ Daemon B, và ngay bên trong hàm xử lý RPC đó, Daemon A lại gọi một RPC đồng bộ (`callRpc`) sang Daemon C để lấy thông tin rồi mới trả lời B. Nếu hàm xử lý chạy trực tiếp trên luồng đọc socket `rxLoop`, thì `rxLoop` đang bị khóa cứng ở `cv.wait_for` chờ phản hồi từ C $\rightarrow$ chính nó không thể đọc socket để nhận gói `RPC_RESPONSE` từ C $\rightarrow$ **Self-Deadlock 100%!**
* **Cách giải quyết đã cài trong repo**: Tách `rxLoop` (chỉ đọc socket siêu nhanh, nếu thấy `RPC_RESPONSE` thì đánh thức ngay `PendingRpc` đang chờ; nếu là Request/Event thì đẩy vào `dispatch_queue_`) và `dispatchLoop` (chạy callback người dùng trên luồng riêng).
* **Giới hạn nâng cao cần nói thêm**: Nếu Daemon A gọi đồng bộ sang B trong khi B cũng đang gọi đồng bộ ngược lại A, chỉ có 1 luồng `dispatchLoop` vẫn sẽ gây **Cross-Daemon Deadlock**! Giải pháp triệt để là dùng **Asynchronous Non-blocking RPC (Callback / Coroutine / State Machine)** hoặc tách Thread Pool riêng cho Control Plane và Data Plane.

### Tình huống B: Tại sao cờ ưu tiên `IPC_FLAG_URGENT` vẫn có thể bị trễ (Head-of-Line Blocking)?
* **Phân tích sâu (`src/ipc/ipc_client.cpp` & `src/app/platform_orchestrator.cpp`)**:
  * Khi có tai nạn nổ túi khí, `tcu-ecalld` gửi lệnh `ACQUIRE_EMERGENCY_CHANNEL` với cờ `IPC_FLAG_URGENT = 1` để chèn lên **đầu hàng đợi** `dispatch_queue_.push_front(msg)` của `tcu-platformd`.
  * **NHƯNG**: Nếu đúng tích tắc trước đó, luồng `dispatchLoop` duy nhất của `tcu-platformd` vừa lấy lệnh `DOWNLOAD_HTTPS_ASSET` ra chạy đồng bộ (đang tải file OTA mất vài giây trên cùng luồng `dispatchLoop`), thì bản tin khẩn cấp dù đứng đầu hàng đợi vẫn phải chờ hàm tải file kết thúc (hoặc chờ callback tiến độ kiểm tra cờ hủy) $\rightarrow$ **Head-of-Line Blocking!**
* **Giải pháp chuẩn Production để trả lời phỏng vấn**:
  1. Không bao giờ chạy tác vụ I/O kéo dài (tải HTTP, ghi flash, quét mạng) bên trong luồng `dispatchLoop` của IPC! `dispatchLoop` chỉ xác nhận lệnh rồi giao cho một **Background Worker Thread** có cơ chế hủy tức thì (`CancellationToken` / `curl_xferinfo_callback` trả về khác 0).
  2. Thậm chí tốt hơn cho tính năng Safety (eCall): tách hẳn một **Dedicated Real-Time Socket / Control Channel** riêng cho điều khiển khẩn cấp để lưu lượng dữ liệu nền không bao giờ chia sẻ hàng đợi hay worker với đường khẩn cấp!

### 🗣️ Sample English Answer (40s)
> *"Putting an urgent flag on an IPC message only moves it to the front of the waiting queue; it cannot preempt a synchronous handler that is already executing on a single dispatch thread. For example, if a platform daemon runs a blocking HTTPS download directly inside its IPC dispatcher, an emergency eCall preemption request experiences head-of-line blocking. In production, I keep IPC dispatchers non-blocking—doing validation and state transitions in microseconds—offload slow I/O to cancellable worker threads, and isolate the emergency control path onto its own dedicated real-time channel."*

