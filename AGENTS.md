# AGENTS.md — HP LaserJet 1010 ESP32-S3 Print Server

Diese Datei richtet sich an KI-Agents, die in diesem Repository arbeiten. Sie
beschreibt die Installation und — wichtiger — die Grenzen, die vor jeder Aktion
gelten.

## 0. Regeln, die immer gelten (zuerst lesen)

1. **WLAN, Access Point und physisches Gerät nie ohne ausdrückliche, aktuelle
   Anweisung des Benutzers anfassen.** Keine Scans, kein Flashen, kein OTA, kein
   Speichern von WLAN-Daten, kein "Ich helfe mal schnell". Vor **jeder** Aktion
   neu fragen.
2. Ein früheres "Ja" ist **keine** Dauererlaubnis. Nach einem "Nein" oder bei
   Themenwechsel gilt die Regel unverändert weiter.
3. **Keine Secrets committen.** `secrets.yaml`, echte WLAN-Zugangsdaten,
   API-Keys und gerätespezifische Firmware-Images gehören nie ins Repo.
4. Nicht ankündigen, was nicht bestellt wurde. Fakten nennen, keine
   Rechtfertigungen erfinden, nichts als "schon erledigt" hinstellen.
5. Bei Unklarheit fragen — nicht raten, nicht ausprobieren.

## 1. Was das Repository ist

Ein ESP32-S3 fungiert als **USB-Host-Brücke** für einen host-basierten HP
LaserJet 1010 (nur USB, kein Netzwerkport). Der Client rendert das Druckbild und
schickt den fertigen PCL-XL-Datenstrom an TCP 9100; der ESP leitet die Bytes
1:1 an den USB-Bulk-OUT-Endpunkt des Druckers weiter.

```
CUPS / Client --TCP 9100--> ESP32-S3 --USB bulk OUT--> HP LaserJet 1010
```

- ESPHome-Device-Name: `hp1010-printserver`
- mDNS: `hp1010-printserver.local`, Port `9100`
- Zielboard: **ESP32-S3-DevKitC-1 N16R8** (16 MB Flash, 8 MB Octal PSRAM),
  USB-OTG im Host-Modus
- Drucker-USB-ID: `03F0:0C17`

## 2. Voraussetzungen

- ESP32-S3 mit freiem USB-OTG-Port (Host-Modus) und USB-Kabel zum Drucker
- HP LaserJet 1010
- Für Pfad A: Browser mit WebSerial (Chrome/Edge) oder `esptool.py`
- Für Pfad B: `esphome` (empfohlen: aktuelle Version)

## 3. Installation — Pfad A: vorgefertigtes Image (kein Build)

Die Datei `firmware.factory.bin` liegt im Repo-Root und hängt an den
[GitHub Releases](https://github.com/cosmicboy6/esp32-s3-hp1010-printserver/releases).

**Browser-Installer:**

```
https://cosmicboy6.github.io/esp32-s3-hp1010-printserver/
```

ESP Web Tools flasht das Board direkt per WebSerial; es wird nichts hochgeladen.
Spiegel: `https://raw.githack.com/cosmicboy6/esp32-s3-hp1010-printserver/main/index.html`

**Oder per Kommandozeile** (gemergtes Image ab Offset `0x0`):

```bash
esptool.py --chip esp32s3 write_flash 0x0 firmware.factory.bin
```

Das vorgefertigte Image enthält **keine echten Zugangsdaten**. Es bootet deshalb
in den Fallback-AP (siehe Abschnitt 5). Eingebackener API-Key:
`9GXBCDmqW1uiTOdRJC60k8qvZ9W/yYW0jiE3xV/obGc=`.

## 4. Installation — Pfad B: aus dem Quellcode

```bash
cp secrets.yaml.example secrets.yaml
$EDITOR secrets.yaml          # SSID, Passwort, AP-Name/Passwort, API-Key
esphome compile hp1010-printserver.yaml

# Erstes Flashen über USB:
esphome upload hp1010-printserver.yaml --device /dev/ttyUSB0

# Danach per OTA:
esphome upload hp1010-printserver.yaml --device hp1010-printserver.local
```

Den API-Key erzeugen mit:

```bash
openssl rand -base64 32
```

`secrets.yaml` ist gitignored. **Nie** echte Werte in `secrets.yaml.example`
oder in eine JSON/HTML-Datei schreiben. Falls eine feste IP nötig ist,
`manual_ip:` lokal im `wifi:`-Block ergänzen — ebenfalls nicht committen.

Logs:

```bash
esphome logs hp1010-printserver.yaml --device hp1010-printserver.local
```

## 5. WLAN-Einrichtung über den Fallback-AP

Wenn das Gerät das konfigurierte WLAN nicht erreicht, öffnet es seinen eigenen
AP:

- SSID: `HP1010 Print Fallback`
- Passwort: `hp1010setup` (nur im vorgefertigten Image; bei Source-Builds der
  Wert aus `secrets.yaml`)

Vorgehen:

1. Mit dem AP verbinden.
2. `http://192.168.4.1` öffnen und SSID/Passwort des echten WLANs eintragen.
3. **Nach dem Speichern verlässt das Gerät den AP und geht ins WLAN — der AP
   verschwindet. Das ist der Erfolgsfall, kein Fehler.**

Known pitfall: Bei ESPHome `web_server` v3 belegt die Weboberfläche den Pfad
`/`. Bleibt die Seite leer, das Portal unter **`/portal`** öffnen; die
Konfigurations-JSON liegt unter **`/config.json`**.

## 6. Installation verifizieren (nur lesen)

- Device ist im Netz erreichbar: `hp1010-printserver.local`, Port `9100`.
- Home Assistant zeigt `binary_sensor.hp1010_print_server_hp1010_printer_ready`
  = **on**, sowie `sensor.hp1010_print_server_hp1010_ip_address` mit der IP.
- ESPHome-Logs: USB-Device `03F0:0C17` erkannt, Interface **Alternate Setting 1**
  gewählt (alt 0 verwirft rohen PCL-XL-Stream).
- Testdruck über die CUPS-Queue erzeugt ein Label; danach `printer-state = idle`.

**Für Agents:** Auch diese Prüfungen nur ausführen, wenn der Benutzer sie
ausdrücklich verlangt. Es handelt sich um Netzwerkzugriff auf das Gerät.

## 7. Client-Setup (CUPS)

Roh-Queue auf Port 9100:

```bash
lpadmin -p HP1010 -E -v socket://hp1010-printserver.local:9100 -m <driver>
```

Der Treiber muss für den LJ1010 rendern (z. B. `foo2zjs` / HP-LJ1010-PPD).
Bei statischer IP die mDNS-Adresse durch `socket://<IP>:9100` ersetzen.

## 8. Home Assistant

Der ESP wird über die ESPHome-Integration mit dem API-Key aus Abschnitt 3
hinzugefügt. Entitäten tragen das Präfix `hp1010_print_server_hp1010_`, u. a.
`printer_ready`, `paper_out`, `error`, `online`, `raw_status`, `page_count`,
`uptime`, `wifi_signal`, `firmware_version`, `ip_address`.

## 9. Repository-Konventionen

- Öffentlich unter `cosmicboy6/esp32-s3-hp1010-printserver`.
- GitHub Pages wird aus dem Repo-Root ausgeliefert (`.nojekyll` vorhanden).
- `firmware.factory.bin` bleibt generisch (Platzhalter-WLAN). Gerätebilder mit
  echten Zugangsdaten werden **nie** committet.
- Credits: OpenCode, Docker-Image `sprisa/opencode:latest`, Big Pickle GO 🥒.

## 10. Häufige Stolpersteine

| Symptom | Ursache / Prüfung |
|---|---|
| Drucker meldet sich, aber druckt nicht | Falscher USB-Alternate-Setting; alt 0 muss auf 1 gewechselt werden |
| Job bricht mit `ESP_ERR_INVALID_STATE` ab | Zweiter paralleler USB-Transfer; nur 4 KiB-Chunks, `MAX_INFLIGHT = 1` |
| fallback-AP sichtbar, kein WLAN | Falsche Zugangsdaten in `secrets.yaml` oder im Portal |
| `/` im Portal leer | `web_server` v3 belegt `/` → `/portal` verwenden |
| CUPS: "Drucker existiert nicht oder ist zurzeit nicht verfügbar" | Gerät ist aus / nicht erreichbar; Strom und Port 9100 prüfen — **nur auf Ansage** |
| Wartungshinweis | `esphome` → `platformio_options` → `build_flags` ist veraltet (Entfernung in 2026.12.0); bei einem Build ggf. umstellen |
