# ESP32-C3 Matter switch

Firmware ESP-Matter tối thiểu: một thiết bị Wi-Fi, một switch ON/OFF giả lập bằng log.
Endpoint 0 là Root Node; endpoint ứng dụng dùng **On/Off Plug-in Unit (0x010A)**,
để controller có thể bật/tắt trực tiếp (thường hiển thị dạng ổ cắm).
Không cần LED, relay hay GPIO ngoài.

SDK được chốt: ESP-IDF **v5.4.1**, Python **3.11**, ESP-Matter release/v1.4 commit
`abdb759d7aae8a445e3edc5769a10f5cb23feffc`. Board cần flash tối thiểu **4 MB**.
ESP32-C3 dùng Matter qua Wi-Fi 2.4 GHz, BLE dùng cho commissioning.

## Build trên Windows + Ubuntu WSL

Trong PowerShell, ở thư mục project:

```powershell
wsl -d Ubuntu -- bash /mnt/c/Users/vdvin/Source/deye-inverter-esp/scripts/bootstrap.sh
wsl -d Ubuntu -- bash /mnt/c/Users/vdvin/Source/deye-inverter-esp/scripts/build.sh
```

Lần đầu bootstrap sẽ cài các gói Ubuntu, tải SDK và toolchain, cần Internet và
vài GB trống. SDK nằm trong `~/esp` của WSL; đổi bằng `ESP_SDK_ROOT` nếu cần.
Bootstrap chỉ cài công cụ cần cho firmware (GN và dependency Python), bỏ qua bộ host
ARM/Clang/QEMU và chip-tool. Python 3.11 được cài riêng, không thay Python hệ thống.
Trên Linux có thể chạy trực tiếp `bash scripts/bootstrap.sh` rồi `bash scripts/build.sh`.
Build tạo `build/deye_matter_switch.bin`, bootloader, partition table và `flasher_args.json`.
Đây là partition ứng dụng đơn, không có OTA; flash qua USB.

## Flash và xem log

Windows cần Python và [uv](https://docs.astral.sh/uv/). Dùng đúng COM của board:

```powershell
.\scripts\flash.ps1 -Port COM3
uv run --with pyserial python -m serial.tools.miniterm COM3 115200
```

`flash.ps1` dùng esptool 4.8.1 và offset do build sinh, không cần chuyển USB vào WSL.
Nếu không kết nối được, giữ BOOT, bấm RESET, thả BOOT rồi flash lại.
Miniterm thoát bằng Ctrl+]; đóng monitor trước khi flash.

Mặc định console là **USB Serial/JTAG**, phù hợp cổng USB native của C3.
Nếu board dùng chip USB-UART ngoài (CH340/CP210x), chạy menuconfig rồi chọn UART0:

```bash
export PATH="$HOME/esp/python311/bin:$HOME/esp/gn:$PATH"
source ~/esp/esp-idf/export.sh
source ~/esp/esp-matter/export.sh
idf.py menuconfig
# Component config -> ESP System Settings -> Channel for console output
idf.py build
```

## Pair và sử dụng

1. Flash, mở monitor, reset board. Tìm `SetupQRCode` / `Manual pairing code`
   trong log Matter; lấy mã đang được firmware in ra để pair.
   Có thể in lại mã từ console bằng `matter onboardingcodes ble`.
2. Trên điện thoại bật Bluetooth, mở controller Matter (Apple Home, Home Assistant
   Companion với Matter Server hoặc Google Home).
3. Chọn thêm thiết bị Matter, nhập manual code hoặc quét QR từ payload trong log.
   Cấu hình thử nghiệm mặc định dùng passcode `20202021`, discriminator `3840`.
   Passcode này dành cho chip-tool; app điện thoại cần manual pairing code/QR.
4. Chọn Wi-Fi **2.4 GHz**, chờ `PAIR complete; switch is ready`.
   Điện thoại/controller và board cần mạng LAN cho phép IPv6, mDNS/multicast;
   tránh mạng guest cô lập client.
5. Bật/tắt trong app; monitor sẽ ghi:

```text
matter_switch: SWITCH endpoint=1 state=ON (simulated)
matter_switch: SWITCH endpoint=1 state=OFF (simulated)
```

Endpoint ID thực tế được in ở log boot. Identify cũng ghi log, không nháy LED.
Credentials và fabric do SDK lưu trong NVS, không cần pair lại sau mỗi lần reboot.
Firmware dùng chứng chỉ và VID/PID thử nghiệm; controller có thể yêu cầu chấp nhận
thiết bị chưa được chứng nhận. Đây là cấu hình phát triển, không dùng để sản xuất.

### Apple Home

Với cấu hình mặc định của repo, manual code là **34970112332**;
[mở QR để quét](docs/pairing-qr.png). Payload đã đọc từ board:
`MT:Y.K9042C00KA0648G00`. Nếu thay đổi credentials, lấy mã mới từ log/console.

- iPhone kết nối Wi-Fi có băng 2.4 GHz, bật Bluetooth, để gần board.
- Mở **Nhà (Home) → + → Thêm phụ kiện (Add Accessory)**, quét QR Matter
  hoặc chọn tùy chọn nhập mã thủ công và nhập **manual pairing code 11 chữ số**
  trong log (không nhập passcode 8 chữ số `20202021`).
- Nếu có cảnh báo phụ kiện chưa được chứng nhận, chấp nhận thiết bị phát triển
  để tiếp tục. Chọn phòng, đặt tên ví dụ **Deye Switch**.
- Chạm ô ổ cắm để ON/OFF; kiểm tra log `SWITCH ... state=ON/OFF` trên COM của board.
- iOS 18 trở lên có thể thêm và điều khiển Matter cục bộ mà không cần home hub;
  muốn điều khiển từ xa cần home hub. Xem
  [Apple: pair Matter](https://support.apple.com/en-ca/102135) và
  [Apple: điều khiển từ xa](https://support.apple.com/en-gb/102056).

## Pair lại / khôi phục

Trong serial console gõ:

```text
matter esp factoryreset
```

Lệnh xóa fabric và Wi-Fi của thiết bị rồi reboot để pair mới. Xóa mục cũ trong app
nếu còn lưu. Khi xóa fabric cuối từ controller, firmware mở cửa sổ commissioning
300 giây trên mạng Wi-Fi hiện tại (DNS-SD). Để thêm hệ sinh thái thứ hai khi vẫn còn
fabric, dùng chức năng chia sẻ thiết bị Matter của controller đang quản lý.

Nếu console không dùng được và cần xóa sạch flash, trong WSL đã export SDK:

```bash
# Chỉ khi board đã được chuyển USB vào WSL và có /dev/ttyACM0 hoặc /dev/ttyUSB0:
idf.py -p /dev/ttyACM0 erase-flash flash monitor
```

Hoặc trên Windows, sau khi đóng monitor:

```powershell
uv run --with 'esptool==4.8.1' python -m esptool --chip esp32c3 --port COM3 erase_flash
.\scripts\flash.ps1 -Port COM3
```

## Kiểm tra trên board

Đã kiểm tra thực tế trên ESP32-C3 revision 0.4, flash 4 MB, COM3:
build thành công, flash được xác minh hash, boot và factory reset hoạt động,
pair Apple Home thành công. Các lệnh ON/OFF từ Home đã xuất hiện đúng trong log
`SWITCH endpoint=1 state=ON/OFF (simulated)`.

- Boot chưa pair: log QR/manual code và cửa sổ pairing.
- Pair: có IP, `PAIR complete`, app hiển thị một switch/ổ cắm.
- ON, OFF, Toggle: trạng thái trong app và log khớp nhau.
- Reboot: controller kết nối lại, không cần pair lại.
- Factory reset: pair lại được từ đầu.

Tham khảo [ESP-Matter release/v1.4](https://github.com/espressif/esp-matter/tree/release/v1.4)
và [hướng dẫn phát triển Espressif](https://docs.espressif.com/projects/esp-matter/en/release-v1.4/esp32/developing.html).
