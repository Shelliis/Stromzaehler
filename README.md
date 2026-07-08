# Ferraris-Stromzähler

ESP8266-Firmware, die die rotierende Scheibe eines alten Ferraris-Stromzählers
optisch abtastet, daraus die aktuelle Leistung (Watt) berechnet und
per HTTP an einen eigenen Server meldet.

Inspiration/Hintergrund zum grundsätzlichen Aufbau (optische Abtastung der
Ferraris-Scheibe per ESP8266): [sisyphus.de – Ferraris Transmitter ESP8266](https://www.sisyphus.de/post/2022-08-13-ferraris-transmitter-esp8266/)

## Hardware

- **Ziel-Hardware:** ESP8266 (esp12e / NodeMCU-kompatibel)
- **Sensor-Pin:** GPIO 13 (`FERRARIS_PIN`), liest die Impulse der Zählerscheibe
- **Konstante:** `FERRARIS_DIVIDER = 150` Umdrehungen pro kWh (an den eigenen Zähler anpassen)

## Setup

1. `include/config.h.example` nach `include/config.h` kopieren und echte WLAN-Zugangsdaten eintragen:
   ```cpp
   const char *SSID = "...";
   const char *PSK = "...";
   ```
   (`config.h` ist in `.gitignore` und wird nicht versioniert.)
2. Ziel-Server-Endpunkt bei Bedarf anpassen: `JSON_ENDPOINT` in [src/main.cpp](src/main.cpp).
3. Bauen und flashen:
   ```bash
   pio run -e esp12e -t upload
   ```
   Standardmäßig ist in [platformio.ini](platformio.ini) **OTA (WLAN-Update)** eingestellt
   (`upload_protocol = espota`, IP anpassen). Für den allerersten Flash bzw. falls OTA
   nicht erreichbar ist, per USB:
   ```bash
   pio run -e esp12e -t upload --upload-port /dev/ttyUSB0
   ```
   (dazu in `platformio.ini` die `esptool`-Zeilen aktivieren, `espota`-Zeilen auskommentieren)

In VSCode: **Strg+Shift+B** baut die `esp12e`-Umgebung (siehe [.vscode/tasks.json](.vscode/tasks.json)
für weitere Tasks: Build wokwi, Upload, Monitor).

## Firmware-Funktionsübersicht

- **Leistungsberechnung:** Zeit zwischen zwei Impulsen (`millis()`-Differenz) →
  `calculateWatts()` in [include/ferraris_calc.h](include/ferraris_calc.h).
- **Zeitstempel:** NTP-Sync bei Start und danach alle 12 Stunden (Zeitzone Europe/Berlin
  inkl. Sommerzeit), pro Messung als `"zeitpkt"` im JSON.
- **JSON-Payload** pro Impuls an `JSON_ENDPOINT`:
  ```json
  {"millis": 1000, "zeitpkt": "2026-07-04 20:28:39", "W": 24000.0, "OFF":475, "ON":475, "ERR":0, "RC":200, "PUF":0}
  ```
- **Nachrichtenpuffer:** Schlägt der HTTP-Versand fehl, wird die Messung in einem
  RAM-sparenden Ringpuffer (`PENDING_MSG_BUF_SIZE = 200` Einträge) zwischengespeichert
  und beim nächsten Versand automatisch zuerst nachgeholt (max. `MAX_FLUSH_PER_CALL`
  Einträge pro Durchlauf, um `loop()` nicht zu blockieren).
  - HTTP `409 Conflict` (z. B. Zeitstempel-Kollision mit bereits gespeicherten Daten)
    verwirft die Messung statt sie endlos zu wiederholen.
- **Geplanter Neustart:** täglich ab `RESTART_HOUR` (Standard 4 Uhr), aber nur wenn der
  Puffer leer ist und die aktuelle Leistung unter `RESTART_MAX_WATTS` (1000 W) liegt —
  Gegenmaßnahme gegen Heap-Fragmentierung bei Dauerbetrieb. Der "letzter Neustart"-Tag
  wird im RTC-Speicher gehalten, damit ein schneller Reconnect danach nicht sofort erneut auslöst.
- **Logging:** zentral über `ENABLE_LOGGING` (0/1) in `main.cpp` ein-/ausschaltbar.
- **OTA-Updates:** `ArduinoOTA` ist aktiv, aber **ohne Passwort** — nur in vertrauenswürdigen Netzen nutzen.

## Wokwi-Simulation

Wokwi unterstützt ESP8266 nicht als programmierbaren Chip — die Simulation läuft daher
auf einem **ESP32-DevKit** (`env:wokwi` in `platformio.ini`, Build-Flag `-D WOKWI_SIM`).
main.cpp schaltet unter `WOKWI_SIM` automatisch auf die dafür passenden Pins/Verhalten um
(u. a. WLAN „Wokwi-GUEST“, verkürzter Zeit-Sync-Zyklus).

- **Start:** VSCode-Wokwi-Extension, `diagram.json` enthält einen Taktgenerator
  (simuliert Impulse) und einen Taster (`WIFI_ERROR_BUTTON_PIN`, simuliert bei Betätigung
  einen HTTP-Fehler, um Puffer-/Fehlerlogik zu testen).
- Details zu Debugging/Setup: siehe `.vscode/launch.json` (Wokwi-GDB-Konfiguration).

## Tests

Reine Berechnungslogik (`ferraris_calc.h`) lässt sich ohne ESP-Hardware testen:
```bash
pio test -e native
```

## Projektstruktur

| Datei/Ordner | Zweck |
|---|---|
| `src/main.cpp` | Firmware (Impulserfassung, WLAN, HTTP, Puffer, NTP, Neustart) |
| `include/ferraris_calc.h` | Reine Berechnungslogik (testbar ohne Hardware) |
| `include/config.h` | WLAN-Zugangsdaten (nicht versioniert) |
| `platformio.ini` | Environments: `esp12e` (Hardware), `wokwi` (Simulation), `native` (Tests) |
| `diagram.json` / `wokwi.toml` | Wokwi-Simulationsaufbau |
| `test/` | Unit-Tests für `native`-Environment |
