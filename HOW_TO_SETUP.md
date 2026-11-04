# 📖 Hướng Dẫn Nhanh Thiết Lập & Triển Khai Dự Án TCU (Quick Setup Pointer)

Toàn bộ tài liệu hướng dẫn cài đặt môi trường, biên dịch CMake với **Mock HAL + Virtual MCU (`tcu-mcu-sim`)**, xây dựng hệ điều hành **Yocto Linux (`qemuarm64` + RAUC A/B OTA)** và tích hợp lên phần cứng **Qualcomm SA515M + Companion MCU** đã được cập nhật chuẩn hóa tại:

👉 **[Xem Chi Tiết Hướng Dẫn Cài Đặt & Kiểm Thử (`docs/SETUP_GUIDE.md`)](docs/SETUP_GUIDE.md)**

---

## Tóm Tắt Các Tài Liệu Kiến Trúc & Thiết Kế Cốt Lõi

1. **[Tổng Quan Kiến Trúc TCU SA515M + MCU (Tiếng Việt)](docs/design/tcu_architecture_overview.md)**: Hệ thống hóa mô hình 2 miền tính toán, quản lý nguồn, phân biệt 3 tầng giao tiếp (UDS/D-Bus vs SPI/UART vs CAN) và ánh xạ 4 tính năng mục tiêu.
2. **[System Requirements Specification (`docs/system_requirements/README.md`)](docs/system_requirements/README.md)**: Đặc tả phần cứng SA515M, MCU S32K144/STM32, dòng ngủ `< 0.8 mA`, pin dự phòng BUB và tiêu chuẩn UN R144 / R156 / ISO 14229.
3. **[Software Requirements Specification (`docs/software_requirements/README.md`)](docs/software_requirements/README.md)**: Yocto Scarthgap/Kirkstone LTS, các layer (`meta-rauc`, `meta-tcu-telematics`) và yêu cầu chức năng chi tiết (`FR-ECALL`, `FR-OTA`, `FR-SVT`, `FR-DIAG`).
4. **[System Design Document (`docs/design/system_design.md`)](docs/design/system_design.md)**: Kiến trúc 3 tầng đa tiến trình, sơ đồ tuần tự đầu-cuối, sơ đồ phân vùng Flash A/B và ngân sách RAM/CPU.
5. **[High-Level Design (`docs/design/high_level_design.md`)](docs/design/high_level_design.md)**: Trách nhiệm của 6 daemons, danh mục IPC Topics/RPC và quy tắc ưu tiên chiếm quyền an toàn (eCall P0).
6. **[Software Design (`docs/design/software_design.md`)](docs/design/software_design.md)**: Cấu trúc gói C++17, Dependency Injection, các interface Pure Virtual HAL và chiến lược giả lập không cần phần cứng.
7. **[Low-Level Design (`docs/design/low_level_design.md`)](docs/design/low_level_design.md)**: Khung nhị phân SPI CRC-16, chuẩn gói tin MSD 140-byte EN 15722, FSM của OTA/SVT, bảng mã UDS và lược đồ SQLite WAL.
8. **[Mô Hình Đa Tiến Trình & Đa Luồng (`docs/design/threading_model.md`)](docs/design/threading_model.md)**: 14 POSIX threads, lập lịch thời gian thực `SCHED_FIFO`, Watchdog 2 tầng và trình tự ngủ an toàn.
9. **[Luồng Vận Hành Hệ Thống Chi Tiết (`docs/design/system_workflow.md`)](docs/design/system_workflow.md)**: Phân tích từng bước mili-giây cho 5 kịch bản vận hành nghiệp vụ trên TCU.
