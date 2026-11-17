# HP LaserJet 1010 → ESP32-S3 Network Print Server

Turn a **host-based HP LaserJet 1010** (USB-only, no network port) into a
network printer using an **ESP32-S3** as a transparent
**raw TCP-9100 (JetDirect / AppSocket) → USB Printer Class** bridge.

Once flashed, the printer appears on the network as `hostname:9100`, so any
machine that can render a print job (CUPS on Linux, Windows/macOS with a
suitable driver) can print to it over WiFi.

```
 CUPS / client driver  --TCP 9100-->  ESP32-S3  --USB bulk OUT-->  HP LaserJet 1010
```

## Install

### Prebuilt firmware (browser, no build required)

A ready-to-flash **factory image** (`firmware.factory.bin`) is committed to the repo
root and attached to the [GitHub Releases](https://github.com/cosmicboy6/esp32-s3-hp1010-printserver/releases).

Connect a blank ESP32-S3 over USB and open the installer page:

> **https://cosmicboy6.github.io/esp32-s3-hp1010-printserver/**

It uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) (Web Serial), so
the browser flashes the board directly — nothing is uploaded. The installer is
served straight from this repo's root via GitHub Pages; an always-available
mirror is `https://raw.githack.com/cosmicboy6/esp32-s3-hp1010-printserver/main/index.html`.

**Or flash it from the command line** — the merged image goes at offset `0x0`:

```bash
esptool.py --chip esp32s3 write_flash 0x0 firmware.factory.bin
```

> **No secrets inside.** The prebuilt image uses placeholder WiFi, so the device boots
> into its fallback AP (`HP1010 Print Fallback` / `hp1010setup`); join it and set your
> real WiFi through the captive portal at `http://192.168.4.1`. It is compiled for the
> exact target board (**ESP32-S3 N16R8**). The baked-in API encryption key is
> `9GXBCDmqW1uiTOdRJC60k8qvZ9W/yYW0jiE3xV/obGc=` — use it when adding the device to
> Home Assistant, or build from source to use your own.

### From source

See [Build & flash](#build--flash) below.

## Why this works

The LJ1010 is a *host-based* ("GDI") printer: it has **no PCL/PostScript
interpreter** and no network interface. The client driver renders the page into
a ready-to-print **PCL XL** stream and sends those bytes to TCP port 9100. This
component does not interpret the data — it forwards the bytes verbatim to the
printer's USB bulk OUT endpoint.

### Why alternate setting 1 is required

The LJ1010 exposes a single interface with three alternate settings:

| Alt | Protocol | Mode | Usable? |
|----|----------|------|---------|
| 0 | `0x03` | IEEE 1284.4 (packet mode / MLC) | **No** — raw PCL is parsed as MLC and silently discarded |
| 1 | `0x02` | Bidirectional | **Yes** — used here |
| 2 | `0x01` | Unidirectional | (works, but no back-channel) |

The printer powers up on alt 0, so the component issues a standard
`SET_INTERFACE` (`bmRequestType 0x01`, `bRequest 0x0B`) to select alt 1 — the
same thing the CUPS `usb-libusb` backend does.

## Key implementation notes

- **4 KiB chunks, one transfer at a time (`MAX_INFLIGHT = 1`).** The ESP-IDF USB
  host controller rejects a second concurrent URB on the same non-periodic
  (bulk) endpoint with `ESP_ERR_INVALID_STATE`, which corrupts the stream.
- **`start_output_()` is serialized with a FreeRTOS mutex.** The TCP task and the
  USB completion callback both feed the pipe; without the lock they can both
  pass the in-flight check and submit out of order.
- **The reverse (bulk IN) channel is drained** so the printer can report status
  and keeps accepting data.
- The printer's USB Printer Class `GET_PORT_STATUS` is polled every 15 s. The
  status byte drives the `HP1010 Paper Out`, `HP1010 Error` and `HP1010 Online`
  binary sensors plus the `HP1010 Raw Status` diagnostic sensor in Home Assistant.
- **Lifetime page count via PJL.** A `@PJL INFO PAGECOUNT` query (bracketed in
  UEL, `ESC%-12345X`) is injected into the same bulk-OUT pipeline used for print
  data, and the reply (`@PJL INFO PAGECOUNT\r\n<digits>\r\n`) is parsed from the
  drained bulk-IN channel. The query is only sent while the pipe is genuinely
  idle (no client connected, nothing queued or in flight), so a PJL command can
  never be interleaved into a print job. It is refreshed shortly after each job
  and at least every 5 minutes. The LJ1010 formatter answers `INFO PAGECOUNT`,
  `INFO ID`, `INFO STATUS` and `INFO CONFIG`.

## Hardware

- **ESP32-S3-DevKitC-1 N16R8** (16 MB flash, 8 MB octal PSRAM) — or any
  ESP32-S3 board with a free USB-OTG port.
- **HP LaserJet 1010** (USB ID `03F0:0C17`).
- USB-OTG port in **host** mode connected to the printer.

## Repository layout

```
hp1010-printserver.yaml                              ESPHome device config
secrets.yaml.example                                 copy to secrets.yaml
index.html                                           browser installer (ESP Web Tools)
manifest.json                                        installer manifest
firmware.factory.bin                                 prebuilt merged image (no secrets)
external_components/hp1010_printserver/__init__.py   component registration
external_components/hp1010_printserver/hp1010_printserver.h
external_components/hp1010_printserver/hp1010_printserver.cpp
```

## Build & flash

```bash
# 1. Fill in your secrets
cp secrets.yaml.example secrets.yaml
$EDITOR secrets.yaml

# 2. Compile
esphome compile hp1010-printserver.yaml

# 3. First flash over USB (substitute your serial port)
esphome upload hp1010-printserver.yaml --device /dev/ttyUSB0

# ... or over the air afterwards
esphome upload hp1010-printserver.yaml --device hp1010-printserver.local
```

Live logs:

```bash
esphome logs hp1010-printserver.yaml --device hp1010-printserver.local
```

## Client setup (CUPS on Linux)

Add a raw queue pointing at port 9100:

```bash
lpadmin -p HP1010 -E -v socket://hp1010-printserver.local:9100 -m <driver>
```

The `<driver>` must be one that renders for the LJ1010 (e.g. `foo2zjs` / an HP
LJ1010 PPD). A generic "raw" queue also works if the client driver does the
rendering itself.

## Home Assistant entities

The component exposes the printer's IEEE-1284 status byte (polled every 15 s),
the lifetime page counter (read over PJL), and a set of ESP32 diagnostics.
ESPHome prefixes every entity with the device name, so the IDs are
`sensor.hp1010_print_server_hp1010_*` / `binary_sensor.hp1010_print_server_hp1010_*`:

| Entity (`hp1010_print_server_hp1010_…`) | Meaning |
|---|---|
| `binary_sensor.…_printer_ready` | Printer enumerated, claimed and on the bidirectional alt setting |
| `binary_sensor.…_paper_out` | Tray empty (status bit 5) |
| `binary_sensor.…_error` | Error / needs attention (status bit 3 clear) |
| `binary_sensor.…_online` | Selected / online (status bit 4) |
| `sensor.…_raw_status` | Raw IEEE-1284 status byte (diagnostic) |
| `sensor.…_page_count` | Lifetime printed pages, `@PJL INFO PAGECOUNT` (diagnostic) |
| `sensor.…_uptime` | Device uptime (diagnostic) |
| `sensor.…_wifi_signal` | WiFi RSSI (diagnostic) |
| `sensor.…_free_heap` | Free heap (diagnostic) |
| `sensor.…_max_free_block` | Largest free heap block (diagnostic) |
| `sensor.…_heap_fragmentation` | Heap fragmentation (diagnostic) |
| `sensor.…_loop_time` | Main-loop iteration time (diagnostic) |
| `sensor.…_firmware_version` | Running firmware version |
| `sensor.…_wifi_ssid` / `…_wifi_bssid` | Connected WiFi network |
| `sensor.…_mac_address` / `…_ip_address` | Device network identity |

## Limitations

- The USB Printer Class status byte reflects the **engine/paper** state, not the
  formatter/PDL state. It can report an **empty tray**, but may **not** catch a
  jam or misfeed; a jam typically shows up as the printer dropping off USB (the
  `HP1010 Printer Ready` sensor goes off).
- The LJ1010 occasionally re-enumerates its USB after certain jobs; CUPS simply
  retries and the job goes through.

## Credits

- Inspired by [`mckinlk/esp32-zp450-print-server`](https://github.com/mckinlk/esp32-zp450-print-server)
  (4 KiB transfers, one at a time).
- Firmware, Home Assistant app and documentation built with
  **[OpenCode](https://opencode.ai)** — the open-source coding agent.
  Docker images are published as `ghcr.io/anomalyco/opencode:<version>`:

  ```sh
  docker pull ghcr.io/anomalyco/opencode:latest
  ```

  Download / Docker docs: <https://opencode.ai/docs/#docker>
- Built by **Big Pickle GO** 🥒
