# ESP32-S3 ANCS Bridge

Cầu nối thông báo: đọc notification trên **iPhone qua ANCS (BLE)** và đẩy lên server
[Noti Bridge](https://github.com/) qua WiFi (HTTP POST). Mục đích: thay thế SePay để
nhận biến động số dư ngân hàng.

Có thêm **GATT server (Nordic UART Service)** để app **Android** ghi notification qua BLE
(Android không có ANCS) → ESP32 trở thành cổng chung cho cả iOS lẫn Android.

## Phần cứng
- ESP32-S3-N16R8 (16MB flash, 8MB PSRAM)
- Nạp/đọc serial qua cổng UART CH343 (COM7) — `ARDUINO_USB_CDC_ON_BOOT=0`

## Kiến trúc
- **iOS:** ESP32 = peripheral quảng cáo (giả HID để iOS liệt kê trong Settings) → iPhone
  connect → ESP32 mở **GATT client trên link sẵn có** (`esp_ble_gattc_open`) → bond →
  subscribe ANCS Notification Source + Data Source → POST.
- **Android:** ESP32 = **GATT server** (NUS), app companion ghi JSON `{package,title,text}`
  vào RX characteristic `6E400002` → ESP32 đẩy vào hàng đợi → POST.
- WiFi + BLE chạy đồng thời (coexistence): init **BLE trước**, `WiFi.setSleep(true)`.

## Build (PlatformIO)
```bash
# 1. Tạo file cấu hình bí mật
cp src/secrets.h.example src/secrets.h   # rồi điền WiFi / token / server URL

# 2. Build + nạp
pio run -t upload
```

## Toolchain
Arduino-ESP32 core 2.0.17 (Bluedroid). Stack BLE = Bluedroid (API IDF thô `esp_ble_gattc_*`,
`esp_ble_gatts_*`). Thư viện: ArduinoJson.

## Lưu ý
- `src/secrets.h` chứa WiFi password + token → **đã .gitignore**, không lên repo.
- `reference/` là ví dụ ble_ancs chính thức của Espressif (public domain) dùng làm tài liệu.
