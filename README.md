# ESP32-C3 Matter switch

Firmware ESP-Matter tối thiểu: một thiết bị Wi-Fi, một switch ON/OFF giả lập bằng log.
Endpoint 0 là Root Node; endpoint ứng dụng dùng **On/Off Plug-in Unit (0x010A)**,
để controller có thể bật/tắt trực tiếp (thường hiển thị dạng ổ cắm).
Không cần LED, relay hay GPIO ngoài.

SDK được chốt: ESP-IDF **v5.4.1**, Python **3.11**, ESP-Matter release/v1.4 commit
`abdb759d7aae8a445e3edc5769a10f5cb23feffc`. Board cần flash tối thiểu **4 MB**.
ESP32-C3 dùng Matter qua Wi-Fi 2.4 GHz, BLE dùng cho commissioning.

## Native build: Windows, macOS và Linux

Dùng cùng lệnh `uv run fw` trên cả ba hệ điều hành, không cần Docker hay WSL.
Python gọi trực tiếp trình cài toolchain và `idf.py` của ESP-IDF, không phụ thuộc
`apt`, `install.sh` hoặc thao tác source shell để cài SDK.

Chuẩn bị:

- Cài [Git](https://git-scm.com/downloads) và [uv](https://docs.astral.sh/uv/getting-started/installation/).
- Windows x64: dùng PowerShell và Git for Windows. Nên clone vào đường dẫn ngắn,
  ví dụ `C:\src\deye-inverter-esp`.
- macOS Intel/Apple Silicon: cài Command Line Tools bằng `xcode-select --install`.
- Linux x64/arm64: cài công cụ hệ thống. Với Ubuntu/Debian:

  ```sh
  sudo apt-get update
  sudo apt-get install -y git cmake ninja-build flex bison gperf libffi-dev libssl-dev libusb-1.0-0
  ```

Đường dẫn project và SDK không được chứa khoảng trắng vì giới hạn xử lý compile
flags của SDK này. Bootstrap cần Internet và vài GB dung lượng trống.

Trong thư mục project, chạy giống nhau từ PowerShell, bash hoặc zsh:

```sh
uv run fw bootstrap
uv run fw build
```

Không cần activate môi trường Python. uv tự chọn/cài Python theo `.python-version`
và chuẩn bị CLI từ `pyproject.toml`, không thay Python hệ thống. SDK được chốt phiên bản
trong `.tools/sdk`; biến môi trường `ESP_SDK_ROOT` cho phép chọn nơi khác.
Trên Windows có thể đặt `$env:ESP_SDK_ROOT = 'C:\esp\deye-sdk'` để rút ngắn đường dẫn;
trên macOS/Linux dùng `export ESP_SDK_ROOT="$HOME/esp/deye-sdk"`.
Đặt cùng giá trị này cho các lần bootstrap/build tiếp theo. Không trỏ vào SDK phiên
bản khác; công cụ sẽ từ chối checkout khác phiên bản.

Trên Windows, bootstrap tự thay các symlink cần cho Matter bằng junction/bản sao
và cài adapter chuyển compile flags CMake sang GN trong checkout SDK đã chốt.
Không cần bật Developer Mode hay chạy shell với quyền Administrator.
Compiler ESP32-C3, CMake, Ninja và GN đều dùng bản Windows native.

Build nằm trong `build/<target>/native-<platform>-<architecture>/`, tách riêng từng target và host để
không tái sử dụng CMake cache của WSL hay máy khác. Có thể chọn rõ đường dẫn:

```sh
uv run fw build --target esp32-c3 --build-dir build/esp32-c3
```

Kết quả có `esp32-c3.bin`, bootloader, partition table và `flasher_args.json`.
Đây là partition ứng dụng đơn, không có OTA; deploy là build rồi flash qua USB.
Không dùng lại thư mục `build/` gốc của luồng WSL cũ.

Tên firmware theo target, không theo hệ điều hành build. `--target` mặc định là
`esp32-c3`. Danh sách target nằm trong [targets.json](targets.json); lệnh `targets`
in danh sách JSON để GitHub Actions tạo matrix tự động. Khi thêm target mới, cần
thêm mapping IDF, cấu hình SDK/hardware phù hợp và xác minh firmware cho chip đó.
Hiện chỉ có `esp32-c3` được bật; chưa mặc định hỗ trợ các chip khác.

## Dev, deploy và xem log

Liệt kê cổng USB rồi chọn đúng cổng của board:

```sh
uv run fw ports
```

| Hệ điều hành | Ví dụ `--port` |
| --- | --- |
| Windows | `COM3` |
| macOS | `/dev/cu.usbmodem1101` |
| Linux | `/dev/ttyACM0` hoặc `/dev/ttyUSB0` |

Các lệnh dưới đây dùng ví dụ Windows; thay `COM3` bằng cổng vừa tìm trên máy:

```sh
# Build + flash
uv run fw deploy --port COM3
# Build + flash + monitor
uv run fw dev --port COM3
# Flash firmware đã build; không cần cài SDK
uv run fw flash --port COM3
# Chỉ xem log
uv run fw monitor --port COM3
```

Nếu dùng `--build-dir`, truyền cùng thư mục cho build, menuconfig, deploy và flash.
Flash dùng esptool 4.8.1 và offset do build sinh; monitor dùng baud 115200.
Nếu không kết nối được, giữ BOOT, bấm RESET, thả BOOT rồi flash lại.
Miniterm thoát bằng Ctrl+]; đóng monitor trước khi flash.
Trên Linux, tài khoản cần quyền truy cập serial (Ubuntu thường là nhóm `dialout`).

Mặc định console là **USB Serial/JTAG**, phù hợp cổng USB native của C3.
Nếu board dùng chip USB-UART ngoài (CH340/CP210x), chọn UART0 trong menuconfig:

```sh
uv run fw menuconfig
# Component config -> ESP System Settings -> Channel for console output
uv run fw build
```

`sdkconfig` nằm trong thư mục build đã chọn; `sdkconfig.defaults` là cấu hình chung
được commit. Lệnh `clean` dọn kết quả biên dịch bằng `idf.py clean` và giữ cấu hình.
Các script `bootstrap.sh`, `build.sh` và `flash.ps1` là
wrapper tương thích gọi bộ lệnh Python này.

## GitHub Actions

Workflow [Native firmware](.github/workflows/build.yml) chạy trên push, pull request
và workflow_dispatch. Matrix lấy target từ `targets.json`. Mỗi job cài SDK và build trực tiếp trên runner tương ứng:
Windows x64, Ubuntu x64, macOS Intel và Apple Silicon. Không dùng container hay WSL.
Các kiểm tra công cụ cũng chạy bằng `uv run fw test`.

Đã xác minh build firmware native trên Windows x64, gồm GN, link và kiểm tra dung
lượng partition. Các job macOS/Linux được cấu hình trong workflow; cần chạy workflow
trên GitHub để xác nhận kết quả trên những runner này.

Mỗi target có một artifact cùng tên, ví dụ `esp32-c3` chứa `esp32-c3.bin` và bộ file
flash. Job Ubuntu xuất artifact; các job Windows/macOS kiểm tra native build của
cùng target. Khi thêm target đã hỗ trợ vào `targets.json`, workflow tự tạo các job
và artifact riêng cho target đó.

Sau khi job thành công, tải artifact `esp32-c3`, giải nén và flash trực tiếp
trên máy có USB, không cần bootstrap SDK:

```sh
uv run fw flash --target esp32-c3 --build-dir path/to/extracted-firmware --port COM3
```

Giữ nguyên cấu trúc `bootloader/`, `partition_table/`, các `.bin` và `flasher_args.json`
khi giải nén. Firmware CI chạy trên cùng ESP32-C3 dù được build từ hệ điều hành nào.

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

Nếu console không dùng được và cần xóa sạch flash, đóng monitor rồi chạy
(thay `COM3` bằng cổng của board; lệnh này xóa cả Wi-Fi và fabric):

```sh
uv run fw erase-flash --port COM3
uv run fw flash --port COM3
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
