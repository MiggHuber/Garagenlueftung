# Changelog – Garagenlüftung Firmware

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

### 🔒 Stabilitä
