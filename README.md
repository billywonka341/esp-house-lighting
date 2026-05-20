# Smart House Lighting System 🏠💡

A complete, high-performance, and secure smart relay control system designed for the **Lolin/Wemos ESP32-S2 Mini**. Features timezone-aware daily/nightly schedules, offline button debouncing, live power monitoring, Basic HTTP Authentication, and a premium responsive dark-mode Web Dashboard compressed to run efficiently on small embedded systems.

Brought to you by **CatCode Lab** 🐾.

---

## ✨ Features

- **⚡ Instant Physical Overrides:** Supports 4 active-low momentary buttons with a non-blocking 30ms debounce algorithm. Operates fully offline even without active WiFi connections.
- **⏱ Timezone-Aware Scheduler:** Up to 4 rules per light. Fully supports midnight-crossover scheduling (e.g. active from `20:00` to `06:00` next morning) while managing active days.
- **📊 Power Consumption Metrics:** Tracks total active seconds, Wh used today, monthly extrapolated kWh, and daily costs (PKR) dynamically inside the ESP32.
- **📈 24h Flex Bar Chart:** Embedded responsive hourly consumption history graph. Serves data dynamically from NVS-backed arrays to avoid browser session resets.
- **🔒 HTTP Basic Authentication:** Protects dashboard and REST API endpoints under a native browser login popup (Realm: `House Lights`).
- **🛡 Boot Grace Period:** Pauses automated schedules for 180 seconds on power-up to allow manual control checks, but keeps buttons and the Web UI fully functional.
- **💾 NVS Persistent Storage:** Saves and restores light states, custom labels (up to 32 chars), wattages, and rule configurations across reboots using `Preferences`.
- **⬆ Dual-Method OTA Updates:** Flash new sketch binaries directly from Arduino IDE over network, or upload `.bin` files via the drag-and-drop web dashboard.

---

## 🔌 Hardware Configuration & Pinout

### Components
- **Microcontroller:** ESP32-S2 Mini (Lolin/Wemos)
- **Relay Board:** 5V 4-Channel Coil Relay Module (active-low trigger, optocoupler isolated, e.g. SRD-05VDC-SL-C)
- **Buttons:** 4x Momentary push buttons (normally open, active-low)
- **Power:** 5V USB-C battery-backed (UPS) power supply

### Pin Assignments
| Peripheral | Pin | ESP32 GPIO | Trigger Logic |
| :--- | :--- | :--- | :--- |
| **Light 1 (Front Green Belt)** | Relay IN1 | `GPIO 11` | `LOW` (Active) |
| **Light 2 (Back Gate)** | Relay IN2 | `GPIO 12` | `LOW` (Active) |
| **Light 3 (Side Wall)** | Relay IN3 | `GPIO 13` | `LOW` (Active) |
| **Light 4 (Front Wall)** | Relay IN4 | `GPIO 14` | `LOW` (Active) |
| **Relay Power VCC** | Relay VCC | `5V` (DC+ bar) | Must be 5V, not 3.3V |
| **Relay Ground GND** | Relay GND | `GND` (DC- bar) | Ground reference |
| **Button 1** | Button Pin A | `GPIO 1` | `LOW` (Pushed, Pull-up) |
| **Button 2** | Button Pin A | `GPIO 2` | `LOW` (Pushed, Pull-up) |
| **Button 3** | Button Pin A | `GPIO 3` | `LOW` (Pushed, Pull-up) |
| **Button 4** | Button Pin A | `GPIO 4` | `LOW` (Pushed, Pull-up) |
| **Shared Ground** | Button Pins B | `GND` | Ground reference |

---

## 🛠 Software Requirements & Setup

### 1. Arduino IDE Configuration
1. Open the Arduino IDE.
2. Navigate to **Preferences** (Settings) and add the Espressif Board URL under **Additional Boards Manager URLs**:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. Go to **Tools** > **Board** > **Boards Manager**, search for `esp32` and install the package.
4. Select board **LOLIN S2 Mini** in the boards list.

### 2. Dependency Libraries
Install the following libraries using the **Library Manager** in Arduino IDE:
* **ArduinoJson** (Version **6.x** - e.g. 6.21.5)

---

## 🚀 Deployment Guide

### Step 1: Clone the Repo
```bash
git clone https://github.com/your-username/house-lighting-system.git
cd house-lighting-system
```

### Step 2: Configure Credentials
Open [`house_lighting_system/house_lighting_system.ino`](file:///Users/bilalhassan/Documents/code%20web/house%20lighting%20system/house_lighting_system/house_lighting_system.ino) and adjust settings at the top of the file:
```cpp
const char* WIFI_SSID         = "YourWiFi";
const char* WIFI_PASSWORD     = "YourPassword";
const char* AUTH_USER         = "admin";
const char* AUTH_PASSWORD     = "yourpassword";
const char* OTA_PASSWORD      = "otapassword";
const float COST_PER_KWH      = 56.0; // Price per kWh (PKR)
```

### Step 3: Run the Web asset compiler (Optional)
If you modify the dashboard file `index.html`, compile it using the Python compression utility:
```bash
python3 compress_html.py
```
This minifies and compresses `index.html` into a Gzipped hex byte array inside `web_ui.h`, reducing memory usage on the ESP32 from 56 KB to just **11.8 KB**.

### Step 4: Flash the Board
1. Open [`house_lighting_system.ino`](file:///Users/bilalhassan/Documents/code%20web/house%20lighting%20system/house_lighting_system/house_lighting_system.ino) in the Arduino IDE.
2. Select your Port and hit **Upload** (`Cmd + U` / `Ctrl + U`).

---

## 📡 REST API Documentation

All routes except `/ping` require standard **HTTP Basic Authentication** headers. Supports OPTIONS preflights with CORS headers.

| Method | Endpoint | Description | Sample Response |
| :--- | :--- | :--- | :--- |
| **GET** | `/ping` | Connectivity health check (Unauthenticated) | `"pong"` |
| **GET** | `/status` | Retrieves current state, uptime, active loads, and chart logs | `{"time":"22:35","day":1,"uptime":3600,...}` |
| **GET** | `/light/{id}/on` | Turns light `id` (1-4) ON | `{"ok":true}` |
| **GET** | `/light/{id}/off` | Turns light `id` (1-4) OFF | `{"ok":true}` |
| **GET** | `/light/{id}/toggle` | Toggles state of light `id` (1-4) | `{"ok":true,"state":true}` |
| **GET** | `/schedules` | Fetches all scheduling rules for all channels | `{"sched1":[{"enabled":true,"on_time":"18:00",...}]}` |
| **POST** | `/schedules` | Saves full scheduling configuration JSON body to NVS | `{"ok":true}` |
| **GET** | `/settings` | Reads light labels and power wattage coefficients | `{"names":["Front Green Belt",...],"watts":[20,...]}` |
| **POST** | `/settings` | Modifies name and power wattages in NVS preferences | `{"ok":true}` |
| **POST** | `/ota` | Multipart file upload interface (flash payload) | `{"status":"ok"}` |

---

## 📄 License
This project is licensed under the MIT License - see the LICENSE file for details.

Developed with 🐾 by **CatCode Lab**.
