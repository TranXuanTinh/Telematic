# 07. SOME/IP (Scalable service-Oriented MiddlewarE over IP) & Automotive Ethernet

> **Tại sao JD hỏi SOME/IP cho vị trí TCU?**
> Trong kiến trúc điện/điện tử (E/E Architecture) hiện đại (Domain / Zonal Architecture trên cả Passenger Car và xe tải thế hệ mới), TCU không chỉ đọc khung CAN thô mà kết nối qua **Automotive Ethernet (`100BASE-T1` / `1000BASE-T1`)** tới **Central Gateway / Vehicle Computer / IVI** bằng giao thức hướng dịch vụ **SOME/IP**!

---

## 1. Khác Biệt Tư Duy: Signal-Oriented (CAN) vs. Service-Oriented (SOME/IP)

| Đặc điểm | Signal-Oriented (CAN / J1939 truyền thống) | Service-Oriented — SOA (SOME/IP over Ethernet) |
| :--- | :--- | :--- |
| **Cách truyền dữ liệu** | Phát quảng bá định kỳ liên tục mọi tín hiệu lên bus dù có ECU nào đang cần hay không (*Sender broadcasts blindly*). | **Chỉ truyền khi có Consumer đăng ký (`Publish/Subscribe`)** hoặc khi được gọi hàm từ xa (**`RPC Method`**). |
| **Khả năng mở rộng** | Băng thông hạn chế (500 kbps – 5 Mbps), cấu trúc khung cứng nhắc theo K-matrix cố định. | Chạy trên UDP/TCP tốc độ 100 Mbps – 1 Gbps, hỗ trợ cấu trúc dữ liệu phức tạp, mảng động, chuỗi và nâng cấp dịch vụ linh hoạt. |
| **Ứng dụng trên TCU** | Đọc tốc độ xe, vòng tua, khóa điện KL15, xung túi khí từ mạng thân vỏ/động cơ. | Cung cấp `CellularService`, `GnssPositionService`, `OtaPackageService`, `RemoteControlService` cho màn hình IVI, ADAS và Zonal Gateway! |

---

## 2. Cấu Trúc Khung Bản Tin SOME/IP (16-Byte Header Chuẩn AUTOSAR)

Mọi gói SOME/IP (dù chạy trên **UDP** cho độ trễ thấp/multicast hay **TCP** cho payload lớn tin cậy) đều bắt đầu bằng **Header 16 bytes theo thứ tự Big-Endian (Network Byte Order)**:

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Service ID           |           Method ID           |  <-- Message ID (32-bit): Bit 15 Method ID = 0 (Method) / 1 (Event)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Length (Bytes)                        |  <-- Độ dài từ trường Request ID đến hết Payload (>= 8)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Client ID           |          Session ID           |  <-- Request ID (32-bit): Ghép cặp Request <-> Response
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Protocol Ver  | Interface Ver | Message Type  |  Return Code  |  <-- ProtoVer = 0x01, InterfaceVer = Major ver của Service
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Payload (Variable)                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### Ba mô hình tương tác chính trong SOME/IP:
1. **Methods (Gọi hàm từ xa — Bit cao của Method ID = `0`, ví dụ `0x0001..0x7FFF`)**:
   * **`REQUEST (0x00)` $\leftrightarrow$ `RESPONSE (0x80)` / `ERROR (0x81)`**: Ví dụ màn hình IVI gọi `TcuCallService::StartEmergencyTestCall()` và nhận kết quả trả về.
   * **`REQUEST_NO_RETURN (0x01)` (Fire & Forget)**: Gửi lệnh một chiều không chờ phản hồi ứng dụng.
2. **Events (Sự kiện thông báo — Bit cao của Method ID = `1`, ví dụ `0x8001..0xFFFE`, `Message Type = NOTIFICATION (0x02)`)**:
   * Gửi từ Server tới các Client đã đăng ký `Eventgroup` khi có sự kiện xảy ra (*On-Change*) hoặc theo chu kỳ (*Cyclic*), ví dụ `OnIncomingCloudCommand` hoặc `OnGnssFixUpdated` (10 Hz).
3. **Fields (Thuộc tính có trạng thái — Kết hợp của Getter + Setter + Notifier)**:
   * Khác **Event** (chỉ là xung sự kiện tức thời, ai vào sau không biết chuyện vừa qua), **Field** đại diện cho một trạng thái hiện hữu (ví dụ `CellularConnectionState` hoặc `ActiveOtaProgress`). Ngay khi Client vừa `Subscribe` thành công, Server gửi ngay giá trị hiện tại (**Initial Event**) cho Client mà không phải đợi lần thay đổi tiếp theo!

---

## 3. Cơ Chế Khám Phá Dịch Vụ Động: `SOME/IP-SD` (Service Discovery)

`SOME/IP-SD` bản chất là một gói SOME/IP đặc biệt có **`Service ID = 0xFFFF`, `Method ID = 0x8100`**, truyền qua **UDP** (thường trên cổng `30490` multicast tới nhóm IP multicast cục bộ của VLAN xe):

```text
[TCU (Server: GnssService 0x1200)]                      [IVI / ADAS ECU (Client)]
        │                                                           │
        │  1. Khởi động: Initial Wait Phase (random jitter)         │
        │     -> Repetition Phase -> Main Phase (định kỳ cyclic)    │
        │                                                           │
        ├──── OfferService(Service=0x1200, Inst=1, TTL=3s,         ─►  (Hoặc Client dậy trước sẽ phát
        │                  IPv4 Endpoint=10.0.20.5:30501/UDP)       │   FindService(0x1200) multicast)
        │                                                           │
        │◄─── SubscribeEventgroup(Service=0x1200, Eventgroup=0x01, ─┤  (Unicast tới TCU, kèm Endpoint nhận Event)
        │                         TTL=3s)                           │
        │                                                           │
        ├──── SubscribeEventgroupAck(TTL=3s) ──────────────────────►│
        ├──── Initial Field Value Notification ────────────────────►│
        ├──── Notification(GnssFix @ 10Hz) ────────────────────────►│
        │                                                           │
        │  (Khi TCU chuẩn bị vào SLEEP hoặc tắt dịch vụ:)           │
        ├──── StopOfferService(Service=0x1200, TTL=0x000000) ──────►│  (Báo hủy ngay lập tức không bắt Client chờ hết TTL!)
```

### Các điểm xoáy sâu phỏng vấn về SOME/IP:
1. **Serialization & TLV (Tag-Length-Value)**:
   * SOME/IP đóng gói tuần tự không chèn padding ngầm như C struct; dùng Big-Endian mặc định; mảng/chuỗi có trường Length đi trước (`1, 2` hoặc `4 bytes`). Để mở rộng tương thích tiến/lùi giữa các phiên bản ECU khác nhau, AUTOSAR hỗ trợ **TLV serialization** (bên nhận bỏ qua các Tag mới chưa biết mà không gãy parser!).
2. **SOME/IP-TP (Transport Protocol)**:
   * Khi truyền gói lớn qua **UDP** vượt quá MTU Ethernet (`~1400 bytes` payload), không nên để tầng IP phân mảnh (IP fragmentation làm mất 1 mảnh là hỏng cả gói và khó qua firewall); bật bit TP trong `Message Type` (`0x20`, `0xA2`...) để phân đoạn ngay ở tầng SOME/IP! (Hoặc cấu hình binding sang **TCP** cho phương thức truyền dữ liệu lớn).
3. **Bảo vệ An toàn & Bảo mật (AUTOSAR E2E + SecOC / TLS / DTLS)**:
   * Trên mạng Ethernet xe hơi, một gói SOME/IP điều khiển xe gắn thêm header **AUTOSAR E2E (End-to-End Protection, ví dụ Profile 4 / Profile 7)** chứa `Length`, `Counter`, `DataID` và `CRC-32/64` để phát hiện mất gói, lặp gói, sai thứ tự hoặc hỏng dữ liệu trên switch; kết hợp **VLAN Segmentation** + **IPsec / DTLS / TLS** để chống giả mạo ECU trên mạng nội bộ!
4. **Triển khai thực tế trên Embedded Linux với `vsomeip` (COVESA)**:
   * Thư viện C++ mã nguồn mở chuẩn công nghiệp trên Yocto (`meta-oe` có sẵn recipe `vsomeip` + `CommonAPI-SomeIP` sinh code từ file đặc tả **Franca IDL `.fidl` / `.fdepl`** hoặc **ARXML**).
   * Trên một SoC Linux có nhiều daemon (như 6 daemon trong repo TCU của chúng ta), chỉ có **1 tiến trình đóng vai trò `routingmanagerd`** mở socket UDP/TCP ra card Ethernet vật lý (`eth0.20`), còn các daemon khác nói chuyện với `routingmanagerd` qua **Unix Domain Sockets cục bộ** rồi được định tuyến trong suốt ra mạng Ethernet xe!

### 🗣️ Sample English Answer (40s)
> *"SOME/IP brings service-oriented communication over Automotive Ethernet using a compact 16-byte big-endian header supporting RPC methods, event notifications, and stateful fields. Through `SOME/IP-SD` on UDP multicast, a TCU dynamically offers services such as GNSS, cellular status, or OTA management, and vehicle clients subscribe only to the eventgroups they need with bounded TTLs and explicit `StopOffer` on sleep. On Embedded Linux with Yocto, I typically use `vsomeip`—configured with a single routing manager bridging local Unix sockets to VLAN-segmented Ethernet—and apply AUTOSAR E2E counters and CRCs plus TLS/DTLS where safety or security requires it."*

