# 09. Bảo Vệ Kiến Trúc Repo, Phản Biện Cấp Senior & Khung Câu Chuyện STAR

Tài liệu này tổng hợp trực tiếp các điểm nhạy cảm nhất từ bản đánh giá kỹ thuật (120 câu hỏi) đối chiếu thẳng vào mã nguồn của repo `Telematic`, giúp bạn làm chủ hoàn toàn cuộc trò chuyện dù người phỏng vấn hỏi lý thuyết hay mở trực tiếp GitHub của bạn ra hỏi xoáy!

---

## 1. Bảng Bảo Vệ & Phản Biện Thẳng Thắn Repo `Telematic` (7 Điểm Đắt Giá Nhất)

> **Tư thế của một Senior Engineer**: Không bao giờ nói quá rằng một bản prototype cá nhân đã đạt chuẩn "production-ready". Việc bạn **chủ động chỉ ra ranh giới nào đã làm thật, ranh giới nào đang giả lập và cách nâng cấp lên chuẩn sản xuất** chính là bằng chứng thuyết phục nhất cho tư duy thiết kế cấp Senior!

| # | Câu hỏi xoáy từ Hội đồng phỏng vấn | Thực trạng chính xác trong Repo `Telematic` | Câu trả lời nâng cấp lên Production chuẩn Senior |
| :-: | :--- | :--- | :--- |
| **1** | *"Repo của bạn đã có MQTT thật chưa?"* | `CurlMqttCloudHAL` trong [`sa515m_hals.cpp`](../src/hal/sa515m/sa515m_hals.cpp) tải file qua `libcurl` là thật, nhưng nhánh publish MQTT hiện đang ghi JSON ra file spool cục bộ để minh họa kiến trúc. | *"It's a transport placeholder behind `ICloudHAL`. In production, I plug in an async MQTT client like Eclipse Paho or mosquitto with mTLS client keys in HSM/TPM, `PUBACK` tracking for QoS 1, LWT, and exponential backoff with jitter."* |
| **2** | *"Gói MSD 140-byte trong repo có đúng chuẩn châu Âu CEN EN 15722 không?"* | [`msd_codec.cpp`](../src/common/msd_codec.cpp) đóng gói đủ các trường nghiệp vụ vào khung 140 bytes kèm magic byte và `CRC-16-CCITT` riêng để kiểm thử round-trip nội bộ. | *"The buffer size and telemetry fields model eCall, but European 112 eCall requires standardized **ASN.1 Unaligned PER (UPER)** encoding per EN 15722 (and SIP/IMS MIME payload per RFC 8147 for NG-eCall), verified against official PSAP conformance test vectors."* |
| **3** | *"Test báo độ trễ quay số `1 ms < 100 ms` có đảm bảo ngoài đời thật không?"* | [`test_ecall_workflow.cpp`](../tests/test_ecall_workflow.cpp) đo độ trễ phần mềm nội bộ từ lúc nhận sự kiện tới lúc gọi hàm `MockModemHAL::dialEmergency` trong RAM. | *"That test verifies low software dispatch overhead in the mock harness, not real cellular call-setup time. On target hardware, I define exact measurement endpoints—including MCU SRS interrupt capture and SPI transit—and measure 99.9th-percentile tail latency under heavy CPU, flash I/O, and modem load."* |
| **4** | *"Cờ `IPC_FLAG_URGENT` có chặn ngang (preempt) được tác vụ đang chạy không?"* | [`ipc_client.cpp`](../src/ipc/ipc_client.cpp) đẩy gói khẩn cấp lên đầu `dispatch_queue_`, nhưng chỉ có 1 luồng `dispatchLoop`. Nếu luồng đó đang bận tải file đồng bộ trong `PlatformOrchestrator`, gói khẩn cấp vẫn phải chờ! | *"Queue priority only reorders waiting messages; it cannot interrupt an active synchronous handler. To eliminate head-of-line blocking, I offload slow downloads/I/O to cancellable worker threads and give emergency eCall control its own dedicated real-time socket."* |
| **5** | *"Khởi tạo mặc định `VehicleState` (`speed=0, PARK, ign=false, kl30=12.6V`) có nguy hiểm gì?"* | Trong [`types.h`](../include/common/types.h), các giá trị mặc định của struct trông giống hệt một chiếc xe đang đỗ an toàn! Nếu MCU chưa gửi khung đầu tiên hoặc dây SPI đứt mà code không kiểm tra cờ hợp lệ, cổng an toàn OTA/Immobilizer có thể hiểu nhầm! | *"Default-initializing vehicle state to zero speed and PARK fails open! In production safety gates, every signal carries explicit validity (`SNA/Unknown`), source health, and monotonic freshness age; missing or stale signals must fail closed and block OTA flashing or starter inhibit."* |
| **6** | *"Dữ liệu Store-and-Forward trong SQLite có sống sót 100% qua cúp điện đột ngột không?"* | [`config_loader.cpp`](../src/common/config_loader.cpp) cho phép `--runtime-dir /run/tcu` đổi cả `db_path` vào `/run` (`tmpfs` mất sạch khi tắt máy!), và [`sqlite_storage_hal.cpp`](../src/hal/sa515m/sqlite_storage_hal.cpp) dùng `WAL + synchronous=NORMAL`. | *"Runtime sockets belong in volatile `/run/tcu`, whereas SQLite must stay on persistent `/data` flash. Also, `synchronous=NORMAL` in WAL mode prevents corruption on power loss but can lose the most recent unsynced commits, so critical events like eCall, confirmed DTCs, and OTA state journals need explicit `FULL` sync / checkpointing."* |
| **7** | *"Đọc `SO_PEERCRED` trong `UdsBroker` đã đủ bảo mật IPC chưa?"* | [`uds_broker.cpp`](../src/ipc/uds_broker.cpp) lấy được `pid/uid/gid` từ kernel nhưng hiện vẫn tin tưởng `DaemonId` do client tự khai báo trong gói `HELLO_REGISTER`. | *"Reading `SO_PEERCRED` only authenticates the OS user/PID; authorization still requires mapping distinct service UIDs (or SELinux/AppArmor labels) to allowed `DaemonId`s, rejecting duplicate registrations, and enforcing per-topic and per-RPC ACLs."* |

---

## 2. Bộ Khung 4 Câu Chuyện STAR Cá Nhân (Điền Dữ Liệu Thật Của Bạn Trước Giờ G!)

Hãy điền chi tiết sự việc thật của bạn vào 4 khung dưới đây và tập nói thành tiếng trong **60 – 90 giây / câu chuyện** (không bịa số liệu KPI nếu không có số đo chính thức):

### ⭐ Story 1: End-to-End Feature Ownership — China eCall Function Owner (FPT Software)
* **Situation**: Trong dự án phát triển TCU cho thị trường Trung Quốc tại FPT Software, tính năng eCall yêu cầu độ tin cậy cao khi xử lý kích hoạt tự động/thủ công, quản lý trạng thái cuộc gọi, truyền dữ liệu khẩn cấp và phối hợp với âm thanh/nguồn dự phòng.
* **Task**: Với vai trò **Function Owner kiêm Technical Lead của nhóm 2 thành viên**, tôi chịu trách nhiệm xuyên suốt từ làm rõ yêu cầu kỹ thuật với khách hàng, viết tài liệu HLD/LLD, triển khai bằng C++11/14 đến kiểm thử tích hợp và bảo trì.
* **Action**: *(Điền 1 tình huống cụ thể bạn đã làm: ví dụ làm rõ một điểm mơ hồ trong state machine callback/retry với khách hàng, tách interface rõ ràng giữa logic trạng thái eCall và tầng modem/audio, review code các nhánh timeout/error recovery cho thành viên trong nhóm)*.
* **Result**: Tính năng vượt qua các bài kiểm thử chấp nhận của khách hàng đúng tiến độ, giảm thiểu lỗi phát sinh khi tích hợp liên module và duy trì ổn định trong giai đoạn bảo trì.

### ⭐ Story 2: Vehicle Network & CANoe Validation — Commercial Vehicle K-Matrix (HCLTech)
* **Situation**: Trong dự án phần mềm nền tảng xe thương mại (ComVeh Basic Software trên Classic AUTOSAR) tại HCLTech, hệ thống cần ánh xạ chính xác hàng loạt tín hiệu mạng xe từ đặc tả **K-matrix** vào stack giao tiếp.
* **Task**: Cấu hình mapping tín hiệu và xác minh tính đúng đắn của luồng thu/phát trên **Vector CANoe** và **ECU WorkX**.
* **Action**: *(Điền 1 ví dụ cụ thể bạn đã bắt được hoặc kiểm chứng kỹ: ví dụ kiểm tra đối chiếu phiên bản K-matrix/DBC, phát hiện lệch scaling/offset, byte order hoặc giá trị SNA/timeout bằng cách tiêm khung biên trong CANoe và so sánh giá trị giải mã ở tầng ứng dụng)*.
* **Result**: Đảm bảo dữ liệu tín hiệu xe chính xác tuyệt đối trước khi bàn giao sang bước tích hợp hệ thống, kèm đầy đủ log trace tái lập được.

### ⭐ Story 3: On-Site Hardware/Integration Debugging — Thermal Systems Project (Hàn Quốc)
* **Situation**: Trong đợt công tác trực tiếp tại Hàn Quốc cho dự án hệ thống nhiệt (Thermal Systems), quá trình tích hợp phần mềm điều khiển trên phần cứng thực tế gặp vấn đề cần xử lý nhanh cùng đội ngũ khách hàng tại chỗ.
* **Task**: Khoanh vùng nguyên nhân gốc rễ giữa phần cứng, cấu hình driver C và tín hiệu điều khiển dưới áp lực thời gian trực tiếp tại lab khách hàng.
* **Action**: *(Điền cách bạn đo đạc bằng debugger/logic analyzer/trace log, trao đổi trực tiếp với kỹ sư phần cứng/khách hàng để cô lập lỗi và kiểm chứng bản sửa ngay trên thiết bị thật)*.
* **Result**: Giải quyết dứt điểm sự cố tại lab, củng cố sự tin tưởng của khách hàng về khả năng gỡ lỗi thực chiến và giao tiếp kỹ thuật trực tiếp.

### ⭐ Story 4: Multi-Component Linux Edge Architecture — Drone Linux/ROS Project
* **Situation**: Hệ thống nhúng chạy Linux trên LattePanda cần thu thập dữ liệu cảm biến thời gian thực, xử lý và truyền thông giữa nhiều tiến trình C++/Python.
* **Task**: Thiết kế giao tiếp liên tiến trình ổn định, tránh nghẽn dữ liệu cảm biến khi tải CPU tăng cao.
* **Action**: *(Điền cách bạn tổ chức các node giao tiếp, quản lý luồng dữ liệu, xử lý mất gói/trễ cảm biến và kiểm thử thực nghiệm)*.
* **Result**: Hệ thống vận hành mượt mà, mang lại kinh nghiệm thực tế quý giá về quản lý tài nguyên và gỡ lỗi đa tiến trình trên Linux để áp dụng sang nền tảng Linux TCU.

---

## 3. Top 10 Câu Trả Lời Tiếng Anh "Thuộc Lòng Phản Xạ" Trước Phỏng Vấn

1. **Giới thiệu bản thân (60s Pitch)**:
   > *"I have around five years of embedded software experience across automotive telematics, commercial-vehicle networking, thermal control, and Classic AUTOSAR. At FPT Software, I served as Function Owner and Technical Lead for a two-engineer team delivering China eCall on a TCU in C++11/14—owning requirements clarification with customers, HLD/LLD, implementation, and validation. Currently at HCLTech, I work on commercial-vehicle Classic AUTOSAR basic software, handling K-matrix CAN signal mapping and validation in Vector CANoe. Alongside my Linux edge experience and my dual-domain Linux-plus-MCU TCU reference project, I'm excited about this role because it brings together TCU feature ownership and vehicle-to-cloud engineering across both passenger cars and CV/OHW platforms."*

2. **Làm sao thiết kế chung 1 nền tảng TCU cho cả CV/OHW và Passenger Car?**
   > *"I keep transport, encrypted store-and-forward, OTA engine, diagnostics framework, and security services completely vehicle-agnostic, while isolating bus protocols and vehicle policies behind configurable adapters and validated profiles. That way, a 24-volt off-highway machine speaking J1939 `PGN/DM1` without a `PARK` gear and a 12-volt passenger car speaking CAN-FD, UDS, and `SOME/IP` share 80% of the hardened core without unsafe hard-coded assumptions."*

3. **Xử lý khi CANoe hiển thị đúng nhưng giá trị trên Cloud bị sai?**
   > *"I pick one deterministic sample and trace it boundary by boundary: raw CAN frame in CANoe, bit extraction and endianness/sign extension, `factor/offset` conversion, internal IPC serialization, persistent queue payload, and cloud ingestion schema. Checking units, signedness, SNA handling, and schema version at each hop isolates the exact first component that corrupts the value."*

4. **Khi nào ưu tiên eCall đè lên OTA và làm sao xử lý bước đang ghi Flash?**
   > *"Emergency eCall always holds top functional priority. Download and verification phases are immediately paused or cancelled, and modem plus cabin audio routes are preemptively claimed for emergency dialing. Because raw bootloader switch steps may have short non-interruptible windows, dual-bank A/B staging writes only to the inactive partition in the background so an unexpected crash trigger never finds the active root filesystem half-written."*

5. **Cách bạn tiếp cận nhanh với J1939 hoặc SOME/IP ở dự án mới?**
   > *"I start from the project's authoritative network specification—the J1939 PGN/SPN mapping or ARXML/Franca IDL service contract—set up a reproducible bench loop in CANoe or `vsomeip` with recorded traces, verify framing, addressing, transport segmentation, and timeout edge cases with unit and HIL tests, and review edge-case interpretations early with the network architect."*

---

## 4. Câu Hỏi Ngược Lại Dành Cho Hội Đồng Phỏng Vấn (Cuối Buổi)

Chọn **2 – 3 câu sắc sảo nhất** dưới đây để hỏi ở 5 phút cuối (thể hiện đúng tầm tư duy của một Senior Engineer sẵn sàng bắt tay vào việc):
1. *"How are responsibilities split today on your TCU hardware between the Embedded Linux SoC and the companion RTOS MCU or vehicle gateway?"*
2. *"Between your Passenger Car and CV/OHW programs, how much of the telematics middleware is shared today, and where are the biggest pain points in adapting across variants?"*
3. *"What HIL setup—such as Vector CANoe restbus, cellular network simulation, or automated power-cut testing—does the team use before releasing an OTA image?"*
4. *"What would a strong 90-day contribution look like for the engineer joining this position?"*

