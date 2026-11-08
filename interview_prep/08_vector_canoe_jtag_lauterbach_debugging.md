# 08. Công Cụ Thực Chiến Phòng Lab: Vector CANoe/CANalyzer, Bus Analyzers & JTAG/Lauterbach Trace32

> **Điểm mạnh thực tế của bạn**: Công việc hiện tại tại **HCLTech (ComVeh Basic Software)** làm việc trực tiếp với **K-matrix, CAN signal mapping, Vector CANoe và ECU WorkX**, kết hợp kinh nghiệm gỡ lỗi nhúng C/C++. Hãy trình bày phần này thật tự tin và cụ thể từng thao tác!

---

## 1. Vector CANoe & CANalyzer (Kiểm Chứng Mạng Xe, Restbus & CAPL)

### 1.1. Phân biệt nhanh CANalyzer vs. CANoe
* **CANalyzer**: Chuyên về **quan sát, phân tích lưu lượng, lọc bản tin, đồ thị hóa tín hiệu và phát khung cơ bản** (IG Block) trên bus thật.
* **CANoe**: Môi trường **Mô phỏng toàn mạng (Restbus Simulation) + Kiểm thử tự động (Test Feature Set / vTESTstudio)** hoàn chỉnh. Khi chỉ có 1 mình bo mạch TCU trên bàn lab (thiếu ECM động cơ, BCM thân vỏ, SRS túi khí), CANoe giả lập toàn bộ các ECU còn lại phát đúng chu kỳ, Alive Counter, E2E CRC và phản hồi Network Management (AUTOSAR NM) để TCU hoạt động y hệt như đang gắn trên xe!

### 1.2. Quy trình chuẩn Kiểm Chứng K-Matrix & Signal Mapping Trong CANoe
1. **Đối chiếu Phiên bản Đặc tả (Database Consistency)**: Kiểm tra phiên bản file `.dbc` / `.arxml` nạp vào Simulation Setup của CANoe khớp 100% với tài liệu **K-matrix** của OEM (kiểm tra CAN ID 11-bit hay 29-bit, Classical CAN hay CAN-FD BRS, chu kỳ `Cycle Time`, byte order Intel/Motorola, `Factor`, `Offset`, giá trị khởi tạo `Init/SNA`).
2. **Kiểm tra Biên & Giá trị Đặc biệt (Boundary & Corner-Case Injection)**:
   * Không chỉ bơm 1 số đẹp ở giữa dải! Bắt buộc bơm đủ **6 trường hợp** cho mỗi tín hiệu:
     1. `Physical Min` (ví dụ `-40 °C` hoặc góc lái âm cực đại — bắt ngay lỗi thiếu mở rộng dấu *Sign Extension*!).
     2. `Physical Max` (bắt lỗi tràn kiểu biến nhận trên ECU).
     3. Giá trị **vượt qua ranh giới nhiều byte (Cross-byte boundary)** — bắt ngay lỗi cấu hình nhầm **Intel (Little-Endian)** thành **Motorola (Big-Endian)**!
     4. Giá trị **`Error Indicator`** & **`Not Available / SNA`** (ví dụ `0xFE` / `0xFF` trên J1939): xác nhận phần mềm TCU gắn cờ `Invalid` chứ không nhân `0xFF * Factor + Offset` thành vận tốc `255 km/h` giả!
     5. **Message Timeout (Ngừng phát khung quá `Timeout` quy định, ví dụ `500 ms`)**: xác nhận TCU chuyển tín hiệu sang trạng thái `Stale` và ghi nhận DTC `U01xx-87` (nếu có yêu cầu chẩn đoán).
     6. **Multiplexed Signals**: Kiểm tra đúng nhánh `Mux Selector` trước khi giải mã tín hiệu con.
3. **Lưu Bằng Chứng Tái Lập (Traceability)**: Xuất file log `.blf` / `.asc` đồng bộ thời gian với log serial/journald của TCU để đính kèm báo cáo test hoặc ticket lỗi.

### 1.3. Mẫu Script `CAPL` Thực Tế Hay Bị Hỏi Trong Phỏng Vấn
Nắm vững cấu trúc sự kiện của **CAPL** (`on start`, `on timer`, `on message`, `on key`, `on sysvar`):

```c
/* Ví dụ CAPL giả lập bản tin Tốc độ xe 100ms kèm nút bấm tiêm lỗi Timeout & CRC sai */
variables {
  msTimer tVehSpeed100ms;
  byte aliveCounter = 0;
  int  injectTimeout = 0;
  int  injectBadCrc  = 0;
}

on start {
  setTimerCyclic(tVehSpeed100ms, 100); // Chu kỳ chuẩn 100 ms theo K-matrix
}

on key 't' {
  injectTimeout = !injectTimeout; // Nhấn 't' để giả lập đứt dây / mất bản tin ECM
  write("Timeout injection = %d", injectTimeout);
}

on key 'c' {
  injectBadCrc = 1; // Nhấn 'c' để tiêm 1 khung sai E2E CRC
}

on timer tVehSpeed100ms {
  message VehSpeed_Msg msg; // Lấy định nghĩa trực tiếp từ DBC/ARXML
  if (injectTimeout) return;

  msg.VehicleSpeed = @sysvar::TestBench::TargetSpeedKmh;
  msg.AliveCounter = aliveCounter;
  aliveCounter = (aliveCounter + 1) & 0x0F;

  msg.Checksum = calcE2ECrc8(msg);
  if (injectBadCrc) {
    msg.Checksum ^= 0xFF; // Đảo bit làm hỏng CRC -> kiểm tra TCU có từ chối khung không!
    injectBadCrc = 0;
  }
  output(msg);
}
```

---

## 2. Bus Analyzers & Oscilloscope (Chẩn Đoán Lỗi Tầng Vật Lý CAN / SPI / UART)

Phần mềm nhìn thấy lỗi chưa chắc do code sai — kỹ sư nhúng giỏi phải biết kết hợp **Logic Analyzer (Saleae)** và **Oscilloscope (Máy hiện sóng)** để khoanh vùng phần cứng vs. phần mềm trong vài phút:

### 2.1. Quy trình điều tra lỗi **CAN Bus-Off** (`TEC > 255`)
1. **Hiểu trạng thái bộ điều khiển CAN**:
   * `Error Active` (`TEC < 128` và `REC < 128`) $\rightarrow$ `Error Passive` (`128..255`) $\rightarrow$ **`Bus-Off` (`TEC > 255`)**: CAN Controller tự ngắt khỏi bus để không làm tê liệt toàn xe!
2. **Kiểm tra phần mềm & cấu hình trước**:
   * Sai lệch tốc độ bit (**Bitrate / Sample Point** — thường chuẩn ô tô đặt Sample Point ở **`75% – 87.5%`**, đặc biệt quan trọng ở pha Data của CAN-FD 2 Mbps / 5 Mbps!).
   * Gửi nhầm khung CAN-FD vào bus chỉ hỗ trợ Classical CAN (các nút cũ phát Error Frame liên tục!).
3. **Kiểm tra tầng vật lý bằng Đồng hồ vạn năng & Oscilloscope**:
   * **Đo điện trở đầu cuối (khi đã ngắt hoàn toàn nguồn xe!)**: Giữa `CAN_H` và `CAN_L` chuẩn phải đạt **`60 Ω`** (do hai điện trở đầu cuối $120\,\Omega$ mắc song song ở hai đầu trục chính). Nếu đo ra **`120 Ω`** $\rightarrow$ đứt 1 nhánh đầu cuối (gây phản xạ sóng làm méo xung); nếu đo ra **`0 Ω` hoặc vài $\Omega$** $\rightarrow$ chập hai dây `CAN_H`/`CAN_L`!
   * **Đo dạng sóng vi sai trên Oscilloscope**: Trạng thái Recessive (bit `1`): cả `CAN_H` và `CAN_L` cùng ở `~2.5V` ($\Delta V \approx 0\text{V}$); trạng thái Dominant (bit `0`): `CAN_H` lên `~3.5V`, `CAN_L` xuống `~1.5V` ($\Delta V \approx 2.0\text{V}$). Quan sát sườn xung có bị vọt lố (*ringing*), sụt nguồn transceiver `VCC = 5V`, hay chạm mass/nguồn 12V/24V không.
4. **Chính sách khôi phục Bus-Off trên ECU**: Không reset controller liên tục ở tốc độ micro-giây! Thực hiện khôi phục có trễ tăng dần (*Fast recovery `50–100 ms` cho vài lần đầu $\rightarrow$ Slow recovery `1000 ms`*) kèm ghi nhận DTC Bus-Off.

### 2.2. Bắt lỗi giao tiếp **SPI** giữa SoC Linux và Companion MCU bằng Logic Analyzer
* Kẹp 4 kênh `CS (Chip Select)`, `SCLK`, `MOSI`, `MISO` (+ chân GPIO `MCU_IRQ`):
  * Kiểm tra khớp **`CPOL` (Clock Polarity) & `CPHA` (Clock Phase)** giữa Linux `spidev` và MCU SPI Slave (lệch pha làm toàn bộ dữ liệu dịch đi 1 bit: `0x55` biến thành `0xAA` hoặc `0x2A`!).
  * Kiểm tra khoảng trễ **Setup Time từ lúc kéo `CS` xuống thấp đến xung `SCLK` đầu tiên**: nếu Linux Master bắn xung clock quá sát sườn `CS`, DMA của MCU Slave chưa kịp nạp byte đầu tiên vào thanh ghi TX FIFO làm mất byte đầu!

---

## 3. JTAG / SWD, Lauterbach Trace32 & Gỡ Lỗi Đa Tầng (MCU RTOS + ARM Linux)

### 3.1. Gỡ lỗi Companion MCU / Bootloader với **JTAG/SWD & Lauterbach Trace32**
Lauterbach (`PowerView` + `TRACE32`) là công cụ chuẩn công nghiệp tại các OEM và Tier-1 (dùng cho S32K, Tricore Aurix, RH850, STM32, Qualcomm SA515M):

1. **PRACTICE Script (`.cmm`)**: Script tự động cấu hình CPU (`SYStem.CPU S32K144`), chọn chuẩn `SWD`/`JTAG`, attach vào chip đang chạy mà không làm reset (`SYStem.Mode Attach`) để giữ nguyên hiện trường lỗi treo, và nạp bảng ký hiệu ELF không nạp đè flash (`Data.LOAD.Elf firmware.elf /NoCODE`).
2. **Giải mã lỗi Crash cứng (`HardFault` / `BusFault` / `UsageFault` trên ARM Cortex-M)**:
   Khi MCU rơi vào `HardFault_Handler`, các bước truy vết chuẩn xác trên Lauterbach/JTAG:
   * **Bước 1 — Đọc thanh ghi trạng thái lỗi hệ thống (`SCB->CFSR` tại `0xE000ED28` và `HFSR` tại `0xE000ED2C`)**:
     * `BFSR.PRECISERR = 1` + `BFARVALID = 1`: Đọc thanh ghi **`SCB->BFAR` (`0xE000ED38`)** $\rightarrow$ chỉ thẳng địa chỉ vùng nhớ không hợp lệ vừa truy cập gây lỗi!
     * `UFSR.INVSTATE = 1`: Gọi con trỏ hàm có bit 0 bằng `0` (quên Thumb bit trên Cortex-M) hoặc con trỏ hàm bị ghi đè rác!
     * `UFSR.UNALIGNED = 1` / `DIVBYZERO = 1`: Truy cập không căn chỉnh hoặc chia cho 0.
   * **Bước 2 — Lần ngược lệnh gây lỗi từ Stack Frame tự động của phần cứng**:
     * Kiểm tra bit 2 của thanh ghi `LR` (`EXC_RETURN`): nếu bằng `0` thì lúc lỗi CPU đang dùng **`MSP` (Main Stack Pointer — ISR/Kernel)**; nếu bằng `1` thì đang dùng **`PSP` (Process Stack Pointer — RTOS Task)**!
     * Đọc 8 thanh ghi phần cứng tự động đẩy lên đỉnh stack đó: `[R0, R1, R2, R3, R12, LR, PC, xPSR]` $\rightarrow$ Giá trị **`Stacked PC`** chỉ chính xác dòng code C/C++ vừa gây nổ lỗi (`Data.List <Stacked_PC>` trong Trace32)!
3. **Bắt bệnh Ghi Đè Bộ Nhớ (Memory / Stack Corruption) bằng Hardware Watchpoint**:
   * Khi một biến toàn cục hoặc canary đầu stack bị ai đó ghi đè ngẫu nhiên sau vài giờ chạy, đặt breakpoint thường không bắt được.
   * Trên Lauterbach dùng **On-Chip Data Watchpoint (`DWT` trên ARM)**: `Break.Set &my_corrupted_var /Write` (hoặc `/Write /Data.Long != 0xDEADBEEF`). CPU chạy đúng tốc độ thực tế 100% và **dừng đứng ngay tại câu lệnh hợp ngữ vừa chạm vào địa chỉ đó**!
4. **Hardware Trace (`ETM` / `ITM`) & RTOS Awareness**:
   * Nạp plugin `TASK.ORTI` (cho AUTOSAR OS) hoặc FreeRTOS awareness trong Trace32 để xem danh sách mọi Task, mức đỉnh Stack đã dùng (*Stack High-Water Mark*) và biểu đồ thời gian chuyển ngữ cảnh qua `ETM` mà không làm chậm CPU!

### 3.2. Bộ Công Cụ Gỡ Lỗi Trên Embedded Linux (ARM Cortex-A)
* **Crash (`SIGSEGV` / `SIGABRT`)**: Bật xuất core dump nén hoặc dùng **`gdbserver :2345 --attach <pid>`** trên board kết nối từ `gdb-multiarch` trên máy host kèm gói debug symbols `${PN}-dbg` sinh bởi Yocto!
* **Phát hiện sớm khi Dev/CI**: Bật cờ CMake `-fsanitize=address,undefined` (**ASAN/UBSAN**) và `-fsanitize=thread` (**TSAN**) khi chạy bộ test tích hợp trên Host PC / QEMU để diệt tận gốc *buffer overflow*, *use-after-free* và *data race* trước khi đưa xuống mạch thật!
* **Treo I/O hoặc tốn CPU bất thường**:
  * `strace -tt -p <pid>`: Xem tiến trình đang bị kẹt ở syscall nào (`futex`, `recvmsg`, `ioctl`, `fsync`).
  * `perf top` / `perf record -g`: Vẽ FlameGraph tìm hàm chiếm CPU cao nhất.
  * `ftrace` / `cyclictest`: Đo độ trễ lập lịch xấu nhất (worst-case wake-up latency) dưới tải nặng cho luồng eCall `SCHED_FIFO`.

### 🗣️ Sample English Answer (40s)
> *"My debugging workflow matches the layer where the symptom lives. For network and signal issues, I use Vector CANoe with CAPL scripts and a logic analyzer or oscilloscope—checking termination resistance, differential eye quality, sample points, and error counters for CAN Bus-Off, or `CPOL/CPHA` framing on SPI. On an ARM companion MCU or bootloader, I attach Lauterbach Trace32 via JTAG/SWD without resetting target state, inspect `CFSR`, `BFAR`, and the stacked `PC` on `MSP` or `PSP` for HardFaults, and set hardware write watchpoints to catch memory corruption at full speed. On Embedded Linux, I combine `gdbserver` with Yocto debug symbols, `strace`, `perf`, and ASAN/TSAN in CI."*

