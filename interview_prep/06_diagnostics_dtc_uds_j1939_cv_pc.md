# 06. Chẩn Đoán Lỗi (DTC), UDS (ISO 14229), ISO-TP & SAE J1939 (CV/OHW vs. Passenger Car)

---

## 1. Tầng Giao Vận Chẩn Đoán ISO-TP (ISO 15765-2) Trên CAN / CAN-FD

Một khung Classical CAN chỉ chở tối đa **8 bytes**, trong khi đọc mã VIN (`0x22 0xF1 0x90`) cần `3 + 17 = 20 bytes`, đọc danh sách DTC hoặc truyền chứng chỉ cần hàng trăm tới **4095 bytes** (hoặc lên tới 4 GB với phần mở rộng 32-bit length). **ISO-TP** giải quyết bài toán phân mảnh & kiểm soát luồng thông qua byte đầu tiên gọi là **N_PCI (Protocol Control Information)**:

```text
1. Single Frame (SF - N_PCI = 0x0L, L = 1..7 trên Classical CAN):
   Tester ──► [03 | 22 F1 90 | AA AA AA AA] ──► ECU

2. Multi-Frame Transfer (FF + FC + CF):
   ECU    ──► [10 14 | 62 F1 90 'V' 'N' '1'] ──► Tester   (First Frame FF: 0x1014 -> tổng dài 0x014 = 20 bytes)
   Tester ──► [30 00 0A | 55 55 55 55 55]    ──► ECU      (Flow Control FC: CTS=0, BlockSize=0 (gửi hết), STmin=10ms)
   ECU    ──► [21 | 'T' 'C' 'U' '5' '1' '5' 'M'] ──► Tester (Consecutive Frame CF #1: SN=1)
   ECU    ──► [22 | '0' '0' '0' '0' '0' '0' '1'] ──► Tester (Consecutive Frame CF #2: SN=2, SN quay vòng 0..15)
```

* **Các tham số sống còn khi debug ISO-TP trên CANoe**:
  * **Flow Status (`FS`)**: `0` = ContinueToSend (CTS), `1` = Wait (WT), `2` = Overflow/Abort (OVFLW — bộ đệm bên nhận không đủ chỗ chứa!).
  * **Block Size (`BS`)**: Số khung `CF` được phép gửi liên tiếp trước khi phải dừng chờ gói `FC` tiếp theo (`0` = gửi liên tục không cần chờ thêm `FC`).
  * **Separation Time minimum (`STmin`)**: Khoảng nghỉ tối thiểu giữa 2 khung `CF` liên tiếp (`0x00..0x7F` tính bằng `ms`, `0xF1..0xF9` tính bằng `100..900 µs`). Nếu gửi nhanh hơn `STmin`, ECU đích bị tràn FIFO nhận và rớt khung!
  * **Timeout chuẩn (`N_As`, `N_Bs`, `N_Cr` — thường `1000 ms`)**: Nếu đang nhận dở các khung `CF` mà quá `N_Cr` không thấy khung kế tiếp hoặc sai số thứ tự `SN` (`0..F`), phải hủy phiên ghép gói ngay lập tức.

---

## 2. Giao Thức UDS (ISO 14229-1) Trọng Tâm Cho Kỹ Sư TCU

### 2.1. Các Service ID (`SID`) bắt buộc thuộc lòng (Cộng `+0x40` cho Positive Response)

| SID | Tên Dịch Vụ | Positive Resp | Ứng dụng thực tế trên TCU (`src/service/uds_diagnostics_service.cpp`) |
| :---: | :--- | :---: | :--- |
| **`0x10`** | `DiagnosticSessionControl` | `0x50` | Chuyển phiên: `0x01` Default, `0x02` Programming (Bootloader), `0x03` Extended, `0x60` Safety/EOL. Trả kèm tham số thời gian `P2` và `P2*`. |
| **`0x11`** | `ECUReset` | `0x51` | `0x01` Hard Reset, `0x03` Soft Reset (sau khi cấu hình EOL hoặc nạp xong). |
| **`0x3E`** | `TesterPresent` | `0x7E` | Giữ phiên khác Default không bị hết hạn sau **`S3_Server` timeout (`5000 ms`)**. Nếu bit 7 bật (`0x3E 0x80` — *suppressPosRspMsgIndicationBit*), ECU gia hạn timer nhưng không gửi trả `0x7E` để đỡ tốn băng thông CAN! |
| **`0x27` / `0x29`** | `SecurityAccess` / `Authentication` | `0x67` / `0x69` | `0x27`: Thử thách **Seed & Key** (lẻ yêu cầu Seed, chẵn gửi Key + khóa chống brute-force `NRC 0x36/0x37`). Trên xe đời mới theo UN R155 dùng thêm **`0x29` PKI Certificate Authentication**. |
| **`0x22`** | `ReadDataByIdentifier` | `0x62` | Đọc DID chuẩn (`0xF190` VIN, `0xF189` SW Version) và DID riêng của TCU (trạng thái ăng-ten GNSS, RSSI/IMEI Modem, SoC/SoH pin dự phòng BUB, trạng thái slot OTA). |
| **`0x2E`** | `WriteDataByIdentifier` | `0x6E` | Ghi cấu hình nhà máy (VIN, APN, vùng thị trường, bật/tắt tính năng theo variant xe) — bắt buộc kiểm tra Session + SecurityAccess + `Speed == 0`. |
| **`0x19`** | `ReadDTCInformation` | `0x59` | Sub-function hay hỏi nhất: `0x01` đếm số DTC theo mặt nạ, **`0x02` đọc danh sách DTC theo Status Mask**, `0x04` đọc **Freeze-Frame Snapshot**, `0x06` đọc Extended Data (số lần xuất hiện, bộ đếm già hóa), `0x0A` đọc toàn bộ DTC được hỗ trợ. |
| **`0x14`** | `ClearDiagnosticInformation` | `0x54` | Xóa DTC và Freeze-Frame khỏi bộ nhớ bền vững (`14 FF FF FF` xóa tất cả nhóm). |
| **`0x31`** | `RoutineControl` | `0x71` | `0x01` Start, `0x02` Stop, `0x03` RequestResults — dùng cực nhiều trên TCU để kích hoạt **Test eCall**, **Audio Loopback Self-Test**, kiểm tra kết nối ăng-ten trên dây chuyền lắp ráp EOL! |
| **`0x85`** | `ControlDTCSetting` | `0xC5` | Tạm tắt ghi nhận DTC (`0x02` Off) trên toàn xe trong lúc đang chạy quy trình nạp phần mềm ECU để tránh báo lỗi mất liên lạc giả (`U0100`). |

### 2.2. Các Negative Response Code (`0x7F <SID> <NRC>`) & Timing `P2 / P2*`
* `0x11` `serviceNotSupported` | `0x12` `subFunctionNotSupported` | `0x13` `incorrectMessageLengthOrInvalidFormat`
* `0x22` `conditionsNotCorrect` (ví dụ yêu cầu chạy Routine hoặc ghi DID khi xe đang lăn bánh `Speed > 0`!)
* `0x31` `requestOutOfRange` (DID không tồn tại) | `0x33` `securityAccessDenied` (chưa mở khóa `0x27`/`0x29`) | `0x7F` `serviceNotSupportedInActiveSession`
* **Cơ chế `0x78` (`requestCorrectlyReceived-ResponsePending`)**:
  * Mặc định ECU phải trả lời trong **`P2_Server_max` (thường `50 ms`)**.
  * Nếu tác vụ tốn nhiều hơn 50 ms (ví dụ xóa Flash DTC hoặc chạy Audio Loopback 200 ms), UDS Server gửi tạm `7F <SID> 78` trước mốc 50 ms để xin gia hạn sang cửa sổ **`P2*_Server_max` (thường `5000 ms`)**, sau khi làm xong mới gửi phản hồi thật!

---

## 3. Chuyên Sâu Về DTC (Diagnostic Trouble Code): Cấu Trúc, Debounce & 8-Bit Status Mask

Đây là một trong các chủ đề bạn cần nắm cực chắc theo yêu cầu phỏng vấn:

### 3.1. Cấu trúc mã DTC chuẩn ISO 15031-6 / ISO 14229 (3 Bytes)
* Ký tự đầu phân nhóm hệ thống (2 bit cao của Byte 1):
  * **`P` (`00`)** — Powertrain (Động cơ, hộp số)
  * **`C` (`01`)** — Chassis (ABS, ESP, hệ thống lái)
  * **`B` (`10`)** — Body (Túi khí, điều hòa, **TCU / Ăng-ten / Loa-Mic eCall / Pin BUB** — ví dụ `B1A01-13` Circuit Open!)
  * **`U` (`11`)** — Network Communication (Mất liên lạc CAN/SPI với ECU khác — ví dụ `U0100-87` Missing Message!)
* Byte thứ 3 là **FTB (Failure Type Byte)** theo chuẩn ISO 15031-6 mô tả chính xác bản chất lỗi vật lý:
  * `0x11`: Circuit Short to Ground (Chập mass — ví dụ cáp đồng trục ăng-ten GNSS bị dập chạm vỏ xe, dòng LNA vọt `> 45 mA`!)
  * `0x12`: Circuit Short to Battery (Chập nguồn dương)
  * `0x13`: Circuit Open (Đứt mạch / tuột giắc ăng-ten, dòng LNA tụt `< 2 mA`!)
  * `0x87`: Missing Message (Timeout bản tin CAN)

### 3.2. Thuật toán Chống Nhiễu Lỗi (DTC Debouncing — Counter-based / Time-based)
Không bao giờ báo lỗi xác nhận ngay ở lần đọc ADC lệch đầu tiên (ví dụ sụt áp thoáng qua vài mili-giây khi đề nổ máy)!
* Mỗi bộ giám sát (Monitor) duy trì biến đếm có dấu `fault_counter` trong khoảng `[-128, +127]`:
  * Mỗi chu kỳ phát hiện lỗi: cộng `step_up` (ví dụ `+16`); mỗi chu kỳ đo đạt bình thường: trừ `step_down` (ví dụ `-8`, hoặc nhảy về `0` nếu bật *Jump-to-Zero*).
  * Chỉ khi `fault_counter >= +127` mới chốt `testFailed = 1`; chỉ khi `fault_counter <= -128` mới chốt `testPassed`.

### 3.3. Ý nghĩa của mặt nạ trạng thái DTC 8-Bit (ISO 14229 StatusOfDTC)
* **Bit 0 (`testFailed` — `0x01`)**: Lỗi đang hiện diện ngay tại lần kiểm tra gần nhất. Khi sửa xong dây ăng-ten, Bit 0 về `0` ngay lập tức!
* **Bit 1 (`testFailedThisOperationCycle` — `0x02`)**: Đã từng lỗi ít nhất 1 lần trong chu kỳ khóa điện (Ignition/Operation Cycle) hiện tại.
* **Bit 2 (`pendingDTC` — `0x04`)**: Lỗi vừa xảy ra ở chu kỳ hiện tại hoặc chu kỳ liền trước (đang chờ xác nhận qua nhiều chu kỳ).
* **Bit 3 (`confirmedDTC` — `0x08`)**: Lỗi đã đủ điều kiện xác nhận chính thức và **ghi bền vững vào Flash/EEPROM kèm Freeze-Frame** (tốc độ xe, điện áp KL30, nhiệt độ, Odometer, timestamp lúc xảy ra lỗi). Giá trị `0x09` (`Bit 0 + Bit 3`) nghĩa là lỗi đã xác nhận và hiện vẫn đang còn lỗi; `0x08` nghĩa là lỗi lịch sử đã xác nhận nhưng hiện tại đã hết lỗi (*healed/intermittent*)!
* **Aging / Self-Healing**: Khi lỗi không tái diễn trong $N$ chu kỳ vận hành liên tiếp (thường `40` warm-up/operation cycles), bộ đếm già hóa (`aging counter`) tự động xóa `confirmedDTC` khỏi danh sách lỗi lưu (trừ một số lỗi an toàn bắt buộc xóa bằng máy chẩn đoán).

---

## 4. SAE J1939 Cho Commercial Vehicle & Off-Highway (CV/OHW)

### 4.1. Cấu trúc gói tin & Transport Protocol trên J1939
* **29-bit Extended CAN ID**: `Priority (3b) | Reserved/EDP (1b) | DP (1b) | PF (8b) | PS (8b) | Source Address (8b)`.
  * Mỗi loại ECU có địa chỉ chuẩn phổ biến: `0x00` Engine #1, `0x03` Transmission, `0x0B` Brakes, `0x17` Instrument Cluster, `0xFB` Telematics/Data Logger (nhưng vẫn phải hỗ trợ **Dynamic Address Claiming** qua `PGN 60928 / 0x00EE00` dùng `64-bit NAME`!).
* **Truyền gói > 8 bytes trên J1939-21 (Classical CAN)**:
  1. **BAM (Broadcast Announcement Message)**: Phát quảng bá cho toàn mạng (`DA = 0xFF`). Gửi 1 khung `TP.CM_BAM` (`PGN 0x00EC00`, báo tổng số byte, tổng số gói, PGN đích) rồi gửi lần lượt các khung dữ liệu `TP.DT` (`PGN 0x00EB00`, byte 1 là số thứ tự gói `1..255`, 7 byte sau là dữ liệu) cách nhau **`50 – 200 ms`**. Không có Flow Control!
  2. **RTS/CTS (Connection Mode)**: Truyền điểm-tới-điểm có điều khiển luồng (`Request To Send` $\rightarrow$ bên nhận trả lời `Clear To Send` cho phép gửi $K$ gói $\rightarrow$ truyền `TP.DT` $\rightarrow$ kết thúc bằng `EndOfMsgACK`).

### 4.2. Chẩn đoán lỗi J1939: `DM1` & `DM2` khác UDS `0x19` như thế nào?
* Trên xe tải/máy xúc, các ECU **tự động phát định kỳ (broadcast mỗi 1 giây khi có lỗi)** bản tin **`DM1` (`PGN 65226 / 0x00FECA`)** lên bus CAN mà TCU không cần gửi lệnh hỏi (polling)! Nếu có nhiều hơn 1 mã lỗi đang kích hoạt, `DM1` dài hơn 8 bytes sẽ tự động được phát qua **BAM**!
* Cấu trúc `DM1`:
  * **Byte 1–2**: Trạng thái 4 đèn cảnh báo trên táp-lô (**Malfunction Indicator Lamp - MIL**, **Red Stop Lamp**, **Amber Warning Lamp**, **Protect Lamp**) + trạng thái nhấp nháy.
  * **Mỗi DTC dài 4 bytes (32 bits)**:
    * **`SPN` (19 bits)**: *Suspect Parameter Number* — chỉ định cảm biến/hệ thống con bị lỗi (ví dụ SPN 110 = Engine Coolant Temperature).
    * **`FMI` (5 bits, `0..31`)**: *Failure Mode Identifier* — bản chất lỗi (ví dụ `FMI 0` = Vượt ngưỡng nghiêm trọng, `FMI 3` = Voltage Above Normal/Short High, `FMI 4` = Voltage Below Normal/Short Low).
    * **`CM` (1 bit)**: Conversion Method + **`OC` (7 bits, `0..126`)**: *Occurrence Count* (số lần xuất hiện lỗi).
* **`DM2` (`PGN 65227 / 0x00FECB`)**: Chứa các lỗi lịch sử đã từng kích hoạt (Previously Active DTCs), thường được yêu cầu qua `Request PGN (0x00EA00)`.

### 🗣️ Sample English Answer (40s)
> *"Passenger cars and commercial or off-highway vehicles differ fundamentally in how TCU software collects diagnostics. Passenger cars typically use request-response UDS ISO 14229—such as `0x19` ReadDTCInformation and `0x22` ReadDID—transported over ISO-TP or DoIP, representing faults as 3-byte DTCs plus an 8-bit status mask. Commercial vehicles and off-highway machines primarily use SAE J1939 over 29-bit CAN, where ECUs broadcast active faults unsolicited via `DM1` (`PGN 0xFECA`), using BAM multi-packet transport when multiple faults exist, encoded as a 19-bit `SPN`, 5-bit `FMI`, and 7-bit occurrence count plus lamp status. On a shared TCU platform, I normalize both into a common fault event model while preserving source ECU address and raw diagnostic metadata."*

