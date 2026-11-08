# 01. C++ Chuyên Sâu Cho Hệ Thống Nhúng Ô Tô (C++11/14/17) & Live Coding

> **Lưu ý định vị hồ sơ**: Trong dự án thương mại China eCall tại FPT Software, bạn sử dụng **C++11/14** (chuẩn phổ biến trong các toolchain OEM ổn định và tương thích tốt với AUTOSAR C++14 / MISRA). Trong repo mẫu `Telematic`, bạn dùng **C++17** (`std::optional`, `std::string_view`, `std::filesystem`, structured bindings). Khi được hỏi, hãy nêu rõ sự khác biệt này và nhấn mạnh việc tuân thủ toolchain/coding guideline của dự án.

---

## 1. Quản Lý Tài Nguyên An Toàn Với RAII & Smart Pointers

### Bản chất kỹ thuật
Trên TCU chạy liên tục nhiều ngày (đặc biệt trên xe tải đường dài hoặc máy công trình), rò rỉ bộ nhớ (memory leak) hoặc rò rỉ **file descriptor** (socket, `/dev/spidev0.0`, SQLite handle) sẽ làm treo daemon hoặc kích hoạt watchdog reset.
* **RAII (Resource Acquisition Is Initialization)**: Gắn vòng đời tài nguyên hệ điều hành vào vòng đời đối tượng trên stack/member. Hàm hủy (`~T()`) luôn tự động đóng `fd`, nhả khóa (`std::lock_guard`, `std::unique_lock`), hoặc dừng worker thread theo đúng thứ tự ngược với lúc khởi tạo.
* **Quy tắc hủy luồng (Thread Destruction Rule)**: Các thành viên dữ liệu (`std::mutex`, queue, HAL reference) phải được khai báo **trước** `std::thread` trong class, và hàm `stop()` phải đặt cờ dừng + `notify_all()` + `join()` trước khi các thành viên dữ liệu bị hủy.
* **Phân biệt bộ ba Smart Pointers**:
  * `std::unique_ptr<T>`: Quyền sở hữu độc quyền, **zero overhead** so với con trỏ thô (khi dùng default deleter). Mặc định dùng trong 90% trường hợp (ví dụ khởi tạo `IMcuIpcHAL`, `IModemHAL` tại `src/daemons/platformd/main.cpp`).
  * `std::shared_ptr<T>`: Đếm tham chiếu nguyên tử (`atomic ref-count` trong control block). Có chi phí đồng bộ cache-line và nguy cơ vòng tham chiếu (cyclic reference). Chỉ dùng khi thực sự chia sẻ vòng đời bất đồng bộ giữa nhiều luồng (ví dụ `PendingRpc` trong `src/ipc/ipc_client.h` để nếu caller timeout rời đi sớm thì luồng `rxLoop` nhận gói trễ vẫn không ghi vào vùng nhớ đã giải phóng — tránh *use-after-free*!).
  * `std::weak_ptr<T>`: Quan sát đối tượng được quản lý bởi `shared_ptr` mà không giữ sống nó; gọi `.lock()` trước khi dùng. Rất hữu ích cho timer/event callbacks.

### 🗣️ Sample English Answer (30s)
> *"In long-running TCU daemons, I rely strictly on RAII for memory, file descriptors, sockets, and locks so error paths clean up deterministically. I default to `unique_ptr` for single ownership and reserve `shared_ptr` only when asynchronous lifetimes genuinely overlap—for example, an in-flight RPC state shared between a caller that might time out and a receiver thread. I also enforce explicit shutdown ordering so worker threads are joined before any dependency they touch is destroyed."*

---

## 2. Kiến Trúc Đa Hình, Pure Virtual HAL & Chi Phí `vtable`

### Bản chất kỹ thuật
* Trong repo (`include/hal/i_*.h`), mọi giao tiếp phần cứng được trừu tượng hóa bằng **Pure Virtual Interface** và truyền vào Orchestrator qua **Constructor Dependency Injection**.
* **Cơ chế dưới mức biên dịch**: Mỗi class có hàm `virtual` mang 1 con trỏ ẩn `vptr` (8 bytes trên ARM64) trỏ tới bảng hàm ảo `vtable` nằm ở phân vùng `.rodata`. Mỗi lời gọi tốn 1 bước gián tiếp con trỏ và khó inline hơn.
* **Đánh giá Trade-off trên TCU**:
  * Với các tác vụ điều khiển tốc độ vừa phải (gửi AT command, đọc GNSS 1–10 Hz, ghi SQLite, gửi khung SPI 100 Hz), chi phí vài nanosecond của `vtable` là **hoàn toàn không đáng kể** so với lợi ích tách biệt phần cứng để chạy Unit Test / Mock HIL trên PC.
  * Ngược lại, trong vòng lặp DSP âm thanh PCM 48 kHz hoặc ISR trên RTOS cần hiệu năng tuyệt đối và không cấp phát động, có thể dùng **Static Polymorphism (Templates / CRTP)** hoặc liên kết tĩnh lúc build.

---

## 3. Cấp Phát Bộ Nhớ Xác Định (Deterministic Memory) & Tránh Phân Mảnh Heap

### Vấn đề thực tế trên Embedded Linux / RTOS
Cấp phát/giải phóng `new`/`delete` hoặc `std::string`/`std::vector` kích thước ngẫu nhiên liên tục trên luồng thời gian thực gây:
1. **Heap Fragmentation** (phân mảnh vùng nhớ tự do làm cấp phát khối lớn thất bại dù tổng RAM trống còn nhiều).
2. **Độ trễ không xác định (Non-deterministic Latency)**: `malloc`/`free` dùng khóa nội bộ của allocator (`ptmalloc` trong glibc) và có thể gọi syscall `brk`/`mmap` gây page fault.

### Giải pháp chuẩn Automotive
* **Pre-allocation lúc khởi động**: Dùng `std::array<uint8_t, N>` cố định (như khung MSD `std::array<uint8_t, 140>` trong `include/common/msd_codec.h`) hoặc gọi `.reserve()` một lần cho buffer nhận dữ liệu.
* **Fixed-Capacity Ring Buffer / Object Pool**: Dùng hàng đợi vòng kích thước cố định cho gói CAN/GNSS thay vì `std::list` (mỗi node cấp phát rời rạc trên heap).
* **Khóa trang nhớ cho luồng khẩn cấp**: Với tiến trình `tcu-ecalld`, cấu hình `mlockall(MCL_CURRENT | MCL_FUTURE)` (cùng `LimitMEMLOCK=infinity` trong `systemd/tcu-ecalld.service`) để ngăn Linux kernel swap/evict trang mã lệnh khi hệ thống thiếu RAM.
* **`std::string_view` (C++17) / Pointer+Length (C++11/14)**: Đọc lát cắt chuỗi/buffer khi parse NMEA, HTTP header, JSON mà không copy sang chuỗi tạm. (Chú ý: `string_view` không đảm bảo kết thúc bằng `\0` và không sở hữu vùng nhớ gốc!).

---

## 4. An Toàn Bitwise, Endianness, Struct Padding & Undefined Behavior (UB)

Đây là vùng câu hỏi cực kỳ hay dùng để loại ứng viên C/C++ nhúng:

### 4.1. Sai lầm kinh điển khi xử lý khung nhị phân (CAN / SPI / J1939 / MSD)
1. **Struct Padding & Unaligned Access**:
   * Compiler tự chèn byte đệm giữa các trường để căn chỉnh địa chỉ (ví dụ `uint8_t` theo sau bởi `uint32_t` sẽ chèn 3 byte đệm).
   * Dù dùng `#pragma pack(push, 1)` hay `__attribute__((packed))`, việc ép kiểu con trỏ thô `reinterpret_cast<const PackedStruct*>(raw_buf)` trực tiếp trên mảng byte chưa căn chỉnh có thể vi phạm **Strict Aliasing Rule** trong C++ và gây **Alignment Fault (`UsageFault` / `BusFault`)** trên một số vi điều khiển ARM Cortex-M0/M3/M4 (khi truy cập lệnh `LDRD`/`LDM` không căn chỉnh).
   * **Cách làm chuẩn an toàn nhất**: Đọc/ghi từng byte rõ ràng theo Endianness bằng phép dịch bit không dấu (`uint32_t`), hoặc dùng `std::memcpy(&pod_struct, raw_buf, sizeof(pod_struct))` (trình biên dịch GCC/Clang hiện đại tự tối ưu `memcpy` kích thước hằng thành lệnh thanh ghi an toàn!).
2. **Dịch bit trên số nguyên có dấu & Integer Promotion**:
   * Trong C/C++, `uint8_t a = 0x80; (a << 24)` sẽ tự động **promote `a` lên `int` có dấu 32-bit** trước khi dịch! Dịch trái vào bit dấu của `int` là **Undefined Behavior**!
   * **Quy tắc vàng**: Luôn ép kiểu sang kiểu không dấu đủ rộng trước khi dịch: `static_cast<uint32_t>(buf[i]) << 24U`.
3. **Tràn số trước khi kiểm tra độ dài (Integer Overflow in Bounds Check)**:
   * Sai: `if (offset + chunk_len <= total_size)` $\rightarrow$ Nếu `offset = 0xFFFFFFF0` và `chunk_len = 0x20` (kiểu `uint32_t`), tổng bị tràn vòng về `0x10 <= total_size` và vượt qua chốt chặn, dẫn đến ghi tràn bộ nhớ (Buffer Overflow)!
   * Đúng: Kiểm tra `if (chunk_len <= total_size && offset <= total_size - chunk_len)`.

### 🗣️ Sample English Answer (35s)
> *"When parsing untrusted CAN, J1939, or SPI byte streams, I treat every length and offset as hostile. I verify buffer bounds using subtraction rather than addition—checking `len <= cap && offset <= cap - len`—to prevent unsigned wrap-around. I promote bytes to `uint32_t` or `uint64_t` before bit shifting so signed integer promotion never triggers undefined behavior, and I decode endianness explicitly or via `std::memcpy` rather than aliasing packed pointers over unaligned buffers."*

---

## 5. Phân Tích 5 Bài Tập Live Coding Trọng Tâm

Toàn bộ mã nguồn chạy thực tế kèm bộ test tự động cho 5 bài này đã được viết sẵn tại **[`coding_practice/interview_coding_drills.cpp`](coding_practice/interview_coding_drills.cpp)**. Hãy nắm chắc tư duy giải từng bài:

### Bài 1: Trích xuất `PGN` từ 29-bit J1939 CAN Identifier
* **Cấu trúc 29-bit ID**: `[28:26] Priority (3b) | [25] Reserved/EDP (1b) | [24] Data Page (1b) | [23:16] PF - PDU Format (8b) | [15:8] PS - PDU Specific (8b) | [7:0] SA - Source Address (8b)`.
* **Bẫy phỏng vấn**:
  1. Nếu frame đọc từ Linux `SocketCAN` (`struct can_frame`), bit 31 (`CAN_EFF_FLAG = 0x80000000U`) đang bật $\rightarrow$ phải mặt nạ `can_id & 0x1FFFFFFFU` trước!
  2. Quy tắc **PDU1 vs. PDU2**:
     * Nếu `PF < 240` (`0x00..0xEF` — **PDU1**, peer-to-peer hoặc broadcast `0xFF`): trường `PS` là **Destination Address (DA)** nên **KHÔNG thuộc PGN** $\rightarrow$ byte thấp của PGN bằng `0x00`! (Ví dụ: Request PGN `0x18EAFF01` có `PF = 0xEA = 234 < 240`, `DA = 0xFF` $\rightarrow$ `PGN = 0x00EA00`).
     * Nếu `PF >= 240` (`0xF0..0xFF` — **PDU2**, broadcast): trường `PS` là **Group Extension (GE)** nên **THUỘC PGN**! (Ví dụ: EEC1 `0x0CF00400` có `PF = 0xF0 = 240` $\rightarrow$ `PGN = 0x00F004 = 61444`).
```cpp
uint32_t extractJ1939Pgn(uint32_t raw_can_id) noexcept {
    const uint32_t id29 = raw_can_id & 0x1FFFFFFFU; // Strip SocketCAN EFF/RTR/ERR flags
    const uint32_t pf   = (id29 >> 16U) & 0xFFU;
    uint32_t pgn        = (id29 >> 8U) & 0x3FFFFU;  // [EDP | DP | PF | PS] (18 bits)
    if (pf < 240U) {
        pgn &= 0x3FF00U; // PDU1: PS is Destination Address -> zero out low byte
    }
    return pgn;
}
```

### Bài 2: Giải mã CAN Signal tổng quát (Arbitrary Bit-Length, Intel/Motorola, Signed Two's Complement)
* Công thức vật lý: $\text{Physical} = \text{Raw}_{\text{signed}} \times \text{Factor} + \text{Offset}$.
* **Điểm mấu chốt**:
  * Kiểm tra giá trị đặc biệt của J1939/K-matrix **trước khi đổi sang đơn vị vật lý** (ví dụ trên J1939 1-byte: `0xFE` = Error Indicator, `0xFF` = Not Available; 2-byte: `0xFE00..0xFEFF` = Error, `0xFF00..0xFFFF` = Not Available).
  * Mở rộng dấu (Sign Extension) cho độ dài bit bất kỳ `bit_len` (ví dụ tín hiệu góc lái 12-bit hoặc dòng pin 14-bit):
    ```cpp
    int64_t signExtend(uint64_t raw, uint8_t bit_len) noexcept {
        if (bit_len == 0 || bit_len >= 64) return static_cast<int64_t>(raw);
        const uint64_t sign_bit = 1ULL << (bit_len - 1U);
        const uint64_t mask     = (1ULL << bit_len) - 1ULL;
        raw &= mask;
        return (raw & sign_bit) ? static_cast<int64_t>(raw) - static_cast<int64_t>(1ULL << bit_len)
                                : static_cast<int64_t>(raw);
    }
    ```

### Bài 3: Hàng Đợi Telemetry Giới Hạn Dung Lượng Có Ưu Tiên & Ranh Giới ACK (Bounded Priority Queue)
* **Nêu chính sách tràn (Overflow Policy) trước khi code**: Khi đầy dung lượng `max_capacity`, gói mới có mức ưu tiên cao hơn (ví dụ `P0 eCall` hoặc `P1 SVT Alarm`) sẽ **đẩy bỏ bản ghi ưu tiên thấp nhất & cũ nhất** (`P3 Normal Telemetry`), tuyệt đối không tự động xóa sự kiện khẩn cấp; nếu toàn bộ hàng đợi đã đầy sự kiện `P0`, tăng bộ đếm `dropped_critical_count` để báo cảnh báo chẩn đoán.
* **Ranh giới xác nhận (Acknowledgment Boundary)**: Không dùng `pop()` xóa ngay khi lấy đi gửi mạng! Dùng cơ chế `peekBatch()` $\rightarrow$ gửi MQTT/HTTPS nhận ACK $\rightarrow$ mới gọi `acknowledge(event_id)`. Nếu rớt mạng hoặc mất nguồn giữa lúc đang gửi, bản ghi vẫn còn nguyên để gửi lại sau khi khởi động (kèm `event_id` chống trùng lặp phía Server).

### Bài 4: Capped Exponential Backoff Với Full Jitter (Chống Thundering Herd)
* Khi hàng ngàn xe tải/ô tô cùng đi ra khỏi hầm hoặc trạm phát sóng phục hồi sau sự cố, nếu tất cả retry cố định sau `2s, 4s, 8s` sẽ đánh sập Cloud Broker (**Thundering Herd**).
* Dùng công thức **AWS Full Jitter**: $\text{sleep\_ms} = \text{UniformRandom}(0, \min(\text{cap\_ms}, \text{base\_ms} \times 2^{\text{attempt}}))$.
* **Chống tràn số mũ**: Giới hạn `attempt` (ví dụ `std::min(attempt, 20U)`) trước khi dịch `1ULL << shift` và kẹp trần `cap_ms`. Tiêm interface RNG & Clock để Unit Test chạy trong 1 ms mà không phải `sleep` thật!

### Bài 5: Bộ Phân Tích Dòng Byte Định Khung (Stateful Stream Parser cho UART/SPI)
* Trên đường truyền UART hoặc SPI DMA thực tế, một lần gọi `read()` có thể trả về **nửa khung**, **nhiều khung dính liền**, hoặc **nhiễu rác đầu dòng** trước byte đồng bộ `0x55 0xAA`.
* Thiết kế chuẩn: Dùng buffer tích lũy giới hạn kích thước, quét tìm cặp `SOF (0x55, 0xAA)`, đợi đủ Header (6 bytes), kiểm tra `payload_len <= kMaxPayload` (nếu vượt ngưỡng $\rightarrow$ khung giả/nhiễu, dịch bỏ 1 byte sau `0x55` đầu tiên để tìm lại `SOF` kế tiếp mà không xóa sạch các byte hợp lệ phía sau!), đợi đủ `6 + payload_len + 2`, kiểm tra `CRC-16-CCITT`, rồi mới callback lên tầng trên.

