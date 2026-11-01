# ESP32-C3 Matter sensors (draft)

Firmware nháp ESP-Matter: một thiết bị Wi-Fi, ba cảm biến nhiệt độ giả lập
**AC Temperature**, **DC Temperature**, **Battery Temperature**. Mỗi cảm biến là một Temperature Sensor (0x0302), có
cluster Temperature Measurement (0x0402), NodeLabel và UniqueID riêng. Có thêm
Contact Sensor **Grid** (0x0015), dùng Boolean State (0x0045).
Endpoint 0 là Root Node; một aggregator gom các thiết bị con theo mô hình Matter bridge.
Switch mẫu đã được bỏ. Không cần cảm biến vật lý, relay hay GPIO ngoài.

| Sensor | Nhiệt độ ban đầu | Khoảng giả lập |
| --- | --- | --- |
| AC Temperature | 32 °C | 25–55 °C |
| DC Temperature | 36 °C | 30–65 °C |
| Battery Temperature | 28 °C | 20–45 °C |

Cứ **5 giây**, mỗi giá trị tăng/giảm ngẫu nhiên tối đa 0,50 °C, giới hạn trong
khoảng trên. Giá trị dùng đơn vị 0,01 °C theo Matter; cập nhật trên Matter thread
để controller nhận qua subscription. Đây chỉ là dữ liệu mô phỏng; AC Temperature/DC Temperature/Battery Temperature
đều là nhiệt độ, chưa có đo điện áp hoặc dòng điện thực.

Grid bắt đầu ở trạng thái **CLOSED**, sau đó chọn ngẫu nhiên OPEN/CLOSED mỗi 5 giây.
`StateValue=true` là CLOSED, `false` là OPEN theo
[quy ước Contact Sensor của Espressif](https://docs.espressif.com/projects/arduino-esp32/en/latest/matter/ep_contact_sensor.html).
Mỗi lần đổi trạng thái, firmware cập nhật attribute và phát event StateChange để
controller nhận thay đổi. Đây là contact giả lập, chưa đọc tín hiệu điện lưới thực.
Grid có NodeLabel/UniqueID riêng và được thêm sau ba sensor nhiệt độ để giữ nguyên
endpoint/định danh của các sensor nhiệt độ.

### Điện áp, dòng điện và công suất (bản nháp)

Năm kênh mới dùng Electrical Sensor (0x0510), Power Topology (TreeTopology) và
cluster **ElectricalPowerMeasurement (0x0090)**. Tên hiển thị theo cùng quy ước
với Temperature: **PV1 Power**, **PV2 Power**, **Grid Power**, **Battery Power**, **Load Power**.

| Endpoint | Tên | PowerMode | Điện áp giả lập | Dòng điện giả lập |
| --- | --- | --- | --- | --- |
| 7 | PV1 Power | DC | 250–450 V | 0–12 A |
| 8 | PV2 Power | DC | 250–450 V | 0–12 A |
| 9 | Grid Power | AC | 220–240 V khi Grid CLOSED; 0 V khi OPEN | −10–10 A; 0 A khi OPEN |
| 10 | Battery Power | DC | 48–56 V | −30–30 A |
| 11 | Load Power | AC | 220–240 V | 0–20 A |

Đây là cấu hình mô phỏng để thử controller, chưa phải dải điện áp/dòng điện của
inverter/BMS thực tế. Các kênh cập nhật mỗi 5 giây bằng random walk có giới hạn.
Grid Power đồng bộ với contact Grid. Quy ước dấu: Grid dương = nhập điện,
âm = xuất điện; Battery dương = xả, âm = sạc; PV dương = phát điện.
Load Power mô phỏng tải đầu ra inverter, luôn có dòng không âm; vẫn được cấp
điện khi Grid OPEN (giả lập inverter dùng PV/pin).

Ưu tiên hai attribute **Voltage** và **ActiveCurrent**; giữ **ActivePower** bắt buộc.
Đơn vị trên Matter là mV, mA, mW; log đổi sang V/A/W. Công suất giả lập tính
`P = V × I` (Grid/Load dùng mô hình hệ số công suất 1). Không thêm frequency, RMS,
reactive/apparent power hay thông số phụ trong bản thử này.

Delegate đọc dữ liệu từ từng kênh; Instance giữ đúng optional mask cho Voltage và
ActiveCurrent. Firmware kiểm tra đường đọc/encode Matter khi khởi động và đánh
dấu reporting khi giá trị thay đổi. Khác với việc chỉ ghi số vào log, các giá trị
này có thể được controller đọc qua cluster. Tham khảo
[hướng dẫn delegate của Espressif](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/app_guide.html).
Apple Home có thể chưa hiển thị Electrical Sensor hoặc từng attribute: cần thử
trên controller hiện tại, không suy ra khả năng UI từ kết quả build/log.

Bridge dùng tree composition: các sensor là endpoint ngang hàng; PartsList của
mỗi sensor rỗng. Temperature không chứa ElectricalPowerMeasurement/PowerTopology.
Các kênh điện dùng PowerTopology **TreeTopology** (FeatureMap `0x02`), nên số đo
chỉ thuộc chính kênh đó. Không dùng NodeTopology vì nó áp dụng số đo cho toàn node,
kể cả các sensor nhiệt độ không liên quan. Đây là cấu hình riêng với Descriptor
composition. Bản sửa PartsList trước đó chưa giải quyết được hiện tượng app hiển thị
tổng Power trong Temperature theo xác nhận của người dùng.
Firmware kiểm tra cả mã hóa/giải mã PartsList khi boot. Nếu app vẫn gom Power
vào Temperature sau cập nhật, cần kiểm tra cách controller đọc topology và cache;
firmware không tự xóa pairing.

Tính năng % pin lưu trữ qua Power Source đã được bỏ khỏi bản hiện tại sau khi
người dùng thử trên Apple Home/HomeKit và nhận thông báo không hỗ trợ. Endpoint 6
không còn được tạo; tính năng này hoãn đến tương lai. Battery Temperature vẫn
chỉ báo nhiệt độ. Không thay % pin bằng một sensor nhiệt độ/độ ẩm giả.

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

**Hiện mới lưu cấu hình logger; chưa kết nối hoặc đọc thanh ghi inverter.**
Các sensor vẫn chạy dữ liệu giả lập. Port 502 chưa đủ để xác định giao thức,
unit ID hoặc register map của logger/inverter thực tế.

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
5. Xem các sensor mà controller hỗ trợ; monitor ghi giá trị giả lập mỗi 5 giây, ví dụ:

```text
deye_sensors: TEMP AC Temperature endpoint=2 value=32.15 C (simulated)
deye_sensors: TEMP DC Temperature endpoint=3 value=35.72 C (simulated)
deye_sensors: TEMP Battery Temperature endpoint=4 value=28.04 C (simulated)
deye_sensors: CONTACT Grid endpoint=5 state=CLOSED (simulated)
deye_sensors: POWER PV1 Power endpoint=7 mode=DC voltage=320.000 V current=5.000 A power=1600.000 W (simulated)
deye_sensors: POWER PV2 Power endpoint=8 mode=DC voltage=340.000 V current=4.000 A power=1360.000 W (simulated)
deye_sensors: POWER Grid Power endpoint=9 mode=AC voltage=230.000 V current=3.500 A power=805.000 W (simulated)
deye_sensors: POWER Battery Power endpoint=10 mode=DC voltage=51.200 V current=-5.000 A power=-256.000 W (simulated)
```

Endpoint ID thực tế được in ở log boot. Identify cũng ghi log, không nháy LED.
Credentials và fabric do SDK lưu trong NVS, không cần pair lại sau mỗi lần reboot.
Các giá trị mô phỏng bắt đầu lại từ nhiệt độ ban đầu sau reboot, không ghi NVS mỗi chu kỳ.
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

## Kiểm tra trên board

Bản sensor đã được build/flash trên ESP32-C3 COM3: ba nhiệt độ ở endpoint 2/3/4,
Grid ở endpoint 5, cập nhật mỗi 5 giây; manufacturer/model/serial đúng cấu hình.
Thử nghiệm Power Source trước đây đã chạy được trên firmware nhưng người dùng
xác nhận Apple Home báo không hỗ trợ, nên endpoint 6 được loại bỏ. Pairing được
giữ khi flash. Checklist cho bản hiện tại:

Năm Electrical Sensor ở 7/8/9/10/11 đã build/flash và qua kiểm tra AAI Read/TLV
encode trên board. Log Voltage/ActiveCurrent/ActivePower đúng đơn vị và P=V×I.
Descriptor PartsList đã được đọc/giải mã trên board: bridge có 9 con, từng
sensor có 0 con. Mức hỗ trợ hiển thị trong Apple Home đang chờ người dùng thử.

- Boot chưa pair: log QR/manual code và bốn sensor AC Temperature, DC Temperature, Battery Temperature, Grid.
- Điện: đọc Voltage/ActiveCurrent/ActivePower của PV1 Power, PV2 Power, Grid Power,
  Battery Power, Load Power ở 7/8/9/10/11; không có endpoint Power Source 6 sau khi startup hoàn tất.
- Pair: có IP, `PAIR complete`, controller nhận temperature/contact; không còn endpoint Power Source.
- Mỗi 5 giây: log ba nhiệt độ và OPEN/CLOSED của Grid.
- Grid: controller nhận cả trạng thái attribute và StateChange khi tiếp điểm đổi trạng thái.
- Tên/UniqueID: phân biệt được AC Temperature, DC Temperature, Battery Temperature, Grid; không đổi định danh sau reboot.
- Metadata: manufacturer theo tên project, model theo target, serial có MAC và hậu tố riêng.
- Reboot: controller kết nối lại; giá trị mô phỏng bắt đầu lại từ 32/36/28 °C.
- Factory reset: pair lại được từ đầu.

Tham khảo [ESP-Matter release/v1.4](https://github.com/espressif/esp-matter/tree/release/v1.4)
và [hướng dẫn phát triển Espressif](https://docs.espressif.com/projects/esp-matter/en/release-v1.4/esp32/developing.html).
