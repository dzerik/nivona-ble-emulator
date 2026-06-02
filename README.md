# Nivona BLE Emulator

[![build](https://github.com/dzerik/nivona-ble-emulator/actions/workflows/build.yml/badge.svg)](https://github.com/dzerik/nivona-ble-emulator/actions/workflows/build.yml)

**Version:** see [`VERSION`](VERSION) · **Changelog:**
[`CHANGELOG.md`](CHANGELOG.md) · **Tags:** `emu-v<X.Y.Z>`

> Companion project to the
> [`dzerik/melitta-barista-ha`](https://github.com/dzerik/melitta-barista-ha)
> Home Assistant integration. The emulator lived inside that repo under
> `esp_emulator/` from v0.44.0 through v0.51.0; it was split out for
> independent release cadence, cleaner HACS indexing, and so its
> ESP-IDF build pipeline doesn't compete with the integration's pytest
> CI. Git history (11 commits, 6 tags `emu-v0.2.0` → `emu-v0.7.0`) was
> preserved via `git subtree split`.

A BLE peripheral emulator that impersonates a Nivona NICR/NIVO coffee machine
on ESP32, for offline development of the Home Assistant `melitta_barista`
integration without access to a real machine. Also works with the official
Nivona Android app for protocol reverse-engineering.

Implements the full Eugster BLE protocol (service `AD00`) — HU handshake,
RC4 frame encryption, all documented `H*` commands, per-family recipe
layouts, and a finite-state machine that simulates brewing cycles.

## Hardware

Selectable at build time with `-DBOARD=<name>` (default `xiao_c6`):

| BOARD | Chip | Flash | Notes |
| ----- | ---- | ----- | ----- |
| `xiao_c6` (default) | ESP32-C6 | 4 MB | Seeed XIAO ESP32-C6. RF switch on GPIO3/14 (PCB antenna / external U.FL). |
| `xiao_s3` | ESP32-S3 | 8 MB | Seeed XIAO ESP32-S3 / S3 Plus (OPI PSRAM). |
| `waveshare_c6_lcd_1_47` | ESP32-C6 | 8 MB | Waveshare ESP32-C6-Touch-LCD-1.47. Touchscreen UI added in a later release; headless today. |

Board profiles live in `boards/<name>/` (each provides `board.c`,
`sdkconfig.board`, `partitions.csv`). The common config is in
`sdkconfig.defaults`.

## Features

| Layer          | Implementation status |
| -------------- | --------------------- |
| Advertising    | Random static address (F1:…), mfg data `0x0D` + customer filter |
| DIS (`180A`)   | Manufacturer / model / serial / hw / fw / sw revisions          |
| GATT `AD00`    | AD01 (control), AD02 (notify), AD03 (write), AD04/5 (stub), AD06 (name) |
| Security       | Just Works pairing + bonding (NVS-persistent)                   |
| Framing        | `S + cmd + [kp] + payload + cs + E`, RC4, MTU chunking, per-cmd size gating |
| HU handshake   | Full 2-round verifier + session key negotiation                 |
| Commands       | HX (status), HV, HL, HI, HS, HR/HW, HA/HB, HE (brew), HZ, HY, HD, HN, Hp, HC/HJ (stub) |
| FSM            | process / sub_process / info / manipulation / progress          |
| Brew cycle     | READY → PRODUCT, progress 0→100%, async unsolicited HX pushes   |
| Storage        | Numerical + alphanumeric registers persisted in NVS             |
| Family switch  | CLI `family` command selects 600/700/79x/900/900-light/1030/1040/8000 |

## Prerequisites

- **ESP-IDF 5.4+** — tested with v5.4.1 RISC-V toolchain
- Host serial access to the board (native USB Serial/JTAG)
- WiFi network reachable by the board (for OTA + telnet)

## Setup

```bash
# 1. Set up WiFi credentials (one-time, never committed)
cp main/wifi_secrets.h.template main/wifi_secrets.h
$EDITOR main/wifi_secrets.h    # fill WIFI_SSID / WIFI_PASS

# 2. Build and flash
. $IDF_PATH/export.sh
idf.py -DBOARD=xiao_c6 build                 # default; or xiao_s3 / waveshare_c6_lcd_1_47
idf.py -DBOARD=xiao_c6 -p /dev/ttyACM0 flash monitor
# switching board (target/flash changes): idf.py -DBOARD=<new> fullclean
```

After the first USB flash, all subsequent updates go over the air:
```bash
curl -X POST --data-binary @build/nivona_emulator.bin \
     http://nivona-emu.local/ota
```

## Runtime endpoints

| Endpoint                        | Purpose                                                     |
| ------------------------------- | ----------------------------------------------------------- |
| `GET  http://<host>/`           | Firmware version, IDF version, compile time                 |
| `GET  http://<host>/diag`       | JSON of all diagnostic counters (connects, frames, HU, HX, …) |
| `POST http://<host>/ota`        | Flash a new `nivona_emulator.bin` — body is the raw binary  |
| `POST http://<host>/reboot`     | Reboot the device                                           |
| `telnet <host> 23`              | Interactive CLI — see commands below                        |

mDNS: the board announces itself as `MDNS_HOSTNAME.local` (default
`nivona-emu.local`), so you can use that instead of an IP.

## CLI commands (telnet or USB console)

```
help                     list registered commands
status                   show FSM state (process, sub_process, manip, progress)
diag                     show diagnostic counters
brew <pv>                start a brew cycle with the given process value
cancel                   cancel active brew
trigger <m>              set a manipulation (water_empty / beans_empty /
                         tray_full / clean / descale / none)
dump                     dump persisted register store
family <key>             switch emulated family — 600 / 700 / 79x / 900 /
                         900-light / 1030 / 1040 / 8000. Takes effect on reboot.
pair                     enter pairing mode (wipes bonds, restarts advertising)
forget                   wipe stored BLE bonds
reboot                   reboot the device
```

## Testing

Python tests in `tests/test_emulator.py` cover protocol helpers, HTTP
diagnostics, and full BLE round-trips. Run against a live board:

```bash
pip install bleak pytest pytest-asyncio requests
EMU_IP=192.168.1.29 EMU_MAC=F1:32:04:33:52:DA python3 tests/test_emulator.py
# or
python3 -m pytest tests/ -v -s
```

## Architecture

```
main/
├── main.c              application entry: lifecycle, RF switch, SM config
├── nivona_ble.c/h      advertising, GAP events, scan response
├── nivona_gatt.c/h     GATT service AD00 with 6 characteristics
├── nivona_dis.c/h      Device Information Service (180A)
├── nivona_frame.c/h    framing (S/E, checksum, RC4, per-cmd size gating)
├── nivona_crypto.c/h   RC4 + HU verifier + HU lookup table
├── nivona_dispatch.c/h command router with HU / HX / HV / HL / HI / HR / HW / …
├── nivona_fsm.c/h      process state machine (thread-safe)
├── nivona_store.c/h    numerical + alphanumeric register store with NVS
├── nivona_brew.c/h     async brew cycle task, unsolicited HX pushes
├── nivona_cli.c/h      esp_console REPL (USB Serial/JTAG)
├── nivona_wifi.c/h     WiFi STA + mDNS
├── nivona_ota.c/h      HTTP server (status / diag / ota / reboot)
└── nivona_telnet.c/h   TCP:23 bridge to esp_console + log mirroring
```

## Protocol references

- Upstream reverse engineering (field notes + reference client):
  `https://github.com/mpapierski/esp-coffee-bridge` — especially `docs/NIVONA.md`
- HA integration — source of truth for protocol constants:
  `../custom_components/melitta_barista/protocol.py`
  `../custom_components/melitta_barista/brands/nivona/` (package, split
  per family into `_family_<key>.py`; crypto in `_crypto.py`, register
  bases in `_registers.py`, serial-prefix → family in `_prefixes.py`)

## Security notes

- `main/wifi_secrets.h` is `.gitignore`-d. Never commit it.
- The emulator accepts any pairing request (Just Works). Don't expose it
  on an untrusted network.
- OTA has no authentication. Keep port 80 on a trusted LAN.

## License

MIT — see [`LICENSE`](LICENSE). Matches the parent
`melitta-barista-ha` integration.

## Legal & trademarks

This is a research and interoperability tool. It is **not**
affiliated with, endorsed by, or sponsored by Nivona Apparate GmbH
or Eugster / Frismag AG. "Nivona", "NICR" and "NIVO" are
trademarks of their respective owners, used here descriptively
only — see [`LEGAL.md`](LEGAL.md) for the full position statement,
the EU/US/RU legal basis, and the cooperation channels for any
legitimate concern.

## Security

Vulnerability reports go through GitHub Security Advisories, not
public issues — see [`SECURITY.md`](SECURITY.md).

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the build quickstart,
branching/commit conventions, and PR checklist.
