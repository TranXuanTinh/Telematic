# 05. Kết Nối Đám Mây Trên TCU: Cellular, HTTP/HTTPS, MQTT & Store-and-Forward

---

## 1. Quy Trình Gỡ Lỗi Kết Nối Mạng Di Động Từ Đáy Lên Đỉnh (Cellular Bring-Up Checklist)

Khi gặp câu hỏi: *"TCU có sóng 4G (RSSI tốt) nhưng không gửi được dữ liệu lên Cloud, bạn debug như thế nào?"* — Hãy trả lời tuần tự từng tầng OSI (tuyệt đối không đoán mò!):

1. **Tầng Vật lý & SIM (Physical / USIM)**: SIM đã nhận chưa (`AT+CPIN?` $\rightarrow$ `READY`), eSIM profile kích hoạt chưa?
2. **Đăng ký mạng vô tuyến (Network Registration)**: Đã attach mạng gói EPS/5GS chưa (`AT+CEREG?` trả về `1` Home hoặc `5` Roaming)? Có sóng (`RSSI`/`RSRP`) chỉ chứng tỏ thu được trạm phát, không có nghĩa nhà mạng cho phép attach!
3. **Phiên dữ liệu gói PDP Context / APN**: Cấu hình đúng **APN** riêng của nhà mạng/doanh nghiệp chưa (`AT+CGDCONT`, `qmicli` / `ModemManager`)? Đã nhận được địa chỉ IP trên giao diện `wwan0` / `rmnet_data0` chưa?
4. **Định tuyến IP & Firewall (Routing / Netfilter)**: Default route hoặc policy routing table có trỏ đúng qua `wwan0` không (`ip route get <broker_ip>`)? Có xung đột subnet với mạng Ethernet nội bộ trên xe (VLAN SOME/IP / DoIP) không? MTU có bị quá lớn gây rơi gói TLS lớn không?
5. **Phân giải tên miền (DNS)**: Server DNS trả về từ nhà mạng có phân giải được hostname của Broker không?
6. **Kết nối TCP & Đồng bộ Thời gian Hệ thống (System Clock & TLS/mTLS)**:
   * **Lỗi thực tế cực hay gặp trên xe**: Xe tháo cọc bình 12V/24V lâu ngày làm đồng hồ RTC nhảy về năm `1970` hoặc `2020`, trước thời điểm chứng chỉ X.509 có hiệu lực (`NotBefore`) $\rightarrow$ **TLS Handshake thất bại ngay lập tức (`CERT_NOT_YET_VALID`)!** Phải lấy giờ từ GNSS hoặc NITZ của nhà mạng trước khi mở TLS!
   * Kiểm tra CA Root chain, Client Certificate/Private Key trong HSM/TPM, hoặc chứng chỉ hết hạn / bị thu hồi.
7. **Tầng Ứng dụng (MQTT CONNACK / HTTP Status)**: Kiểm tra mã lỗi `CONNACK` (Bad ClientID, Unauthorized Topic ACL) hoặc HTTP `401/403/429`.

---

## 2. Khi Nào Dùng MQTT, Khi Nào Dùng HTTP/HTTPS Trên TCU?

| Tiêu chí | MQTT 3.1.1 / MQTT 5.0 over mTLS | HTTP/1.1 & HTTP/2 over mTLS (REST) |
| :--- | :--- | :--- |
| **Mô hình giao tiếp** | Kết nối TCP/TLS duy trì hai chiều (**Persistent Bi-directional Pub/Sub**). | Yêu cầu – Phản hồi (**Stateless Request/Response**). |
| **Overhead băng thông & Pin** | Header tối thiểu chỉ **2 bytes**; không phải bắt tay lại TLS cho từng bản tin định kỳ 5–10 giây $\rightarrow$ tiết kiệm data di động và pin. | Header lớn; phù hợp gọi không thường xuyên hoặc gom lô lớn (batch upload). |
| **Lệnh điều khiển từ xa (Downlink)** | **Xuất sắc**: Server đẩy lệnh xuống tức thì (Remote Lock, Pre-conditioning HVAC, SVT Activate, Wake-up OTA trigger) với độ trễ mili-giây mà xe không cần mở port inbound qua NAT nhà mạng! | Kém cho lệnh thời gian thực (phải polling tốn pin hoặc giữ long-polling). |
| **Tải/Đẩy File lớn** | **Không phù hợp** (nhồi file OTA vài trăm MB qua MQTT gây nghẽn kết nối telemetry và tốn RAM broker). | **Tiêu chuẩn vàng**: Hỗ trợ CDN toàn cầu, caching, kiểm soát luồng và đặc biệt là header **`Range: bytes=<offset>-`** để tải tiếp nối (Resumable Download) sau khi xe đi qua hầm hoặc tắt máy! |
| **Thiết kế chuẩn trên TCU** | Dùng **MQTT** cho Telemetry định kỳ, Cảnh báo sự kiện (DTC, Tamper, eCall audit) & Nhận lệnh từ xa (bao gồm cả thông báo có chiến dịch OTA mới). | Dùng **HTTPS** cho tải gói cập nhật OTA (`.raucb`, MCU `.bin`), tải lên file log chẩn đoán lớn (crash dump, CAN trace) và gọi API cấp phát chứng chỉ (Provisioning/EST). |

---

## 3. MQTT Chuyên Sâu: QoS 0/1/2, LWT, Session & Bẫy "Exactly-Once"

### 3.1. Ba mức QoS trong MQTT
* **`QoS 0` (At most once)**: Gửi 1 lần không ACK (TCP vẫn đảm bảo ở tầng vận chuyển, nhưng nếu kết nối đứt đúng lúc đang nằm trong socket buffer thì mất gói). Phù hợp cho dữ liệu vị trí/tốc độ tần suất cao chỉ cần giá trị mới nhất khi đang xem trực tiếp.
* **`QoS 1` (At least once — `PUBLISH` $\leftrightarrow$ `PUBACK`)**: Đảm bảo gói tin tới được Broker ít nhất 1 lần. Nếu mạng chập chờn làm mất gói `PUBACK`, TCU gửi lại với cờ `DUP=1` $\rightarrow$ **Có thể sinh bản sao trùng lặp (Duplicates)**. Đây là mức được dùng phổ biến nhất trong sản xuất!
* **`QoS 2` (Exactly once trên chặng MQTT — 4 bước bắt tay: `PUBLISH` $\rightarrow$ `PUBREC` $\rightarrow$ `PUBREL` $\rightarrow$ `PUBCOMP`)**: Tốn gấp đôi số lượt gói qua mạng di động sóng yếu.

### 3.2. Câu hỏi kinh điển: *"Dùng MQTT QoS 2 có đảm bảo xử lý nghiệp vụ đúng 1 lần (End-to-End Exactly-Once) không?"*
* **Trả lời dứt khoát**: **KHÔNG!** `QoS 2` chỉ là hợp đồng truyền nhận trên **đúng một chặng (single hop)** giữa MQTT Client và MQTT Broker!
  * Phía sau Broker còn có Kafka, microservice, cơ sở dữ liệu đám mây; phía trên xe, nếu TCU gửi xong hoặc nhận lệnh xong nhưng sụt nguồn trước khi kịp ghi trạng thái xuống Flash, lệnh vẫn có thể lặp lại.
* **Giải pháp chuẩn Production**: Dùng **`QoS 1` kết hợp với Idempotency (Tính lũy đẳng đầu-cuối)**:
  * Với **Telemetry / DTC gửi lên**: Gắn mỗi sự kiện một định danh duy nhất không đổi khi gửi lại (**`event_uuid`** hoặc **`device_id + boot_id + monotonic_seq`**) để Backend tự loại bỏ bản trùng (Deduplication).
  * Với **Remote Command gửi xuống** (ví dụ Mở khóa cửa, Kích hoạt còi, Khóa đề SVT): Gắn **`command_id` + `expiration_timestamp_utc` + chữ ký xác thực**, lưu bảng `processed_command_ids` bền vững trên Flash. Nếu nhận lại đúng `command_id` cũ, TCU trả về kết quả trước đó chứ **không thực thi lại hành động vật lý trên xe**!

### 3.3. Các tính năng MQTT quan trọng khác trên ô tô
* **Last Will and Testament (LWT)**: Đăng ký lúc `CONNECT`; nếu TCU mất sóng đột ngột hoặc bị cắt nguồn mà không gửi được `DISCONNECT`, Broker tự động phát bản tin trạng thái `{"online": false, "reason": "unexpected_disconnect"}` sau thời gian `Keep-Alive` (ví dụ 60–120s).
* **Persistent Session (`CleanSession=0` ở MQTT 3.1.1 / `CleanStart=0 + SessionExpiryInterval > 0` ở MQTT 5.0)**: Giúp Broker giữ lại các lệnh điều khiển quan trọng (như thông báo OTA hoặc lệnh SVT có TTL hợp lệ) khi xe đang tắt máy trong hầm và giao ngay khi xe thức dậy kết nối lại.

### 🗣️ Sample English Answer (35s)
> *"On a TCU, I use MQTT over mTLS for ongoing telemetry, fault alerts, and low-latency downlink commands, while using HTTPS with `Range` headers for resumable OTA downloads and large diagnostic uploads. Even MQTT QoS 2 only applies to the single client-to-broker hop—it doesn't make downstream cloud ingestion or vehicle actuation exactly-once across crashes. Instead, I use QoS 1 paired with end-to-end idempotency: deterministic event IDs for server deduplication, and persisted command IDs with expiry timestamps so duplicate downlink messages never re-trigger physical vehicle actions."*

---

## 4. Thiết Kế Bộ Đệm Ngoại Tuyến (Store-and-Forward) & Độ Bền Flash

Trên xe tải khai khoáng, lâm nghiệp hoặc tàu/máy công trình (OHW), mất sóng 48–72 giờ là chuyện bình thường. Khi thiết kế hàng đợi lưu trữ cục bộ (như SQLite trong `src/hal/sa515m/sqlite_storage_hal.cpp`), cần trả lời rõ 4 điểm kỹ thuật:

1. **Tính toán dung lượng (Buffer Sizing Math)**:
   $$\text{Raw Bytes} = \text{Bytes/record} \times \text{Rate (Hz)} \times \text{Offline Seconds}$$
   * Ví dụ: `250 bytes/bản ghi` $\times$ `1 bản ghi / 5 giây (0.2 Hz)` $\times$ `72 giờ (259,200 giây)` $\approx 12.96\text{ MB}$ dữ liệu thô; cộng overhead AES-256-GCM (`IV 12B + Tag 16B`), cột chỉ mục B-Tree của SQLite và hệ số dự phòng $\times 2 \Rightarrow$ cấp hạn mức khoảng **`30 – 50 MB`** trên phân vùng `/data`.
2. **Độ bền khi mất điện (Power-Loss Durability trong SQLite WAL)**:
   * Trong repo mẫu đang đặt `PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;` để tối ưu tốc độ.
   * **Lưu ý phản biện cấp Senior**: Với `synchronous=NORMAL` trong chế độ WAL, cơ sở dữ liệu **không bị hỏng (corrupt)** khi mất điện, nhưng các giao dịch vừa commit chưa kịp `fsync` từ bộ đệm trang RAM xuống chip eMMC/NAND **có thể bị mất (rollback)**!
   * **Giải pháp**: Với telemetry thường (`P2/P3`), gom lô theo chu kỳ 5–10 giây để bảo vệ tuổi thọ ghi xóa (Write Endurance / Wear Leveling) của chip eMMC; riêng với sự kiện sống còn (`P0 eCall`, `P1 Confirmed Critical DTC`, `OTA State Journal`, `Replay Command Nonce`), thực thi `synchronous=FULL` / `WAL checkpoint` tường minh ngay tại điểm chốt!
3. **Chính sách đào thải theo độ ưu tiên (Priority-Aware Eviction)**:
   * Khi đạt trần dung lượng, chỉ xóa các bản ghi `P3` (vị trí/nhiệt độ định kỳ cũ nhất), bảo vệ tuyệt đối các bản ghi `P0/P1` (tai nạn, trộm xe, mã lỗi động cơ nghiêm trọng) và gắn cờ thống kê số lượng bản ghi bị rơi để báo cáo lên Cloud khi có mạng lại.
4. **Nhận xét trung thực về Repo khi phỏng vấn**:
   * Trong repo `Telematic`, lớp `CurlMqttCloudHAL` hiện ghi ra file spool cục bộ để minh họa luồng kiến trúc mà chưa nối Paho MQTT / `libmosquitto` thật. Nhờ đã tách `ICloudHAL`, khi lên sản phẩm chỉ cần cài đặt adapter Paho MQTT C/C++ bất đồng bộ bên dưới interface đó mà không phải sửa logic của `PlatformOrchestrator` hay `SvtOrchestrator`.

