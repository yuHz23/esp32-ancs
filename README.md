# ESP32-S3 ANCS Bridge

Cầu nối thông báo: đọc notification trên **iPhone qua ANCS (BLE)** và đẩy lên server
[Noti Bridge](https://github.com/yuHz23/noti-bridge) qua WiFi (HTTP POST) — thay thế SePay
để nhận biến động số dư ngân hàng. Có **GATT server (Nordic UART Service)** để app **Android**
(không có ANCS) gửi notification qua BLE → ESP32 trở thành cổng chung cho cả iOS lẫn Android.

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

## Cấu hình lần đầu (không hardcode WiFi/token)
Boot khi **chưa có cấu hình** → ESP32 phát **hotspot `BankBridge-Setup`** (captive portal):
1. Điện thoại/laptop kết nối WiFi **`BankBridge-Setup`**.
2. Mở trình duyệt → **http://192.168.4.1** (thường tự bật).
3. Chọn WiFi nhà + nhập mật khẩu, **Server URL** (`http://<ip>:8787/notification`), **Token**.
4. Lưu → ESP32 khởi động lại, kết nối WiFi + BLE → chạy bình thường.

Cấu hình lưu trong NVS (`Preferences`), **không** nằm trong source.

## Factory reset
Giữ nút **BOOT (GPIO0) ~3 giây** bất cứ lúc nào → xoá cấu hình → quay về chế độ hotspot setup.

## Build (PlatformIO)
```bash
pio run -t upload      # nạp; không cần điền secret trong code
```

## Toolchain
Arduino-ESP32 core 2.0.17 (Bluedroid). BLE = Bluedroid (API IDF thô `esp_ble_gattc_*` /
`esp_ble_gatts_*`). Thư viện: ArduinoJson. `reference/` là ví dụ ble_ancs của Espressif (public domain).
