# Garagenlüftung

Arduino / ESP32 Projekt für eine **zeitgesteuerte Garagenlüftung** mit Web-UI, Display und MQTT-Anbindung.

Das Projekt steuert ein Garagentor so, dass es:
1. automatisch auf Lüftungsposition fährt  
2. für eine definierte Zeit lüftet  
3. optional pausiert  
4. anschließend wieder schließt  

---

## ✨ Features

- ⏱️ Zeitgesteuerte Lüftungszyklen
- 🌐 Web-UI zur Konfiguration
- 📟 OLED Display (SSD1306)
- 📡 MQTT-Unterstützung
- 🔘 Manuelle Steuerung per Taster
- 🧠 Zustandsbasierte Logik (keine blockierenden Delays)
- 🧩 Versionsunabhängiger Quellcode (ab v3.0.6)

---

## 🧱 Projektstruktur (ab v3.0.6)

```text
Garagenlueftung/
│
├─ Garagenlueftung/            # 🔹 Aktuelle, versionsunabhängige Source
│  ├─ Garagenlueftung.ino
│  └─ BA_3.0.x.html             # Web-UI
│
├─ Garagenlueftung_v3.0.4_UI/  # 🔸 Alte, versionsgebundene Version
├─ Garagenlueftung_v3.0.5/     # 🔸 Alte, versionsgebundene Version
│
├─ .gitignore                  # Ignoriert Build-Artefakte
├─ Changelog.md                # Versionshistorie
└─ README.md                   # Diese Datei


### 🔄 OTA (Over-the-Air Update)
- OTA-Update weiterhin unterstützt
- Empfehlung: Update auf v3.0.6 wegen Hardware-Fix (GPIO 0)
