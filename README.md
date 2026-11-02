# ESP32-C3 Deye Matter sensors

Firmware ESP-Matter đọc dữ liệu thật qua **Modbus TCP, FC03**, đưa lên 9 sensor
Matter: AC/DC/Battery Temperature, Grid contact, PV1/PV2/Grid/Battery/Load Power.
Đã bỏ simulator; không phát random hoặc số mặc định khi mất kết nối.
Giữ nguyên endpoint, UniqueID, metadata và pairing của bản trước.

Profile hiện tại là **deye-single**, từ dự án Homebridge đã dùng với
**SUN-6K-SG05LP1-EU-SM2-P**, unit ID **1**. Không mặc định hỗ trợ mọi model Deye.
Register map dùng chung cho firmware và CLI ở
[profiles/deye-single.json](profiles/deye-single.json); firmware sinh header khi build.
Tham khảo [Deye Modbus protocol v118](https://www.photovoltaikforum.com/core/attachment/449240-deye-modbus-protocol-v118-pdf/).

| Endpoint | Sensor | Register | Quy đổi |
| --- | --- | --- | --- |
| 2 | AC Temperature | 91 | (raw − 1000) / 10 °C |
| 3 | DC Temperature | 90 | (raw − 1000) / 10 °C |
| 4 | Battery Temperature + % pin | Temperature=182, SOC=184 | (raw − 1000) / 10 °C; 1% |
| 5 | Grid | 150 | Điện áp lưới; CLOSED khi ≥100 V, giữ CLOSED khi ≥80 V |
| 7 | PV1 Power (DC) | W=186, V=109, A=110 | 1 W; 0,1 V; 0,1 A |
| 8 | PV2 Power (DC) | W=187, V=111, A=112 | 1 W; 0,1 V; 0,1 A |
| 9 | Grid Power (AC) | W=169, V=150, A=160 | 1 W; 0,1 V; 0,01 A |
| 10 | Battery Power (DC) | W=190, V=183, A=191 | 1 W; 0,01 V; 0,01 A |
| 11 | Load Power (AC) | W=178, V=157, A=179 | 1 W; 0,1 V; 0,01 A |

Grid/Load/Battery W và A là signed int16; raw `65535` nghĩa là −1,
không phải unavailable. Điện áp và PV W/A là unsigned, `65535` là unavailable;
0 là số đo hợp lệ. Grid dương=nhập, âm=xuất; Battery dương=xả, âm=sạc.
**ActivePower đọc trực tiếp từ thanh ghi**, không tính V×A, không cộng tổng các kênh.
Grid dùng internal metering, không phải external CT 170–172. Load là total load
với điện áp/dòng L1 theo map single-phase; cần so với màn hình và wiring thực tế.

### Gom nhóm đọc và xử lý lỗi

Mỗi vòng dùng 6 nhóm tuần tự: **90–91, 109–112, 150, 157–160, 169, 178–191**,
tổng **26 words**. Không đọc cả dải lớn hoặc gửi request song song trong một client.
Nhóm lỗi chỉ làm các giá trị thuộc nhóm đó unavailable; các nhóm khác vẫn được đọc.
Mỗi nhóm có deadline 3 giây, kiểm tra MBAP transaction/protocol/unit ID, độ dài,
FC03/exception và byte count, hỗ trợ response bị chia nhiều TCP packet.
Mỗi nhóm dùng kết nối riêng theo cách logger đã hoạt động trong dự án Homebridge.

Worker mạng chạy riêng, chỉ chuyển snapshot sang Matter thread để cập nhật.
Vòng tiếp theo bắt đầu **5 giây sau khi vòng trước đã publish xong**, không chồng vòng.
Nếu mất dữ liệu, nhiệt độ và từng V/A/W trả `null`; sensor thiếu dữ liệu được đánh
`Reachable=false`. Grid giữ trạng thái trước đó nhưng unreachable, không phát
sự kiện mất điện từ lỗi mạng. Khi đọc lại thành công, sensor tự phục hồi.
Không ép số điện về 0 dựa trên Grid contact; giữ nguyên số inverter báo.

Electrical Sensor (0x0510) dùng PowerTopology **TreeTopology** (`0x02`) và
ElectricalPowerMeasurement (0x0090), đúng AC/DC cho từng kênh, đơn vị mV/mA/mW.
Không thêm frequency, RMS hoặc energy counter. Accuracy dùng biên biểu diễn
thanh ghi rộng; không quảng cáo độ chính xác hiệu chuẩn chưa biết.
Mỗi leaf có PartsList rỗng, tất cả là con của aggregator 1. Không dùng NodeTopology
vì số đo sẽ áp dụng toàn node. Người dùng đã xác nhận sửa TREE hết lỗi Power
hiển thị trong Temperature; chưa xác định số Power trước đó thuộc kênh nào.
Firmware kiểm tra FeatureMap, PartsList Read/TLV decode và EPM Read/TLV encode
khi boot, rồi kiểm tra EPM thêm khi có bộ số đo thật đầu tiên.

Battery Temperature có thêm cluster Power Source với `BatPercentRemaining`, lấy SOC thật
từ register 184 (0–100%, nằm sẵn trong nhóm 178–191, không thêm request).
Matter mã hóa 0–200 tương ứng 0–100%; lỗi đọc/SOC ngoài khoảng trả `null`,
Power Source unavailable và Battery Temperature unreachable; nhiệt độ vẫn cập nhật độc lập.
EndpointList của Power Source chỉ gồm endpoint 4; endpoint 6 vẫn được bỏ trống.
Battery Power chỉ có W/V/A, không còn cluster Power Source. Battery Temperature tiếp tục
đo nhiệt độ thật từ register 182 theo °C; endpoint, identity và pairing được giữ nguyên.
Người dùng báo % pin dường như không hiện khi gắn vào Battery Power và đã yêu cầu
chuyển sang Battery Temperature. Khả năng hiển thị % trên Apple Home cần thử trên app.
Đã build và flash COM3 ngày 2026-11-02, xác nhận ba vòng đọc SOC 100% tại endpoint 4
(Matter raw 200) cùng nhiệt độ thật 38 °C; W/V/A ở endpoint 10 tiếp tục cập nhật.

### CLI đọc local và log đối chiếu

Dùng host/port từ `config.local.json` hoặc `--config`, có thể override bằng
`--host`, `--logger-port`. Không cần board ESP32 hay SDK để dùng CLI đọc logger.
CLI chỉ gửi FC03, không có lệnh ghi thanh ghi inverter.

```sh
# Một snapshot, in bảng W/V/A và nhiệt độ
uv run fw read
# Ba snapshot, nghỉ 5 giây sau mỗi vòng
uv run fw read --count 3
# Quan sát foreground; thoát bằng Ctrl+C
uv run fw read --watch
# JSONL đầy đủ trên stdout và file
uv run fw read --once --json --log .local/deye.jsonl
# Chỉ kiểm tra điện áp lưới, đúng một request
uv run fw read --grid-only
# Override khi cần chẩn đoán logger khác
uv run fw read --host 192.168.1.100 --logger-port 502 --unit-id 1
```

Mặc định append `.local/deye.jsonl` (gitignore). Mỗi dòng có timestamp UTC,
profile, host/port/unit, giá trị theo đơn vị C/V/A/W, raw từng metric, địa chỉ
thanh ghi, toàn bộ words của mỗi nhóm, lỗi và thời gian đọc. CLI báo lỗi nếu
snapshot thiếu dữ liệu nhưng vẫn lưu phần đọc được. `--unit-id` và `--timeout`
là tùy chọn chẩn đoán CLI; firmware hiện dùng unit 1, timeout 3 giây.

Metadata của bridge và từng thiết bị con:

- Manufacturer/VendorName: `deye-inverter-esp` (tên project).
- Model/ProductName: tên target, hiện là `esp32-c3`.
- SerialNumber: `<target>-<MAC>`; sensor con có hậu tố riêng, ví dụ `-AC`, `-DC`, `-Battery`, `-Grid`.
- Serial của bridge/sensor và UniqueID của sensor ổn định qua reboot/factory reset; endpoint AC Temperature/DC Temperature/Battery Temperature/Grid
  giữ nguyên 2/3/4/5. VID/PID vẫn là giá trị thử nghiệm đã chốt trong SDK config.

Tên hiển thị thống nhất: AC Temperature, DC Temperature, Battery Temperature,
Grid, PV1 Power, PV2 Power, Grid Power, Battery Power, Load Power. Key identity/serial của
sensor cũ vẫn là AC/DC/Battery/Grid; kênh mới dùng PV1Pwr/PV2Pwr/GridPwr/BatPwr/LoadPwr.
Firmware tự chuyển các NodeLabel mặc định cũ sang tên mới trong NVS, giữ các tên
tùy chỉnh khác. Controller có thể giữ tên riêng đã đặt trong app; nếu vậy đổi tên
ngay trong app, không cần factory reset chỉ để đổi nhãn.

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

## Cấu hình Deye logger qua USB

Tạo `config.local.json` theo [config.example.json](config.example.json), rồi nhập
host và port của logger:

```json
{
  "logger": {
    "host": "192.168.1.100",
    "port": 502
  }
}
```

`config.local.json` được gitignore, chỉ dùng cho môi trường local dev/test.
Không nhúng địa chỉ local vào binary hoặc artifact CI. Khi file có mặt ở gốc repo,
`flash`, `deploy`, `dev` tự ghi cấu hình sau bước flash. Khi không có file, các lệnh
này giữ nguyên cấu hình đã lưu trên board. Có thể chỉ rõ file khác bằng `--config`:

```sh
uv run fw deploy --port COM3
uv run fw flash --port COM3 --config config.local.json
# Chỉ đổi cấu hình, không build/flash/reset hoặc pair lại
uv run fw config --port COM3
```

CLI kiểm tra JSON trước khi build/flash, gửi lệnh console qua USB và chỉ báo thành
công khi firmware commit/read-back NVS rồi trả xác nhận đúng request token.
Nếu không nhận xác nhận, CLI báo lỗi; firmware có thể đã flash xong nhưng chưa
xác nhận cấu hình. Board phải chạy firmware có hỗ trợ lệnh logger.
Trên macOS/Linux, thay `COM3` bằng cổng serial tương ứng.

Cấu hình được lưu thành một blob version 1 trong namespace NVS `deye_logger`,
giữ qua reboot. Thao tác này không xóa hoặc ghi đè Wi-Fi/fabric Matter, không đổi
partition layout. Host hỗ trợ IPv4/hostname ASCII dài 1–63 ký tự; port 1–65535.
Xem cấu hình bằng console `matter esp logger`.

Firmware đọc cấu hình ở đầu mỗi vòng, nên đổi host/port qua USB có hiệu lực
ở vòng tiếp theo. Profile firmware hiện cố định deye-single, unit ID 1;
không suy ra register map chỉ từ port 502.

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
4. Chọn Wi-Fi **2.4 GHz**, chờ `PAIR complete; temperature, contact and electrical power sensors are ready`.
   Điện thoại/controller và board cần mạng LAN cho phép IPv6, mDNS/multicast;
   tránh mạng guest cô lập client.
5. Xem các sensor mà controller hỗ trợ; monitor in dữ liệu Modbus, ví dụ:

```text
deye_modbus: FC03 logger:502 unit=1 registers=90..91 status=ok elapsed_ms=110
deye_sensors: TEMP AC Temperature endpoint=2 value=43.60 C register=91 (Modbus)
deye_sensors: CONTACT Grid endpoint=5 state=CLOSED voltage=214.700 V (Modbus)
deye_sensors: POWER Grid Power endpoint=9 mode=AC voltage=214.700 V current=0.890 A power=1160.000 W (Modbus)
```

Các số trên là ví dụ một snapshot, không phải giá trị cố định.

Endpoint ID thực tế được in ở log boot. Identify cũng ghi log, không nháy LED.
Credentials và fabric do SDK lưu trong NVS, không cần pair lại sau mỗi lần reboot.
Sau reboot, giá trị bắt đầu unavailable đến khi đọc được Modbus; không ghi số đo vào NVS mỗi chu kỳ.
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
  để tiếp tục. Chọn phòng, đặt tên bridge ví dụ **Deye Sensors**.
- Kiểm tra AC Temperature, DC Temperature, Battery Temperature và Grid. Cách gom nhóm,
  hiển thị hoặc cho đổi tên từng sensor tùy controller; đối chiếu với log `TEMP`.
- iOS 18 trở lên có thể thêm và điều khiển Matter cục bộ mà không cần home hub;
  muốn điều khiển từ xa cần home hub. Xem
  [Apple: pair Matter](https://support.apple.com/en-ca/102135) và
  [Apple: điều khiển từ xa](https://support.apple.com/en-gb/102056).

## Pair lại / khôi phục

Khi nâng cấp từ firmware switch cũ sang bản sensor, xóa thiết bị cũ khỏi controller
và factory reset rồi pair lại để controller đọc cấu trúc endpoint mới.
Nếu controller chưa nhận Grid sau khi nâng cấp từ bản ba sensor, thử tải lại danh
sách phụ kiện của bridge; nếu vẫn thiếu, xóa thiết bị và factory reset/pair lại.

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

## Kiểm chứng

Chạy `uv run fw test`: kiểm tra CLI/config, fake logger loopback, response phân
mảnh, lỗi MBAP/FC03, timeout, lỗi một nhóm, sentinel/signed/scale và JSONL.
Nếu có compiler C++ host (g++/clang++), suite biên dịch và chạy decoder/protocol
firmware trên host; không cần SDK hoặc truy cập inverter cho các unit test.

Giữ các kiểm tra trên board: 9 leaf endpoint/identity ổn định, PowerTopology TREE,
PartsList rỗng, EPM read/encode với số đo thật, temp/contact/power cập nhật và
phục hồi sau lỗi logger. Build/read/log pass không đồng nghĩa mọi số đo đã được
đối chiếu với màn hình inverter hoặc mọi attribute hiển thị trong Apple Home.
Không tự factory reset để kiểm chứng cập nhật này.

Đã kiểm chứng bản Modbus ngày 2026-11-01: native Windows build, flash COM3,
23 tests pass (gồm C++ native), 5 kênh EPM encode được số đo thật. Thử mất logger
và khôi phục trong cùng phiên USB xác nhận null/unreachable rồi tự phục hồi,
không reboot, không phát Grid outage giả; cấu hình local và pairing được giữ.
Ba vòng CLI đọc thật mất 672/500/485 ms; đây là số quan sát, không phải cam kết
hiệu năng. Log local bị ignore: `.local/deye.jsonl`, bảng đối chiếu
`.local/deye-readings.md`. Chưa đối chiếu dòng AC với màn hình inverter.

Tham khảo [ESP-Matter release/v1.4](https://github.com/espressif/esp-matter/tree/release/v1.4)
và [hướng dẫn phát triển Espressif](https://docs.espressif.com/projects/esp-matter/en/release-v1.4/esp32/developing.html).
