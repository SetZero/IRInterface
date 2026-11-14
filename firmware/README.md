# Firmware

Two Zephyr applications read an electricity meter through its optical port:

| Application | Board | Role |
|---|---|---|
| `apps/ir_head` | `irinterface` (STM32G030F6) | Remote UART at the meter: forwards what the phototransistor sees over RS-485 and transmits through the IR LED on request. |
| `apps/s0_master` | `s0_master` (ESP32-S3) | Decodes the telegrams (SML and IEC 62056-21), finds the meter's settings by itself, and serves the values over a web dashboard, REST, WebSocket and MQTT. |

```
apps/ir_head/           IR head application
apps/s0_master/
  dashboard/            the web page, compiled into the image
  src/core/             fixed-capacity containers, units, JSON
  src/meter/            data model, SML and IEC decoders, shared meter state
  src/link/             RS-485 link to the head, meter auto-detection
  src/network/          Wi-Fi (station + setup access point), captive portal, MQTT
  src/web/              REST API, HTTP and WebSocket handling
  src/system/           settings storage, Zephyr C glue (HTTP resources, shell)
boards/                 board definitions for both PCBs
lib/irlink/             link protocol shared by both applications (header only)
samples/bringup/        raw RS-485/IR echo test for new hardware
tests/                  host tests for the decoders and the link framing
tools/lint.sh           clang-format and clang-tidy checks
```

## Building

Needs a Zephyr 4.4 west workspace, Zephyr SDK 1.0 or newer with the
`arm-zephyr-eabi` and `xtensa-espressif_esp32s3_zephyr-elf` toolchains, the
Espressif radio blobs (`west blobs fetch hal_espressif`) and `esptool` on the
PATH.

```sh
export ZEPHYR_BASE=<workspace>/zephyr
export ZEPHYR_SDK_INSTALL_DIR=<sdk>

firmware/build.sh ir_head       # -> build/ir_head/zephyr/zephyr.hex
firmware/build.sh s0_master     # -> build/s0_master/zephyr/zephyr.bin
firmware/tests/run.sh           # host tests, needs a C++26 compiler (CXX, default clang++)
firmware/tools/lint.sh          # layout and naming; --fix reformats
```

The application code is C++26. Naming is checked by clang-tidy: types,
enumerators and template parameters in `CamelCase`, everything else in
`lower_case`, private members with a trailing underscore, no names shorter
than three characters.

## Flashing

**IR head**, over SWD with a J-Link on the 10-pin header:

```
JLink -device STM32G030F6 -if SWD -speed 4000
  loadfile build/ir_head/zephyr/zephyr.hex
  r
  g
```

**S0 master**, over UART0 on the 4-pin DEBUG header (3V3, RXD0, TXD0, GND).
The USB-C socket only supplies power and EN/BOOT are not on the header, so the
first time hold BOOT, tap EN, release BOOT. Afterwards `s0 bootloader` on the
console or `POST /api/v1/bootloader` restarts into download mode.

```sh
esptool --port <port> --before no-reset --after watchdog-reset \
        write-flash --flash-mode dio --flash-freq 40m 0x0 build/s0_master/zephyr/zephyr.bin
```

## First start

Without stored Wi-Fi credentials the master opens the access point
`S0-Master-xxxx` (no password). It is a captive portal: a phone that joins it
offers to open the setup page by itself ("Sign in to network"). If it does
not, open `http://192.168.4.1/`. Pick a network there. The access point closes half a minute after the device has joined
that network; it reopens whenever the network cannot be reached. On the
network the device answers at `http://s0-master.local/`.

The same can be done on the serial console (115200 baud):
`s0 wifi <ssid> [password]`, `s0 status`, `s0 reboot`, `s0 bootloader`,
`s0 factory_reset`, and `s0 crash` to test the automatic restart after a
firmware error. The status document reports the reason for the last restart
as `device.last_restart`.

## REST API

All answers are JSON. Errors come as `{"error": "..."}` with status 400.

| Method and path | Purpose |
|---|---|
| `GET /api/v1/status` | Device, Wi-Fi, read head, meter detection and MQTT state. |
| `GET /api/v1/reading` | The latest telegram: `power_w`, `energy_import_wh`, `energy_export_wh` and every value with its OBIS code. `{"valid": false}` before the first one. |
| `GET /api/v1/history` | Average power per 10 s interval over the last hour, oldest first; `null` for intervals without data. |
| `GET /api/v1/raw` | The last 1024 bytes from the optical interface as hex, for identifying an unknown meter. |
| `GET /api/v1/config` | Current settings. Passwords are never returned. |
| `POST /api/v1/config` | Change settings; send only the members to change: `meter_mode` (`auto`, `passive`, `active`), `ir_baud`, `ir_format` (`8N1`, `7E1`), `poll_interval_s`, `mqtt_enabled`, `mqtt_host`, `mqtt_port`, `mqtt_user`, `mqtt_password`, `mqtt_topic`, `mqtt_discovery`. |
| `POST /api/v1/wifi` | `{"ssid": "...", "psk": "..."}`; an empty `ssid` forgets the network. |
| `GET /api/v1/wifi/scan` | Networks in range; starts a scan when the list is older than 15 s. |
| `POST /api/v1/meter/pin` | Flash a PIN at the meter's optical button: `{"pin": "0420"}`, optionally with `wake_pulses`, `wake_pause_ms`, `pulse_ms`, `gap_ms`, `digit_pause_ms`. The PIN is not stored. |
| `POST /api/v1/meter/pulses` | One burst of flashes: `{"count": 1, "on_ms": 400, "off_ms": 400}`. |
| `POST /api/v1/meter/cancel` | Stop a running light signal. |
| `POST /api/v1/inject` | Body is hex text that is fed to the decoders as if received optically. For testing without a meter. |
| `POST /api/v1/reboot`, `/bootloader`, `/factory-reset` | Restart; restart into the ROM download mode; erase all settings and restart. |

The API has no authentication: anyone on the network can change settings.
Keep the device on a trusted network.

## Live data

**WebSocket** at `/ws`, used by the dashboard. The server sends text messages
`{"type": "status", "data": ...}` every two seconds and
`{"type": "reading", "data": ...}` with every telegram; `data` has the shape of
the corresponding REST answer. A new client gets both at once.

**MQTT** 3.1.1, when enabled. With the default topic prefix `s0master`:

| Topic | Payload |
|---|---|
| `s0master/state` | One JSON object per telegram, e.g. `{"energy_import": 1234567.8, "power": 420, "voltage_l1": 230.4, "meter_id": "...", "protocol": "sml"}`. Energy in Wh, power in W. |
| `s0master/availability` | `online` / `offline` (retained, last will). |
| `homeassistant/sensor/s0master_<id>/<key>/config` | Home Assistant discovery, one per quantity the meter reports (retained). |

## Unlocking a meter

Meters without a mechanical button are operated by light: flashes at the
optical port step through the display and enter the PIN that releases the
detailed values. The read head produces such flashes with its LED, by holding
it lit instead of sending serial data.

A PIN is sent as a wake-up burst (two flashes by default) and then, per digit,
as many flashes as the digit counts, each digit followed by a pause that
accepts it. The timing differs between manufacturers and is adjustable; the
meter's manual has the procedure. Progress is reported in the status document
under `meter.signal`. Meter detection pauses while a signal runs.

Whether a meter's light sensor responds to the head's infrared LED depends on
the meter. A phone camera shows the LED as a faint glow, which is a quick way
to see that the head is flashing.

## Meter detection

In automatic mode the master tries, in turn: listening at 9600 baud (SML and
pushed text telegrams), an IEC 62056-21 request at 300 baud 7E1, then listening
at 2400, 4800, 1200, 19200 and 300 baud. It stays with whatever produces a
telegram and starts over when the meter has been silent for 90 seconds. The
status document shows the current step and, for SML, why the last telegram was
rejected.
