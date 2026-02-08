# Changelog – Garagenlüftung Firmware

Alle relevanten Änderungen an der Firmware werden hier dokumentiert.  
Versionen folgen dem Schema **MAJOR.MINOR.PATCH**.
# Changelog – Garagenlüftung

## v3.0.7 – 2026-03-01
### Neu
- ➕ **Separater „Öffnen“-Befehl**
  - Löst **nur einen Torimpuls** aus
  - Kein Lüfter, kein Zyklus
  - Ideal zum Öffnen aus der Wohnung (nicht draußen warten)

- 🌐 **HTTP-Endpunkt `/open`**
  - `GET /open` oder `POST /open`
  - Direkte Auslösung ohne MQTT
  - Für Kunden ohne Home Assistant / MQTT geeignet

- 🧠 **Priorisierte OPEN-Verarbeitung**
  - `/open` wird sofort ausgeführt
  - Unabhängig von laufendem Zyklus, UI oder MQTT
  - Vermeidet Verzögerungen bei hoher Last

- 🏠 **Home Assistant**
  - Neuer MQTT-Discovery Button: *„Garagentor Öffnen“*
  - Eigene Entity, unabhängig von Start/Abbrechen

- 🖥️ **Web-UI**
  - Neuer Button **🚪 Öffnen**
  - Immer verfügbar
  - Führt exakt denselben Torimpuls aus wie MQTT / HTTP

### Verhalten
- Wird **OPEN während eines Zyklus** ausgelöst:
  - Zyklus wird sauber abgebrochen
  - Lüfter aus
  - Torimpuls wird sofort ausgeführt

### Technisch
- Einführung eines internen `openRequested`-Flags
- Einheitliche Logik für:
  - Hardware-Taster
  - MQTT
  - Web-UI
  - HTTP

---

## v3.0.6
- UI-Redesign mit Fokus auf „Start“
- Preset-Änderung während Zyklus gesperrt
- Verbesserte Status- und Restzeitanzeige
- MQTT Discovery stabilisiert

---

## v3.0.6 – 2026-02-04

### 🔧 Änderungen
- **Hardware-Pin angepasst**
  - Bisher: **PIN 10**
  - Neu: **PIN 0**
  - Grund: Konflikt mit **WLED** (Pin 10 bereits belegt)
- **Versionsunabhängige Source-Struktur eingeführt**
  - Gemeinsamer, zentraler Quellcode statt versionsspezifischer Ordner
  - Erleichtert Wartung und zukünftige Releases

### 📝 Korrekturen
- **Orthografie- und Bezeichnerfehler** im Code und in der UI korrigiert

### 🧹 Repository-Pflege
- **Build-Artefakte entfernt**
  - `.bin`, `.elf`, `.map`, `bootloader.bin`, `partitions.bin`
- `.gitignore` ergänzt, damit Build-Dateien künftig nicht mehr eingecheckt werden

---

## v3.0.5 – 2026-02-01

### 🛠️ Fixes & Verbesserungen
- **Korrigierte Tor-Impulslogik nach Lüftung**
  - Bei **kurzer Pausenzeit** (Pause < Torfahrzeit − 1 s):  
    → Tor wird gestoppt, kurze Wartezeit, danach gezielt geschlossen
  - Bei **langer Pausenzeit** (Pause ≥ Torfahrzeit):  
    → Nur ein Impuls zum Schliessen erforderlich
- **Delay-freie State-Machine**
  - Neuer Zwischenzustand `STOPPING_BEFORE_CLOSE`
  - Keine blockierenden `delay()` mehr
  - Web-UI, MQTT und Taster bleiben jederzeit reaktionsfähig
- **Robuste Zeitlogik**
  - Grenzfälle bei gleichen oder sehr kurzen Zeiten entschärft
  - Sicherheitsabstand von 1 Sekunde integriert
- **Validierung der Konfiguration**
  - Clamping im Web-UI (`/save`)
    - Torfahrzeit: 1.0 – 60.0 s
    - Pause: 0 – 600 s
  - Zusätzlicher Laufzeitschutz gegen Unterläufe

### 🧠 Logik (Kurzfassung)
- Startposition: **Tor offen**
- Fahrt zur **Lüftungsschlitz-Position**
- Lüfter läuft für definierte Lüftungszeit
- Danach Öffnen für Pausenzeit
- Abhängig von der Pausenlänge:
  - **Stop + Schliessen** (kurze Pause)
  - **Direktes Schliessen** (lange Pause)

### 🖥️ UI / Status
- Neuer Status: **STOP → SCHLIESSEN**
- Konsistente Anzeige in Web, MQTT und Display

---

## v3.0.4 – 2026-01-14

### 🚀 Initiale Version
- Zeitgesteuerte Garagenlüftung
- Web-UI zur Konfiguration
- MQTT-Anbindung
- Display-Unterstützung (SSD1306)
- Manuelle Steuerung per Taster
