# Working on deye-inverter-esp

These instructions apply to the entire repository. Read this file before changing
code and cross-check it against the current code and README. New user requests
take precedence over recorded decisions. Update this file when project direction
changes. Keep AGENTS.md in English; continue communicating with the user in Vietnamese.

## Goals and collaboration

- This is ESP-Matter firmware for the Deye inverter project. It exposes
  sensors from a real inverter over read-only Modbus TCP. The original random simulator
  has been replaced; never silently substitute simulated values for failed reads.
- Communicate concisely in Vietnamese. Report build/flash results and what remains
  unverified. Complete authorized work without asking again about settled decisions.
- Use English throughout source code, comments, log/error messages and HTML/UI.
  Vietnamese is the language for conversations with the user, not the product UI.
- Prefer native Windows, macOS, and Linux workflows. Do not move the main workflow
  to WSL, Docker, `/mnt/c` paths, automatic `apt` installation, or machine-specific scripts.
- Use short commands such as `uv run fw ...` in documentation and instructions.
  Do not require venv activation or repeatedly use long Python command lines.
- Do not add hardware, inverter protocols, or targets without a request.
  Do not restore the sample switch or outlet on the generic target. The requested
  Sonoff BASIC R4 relay below is a board-specific exception.

## Code map

- `main/app_main.cpp`: bridge/endpoints, live snapshot publication, attribute/event updates,
  pairing lifecycle, topology validation, and console.
- `main/device_identity.h`, `main/device_identity.cpp`: manufacturer/model/serial
  and the root device identity provider.
- `main/electrical_measurements.h`, `main/electrical_measurements.cpp`: five
  Electrical Sensor channels, delegates/AAI, Matter read checks, and live electrical values.
- `main/CMakeLists.txt`: application dependencies and target-derived model name.
- `main/sonoff_relay.cpp`, `.h`, `main/button_debounce.h`: BASIC R4-only GPIO
  relay, Matter OnOff synchronization and debounced physical button.
- `scripts/test_sonoff.py`, `scripts/button_core_test.cpp`: native button checks.
- `main/logger_config.h`, `main/logger_config.cpp`: persisted logger host/port and
  console commands/shared validated commit-and-read-back API; read by the Modbus worker each cycle.
- `main/logger_web.cpp`, `.h`, `main/logger_config.html`: embedded LAN HTTP logger form.
- `main/html_template.h`: allocation-free HTML template streaming, exercised by
  the native logger form tests.
- `main/logger_config_validation.h`, `main/logger_form.h`: portable shared validation
  and bounded URL-encoded form parser; native checks in `scripts/test_logger_web.py`
  and `scripts/logger_form_core_test.cpp`.
- `scripts/configure_logger.py`: serial config transfer with commit acknowledgement.
- `main/inverter_data.cpp`, `.h`, `main/modbus_protocol.h`: network worker, snapshots,
  portable decoding and Modbus framing.
- `profiles/deye-single.json`, `scripts/generate_registers.py`: shared CLI/firmware map.
- `scripts/modbus.py`, `scripts/test_modbus.py`, `scripts/modbus_core_test.cpp`:
  read-only local CLI, fake logger tests, and native C++ decoder/protocol checks.
- `scripts/project.py`: `fw` CLI, SDK installation, build/menuconfig, serial, and flash.
- `scripts/native_args.py`: Windows adapter for CMake-to-GN compiler flags.
- `scripts/test_project.py`: CLI/tooling, flash manifest, quoting, and junction tests.
- `targets.json`: shared target list for the CLI and GitHub Actions matrix.
- `sdkconfig.defaults`, `partitions.csv`: default configuration and flash layout.
- `.github/workflows/build.yml`: native CI and artifacts named after targets.
- `README.md`: user instructions, pairing, live Modbus diagnostics, and verification status.

## Build, development, and flash

Run from the repository root:

```sh
uv run fw bootstrap
uv run fw build
uv run fw test
uv run fw ports
uv run fw flash --port COM3
uv run fw deploy --port COM3
uv run fw dev --port COM3
uv run fw monitor --port COM3
uv run fw menuconfig
```

- `build` compiles; `flash` writes an existing build; `deploy` builds and flashes;
  `dev` builds, flashes, and monitors. Exit the monitor with **Ctrl + ]**.
- Default target: `esp32-c3` (IDF ID: `esp32c3`). Use `--target` to select explicitly.
- Default local build directory: `build/<target>/native-<platform>-<architecture>/`.
  When using `--build-dir`, pass the same directory to build/menuconfig/flash/deploy.
  Do not mix CMake caches across hosts, targets, or the old WSL workflow.
- Main output: `esp32-c3.bin`. Name firmware after its target, not a demo or host OS.
  Validate configuration and firmware on each added chip; adding a JSON entry alone
  does not establish hardware support.
- Flash using the generated `flasher_args.json`, including binary paths and offsets.
  Do not hardcode offsets or flash only the app when bootloader/partition table are needed.
- Default SDK location: `.tools/sdk`; override with `ESP_SDK_ROOT`. With the pinned
  SDK, project and SDK paths must not contain spaces.
- `.python-version` selects Python 3.11.13; `pyproject.toml` registers `fw`.
  The CLI uses `.venv`. ESP-IDF venv creation must use `BASE_PYTHON`, not the project
  venv interpreter to create a nested SDK venv.
- Pinned SDK: ESP-IDF `v5.4.1`, ESP-Matter
  `abdb759d7aae8a445e3edc5769a10f5cb23feffc`, GN
  `05eed8f6252e2dd6b555e0b65192ef03e2c4a276`. Do not upgrade just to remove a warning.
- On Windows, bootstrap handles symlinks using junctions/copies and the GN adapter.
  Pass CMake paths with `/` separators and pin CMake Python to the SDK venv.
  Bootstrap creates the minimal Pigweed environment file; do not rely on an SDK
  manually prepared in an earlier session.
- Do not directly patch `managed_components/` or the SDK cache as a lasting fix.
  Changes must be reproducible on clean machines and CI.

## Agreed Matter model

One Root Node, one aggregator/bridge, and these child devices:

| Endpoint | Name | Type / data |
| --- | --- | --- |
| 0 | Root Node | Basic Information, commissioning |
| 1 | Aggregator | Groups child devices |
| 2 | AC Temperature | Temperature Sensor; register 91 |
| 3 | DC Temperature | Temperature Sensor; register 90 |
| 4 | Battery Temperature | Temperature Sensor; register 182; Power Source battery percentage (register 184) |
| 5 | Grid | Contact Sensor; register 150 voltage presence with hysteresis |
| 7 | PV1 Power | DC Electrical Sensor; Voltage, ActiveCurrent, ActivePower |
| 8 | PV2 Power | DC Electrical Sensor; Voltage, ActiveCurrent, ActivePower |
| 9 | Grid Power | AC Electrical Sensor; Voltage, ActiveCurrent, ActivePower |
| 10 | Battery Power | DC Electrical Sensor; Voltage, ActiveCurrent, ActivePower |
| 11 | Load Power | AC Electrical Sensor; Voltage, ActiveCurrent, ActivePower |

Use the agreed display names above. Spell Temperature and
ElectricalPowerMeasurement correctly. There is currently no battery percentage endpoint.
Keep the old identity/serial keys `AC`, `DC`, and `Battery` at endpoints 2/3/4;
do not derive identity from display names when renaming. Only migrate NVS NodeLabels
that exactly match the old defaults (`AC`, `DC`, `Battery` at 2/3/4); preserve other
names assigned by users/controllers.

- Preserve existing endpoint IDs and UniqueIDs when adding devices. Do not change
  creation order for existing sensors or use long display names as identity keys
  that can truncate to the same value.
- Each child has Bridged Device Basic Information, the aggregator as its parent,
  and its own NodeLabel, UniqueID, and metadata.
- Publish each completed Modbus cycle, then wait **5 seconds** before the next cycle.
  Matter temperatures use nullable int16 in 0.01 °C, decoded as (raw-1000)*10.
- Grid uses voltage presence hysteresis: become CLOSED at >=100 V, remain CLOSED
  at >=80 V. true=CLOSED, false=OPEN. Emit StateChange only on a real state change.
  Read errors mark it unreachable and retain its state, never fabricate an outage.
- The standalone Power Source was removed after the user reported it unsupported
  in Apple Home (2026-11-01). On 2026-11-02 the user requested percentage on
  Battery Power, with Battery Temperature as fallback. The Battery Power trial
  passed firmware checks, but the user reported percentage apparently did not
  display and explicitly requested moving it to Battery Temperature (2026-11-02).
  Battery Temperature (endpoint 4) now carries a Power Source Battery cluster and
  nullable BatPercentRemaining from register 184. Remove that cluster from
  Battery Power (endpoint 10); keep all endpoint IDs and identities stable.
  There is no separate percentage endpoint. Power Source EndpointList is only {4}.
  Keep temperature on register 182 in degrees Celsius; never encode SOC as temperature.
  Read failures/out-of-range SOC publish null, Status unavailable, and mark Battery
  Temperature incomplete/unreachable while preserving independent temperature readings.
  Battery Power reachability depends only on its W/V/A again. On 2026-11-02 the
  user confirmed Apple Home correctly displays percentage on Battery Temperature.
  Keep this attachment; this confirms this UI behavior, not every Power Source attribute.
  Earlier Battery Power trial: native Windows build, 23 tests (including native C++),
  COM3 flash and config acknowledgement; live SOC=100% read as Matter raw=200 at
  endpoint 10, with null/offline and recovery verified without reboot. Those logs
  do not verify the new endpoint 4 attachment. Logs: ignored .cache/battery-percent-*.log.
  Endpoint 4 attachment verified on 2026-11-02: native Windows build, COM3 flash
  with hash verification and config commit/read-back acknowledgement. Three live
  cycles read SOC=100% as raw=200 at endpoint 4 alongside Battery Temperature=38 C;
  Battery Power W/V/A continued at endpoint 10. Startup asserts that only Battery
  Temperature among temperature leaves has Power Source and endpoint 10 has none.
  Logs: ignored .cache/battery-temp-percent-*.log. App display was confirmed by the user.
- Endpoint 6 and key `SOC` belong to the retired Power Source experiment. Do not
  reuse them for another sensor type. If restoration is requested, verify actual
  controller support; raw BatPercentRemaining 0..200 represents 0..100%.
- FreeRTOS tasks schedule work through `PlatformMgr().ScheduleWork`. Write attributes
  and events on the Matter thread or under the ChipStack lock, not arbitrary timers/tasks.

### Requested ElectricalPowerMeasurement support

- Prioritize Voltage/ActiveCurrent. ActivePower is mandatory. The user considers
  frequency, RMS, and other metrics unimportant for now; do not expand all AC attributes.
- Electrical Sensor device type 0x0510 uses PowerTopology TreeTopology and EPM 0x0090.
  Enable only DirectCurrent for PV1/PV2/Battery and only AlternatingCurrent for
  Grid/Load. Avoid the SDK convenience helper that enables both AC and DC on every channel.
- Matter units: nullable int64 mV/mA/mW; log V/A/W. Read inverter-reported watts
  directly. Do not compute active AC power as V*I, force zeros from Grid contact,
  clamp real measurements to old simulation ranges, or aggregate unrelated channels.
- The delegate provides PowerMode, three MeasurementTypes, and Accuracy with
  measured=true. Since calibrated tolerance is unknown, use conservative full-span
  register bounds, not invented precision. Partial failures preserve independent
  attributes, set missing values null, and mark incomplete sensors unreachable.
- Retain a separate Instance/Delegate per endpoint and register after `esp_matter::start`
  under the ChipStack lock. The SDK default callback uses optional mask 0, rejecting
  Voltage/ActiveCurrent even when their attributes exist. Retain the correct mask.
- Updating the backing store with `attribute::update` is insufficient for delegate-backed
  reads. Update delegate state and call `MatterReportingAttributeChangeCallback`.
- Startup checks exercise AAI Read + TLV encoding for FeatureMap, PowerMode,
  NumberOfMeasurementTypes, Accuracy, Voltage, ActiveCurrent, and ActivePower.
- The public SDK allocates sequential IDs only. Reserve retired slot 6 with a temporary
  aggregator, then remove it under the ChipStack lock immediately after start and before
  registering the new AAIs. Final runtime state has no endpoint 6;
  electrical channels remain at 7..11. Do not reuse SOC or the retired device identity.
- Passing on-board read/encoding checks does not establish Apple Home EPM support.
  The current goal is to let the user test actual controller support.

## Sonoff BASIC R4 target (requested 2026-11-07)

- `sonoff-basic-r4` is a board variant using IDF `esp32c3`; `esp32-c3` stays the
  default. Build with `uv run fw build --target sonoff-basic-r4`. CMake receives
  `FIRMWARE_TARGET` separately from `IDF_TARGET`, names the binary/model after the
  selected board, and rejects reuse of a build directory for another variant.
- Keep all Deye sensors, endpoint IDs and identity keys. Append a bridged On/Off
  Plug-in Unit named `Relay` at endpoint 12 only on this target. Slot 6 remains retired.
  Include endpoint 12 in the bridge/leaf Descriptor checks; no EPM on this relay.
- BASIC R4 button is GPIO9 active-low with pull-up; relay is GPIO4 active-high.
  Pin reference: https://digiblur.com/wiki/devices/relays/sonoff-basic-r4/ .
  Do not confuse BASIC R4 with MINI R4. `main/sonoff_relay.cpp` is compiled only
  for BASIC R4. Poll/debounce the button for 40 ms, schedule changes onto the Matter
  thread, and share the OnOff attribute with remote commands. Short release toggles
  once; holding for 5 seconds opens Matter pairing once without toggling the relay,
  including on release. A button held during startup must be released before use.
- Long press opens a 600-second Basic Commissioning Window on the Matter thread.
  Uncommissioned devices use all supported transports (BLE/network); commissioned
  devices use DNS-SD on their existing network. Preserve Wi-Fi/fabrics, never reset
  or erase NVS for this action. Retain an already-open commissioning session and
  report failures. Print onboarding codes when opening a new window.
- Drive the relay OFF before Matter/NVS initialization and explicitly publish OFF
  at startup. Power-on behavior is always OFF, independent of previous state.
  Relay reachability is independent of Modbus. No inverter writes are added.
- `sdkconfig.defaults.sonoff-basic-r4` overlays the common defaults: UART0 console
  on the serial pads (115200), no USB Serial/JTAG console, DIO flash. Keep the
  generic target's USB console. Use a distinct build directory and flash manifest.
- The physical BASIC R4 serial pads do not provide the normal dev-board reset
  wiring. In this session RTS hard reset and ROM soft reset did not reliably start
  the flashed app. If config acknowledgement times out after verified flash, power
  cycle with the button released, then use `uv run fw config --port COM5`. Do not
  confuse post-flash config failure with failure to write the firmware image.
- Poll the physical button using the shared `ESP_TIMER_TASK` timer service, then
  ScheduleWork for Matter actions. Avoid a dedicated button task/stack on C3.
- Scope is short-press local relay, 5-second Matter pairing and Matter OnOff control.
  GPIO5 Magic Switch, GPIO6 LED and thermal protection are not implemented.
  Never claim full stock feature/protection parity. Disconnect mains entirely
  while connecting serial; use 3.3 V UART. Physical relay/button and controller UI
  behavior remain unverified until tested on actual BASIC R4 hardware.
- COM5 was observed as CH340 and ESP32-C3 with 4 MB flash.
  Full flash backup on 2026-11-07 passed esptool digest comparison; it does not
  establish hardware verification of this new firmware. Do not overwrite stock
  merely to test a build without authorization for that flash.
- Verified on 2026-11-07: native Windows builds and native C++ Modbus, debounce,
  short/long-press and HTML form/streaming checks (27 tests). The Sonoff manifest
  selects `sonoff-basic-r4.bin`, ESP32-C3, DIO, 4 MB and UART0 console. The user
  explicitly authorized flashing COM5. Final firmware writes passed hash checks;
  startup verified the bridge with 10 children, five EPM/TREE checks and relay OFF.
  Physical button/relay, long-press pairing and controller UI remain unverified.
- After the RAM fix, COM5 ran multiple unavailable-data cycles with monotonic
  uptime (11219 -> 30157 ms in one 40-second capture), no abort/reboot, and HTTP
  port 80 listening. Heap after service startup was 28444 bytes; after main-task
  cleanup, diagnostics showed 32440 -> 32360 bytes free and largest block 10752.
  Local logger configuration passed fresh-token commit/read-back and console get.
  The standard `uv run fw config --target sonoff-basic-r4 --port COM5` command
  also passed after the app was running. Both final target builds passed on Windows.
  The board has no commissioned Wi-Fi/fabric yet; real Modbus data and browser
  access need commissioning. Manual pairing code from this boot: `34970112332`.
  Logs: ignored `.cache/sonoff-memory-final-{flash,runtime,verify}.log`.

## Metadata and SDK notes

### C3 RAM budget

- The first Sonoff flash exposed an allocation failure creating the 6144-byte
  Modbus task after Matter/console startup. A first pool reduction booted with
  only about 10.9 KB free and a 4096-byte largest block; do not consider that
  enough headroom for copying/growing a complete HTML page.
- Keep the common bounded BLE/Wi-Fi pools in `sdkconfig.defaults`: one BLE
  commissioning connection, peripheral/broadcaster roles, MSYS pools 8/8,
  transport ACL/event/discard counts 8/12/4, Wi-Fi static RX 6 (BA window 6),
  dynamic RX/TX 16/16. BLE security, Wi-Fi/BLE coexistence and IPv4/IPv6 remain enabled.
- Wi-Fi IRAM performance options are disabled to leave RAM for the bridge,
  Modbus, UART and HTTP. This favors memory over high-throughput Wi-Fi. Do not
  reduce task stacks to hide allocation failures or disable BLE commissioning.
- Exclude unused cluster servers using the pinned SDK light example as a base,
  preserving bridge, temperatures, Boolean State, EPM, TREE, Power Source and relay
  clusters. Keep startup topology/read checks. Do not restore every optional
  cluster by default without checking RAM and the required endpoint model.
- Defaults do not override old values in an existing build's sdkconfig. Apply
  changes explicitly or use a fresh per-target/host build directory when checking
  the new budgets. Never patch the SDK or managed components.
- Log free heap and largest block around service startup. Serve the HTML template
  directly from embedded flash in chunks with `html_template.h`; do not allocate
  or grow a whole-page std::string. Native tests cover substitutions, empty chunks
  and send-failure propagation. Physical commissioning/web use still needs checking.

- Manufacturer/VendorName is the **project name `deye-inverter-esp`**.
- Model/ProductName follows the **target**, currently `esp32-c3`, supplied by CMake.
- Root serial: `<target>-<MAC>`; children have distinct suffixes such as `AC`, `DC`,
  `Battery`, and `Grid`. Do not hardcode the attached board's serial.
- Keep serial numbers and sensor UniqueIDs stable across reboot/factory reset.
- Root Basic Information reads manufacturer/model/serial through DeviceInstanceInfoProvider.
  Install the project provider after `esp_matter::start`, under the ChipStack lock;
  delegate other fields to the SDK provider to preserve VID/PID and factory behavior.
- Child metadata belongs in Bridged Device Basic Information.
- The SDK creates bridged node UniqueID with an empty value. Retrieve the existing
  attribute and use `set_val`; another `create_unique_id` call will not update it.
- Do not replace test VID/PID with a real vendor's IDs when changing manufacturer strings.
- The `freertos/task_snapshot.h` warning currently comes from `esp_diagnostics`.
  In IDF 5.4.1 it forwards to `esp_private/freertos_debug.h`; this is not a build failure.
  Do not disable all compiler warnings or upgrade the SDK just to hide it.

## Hardware, pairing, and verification

- Board used in this session: ESP32-C3 USB Serial/JTAG on Windows **COM3**.
  This is the last observed device; run `uv run fw ports` to confirm the current port
  and device before flashing. Close monitors before opening the port with esptool.
- The user authorized build/flash and console verification during this session's firmware
  updates. Continue within that authorization; a previously flashed board is not permanent
  authorization for unrelated hardware work.
- Normal flash preserves NVS/Wi-Fi/fabrics. Factory reset or full erase requires explicit
  user request/authorization. Do not remove pairing just to test a new endpoint.
- Factory reset console command: `matter esp factoryreset`. It clears Wi-Fi/fabrics
  and reboots. Endpoint structure changes may require controller refresh or re-pairing.
- Current test commissioning configuration: manual code `34970112332`, QR at
  `docs/pairing-qr.png`. Prefer actual board output if credentials have changed.
- Verified in the 2026-11-01 session: native Windows build; COM3 flash with hash verification;
  updates from all three temperature sensors; Grid OPEN/CLOSED; correct root
  manufacturer/model/serial. Power Source previously worked in firmware but was removed
  after the user confirmed Apple Home did not support it. Firmware manufacturer is
  `deye-inverter-esp`, model `esp32-c3`.
- All five Electrical Sensors at 7/8/9/10/11 were built/flashed and passed on-board
  AAI Read + TLV encoding checks for all three quantities and required metadata.
  Those earlier simulator logs confirmed units and P=V*I only. Live Modbus uses
  register-reported watts instead; see the live verification below.
- Distinguish build success, flash success, runtime checks, and controller UI verification.
  Old Power Source logs do not prove Apple Home percentage support. Do not claim
  macOS/Linux jobs passed without corresponding CI results.
- Session diagnostic logs and serial helpers may be in ignored `.cache/`, which can
  be deleted. The main build workflow must not depend on cached helpers.
- When capturing serial with Python on Windows, use UTF-8 (`PYTHONUTF8=1`) so non-ASCII
  log output does not crash the capture script through cp1252 encoding.
- Run checks appropriate to the change. Firmware changes: build the target and, when
  authorized, flash and inspect several update cycles. CLI changes: `uv run fw test`
  and affected commands. Documentation changes do not require rebuilding firmware.

## GitHub Actions and artifacts

- The workflow runs on pushes (including tags), pull requests, and workflow_dispatch.
- The matrix reads targets from `targets.json` and checks native builds on Windows,
  Ubuntu, Intel macOS, and Apple Silicon.
- Each target has **one artifact named after the target**. The Ubuntu job uploads it;
  other hosts validate builds. For example, `esp32-c3.zip` contains `esp32-c3.bin`,
  `.elf`, bootloader, partition table, `flasher_args.json`, and sdkconfig.
- Preserve artifact directory structure so flashing follows the manifest. Do not name
  build files after the host OS or revert to `deye_matter_switch.bin`.
- The workflow does not currently create GitHub Releases. Do not push, tag, or publish
  just to test CI unless the user requests that action.

## Descriptor and controller grouping

- The pinned SDK does not set a composition mode for dynamic endpoints. Without one,
  Descriptor PartsList Read does not encode data. After start, set TreeComposition
  on the bridge and each sensor; keep every sensor's parent as aggregator 1.
  The user confirmed that this Descriptor fix alone did NOT resolve Power appearing
  under temperature sensors; do not claim it fixed that UI issue.
- Verify the actual PartsList through AAI Read + TLV decoding: the bridge contains
  2/3/4/5/7/8/9/10/11, every leaf encodes an empty array, and temperature endpoints
  have no EPM/PowerTopology. Do not patch the SDK.
- Distinguish firmware topology fixes from controller cache/UI behavior. Do not
  automatically factory reset when a controller still displays the old structure.

### Electrical measurement scope correction

- The user reported that temperature sensors still displayed an unrelated Power
  value after the Descriptor fix. They later clarified that they did not know whether
  it was a total or another reading; do not record an unverified sum as fact.
  All five electrical channels had
  incorrectly used PowerTopology NodeTopology, which scopes measurements to the entire
  node. Use PowerTopology TreeTopology (FeatureMap exactly 0x02) for each electrical
  leaf, so its measurement scope is itself and its empty PartsList. This is distinct
  from Descriptor TreeComposition. Do not restore NodeTopology for independent channels.
- At startup, read each PowerTopology FeatureMap through the Matter attribute read path
  and verify TREE only; retain the decoded empty PartsList checks. Keep IDs, UniqueIDs,
  and pairing unchanged.
- Specification: https://csa-iot.org/wp-content/uploads/2024/05/matter-1-3-core-specification.pdf
  (Power Topology cluster, section 11.8).

- Scope correction verified on COM3 (2026-11-01): all five TREE FeatureMaps read
  back as 0x02, five EPM read/encoding checks passed, bridge/leaf PartsList checks
  passed, and temperature/power updates continued without a crash.
- The user subsequently confirmed on 2026-11-01 that the display issue was fixed
  after the PowerTopology TREE correction. Power no longer appeared under temperature
  sensors. This confirms that specific UI fix, not support for every electrical attribute.
- Regression rule: independent electrical channels must remain TREE-only (0x02),
  with empty PartsLists and no EPM/PowerTopology on temperature endpoints. Preserve
  startup checks when adding channels or changing endpoint composition. Descriptor
  TreeComposition alone does not constrain the scope of electrical measurements.
- The old simulator randomized Voltage/Current and computed V*I; it never produced
  an aggregate total. It is now removed. Do not diagnose unexpected controller values
  as a sum or cache issue without evidence.

## Local logger configuration

- The user supplied `192.168.5.190:502` as the default for local development/testing.
  Keep it in ignored `config.local.json`, not firmware defaults, tracked examples,
  CI, or public artifacts. `config.example.json` documents the generic schema.
- `flash`, `deploy`, and `dev` automatically apply root `config.local.json` when
  present; `--config <file>` overrides it. Without a file, preserve stored config.
  `uv run fw config --port COM3` applies config only, without rebuilding/flashing.
- Validate JSON before build/flash. Accept only logger.host (1..63 ASCII hostname/IPv4
  characters) and integer logger.port (1..65535). Reject console injection/extra keys.
- Transfer through the existing USB Matter console or the requested LAN web form,
  not by overwriting the NVS
  partition image. The firmware stores one versioned blob in namespace `deye_logger`,
  commits and reads back before acknowledging a fresh request token. No partition
  changes, pairing deletion, or Wi-Fi/fabric replacement are needed.
- Console: `matter esp logger` shows stored configuration; the CLI sends
  `matter esp logger set <host> <port> <8-hex-token>`. Report errors/timeouts, never
  treat echoed input or an old acknowledgement as a successful config write.
- Config persistence is separate from the Modbus worker. Firmware polls configured
  host/port with the explicitly selected deye-single map/unit 1; do not infer a
  different register map or protocol merely from port 502.

### Embedded logger web form (requested 2026-11-07)

- Both targets serve GET `/` and GET/POST `/logger` on HTTP port 80 after Matter
  startup. Wi-Fi still comes from Matter commissioning; no AP, cloud service or
  new provisioning protocol. Log the station IPv4 URL after server startup/IP changes.
- The embedded English form loads saved host/port and saves via the same
  `logger_config::set` API as the USB console. Serialize reads/writes with a FreeRTOS
  mutex, validate host/port on the device, commit and read back before reporting
  success. Preserve the blob version/layout, console token acknowledgement, Wi-Fi,
  fabrics and endpoint identities. The worker picks up changes next cycle.
- Accept only bounded URL-encoded host/port/token fields (256-byte body maximum).
  Reject duplicates, extra/missing fields, malformed encoding, NUL/control/non-ASCII
  input, invalid host/port and bad tokens. Use a random 128-bit per-boot form token,
  no-store responses and CSP; no CORS or third-party assets. HTTP/NVS work stays on
  the HTTP task, outside the Matter thread/ChipStack lock.
- This is a LAN HTTP configuration form without login/TLS. A LAN client can read
  and change the logger configuration. Do not imply the form token authenticates
  users. Only local logger connection settings change, never inverter registers.
- Web changes do not rewrite the PC's `config.local.json`; later USB config/flash
  application can overwrite settings with that file as before. Keep both paths.
- Native Windows builds of both targets and the 27-test suite passed on 2026-11-07,
  including parser/validation checks. Logs: ignored `.cache/logger-web-*.log`.
  Subsequent COM5 RAM-fix verification confirmed the HTTP listener and shared NVS
  save/read-back through USB. Browser HTTP requests, web-save persistence and live
  Modbus adoption through the web remain unverified without commissioned Wi-Fi.

- Verified on 2026-11-01: native Windows build, 15 CLI tests, COM3 flash and
  automatic local config application acknowledged after NVS commit/read-back.
  After USB reset, console read still returned 192.168.5.190:502; existing Matter
  traffic and temperature/power updates continued. Config-only USB application
  was also checked without another flash. This was the config-only verification;
  subsequent live Modbus work is described below.

## Live Modbus implementation

- The user requested reusing the earlier Homebridge inverter model/map. Found it in
  sibling `../deye-inverter` (read-only reference): SUN-6K-SG05LP1-EU-SM2-P,
  single-phase hybrid, Modbus TCP FC03, unit 1. Preserve ESP project metadata rather
  than hardcoding the inverter hardware model into every sensor's identity.
- Only FC03 is implemented. Never write inverter registers/settings without an
  explicit new request; writing local log files is not authorization for Modbus writes.
- `profiles/deye-single.json` is the single register/scale source for Python and
  generated firmware header. Six sequential groups: 90-91, 109-112, 150, 157-160,
  169, 178-191 (26 words). One connection per group matches the earlier logger behavior.
  No energy counters or speculative broad scans. A failed group does not cancel others.
- W/V/A registers: PV1 186/109/110; PV2 187/111/112; Grid 169/150/160;
  Load 178/157/179; Battery 190/183/191. Voltage scale: PV/Grid/Load 0.1 V,
  Battery 0.01 V. Current scale: PV 0.1 A, Grid/Load/Battery 0.01 A. Power is 1 W.
  Grid/Load/Battery power/current are signed int16 (0xffff=-1); unsigned 0xffff
  is unavailable. Zero is valid. Battery power positive=discharge, negative=charge;
  Grid positive=import, negative=export. Grid is internal metering, not external CT.
- Temperatures: DC 90, AC 91, Battery 182; offset 1000, 0.1 C, raw valid 0..3000.
  SOC 184 (0..100%) is used by the CLI and Battery Temperature's Power Source cluster;
  it adds no request to the existing battery block. The standalone retired
  percentage endpoint 6 remains removed.
- Firmware network I/O runs in its own task, never under the ChipStack lock or on
  the Matter thread. Publish one owned snapshot via ScheduleWork; wait for its
  completion before starting the next interval, preventing an unbounded work queue.
  Config is read once at each cycle, so USB config updates apply next cycle.
- Per-group deadline: 3 seconds, bounded response <=260 bytes. Validate transaction,
  protocol, unit, MBAP length, function/exception, and byte count; handle fragmented
  sends/reads and socket closure. No stale values or random fallback after failures.
- `uv run fw read` defaults to one snapshot using config.local.json, logs JSONL to
  ignored `.local/deye.jsonl`. Use --count N for finite sampling or --watch in the
  foreground (Ctrl+C). --grid-only reads only 150. --json prints JSONL to stdout;
  --log selects an append log with UTC timestamps, values, raw/registers, group words,
  errors and timings. --host/--logger-port/--unit-id/--timeout are CLI overrides;
  firmware currently fixes unit=1/profile=deye-single. No background polling CLI.
- Tests: `uv run fw test` discovers all scripts/test_*.py. Fake logger covers
  fragmentation, invalid MBAP/FC03, exceptions, timeout/closure, group isolation,
  sentinels, scaling, finite logging and recovery. When a host g++/clang++ exists,
  compile/run the actual firmware decoder/protocol tests too.
- A successful read establishes transport/decoding, not agreement with the inverter
  display. The first live CLI snapshot had Grid 1160 W/214.7 V/0.89 A and Load
  1110 W/216.1 V/0.27 A. Preserve raw data and the documented 0.01 A scale; these
  AC currents still need comparison with the physical display. Do not change the
  scale or infer current from watts just to make values look consistent.
- Protocol references: https://www.modbus.org/docs/Modbus_Messaging_Implementation_Guide_V1_0b.pdf
  and https://www.photovoltaikforum.com/core/attachment/449240-deye-modbus-protocol-v118-pdf/

- Live implementation verified on 2026-11-01: native Windows firmware build/COM3
  flash, 23 tests including native C++ checks, five live EPM read/encoding checks,
  and repeated real logger snapshots. A single continuously open USB session proved
  all sensors unavailable during an offline test and recovered after restoring
  local config, with monotonic uptime (no reboot), no fabricated Grid outage, and
  no crash. Logger config restored to 192.168.5.190:502; pairing retained.
  Three CLI samples took 672/500/485 ms; these are observations, not performance guarantees.
  Local comparison files: .local/deye.jsonl and .local/deye-readings.md (ignored).
