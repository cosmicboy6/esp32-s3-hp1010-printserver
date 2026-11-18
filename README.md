# HP LaserJet 1010 / DYMO LabelWriter 450 → ESP32-S3 Network Print Server

Turn a **USB-only printer** into a network printer using an **ESP32-S3** as a
transparent **raw TCP-9100 (JetDirect / AppSocket) → USB Printer Class** bridge.

Two targets ship in this repo:

| Printer | USB ID | ESPHome config | PJL page count |
|---|---|---|---|
| **HP LaserJet 1010** | `03F0:0C17` | `hp1010-printserver.yaml` | yes |
| **DYMO LabelWriter 450** | `0922:0020` | `dymo-printserver.yaml` | no (see below) |

Once flashed, the printer appears on the network as `hostname:9100`, so any
machine that can render a print job (CUPS on Linux, Windows/macOS with a
suitable driver) can print to it over WiFi.

```
  CUPS / client driver  --TCP 9100-->  ESP32-S3  --USB bulk OUT-->  printer
```

For the **DYMO LabelWriter 450** the ESP32 additionally advertises itself over
mDNS as a **“DYMO LabelWriter 450”** raw printer (`_pdl-datastream._tcp`, port
9100), so Bonjour-aware clients can discover it; it answers on the same
`socket://<host>:9100` endpoint DYMO's own print server uses.

## Install

### Prebuilt firmware (browser, no build required)

Ready-to-flash **factory images** are committed to the repo root:

- `firmware.factory.bin` — HP LaserJet 1010
- `dymo-firmware.factory.bin` — DYMO LabelWriter 450

Connect a blank ESP32-S3 over USB and open the installer page:

> **https://cosmicboy6.github.io/esp32-s3-hp1010-printserver/**

It uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/) (Web Serial), so
the browser flashes the board directly — nothing is uploaded. The installer is
served straight from this repo's root via GitHub Pages; an always-available
mirror is `https://raw.githack.com/cosmicboy6/esp32-s3-hp1010-printserver/main/index.html`.

**Or flash it from the command line** — the merged image goes at offset `0x0`:

```bash
esptool.py --chip esp32s3 write_flash 0x0 firmware.factory.bin        # HP1010
esptool.py --chip esp32s3 write_flash 0x0 dymo-firmware.factory.bin   # DYMO 450
```

> **No secrets inside.** The prebuilt images use placeholder WiFi, so the device
> boots into its fallback AP (`HP1010 Print Fallback` / `hp1010setup`, or
> `DYMO Print Fallback` / `dymosetup`); join it and set your real WiFi through
> the captive portal at `http://192.168.4.1`. They are compiled for the exact
> target board (**ESP32-S3 N16R8**). The baked-in API encryption key is
> `9GXBCDmqW1uiTOdRJC60k8qvZ9W/yYW0jiE3xV/obGc=` — use it when adding the device
> to Home Assistant, or build from source to use your own.

### From source

See [Build & flash](#build--flash) below.

## Why this works

Both printers are driven as a **standard USB Printer Class device** (class `0x07`):
the client renders the job into a ready-to-print byte stream and sends those bytes
to TCP port 9100, and this component forwards them verbatim to the printer's USB
bulk OUT endpoint. It does not interpret the data.

- **HP LaserJet 1010** is a *host-based* ("GDI") printer: it has no PCL/PostScript
  interpreter and no network interface, so the client driver renders the page into
  a **PCL XL** stream.
- **DYMO LabelWriter 450** speaks DYMO's ESC raster protocol; the DYMO driver (or
  `dyomo-cups`/`dymo-cups-drivers` on Linux) renders the label into that stream.

### Alternate settings are chosen adaptively

USB printers expose their data interface differently:

| Printer | Interface | Alternates | Chosen |
|----|----|----|----|
| HP LaserJet 1010 | 0 | alt 0 = IEEE 1284.4 packet mode (`0x03`), alt 1 = bidirectional (`0x02`), alt 2 = unidirectional (`0x01`) | **alt 1** |
| DYMO LabelWriter 450 | 0 | single alternate, bidirectional (`0x02`) | **alt 0** |

The component walks the configuration descriptor, prefers the **bidirectional**
setting that carries a bulk OUT endpoint, and issues `SET_INTERFACE` only when
the interface actually offers more than one alternate. On the LJ1010 this avoids
alt 0, where raw PCL is parsed as MLC packets and silently discarded — the same
thing the CUPS `usb-libusb` backend does.

## Configuration

The component is shared by both configs; the printer-specific bits are options:

```yaml
hp1010_printserver:
  id: prn
  port: 9100
  vid: 0x0922      # USB vendor id to match
  pid: 0x0020      # USB product id to match
  page_count: false    # PJL INFO PAGECOUNT query (HP only)
  status_polling: true # USB Printer Class GET_PORT_STATUS poll
```

| Option | Default | Meaning |
|---|---|---|
| `port` | `9100` | TCP port the raw print server listens on |
| `vid` / `pid` | `0x03F0` / `0x0C17` | USB device to bind to |
| `page_count` | `true` | Send the HP-only `@PJL INFO PAGECOUNT` query. **Must be `false`** for printers without PJL (e.g. the DYMO), or the control bytes corrupt the print stream |
| `status_polling` | `true` | Poll `GET_PORT_STATUS` for the IEEE-1284 status byte |

### Advertising the DYMO identity (mDNS)

`dymo-printserver.yaml` adds an ESPHome `mdns: services:` block that publishes a
`_pdl-datastream._tcp` (Port 9100) service with the CUPS/Bonjour TXT records that
make the queue show up as a DYMO:

```yaml
mdns:
  services:
    - service: _pdl-datastream
      protocol: _tcp
      port: 9100
      txt:
        ty: "DYMO LabelWriter 450"
        usb_MFG: "DYMO"
        usb_MDL: "LabelWriter 450"
        product: "(DYMO LabelWriter 450)"
```

## Key implementation notes

- **4 KiB chunks, one transfer at a time (`MAX_INFLIGHT = 1`).** The ESP-IDF USB
  host controller rejects a second concurrent URB on the same non-periodic
  (bulk) endpoint with `ESP_ERR_INVALID_STATE`, which corrupts the stream.
- **`start_output_()` is serialized with a FreeRTOS mutex.** The TCP task and the
  USB completion callback both feed the pipe; without the lock they can both
  pass the in-flight check and submit out of order.
- **The reverse (bulk IN) channel is drained** so the printer can report status
  and keeps accepting data.
- The printer's USB Printer Class `GET_PORT_STATUS` is polled every 15 s (when
  `status_polling` is enabled). The status byte drives the *Paper Out*, *Error*
  and *Online* binary sensors plus the *Raw Status* diagnostic sensor.
- **Lifetime page count via PJL (HP only).** A `@PJL INFO PAGECOUNT` query
  (bracketed in UEL, `ESC%-12345X`) is injected into the same bulk-OUT pipeline
  used for print data, and the reply (`@PJL INFO PAGECOUNT\r\n<digits>\r\n`) is
  parsed from the drained bulk-IN channel. The query is only sent while the pipe
  is genuinely idle (no client connected, nothing queued or in flight), so a PJL
  command can never be interleaved into a print job. It is refreshed shortly
  after each job and at least every 5 minutes. The LJ1010 formatter answers
  `INFO PAGECOUNT`, `INFO ID`, `INFO STATUS` and `INFO CONFIG`.
- **The DYMO LabelWriter 450 has no PJL**, so `page_count: false` is set in its
  config; sending the query would inject control bytes into the raster stream.

## Hardware

- **ESP32-S3-DevKitC-1 N16R8** (16 MB flash, 8 MB octal PSRAM) — or any
  ESP32-S3 board with a free USB-OTG port.
- **HP LaserJet 1010** (USB ID `03F0:0C17`) **or DYMO LabelWriter 450** (USB ID
  `0922:0020`).
- USB-OTG port in **host** mode connected to the printer.

> The ESP32-S3 has a single USB-OTG peripheral: it is a USB **host** to the
> printer. It cannot also act as a USB *device* at the same time.

## Repository layout

```
hp1010-printserver.yaml                              HP1010 ESPHome device config
dymo-printserver.yaml                                DYMO 450 ESPHome device config
secrets.yaml.example                                 copy to secrets.yaml
index.html                                           browser installer (ESP Web Tools)
manifest.json                                        installer manifest (HP1010)
dymo-manifest.json                                   installer manifest (DYMO 450)
firmware.factory.bin                                 prebuilt HP1010 merged image (no secrets)
dymo-firmware.factory.bin                            prebuilt DYMO 450 merged image (no secrets)
external_components/hp1010_printserver/__init__.py   component registration
external_components/hp1010_printserver/hp1010_printserver.h
external_components/hp1010_printserver/hp1010_printserver.cpp
```

## Build & flash

```bash
# 1. Fill in your secrets
cp secrets.yaml.example secrets.yaml
$EDITOR secrets.yaml

# 2. Compile (pick your printer's config)
esphome compile hp1010-printserver.yaml     # or dymo-printserver.yaml

# 3. First flash over USB (substitute your serial port)
esphome upload hp1010-printserver.yaml --device /dev/ttyUSB0

# ... or over the air afterwards
esphome upload hp1010-printserver.yaml --device hp1010-printserver.local
esphome upload dymo-printserver.yaml --device dymo-labelwriter-450.local
```

Live logs:

```bash
esphome logs dymo-printserver.yaml --device dymo-labelwriter-450.local
```

## Client setup

### HP LaserJet 1010 (CUPS on Linux)

Add a raw queue pointing at port 9100:

```bash
lpadmin -p HP1010 -E -v socket://hp1010-printserver.local:9100 -m <driver>
```

The `<driver>` must be one that renders for the LJ1010 (e.g. `foo2zjs` / an HP
LJ1010 PPD). A generic "raw" queue also works if the client driver does the
rendering itself.

### DYMO LabelWriter 450

Point the DYMO software (or CUPS) at the raw 9100 endpoint — the same connection
DYMO's own print server uses (**AppSocket / HP JetDirect**, port 9100):

```bash
# Linux (dymo-cups-drivers)
lpadmin -p DYMO450 -E -v socket://dymo-labelwriter-450.local:9100 \
        -m /usr/share/cups/model/lw450.ppd
```

On Windows use the DYMO driver on a **Standard TCP/IP Port** (RAW, port 9100);
on macOS the Bonjour entry *DYMO LabelWriter 450* should appear automatically.

## Home Assistant entities

The component exposes the printer's IEEE-1284 status byte (polled every 15 s),
optionally the lifetime page counter (HP, read over PJL), and a set of ESP32
diagnostics. ESPHome prefixes every entity with the device name.

**HP1010** (`hp1010_print_server_hp1010_…`):

| Entity | Meaning |
|---|---|
| `binary_sensor.…_printer_ready` | Printer enumerated, claimed and on the usable alternate setting |
| `binary_sensor.…_paper_out` | Tray empty (status bit 5) |
| `binary_sensor.…_error` | Error / needs attention (status bit 3 clear) |
| `binary_sensor.…_online` | Selected / online (status bit 4) |
| `sensor.…_raw_status` | Raw IEEE-1284 status byte (diagnostic) |
| `sensor.…_page_count` | Lifetime printed pages, `@PJL INFO PAGECOUNT` (diagnostic) |
| `sensor.…_uptime` / `…_wifi_signal` | Device diagnostics |
| `sensor.…_free_heap` / `…_max_free_block` / `…_heap_fragmentation` / `…_loop_time` | Device diagnostics |
| `sensor.…_firmware_version` | Running firmware version |
| `sensor.…_wifi_ssid` / `…_wifi_bssid` / `…_mac_address` / `…_ip_address` | Device network identity |

**DYMO 450** (`dymo_labelwriter_450_…`): the same set **minus** `page_count`
(no PJL), with the entity names un-prefixed (*Printer Ready*, *Paper Out*,
*Error*, *Online*, *Raw Status*, …).

## Limitations

- The USB Printer Class status byte reflects the **engine/paper** state, not the
  formatter/PDL state. It can report an **empty tray**, but may **not** catch a
  jam or misfeed; a jam typically shows up as the printer dropping off USB (the
  *Printer Ready* sensor goes off).
- The LJ1010 occasionally re-enumerates its USB after certain jobs; CUPS simply
  retries and the job goes through.
- mDNS advertising makes the DYMO **discoverable**; if your DYMO software only
  supports manual network setup, add it by hostname/IP on port 9100.

## Credits

- Inspired by [`mckinlk/esp32-zp450-print-server`](https://github.com/mckinlk/esp32-zp450-print-server)
  (4 KiB transfers, one at a time).
- Firmware, Home Assistant app and documentation built with
  **[OpenCode](https://opencode.ai)**, running in the
  [`sprisa/opencode`](https://hub.docker.com/r/sprisa/opencode) Docker image
  (source: [`sprisa/opencode-server`](https://github.com/sprisa/opencode-server)):

  ```sh
  docker pull sprisa/opencode:latest
  ```

- Built by **Big Pickle GO** 🥒
