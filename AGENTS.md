# Working on deye-inverter-esp

These instructions apply to the entire repository. Read this file before changing
code and cross-check it against the current code and README. New user requests
take precedence over recorded decisions. Update this file when project direction
changes. Keep AGENTS.md in English; continue communicating with the user in Vietnamese.

## Goals and collaboration

- This is ESP-Matter firmware for the Deye inverter project. It currently simulates
  sensors for testing Matter controllers; it does not read a real inverter or sensors.
- Communicate concisely in Vietnamese. Report build/flash results and what remains
  unverified. Complete authorized work without asking again about settled decisions.
- Prefer native Windows, macOS, and Linux workflows. Do not move the main workflow
  to WSL, Docker, `/mnt/c` paths, automatic `apt` installation, or machine-specific scripts.
- Use short commands such as `uv run fw ...` in documentation and instructions.
  Do not require venv activation or repeatedly use long Python command lines.
- Do not add hardware, inverter protocols, or targets without a request.
  Do not restore the sample switch or outlet.

## Code map

- `main/app_main.cpp`: bridge/endpoints, simulator, attribute/event updates,
  pairing lifecycle, topology validation, and console.
- `main/device_identity.h`, `main/device_identity.cpp`: manufacturer/model/serial
  and the root device identity provider.
- `main/electrical_measurements.h`, `main/electrical_measurements.cpp`: five
  Electrical Sensor channels, delegates/AAI, Matter read checks, and electrical simulation.
- `main/CMakeLists.txt`: application dependencies and target-derived model name.
- `scripts/project.py`: `fw` CLI, SDK installation, build/menuconfig, serial, and flash.
- `scripts/native_args.py`: Windows adapter for CMake-to-GN compiler flags.
- `scripts/test_project.py`: CLI/tooling, flash manifest, quoting, and junction tests.
- `targets.json`: shared target list for the CLI and GitHub Actions matrix.
- `sdkconfig.defaults`, `partitions.csv`: default configuration and flash layout.
- `.github/workflows/build.yml`: native CI and artifacts named after targets.
- `README.md`: user instructions, pairing, simulation, and verification status.

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
| 2 | AC Temperature | Temperature Sensor; initially 32 °C, range 25–55 °C |
| 3 | DC Temperature | Temperature Sensor; initially 36 °C, range 30–65 °C |
| 4 | Battery Temperature | Temperature Sensor; initially 28 °C, range 20–45 °C |
| 5 | Grid | Contact Sensor; initially CLOSED |
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
- Simulation updates every **5 seconds**. Temperatures change by at most ±0.50 °C
  per update and stay within their ranges. Matter uses `nullable<int16_t>` in 0.01 °C.
- Grid randomly chooses OPEN/CLOSED: `StateValue=true` means CLOSED; `false` means OPEN.
  Update the Boolean State attribute and emit `StateChange` only when the state changes.
- **Power Source / battery percentage was removed at the user's request**:
  the user tested the Battery endpoint in Apple Home/HomeKit and reported it as
  unsupported (2026-11-01). Defer storage battery monitoring. Do not restore it
  unprompted, attach percentage to Battery Temperature, or disguise it as temperature/humidity.
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
- Matter units: mV/mA/mW, nullable int64. Log V/A/W. Simulated ranges: PV 250–450 V,
  0–12 A; Grid 220–240 V, ±10 A; Battery 48–56 V, ±30 A; Load 220–240 V, 0–20 A.
  These profiles do not describe real inverter/BMS hardware.
- Update bounded values every 5 seconds. Grid OPEN sets Grid Power to 0 V/A/W.
  Positive Grid means import, negative means export; positive Battery means discharge,
  negative means charge. P=V×I, with PF=1 for Grid/Load.
  Load represents inverter output and remains supplied in the simulation when Grid
  is OPEN. Only Grid Power depends on the contact; do not infer grid_dependent from AC mode.
- The delegate provides PowerMode, three MeasurementTypes, and Accuracy for synthetic
  data (`measured=false`). Do not claim actual hardware measurement accuracy.
- Retain a separate Instance/Delegate per endpoint and register after `esp_matter::start`
  under the ChipStack lock. The SDK default callback uses optional mask 0, rejecting
  Voltage/ActiveCurrent even when their attributes exist. Retain the correct mask.
- Updating the backing store with `attribute::update` is insufficient for delegate-backed
  reads. Update delegate state and call `MatterReportingAttributeChangeCallback`.
- Startup checks exercise AAI Read + TLV encoding for FeatureMap, PowerMode,
  NumberOfMeasurementTypes, Accuracy, Voltage, ActiveCurrent, and ActivePower.
- The public SDK allocates sequential IDs only. Reserve retired slot 6 with a temporary
  aggregator, then remove it under the ChipStack lock immediately after start and before
  registering the new AAIs. Final runtime state has no endpoint 6/Power Source;
  electrical channels remain at 7..11. Do not reuse SOC or the retired device identity.
- Passing on-board read/encoding checks does not establish Apple Home EPM support.
  The current goal is to let the user test actual controller support.

## Metadata and SDK notes

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
  Logs confirm units, ranges, and P=V×I. This does not verify Apple Home UI behavior.
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
- Simulation randomizes Voltage and ActiveCurrent per electrical channel every
  5 seconds and computes ActivePower as V*I. There is no separately randomized Power
  value or aggregate total. Temperature sensors randomize temperature only. Do not
  attribute an unexpected controller value to a sum or cache without evidence.
