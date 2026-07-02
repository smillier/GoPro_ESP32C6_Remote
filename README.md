# GoPro_ESP32C6_Remote

ESP-IDF target **5.5.4**, `esp32c6` chip.

BLE-based GoPro control firmware ("Legacy Pairing" bonding, Open GoPro
protocol) with 4 physical buttons and an RGB status LED.

> An earlier version included a WiFi portal (SoftAP + mobile web
> interface). It was removed: BLE/WiFi radio coexistence on the
> ESP32-C6 (a single shared antenna) caused too many reliability
> issues (WiFi association timeouts while BLE was scanning). Control
> is now done entirely through the 4 physical buttons.

## Architecture

The firmware is built around a centralized state layer (`app_state`)
and a shared control layer (`gopro_control`): the physical buttons
call control functions that read/write a central state — no
duplicated logic.

```
                    ┌──────────────┐
                    │   buttons    │
                    │ (GPIO2/3/4)  │
                    └──────┬───────┘
                           │
                           ▼
                   gopro_control.c
                (high-level actions)
                           │
             ┌─────────────┼─────────────┐
             ▼             ▼             ▼
       gopro_ble.c    app_state.c   led_status.c
       (BLE/GATT)    (shared state)  (WS2812 LED)
```

## Structure

```
gopro_esp32c6_pairing/
├── CMakeLists.txt          # ESP-IDF root project
├── sdkconfig.defaults       # NimBLE + bonding config, esp32c6 target
├── .vscode/                 # VSCode config + ESP-IDF extension
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml    # pulls in espressif/led_strip (WS2812 driver)
    ├── version.h             # firmware name/version
    ├── main.c               # app_main: NVS, NimBLE host, security params
    ├── app_state.c/.h        # shared state (connection / recording / mode)
    ├── gopro_control.c/.h    # high-level actions (buttons)
    ├── gopro_ble.c/.h        # scan -> connect -> bonding -> discovery -> commands
    ├── led_status.c/.h       # WS2812 RGB LED (GPIO8) = connection status
    └── buttons.c/.h          # GPIO2/3/4/18 = shutter / power / mode / reset
```

## Hardware (ESP32-C6 Super Mini)

- **WS2812 RGB LED**: already built into the board, wired to **GPIO8**.
  Nothing to connect, the code drives it directly.
- **4 buttons**, each wired between its GPIO and **GND** (internal
  pull-up enabled in code, no external resistor needed):
  - **GPIO2**: start/stop recording (shutter)
  - **GPIO3**: camera wake/sleep (power)
  - **GPIO4**: mode change (Video → Photo → Timelapse → ...)
  - **GPIO18**: ESP32 restart (software reset, `esp_restart()`)

### LED color meaning

| Color                   | State                                              |
|--------------------------|-----------------------------------------------------|
| Blinking blue            | Searching for the GoPro (put it in pairing mode)   |
| Solid yellow             | Connected, GATT discovery / bonding in progress     |
| Solid green              | Ready — press the button to start                   |
| Blinking red             | Recording in progress                                |

### Button behavior

- **Shutter (GPIO2)**: ignored until the LED is green. Toggles
  start/stop on each press.
- **Power (GPIO3)**:
  - If the GoPro is connected → sends the `Sleep` command (0x05),
    the camera goes to sleep and disconnects.
  - If the GoPro is not connected → immediately restarts a scan.
    A sleeping GoPro keeps advertising over BLE; reconnecting to it
    is what wakes it up. **There is no separate "power on" BLE
    command** — reconnecting itself is what wakes the camera, so
    this button genuinely behaves like a real wake/sleep button in
    both directions.
- **Mode (GPIO4)**: sends `Load Preset Group` (0x3E) and cycles
  through Video → Photo → Timelapse → Video... Ignored until the
  camera is ready. The GoPro rejects this change while recording
  (normal camera behavior, not a code error).
- **Reset (GPIO18)**: restarts the ESP32 (`esp_restart()`), with a
  short delay to let any in-flight logs/responses finish cleanly.

## Why this works (recap)

1. The GoPro must be put into pairing mode from its screen
   (Preferences → Connections → Connect Device).
2. We scan for advertisements filtered on service `0xFEA6`.
3. **Key step**: once connected, we first discover the GATT
   services/characteristics, then call `ble_gap_security_initiate()`
   to trigger bonding (Just Works, no code entry — the GoPro has no
   input screen). **Confirmed on this camera**: you must use
   **Legacy Pairing** (`ble_hs_cfg.sm_sc = 0`) rather than LE Secure
   Connections — with `sm_sc = 1`, the GoPro consistently terminated
   the connection (reason 531 / HCI 0x13) as soon as security was
   requested, even after letting the link settle via GATT discovery
   and narrowing key distribution to `ENC` only. Without successful
   bonding, the GATT connection is accepted but the GoPro silently
   ignores every command.
4. Once the link is encrypted (`BLE_GAP_EVENT_ENC_CHANGE` with
   `status == 0`), we discover the FEA6 service's characteristics and
   subscribe (CCCD write) to the `notify` ones.
5. `gopro_send_shutter(true/false)` writes to the Command
   characteristic (`0x0072`) to start/stop recording.

Bonding keys (LTK) are persisted in NVS
(`CONFIG_BT_NIMBLE_NVS_PERSIST=y`), so manual re-pairing is only
needed after a factory reset of the camera.

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/)
  **5.5.4** (installed via the VSCode extension or the CLI). The code
  only uses NimBLE APIs stable across the 5.x series, but has been
  specifically verified against the official
  `examples/bluetooth/nimble/blecent` example from the 5.5 branch.
- "Espressif IDF" VSCode extension (automatically suggested when
  opening the folder, see `.vscode/extensions.json`)
- ESP32-C6 board

## Opening in VSCode

1. Unzip the project, open the folder in VSCode.
2. Install the ESP-IDF extension if prompted.
3. `Ctrl+Shift+P` → "ESP-IDF: Configure ESP-IDF extension" (first
   time only) to point it at your IDF/Python installation.
4. `Ctrl+Shift+P` → "ESP-IDF: Set Espressif Device Target" → `esp32c6`.
5. `Ctrl+Shift+P` → "ESP-IDF: Build your project".
6. Plug in the board, select the serial port, then "ESP-IDF: Flash".
7. "ESP-IDF: Monitor" to view the logs.

## CLI (alternative to VSCode)

```bash
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Important notes

- **Bluedroid is not supported on the ESP32-C6** in ESP-IDF: NimBLE is
  the only available host, already configured in
  `sdkconfig.defaults`.
- `nimble_port_init()` returns an `esp_err_t` and handles BT
  controller initialization internally (no need to call
  `esp_nimble_hci_init()` separately) — this has been the behavior
  since ESP-IDF 5.0, confirmed on the 5.5 branch.
- Before connecting, the GoPro **must** be in pairing mode (it only
  advertises `0xFEA6` in that state, or for 8h after going to sleep).
- The shutter command format (`03 01 01 01` / `03 01 01 00`) comes
  from the Open GoPro Command TLV protocol:
  `[length][command_id][param_id][param_length][value]`.
- The first `idf.py build` automatically downloads the
  `espressif/led_strip` component (declared in
  `main/idf_component.yml`) from the ESP-IDF component registry — an
  internet connection is therefore needed at least for that first
  build.
- Partition table: ESP-IDF's default (1MB) is more than enough
  without WiFi/HTTP server.

## Going further

- Replace the simplified GATT discovery structure used here with the
  official `peer.c` pattern from ESP-IDF's `blecent` examples if you
  need to handle several services/peripherals in parallel.
- If remote control is still desired despite the radio coexistence
  issues, consider a second module (a classic Bluedroid-capable ESP32,
  or an external WiFi module talking over UART/I2C to this firmware)
  rather than sharing the C6's single antenna between BLE and WiFi.
