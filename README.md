# Cute Bear PC Resource Monitor ʕ•ᴥ•ʔ

A modern PC Hardware Resource Monitor designed for the **Seeed Studio XIAO ESP32-S3 Plus** with the **1.47" Touch Display** (172 x 320 IPS) housed in a custom 3D-printed cute bear desk enclosure.

---

## 🌟 Multi-Page Touch UI (4 Distinct Screens)

Tap anywhere on the touch screen (or press physical side buttons **USR1 / USR2**) to switch pages:

| Page | Name | Highlights |
| :---: | :--- | :--- |
| **0** | **Summary Dashboard** | Compact cyberpunk overview: CPU%, GPU%, RAM%, Network speeds, and dynamic Bear Mood. |
| **1** | **Core Workload** | **Extra-Large Readability**: Giant bold typography for CPU%, GPU%, and RAM% with glowing progress bars and GB counters. Optimized for easy reading on the small 1.47" screen from across the desk. |
| **2** | **Network Activity** | **Dedicated Bandwidth Monitor**: Huge bold Download & Upload readouts in MB/s with live real-time download sparkline waveform. |
| **3** | **Dual Thermals Graph** | **Simultaneous CPU & GPU History**: Dual-trace thermal oscilloscope graph (Cyan = CPU Temp, Amber = GPU Temp) with 40°C, 60°C, and 80°C alert threshold lines. |

---

## 🎨 Modern Dark-Theme GUI Companion (.exe)

`BearMonitor.exe` features a sleek **Dark Theme Windows 11 Desktop GUI**:
- **Live Visual Cards**: Glowing cyan, amber, magenta, and emerald cards for CPU, GPU, RAM, and Network.
- **Exact Windows Task Manager CPU Load**: Queries the native Windows `pdh.dll` performance counter (`\Processor Information(_Total)\% Processor Utility`) for 100% accuracy matching Task Manager.
- **Dual Connection Modes**: Toggle between **Bluetooth LE** and **USB Cable** with a single click.
- **Auto-Reconnect**: Seamless background reconnection if Bluetooth signals momentarily drop.

---

## 🚀 Quick Start Guide

### 1. Upload Firmware to XIAO ESP32-S3
1. Open Arduino IDE and load `bear_pc_monitor_esp32/bear_pc_monitor_esp32.ino`.
2. Install **LovyanGFX** from the Arduino Library Manager.
3. Configure under **Tools**:
   - **Board**: `XIAO_ESP32S3`
   - **PSRAM**: `OPI PSRAM`
   - **USB CDC On Boot**: `Enabled`
4. Click **Upload**.

### 2. Launch the PC Companion
- Double-click **`BearMonitor.exe`**.
- The dark mode desktop window will open and automatically connect to your Bear Monitor over Bluetooth LE.
- Tap the screen on your bear to browse between the 4 telemetry pages!
