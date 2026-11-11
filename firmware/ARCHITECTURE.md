# Custom firmware for the Mi Air Purifier 2S (88MW300)

Goal: the purifier works like stock, but is controlled locally from Home Assistant through the **ESPHome native API**. No cloud, no Xiaomi app. It updates **over the air**, because opening this unit is very painful.

Pins, peripherals and behavior were reverse-engineered from the stock firmware of one unit. Source comments cite those notes as `evidence/*.md` (with stock code addresses); the notes themselves are not part of this repo.

## Hard constraints

- **Keep stock boot2, the partition tables, PSM and wififw.** We only write an appfw slot (A @0xA000 or B @0x8A000, 0x80000 each).
- The image format is MRVL with per-segment CRC-32 (reflected 0xEDB88320, init 0, no final xor). boot2 checks the CRCs and has no signature check. Built by `tools/mrvl_pack.py`.
- **OTA must never brick the unit:**
  1. Write the passive slot.
  2. Verify its CRCs by reading them back.
  3. Only then bump that slot's gen in the partition table, as stock does: gen = other + 1, written to the ping-pong table copy.
  4. Reboot.

  boot2's automatic rollback is **not** confirmed, so the new image must be verified before the switch. The previous image stays in the other slot as the fallback. (After the first UART install that is stock; the first OTA overwrites stock.)
- Recovery of last resort is the UART boot ROM (`../flash/`), which needs disassembly. Avoid needing it.
- **Don't run the SDK's own build scripts** (make/cmake/shell). Our own `Makefile` compiles the SDK sources we need and links its prebuilt `.a` libs.
- Secrets (Wi-Fi SSID/password, API/OTA password) go in `firmware/secrets.h`, which is gitignored. A template lives in `secrets.example.h`.

## SDK

Amazon FreeRTOS `vendors/marvell/WMSDK/mw320`, commit `41244b002675843d44fc79c8752a87c344eaa855`, cloned to `~/sdk/amazon-freertos` by default (`make AFR=/path` overrides; not in the repo). It includes:
- WMSDK drivers (source);
- FreeRTOS (submodule `freertos_kernel`);
- FreeRTOS+TCP (submodule `libraries/freertos_plus/standard/freertos_plus_tcp`; not lwIP);
- prebuilt `libwifidriver.a libwlcmgr.a libpart.a libpsm.a librfget.a`;
- the radio firmware blob `mw30x_uapsta_W14.88.36.p135.bin.xz`, embedded into our image.

The toolchain is `arm-none-eabi-gcc` (tested with 10.3.1) with `-mcpu=cortex-m4 -mthumb -mfloat-abi=softfp -mfpu=fpv4-sp-d16` (softfp because the prebuilt libs are).

## Layout

```
firmware/
  Makefile             device build -> build/purifier.elf -> build/purifier.mrvl
  include/             shared interfaces (below); the contract between modules
  src/app/             main, task wiring, settings persistence
  src/purifier/        PORTABLE: state machine, modes, fan curve, AQI, filter, UI logic
  src/esphome/         PORTABLE: ESPHome native API server (Noise-encrypted or plaintext), mDNS responder
  src/ota/             OTA receiver: HTTP POST -> passive slot -> verify -> switch
  src/hal_mw300/       device HAL: fan PWM/FG, PM UART, SHT I2C, OLED SPI, LEDs, buzzer, keys, EEPROM, light ADC
  src/net_mw300/       Wi-Fi bring-up + netio over FreeRTOS+TCP
  src/host/            host HAL/netio stubs (POSIX sockets, fake sensors) for tests
  src/log/, src/diag/  log ring for remote logs; reset reason and diagnostics
  tests/               host unit tests + python integration tests (tests/*/host.mk)
  tools/               mrvl_pack.py, ota_upload.py, wmpt.py
```

"PORTABLE" code uses only `include/*.h` and the C standard library. It builds and is tested on the host.

## Interfaces (`include/`)

- `hal.h`: hardware. The purifier logic calls only these. Implemented in `src/hal_mw300` (device) and `src/host` (fakes).
- `netio.h`: minimal TCP/UDP sockets (non-blocking, poll-style). Implemented over FreeRTOS+TCP and over POSIX.
- `purifier.h`: purifier state + commands + `purifier_tick()`. Used by the ESPHome API layer and the UI.
- `esphome_api.h`: `api_server_init()`, `api_server_poll()`, plus an entity table built from `purifier.h`.
- `ota.h`: `ota_begin/ota_write/ota_end` on the passive slot (device), `ota_server_poll()`.
- `settings.h`: persisted settings (mode, favorite level, LED, buzzer, child lock, filter hours). Our own settings outside the stock layout (Auto speed bias at 0x800) are kept by `src/purifier/store.c` directly.

## Entities exposed to Home Assistant

| HA entity | Type | Notes |
|---|---|---|
| Purifier | fan | on/off, speed (favorite level 1..N), preset modes Auto / Silent / Favorite |
| Fan level | number 1..15, slider | the Favorite level, shown in every mode (the slider keeps it in Auto/Silent). Setting it = Favorite at that level, powering on if off (like the fan's speed). On the device page; HA shows the fan's own slider only in its pop-up |
| Mode | select | Auto / Silent / Favorite = the fan's preset; powers on if off (stock mode command) |
| PM2.5 | sensor µg/m³ | stock "aqi" is really PM2.5. Pushed on every change (1/s API throttle), not with stock's cloud report limits. "Unknown" (`missing_state`) while it is a placeholder: boot (20) before the first reading, and during a PM sensor fault (23, still shown on the panel as in stock) |
| Temperature, Humidity | sensor | SHT3x |
| Fan speed | sensor rpm | FG, diagnostic |
| Filter life | sensor % | |
| Filter used | sensor h | diagnostic |
| Ambient light | sensor % | diagnostic; the light sensor behind the auto display brightness (not stock) |
| Wi-Fi signal | sensor dBm | diagnostic |
| Display LED | switch | config |
| Buzzer | switch | config |
| Child lock | switch | config |
| Filter door | binary_sensor door | |
| Motor fault | binary_sensor problem | diagnostic; stock fault "m1_stuck" (standby 4), cleared by the next power-on/off |
| PM sensor fault | binary_sensor problem | diagnostic; stock fault "no_sensor" (> 5 failed reads), cleared by the next reading |
| Reset filter | button | config |
| Auto speed bias | number -50..50 % step 10, slider | config; not stock. In Auto the stock auto target is scaled by (100 + bias) %, rounded, clamped to 350..1700 rpm, 1700 being the fastest speed stock ever runs (a target already below 350, i.e. a custom m1_silent, is never raised, one above 1700 is never lowered; 0 = stop stays 0). The auto zones, deadbands, hysteresis and 10 s hold run on the unscaled stock target, so only the output changes; 0 = bit-exact stock. A change re-targets at once in Auto; Silent and Favorite ignore it. Persisted at EEPROM 0x800 (page 64, int32 LE; outside every stock location, see `store.c`); erased/invalid reads as 0 and is not written back |
| Firmware | text_sensor | diagnostic; `FW_VERSION` = `git describe --always --dirty --tags` at build time ("unknown" without git), see below |
| Reset reason | text_sensor | diagnostic; PMU `LAST_RST_CAUSE` (latched by the SDK's `boot_init()`): `power_on` (no bit), `brownout` (bits 0-2), `software` (bit 3 SYSRESETREQ: OTA reboot, crash/assert reset, hang guard, Wi-Fi reset combo), `restart` (bit 3 + the Restart button's tag in retention RAM), `lockup` (bit 4), `watchdog` (bit 5); `src/diag/` |
| OTA slot | text_sensor | diagnostic; running appfw slot, "A" / "B", plus " (trial)" while unconfirmed; pushed when the trial confirms |
| IP address | text_sensor | diagnostic; checked every 1 s, pushed on change, unknown without a lease |
| Uptime | sensor s, duration, total_increasing | diagnostic; pushed every 60 s |
| Free heap, Min free heap | sensor B, data_size | diagnostic; checked every 60 s, pushed only after a >= 1 KB move. The minimum is sampled every app-loop pass (10 ms) since the API server started (heap_marvell keeps no min-ever counter in this build) |
| Restart | button restart | diagnostic; reboots 500 ms later from the app loop (`api_entity_poll()`, via `hal_reboot()`: settings flushed first). Refused (WARN log) while an OTA upload runs, after an update was committed, and during an unconfirmed trial that can revert (any reset would boot the previous image) |

No entity exposes the MAC, DID, token, passwords or keys.

## Firmware version and build time

The Makefile writes `build/gen/fw_version.h` (`#define FW_VERSION "..."`, from `git describe --always --dirty --tags`, `make FW_VERSION=...` overrides; only `[A-Za-z0-9._+-]`, at most 40 chars) when the value changes, and force-includes it into every app object, which all depend on it. So a commit or a dirty tree rebuilds them all and no object keeps an old version. On every link `tools/gen_build_info.py` writes `build/gen/build_info.c` (`fw_build_time`, "YYYY-MM-DD HH:MM:SS UTC") and `build/gen/build_epoch`, which the packer uses as the MRVL header timestamp, so `/version`'s `build` and `image_time` give the same second. `SOURCE_DATE_EPOCH` makes the image reproducible. `GET /version` reports `version` = `FW_VERSION`, `build` = `fw_build_time`. ESPHome `DeviceInfo.project_version` = `FW_VERSION` (via `api_config.version`).

## Wi-Fi reconnect policy
Lives in `src/net_mw300/wifi_policy.c` (pure, host-tested in `tests/net/`): kick after 15 s disconnected, re-kick after 120 s stuck mid-connect (capped at 2 unanswered kicks), and with the optional `WIFI_SSID_2`/`WIFI_PASSWORD_2` from `secrets.h`, fail over after 120 s without an IP and retry the primary every 30 min from the backup (60 s dwell).

## OTA trial confirm
A trial image is confirmed by an authenticated `POST /confirm` (accepted from 150 s trial uptime; `ota_upload.py` sends it after checking slot, trial flag and image timestamp) or by the fallback after >= 5 min with IP, OTA listening and an HA API session (`api_server_client_count()`) unbroken for 60 s. Unconfirmed after 10 min: revert. A trial hang guard (software timer in `main.c`) resets after 60 s without an app-loop pass. `ota_upload.py --no-confirm IMAGE` for revert tests; `--confirm-only` confirms a running trial.

## Remote logs
All console output (wmprintf/printf/platform_printf via a tee on wmstdio's `c_stdio_funcs`, `wm_printf`, `platform_logf`/OTA_LOG, fatal hooks via `platform_fatalf`, and through `-Wl,--wrap=ll_printf` the SDK ISR/fault prints) also goes into a 4 KB RAM ring (`src/log/logring.c`, PRIMASK critical sections, safe from any context, drops oldest). UART output is unchanged. ESPHome clients that send SubscribeLogsRequest get the ring's history first (the boot log), then live lines, filtered by their level; lines are sent only from TX space beyond the request reserve, and a client that falls behind gets `[log] N bytes dropped`. The last 512 bytes and the fatal/reboot reason are mirrored into AON RAM (`.nvram_uninit.logring`, magic plus CRC on the reason) and replayed on the next boot as `[prev] ...` and `[W][prev] reset reason: ...`. SDK asserts (`_wm_assert`, wrapped) reset instead of spinning. Logs contain no secrets; the MAC is printed as its OUI only. Read them with `aioesphomeapi-logs` or HA's ESPHome log viewer.

## Tests

Host tests, run from `firmware/` (no hardware; a host C compiler with ASan/UBSan):

```sh
make -f tests/purifier/host.mk test              # purifier logic vs a fake HAL
make -f tests/net/host.mk test                   # Wi-Fi reconnect policy
make -f tests/ota/host.mk test-ota test-ota-e2e  # OTA on a simulated flash + a real local HTTP upload (run `make` first)
make -f tests/esphome/host.mk test PYTHON=...    # API server; PYTHON needs `pip install aioesphomeapi zeroconf`
```

The flashing tools have their own offline tests in `../flash/`: `python3 test_protocol.py`, `python3 test_flash.py`, and `./build_stub.sh` (rebuilds the RAM programs into `build/`, compares them with the shipped binaries and runs the writer's guard tests against a simulated flash chip).
