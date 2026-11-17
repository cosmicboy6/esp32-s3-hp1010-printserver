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
- The printer's USB Printer Class `GET_PORT_STATUS` is polled every 15 s and
  logged (paper-empty / selected / not-error bits).

## Hardware

- **ESP32-S3-DevKitC-1 N16R8** (16 MB flash, 8 MB octal PSRAM) — or any
  ESP32-S3 board with a free USB-OTG port.
- **HP LaserJet 1010** (USB ID `03F0:0C17`).
- USB-OTG port in **host** mode connected to the printer.

## Repository layout

```
hp1010-printserver.yaml                              ESPHome device config
secrets.yaml.example                                 copy to secrets.yaml
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

## Limitations

- The USB Printer Class status byte reflects the **engine/paper** state, not the
  formatter/PDL state. It can report an **empty tray**, but may **not** catch a
  jam or misfeed; a jam typically shows up as the printer dropping off USB (the
  `HP1010 Printer Ready` sensor goes off).
- The LJ1010 occasionally re-enumerates its USB after certain jobs; CUPS simply
  retries and the job goes through.

## Credits

Inspired by [`mckinlk/esp32-zp450-print-server`](https://github.com/mckinlk/esp32-zp450-print-server)
(4 KiB transfers, one at a time).
