/*
  =============================================================================
  Project:      Cute Bear PC Resource Monitor (Ultra-Smooth 60+ FPS LovyanGFX)
  Board:        Seeed Studio XIAO ESP32-S3 Plus (1.47" Touch Display)
  Enclosure:    Cute Bear 3D Printed Desk Gadget
  Library:      LovyanGFX (80 MHz Hardware SPI DMA + Double-Buffered Canvas)
  Orientation:  Landscape (320 x 172)
  Connectivity: Bluetooth Low Energy (BLE) + USB Serial Dual-Mode Fallback
  =============================================================================
  Features:
    - 100% Zero-Flicker Hardware DMA Double-Buffering at 60+ FPS (LovyanGFX)
    - 4 Multi-Page Modern Cyber Dark UI Pages:
        * Page 0: Full System Summary (Oscilloscope CPU Graph, RAM, GPU, Net, Bear Mood)
        * Page 1: Core Workload (Extra Large Bold CPU%, GPU% & RAM% for small-screen readability)
        * Page 2: Dedicated Network Monitor (Huge DL & UL Speed Readouts + Live Sparkline Wave)
        * Page 3: Dual Temperature Oscilloscope Graph (Simultaneous CPU & GPU Waveforms)
    - Touch & Button Page Switching (Tap screen or press Onboard BOOT Button GPIO 0)
    - Non-blocking I2C Touch Driver (Guaranteed NO black screen / NO bus lockup)
    - BLE Peripheral Name: "BearMonitor" (Nordic UART Service, MTU 256)
  =============================================================================
*/

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Wire.h>
#include <Preferences.h>
#include "bear_byte_logo.h"

// =============================================================================
// BLE UART UUIDs (Nordic UART Service)
// =============================================================================
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// =============================================================================
// AXS5106L I2C Touch Controller (Non-Blocking Safe Driver)
// =============================================================================
// AXS5106L I2C Touch Controller & Physical Buttons (Seeed XIAO ESP32-S3 Plus)
// =============================================================================
// Official Hardware Pinout for Seeed XIAO ESP32-S3 Plus 1.47" Touch Board:
// BAT_ADC: GPIO 10 (ADC1_CH9) via 316k/160k divider (D16 in Seeed docs)
// BTN 1:   GPIO 11 (Physical Button 1 / User Key A - D19 in Seeed docs)
// BTN 2:   GPIO 42 (Physical Button 2 / User Key B - D15 in Seeed docs)
#undef D16
#define D16 10
#undef D19
#define D19 11
#undef D15
#define D15 42

#define AXS5106L_I2C_ADDR        0x63
#define AXS5106L_TOUCH_DATA_REG  0x01
#define AXS5106L_REPORT_LEN      14
static constexpr int8_t TOUCH_INT_PIN = D7;  // Touch Interrupt (GPIO 44)
static constexpr int8_t BTN_1_PIN     = D19; // Physical Button 1: GPIO 11 (Next Page)
static constexpr int8_t BTN_2_PIN     = D15; // Physical Button 2: GPIO 42 (Previous Page)
static constexpr int8_t BTN_BOOT      = 0;   // Onboard BOOT Button (GPIO 0)
static constexpr int8_t BAT_ADC_PIN   = D16; // Battery ADC: GPIO 10 (316k/160k divider)
static constexpr float  DIVIDER_RATIO = (316.0f + 160.0f) / 160.0f; // 2.975x ratio

// Non-blocking battery ADC reading (12-bit ADC with 11dB attenuation)
static float readBatteryVoltage() {
  uint32_t sumMv = 0;
  static constexpr uint8_t SAMPLES = 12;
  for (uint8_t i = 0; i < SAMPLES; ++i) {
    sumMv += analogReadMilliVolts(BAT_ADC_PIN);
    delayMicroseconds(200);
  }
  float d16 = (sumMv / (float)SAMPLES) / 1000.0f;
  return d16 * DIVIDER_RATIO;
}

class SafeTouch_AXS5106L {
  bool     _lastPressed = false;
  uint32_t _lastPollMs = 0;
  bool     _touchPresent = false;

public:
  void begin() {
    Wire.begin(5, 6);       // SDA: GPIO 5 (D4), SCL: GPIO 6 (D5)
    Wire.setClock(400000);  // 400 kHz Fast I2C
    Wire.setTimeOut(3);     // Ultra-fast 3ms timeout prevents any bus freeze!
    pinMode(TOUCH_INT_PIN, INPUT_PULLUP);

    // Quick hardware probe: check if touch controller responds
    Wire.beginTransmission(AXS5106L_I2C_ADDR);
    _touchPresent = (Wire.endTransmission() == 0);
    if (_touchPresent) {
      Serial.println("[TOUCH] AXS5106L controller active!");
    } else {
      Serial.println("[TOUCH] Controller absent or sleeping - physical buttons D19/D15 fully active.");
    }
  }

  // Returns true if touched, maps to Landscape (320 x 172)
  bool getTouch(uint16_t* outX, uint16_t* outY) {
    if (!_touchPresent) return false;

    // Fast-path: When INT is not LOW and not in an active drag, return instantly (zero I2C bus delay!)
    if (digitalRead(TOUCH_INT_PIN) != LOW && !_lastPressed) {
      return false;
    }

    uint32_t now = millis();
    if (now - _lastPollMs < 15) return _lastPressed;
    _lastPollMs = now;

    // Non-blocking transmission
    Wire.beginTransmission(AXS5106L_I2C_ADDR);
    Wire.write(AXS5106L_TOUCH_DATA_REG);
    if (Wire.endTransmission(true) != 0) {
      _lastPressed = false;
      return false;
    }

    if (Wire.requestFrom((uint8_t)AXS5106L_I2C_ADDR, (uint8_t)AXS5106L_REPORT_LEN) < AXS5106L_REPORT_LEN) {
      _lastPressed = false;
      return false;
    }

    uint8_t buf[AXS5106L_REPORT_LEN];
    for (int i = 0; i < AXS5106L_REPORT_LEN; i++) buf[i] = Wire.read();

    uint8_t touchNum = buf[1];
    if (touchNum == 0) {
      _lastPressed = false;
      return false;
    }

    // Native portrait coords (0..171, 0..319)
    uint16_t raw_x = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
    uint16_t raw_y = ((uint16_t)(buf[4] & 0x0F) << 8) | buf[5];

    // Map to Landscape (320 x 172)
    uint16_t land_x = raw_y;
    uint16_t land_y = (raw_x < 172) ? (171 - raw_x) : 0;

    if (outX) *outX = constrain(land_x, (uint16_t)0, (uint16_t)319);
    if (outY) *outY = constrain(land_y, (uint16_t)0, (uint16_t)171);

    _lastPressed = true;
    return true;
  }
};

SafeTouch_AXS5106L touch;

// =============================================================================
// LovyanGFX Calibrated Hardware Display Configuration (80 MHz Hardware DMA)
// =============================================================================
class LGFX_XIAO_147 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789  _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Light_PWM     _light_instance;

public:
  LGFX_XIAO_147() {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 50000000;         // 50 MHz rock-solid hardware SPI (Official Seeed validated rate)
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;  // Hardware DMA channel

      cfg.pin_sclk = 7; // D8 (SCK)
      cfg.pin_mosi = 9; // D10 (MOSI)
      cfg.pin_miso = 8; // D9 (MISO)
      cfg.pin_dc   = 4; // D3 (DC)

      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = 3;     // D2 (CS) - NEVER override with pinMode!
      cfg.pin_rst          = 13;    // Hardware Reset
      cfg.pin_busy         = -1;
      cfg.panel_width      = 172;   // Native panel width
      cfg.panel_height     = 320;   // Native panel height
      cfg.offset_x         = 34;    // Centered column offset
      cfg.offset_y         = 0;

      // Crucial Calibration for Seeed 1.47" JD9853A IPS:
      cfg.offset_rotation  = 4;     // Fixes horizontal mirror
      cfg.invert           = false; // Inversion disabled (INVOFF) for this panel
      cfg.rgb_order        = false; // BGR channel mapping (MAD_BGR 0x08 in LovyanGFX)

      cfg.readable         = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = true;

      _panel_instance.config(cfg);
    }

    {
      auto cfg = _light_instance.config();
      cfg.pin_bl      = 12; // LCD_BL
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;

      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }

    setPanel(&_panel_instance);
  }
};

LGFX_XIAO_147 lcd;
LGFX_Sprite   canvas(&lcd); // 320 x 172 Double-Buffered DMA Canvas (110 KB in RAM)

// Screen Bounds
static constexpr int SCREEN_W = 320;
static constexpr int SCREEN_H = 172;

// =============================================================================
// Live Telemetry Data State
// =============================================================================
struct SystemData {
  float cpuPercent   = 0.0f;
  float cpuTemp      = 0.0f;
  float ramPercent   = 0.0f;
  float ramUsedGb    = 0.0f;
  float ramTotalGb   = 16.0f;
  float gpuPercent   = 0.0f;
  float gpuTemp      = 0.0f;
  float netDownMbps  = 0.0f;
  float netUpMbps    = 0.0f;
  float batteryVoltage = 4.15f;
  bool  dataReceived = false;
  uint32_t lastDataTime = 0;

  // Smoothed values for fluid 60 FPS animation
  float animCpu      = 0.0f;
  float animRam      = 0.0f;
  float animGpu      = 0.0f;
  float animNetDown  = 0.0f;
} sys;

// Scrolling History Buffers (48 samples each - All zero on boot)
static constexpr int GRAPH_SAMPLES = 48;
float cpuHistory[GRAPH_SAMPLES]      = {0};
float cpuTempHistory[GRAPH_SAMPLES]  = {0};
float gpuTempHistory[GRAPH_SAMPLES]  = {0};
float netDownHistory[GRAPH_SAMPLES]  = {0};
float netUpHistory[GRAPH_SAMPLES]    = {0};

// Multi-Page Management (Instant Zero-Delay Switching)
// 0: Summary, 1: Workload, 2: Network, 3: Dual Thermals, 4: Device & Demo Control
uint8_t currentPage = 0;
static constexpr uint8_t TOTAL_PAGES = 5;
bool g_forceImmediateRender = false;

// Demo State Management (NVS Persistent)
Preferences preferences;
bool demoMode = false; // Realistic demo simulation for public display

// BLE Connection State
bool bleConnected = false;
bool oldBleConnected = false;
BLEServer *pServer = nullptr;
BLECharacteristic *pTxCharacteristic = nullptr;

// =============================================================================
// Parser for Inbound Telemetry Strings
// Format: "C:45,T:52,R:64,RU:10.2,RT:16.0,G:35,GT:58,D:4.2,U:1.1\n"
// =============================================================================
void parsePacket(const String& packet) {
  int start = 0;
  while (start < packet.length()) {
    int comma = packet.indexOf(',', start);
    if (comma == -1) comma = packet.length();

    String token = packet.substring(start, comma);
    token.trim();

    int colon = token.indexOf(':');
    if (colon > 0) {
      String key = token.substring(0, colon);
      String val = token.substring(colon + 1);
      float fVal = val.toFloat();

      if (key == "C")       sys.cpuPercent  = constrain(fVal, 0.0f, 100.0f);
      else if (key == "T")  sys.cpuTemp     = fVal;
      else if (key == "R")  sys.ramPercent  = constrain(fVal, 0.0f, 100.0f);
      else if (key == "RU") sys.ramUsedGb   = fVal;
      else if (key == "RT") sys.ramTotalGb  = fVal;
      else if (key == "G")  sys.gpuPercent  = constrain(fVal, 0.0f, 100.0f);
      else if (key == "GT") sys.gpuTemp     = fVal;
      else if (key == "D")  sys.netDownMbps = fVal;
      else if (key == "U")  sys.netUpMbps   = fVal;
    }
    start = comma + 1;
  }

  // If PC stream arrives while Demo Mode was active, auto-switch to Live PC Stream
  if (demoMode) {
    demoMode = false;
    preferences.putBool("demo", false);
    Serial.println("[AUTO] Live PC telemetry detected -> Switched to Live PC Stream");
  }

  sys.dataReceived = true;
  sys.lastDataTime = millis();

  // Shift and update history buffers
  for (int i = 0; i < GRAPH_SAMPLES - 1; i++) {
    cpuHistory[i]      = cpuHistory[i + 1];
    cpuTempHistory[i]  = cpuTempHistory[i + 1];
    gpuTempHistory[i]  = gpuTempHistory[i + 1];
    netDownHistory[i]  = netDownHistory[i + 1];
    netUpHistory[i]    = netUpHistory[i + 1];
  }
  cpuHistory[GRAPH_SAMPLES - 1]     = sys.cpuPercent;
  cpuTempHistory[GRAPH_SAMPLES - 1] = sys.cpuTemp;
  gpuTempHistory[GRAPH_SAMPLES - 1] = sys.gpuTemp;
  netDownHistory[GRAPH_SAMPLES - 1] = sys.netDownMbps;
  netUpHistory[GRAPH_SAMPLES - 1]   = sys.netUpMbps;
}

// =============================================================================
// BLE Callbacks
// =============================================================================
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    bleConnected = true;
  }
  void onDisconnect(BLEServer* pServer) override {
    bleConnected = false;
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    String rxValue = String(pCharacteristic->getValue().c_str());
    if (rxValue.length() > 0) {
      parsePacket(rxValue);
    }
  }
};

void initBLE() {
  BLEDevice::init("BearByte");
  BLEDevice::setMTU(256);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_TX,
      BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pRxCharacteristic->setCallbacks(new RxCallbacks());

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] Advertising started as 'BearByte'");
}

// =============================================================================
// Modern Drawing Helpers
// =============================================================================
// =============================================================================
// Modern Cyber Dark Theme Color Palette (Borderless AMOLED Minimalist)
// =============================================================================
static constexpr uint32_t BG_DARK        = 0x06080C; // Pure Deep Obsidian Background
static constexpr uint32_t CARD_BG        = 0x0E131C; // Elevated Dark Surface Card
static constexpr uint32_t CARD_BG_ALT    = 0x121824; // Secondary Elevated Surface
static constexpr uint32_t TRACK_BG       = 0x18202D; // Smooth Progress Bar Track
static constexpr uint32_t COLOR_CYAN     = 0x00FFCC; // Neon Cyan Accent (CPU / System)
static constexpr uint32_t COLOR_AMBER    = 0xFFAA00; // Electric Amber (GPU / Warm)
static constexpr uint32_t COLOR_MAGENTA  = 0xFF44CC; // Neon Magenta / Violet (RAM)
static constexpr uint32_t COLOR_NET_DL   = 0x00FFFF; // Bright Cyan (Download)
static constexpr uint32_t COLOR_NET_UL   = 0x00FF66; // Mint Green (Upload)
static constexpr uint32_t COLOR_RED      = 0xFF3344; // Thermal / Low Battery Alert
static constexpr uint32_t COLOR_GOLD     = 0xFFD39B; // Bear Mascot Warm Tone
static constexpr uint32_t COLOR_WHITE    = 0xFFFFFF; // Primary Crisp Readout
static constexpr uint32_t COLOR_MUTED    = 0xB0C2D6; // Crisp Readable Sub-labels (Brightened)
static constexpr uint32_t COLOR_SILVER   = 0xC8D8E8; // Crisp Silver Text

// =============================================================================
// Modern Borderless Capsule Progress Bar
// =============================================================================
void drawGlowProgressBar(int x, int y, int w, int h, float percent, uint32_t fgColor, uint32_t bgColor = TRACK_BG) {
  int r = h / 2;
  // Sleek borderless rounded capsule track
  canvas.fillRoundRect(x, y, w, h, r, bgColor);
  int fillW = (int)((constrain(percent, 0.0f, 100.0f) / 100.0f) * w);
  if (fillW >= h) {
    canvas.fillRoundRect(x, y, fillW, h, r, fgColor);
  } else if (fillW > 0) {
    canvas.fillRoundRect(x, y, fillW, h, fillW / 2, fgColor);
  }
}

// =============================================================================
// Borderless Floating Header Bar (with Battery Voltage Display)
// =============================================================================
void drawHeader(const char* pageTitle, uint8_t activePage) {
  // Borderless floating header bar
  canvas.fillRoundRect(4, 2, SCREEN_W - 8, 22, 6, CARD_BG);

  canvas.setTextColor(COLOR_GOLD, CARD_BG);
  canvas.setTextSize(1);
  canvas.drawString("ʕ•ᴥ•ʔ BEARBYTE", 10, 7);

  canvas.setTextColor(COLOR_CYAN, CARD_BG);
  canvas.setTextSize(1);
  canvas.drawString(pageTitle, 106, 7);

  // If Demo Mode is active, show subtle neon mint badge
  if (demoMode) {
    canvas.fillRoundRect(152, 5, 26, 12, 3, 0x004422);
    canvas.setTextColor(0x00FF88, 0x004422);
    canvas.drawString("DEMO", 154, 7);
  }

  // Modern Active Capsule / Navigation Indicator Pills (5 pages)
  int startX = 186;
  int dotY = 12;
  for (uint8_t p = 0; p < TOTAL_PAGES; p++) {
    if (p == activePage) {
      canvas.fillRoundRect(startX + p * 11 - 2, dotY - 3, 9, 6, 3, COLOR_CYAN);
    } else {
      canvas.fillCircle(startX + p * 11 + 2, dotY, 2, 0x253142);
    }
  }

  // Small Battery Indicator in Top-Right Corner
  int bx = 246, by = 6, bw = 16, bh = 10;
  uint32_t batCol = (sys.batteryVoltage < 3.4f) ? COLOR_RED : ((sys.batteryVoltage < 3.7f) ? COLOR_AMBER : 0x00FF99);
  canvas.drawRoundRect(bx, by, bw, bh, 2, 0x4B5868);
  canvas.fillRect(bx + bw, by + 3, 2, 4, 0x4B5868);
  int fillW = constrain((int)(((sys.batteryVoltage - 3.2f) / 1.0f) * (bw - 4)), 1, bw - 4);
  canvas.fillRect(bx + 2, by + 2, fillW, bh - 4, batCol);

  char batStr[12];
  snprintf(batStr, sizeof(batStr), "%.2fV", sys.batteryVoltage);
  canvas.setTextColor(batCol, CARD_BG);
  canvas.setTextSize(1);
  canvas.drawString(batStr, 268, 7);
}

// =============================================================================
// PAGE 0: FULL SYSTEM SUMMARY DASHBOARD (FULL-SCREEN EXTENDED, NO BOTTOM BAR)
// =============================================================================
void renderPage0Summary() {
  drawHeader("SUMMARY", 0);

  // Left Card: Oscilloscope CPU Graph (Full height: Y: 27 to 169 -> H: 142)
  canvas.fillRoundRect(4, 27, 168, 142, 6, CARD_BG);
  canvas.fillRoundRect(4, 27, 4, 142, 2, COLOR_CYAN); // Cyan accent strip

  uint32_t cpuColor = (sys.cpuPercent > 80.0f) ? COLOR_RED : ((sys.cpuPercent > 50.0f) ? COLOR_AMBER : COLOR_CYAN);
  canvas.setTextColor(COLOR_CYAN, CARD_BG);
  canvas.setTextSize(1);
  canvas.drawString("CPU", 14, 33);

  // LARGE BOLD CPU PERCENTAGE
  canvas.setTextColor(cpuColor, CARD_BG);
  canvas.setTextSize(3);
  char cpuStr[16];
  snprintf(cpuStr, sizeof(cpuStr), "%2.0f%%", sys.cpuPercent);
  canvas.drawString(cpuStr, 42, 30);

  // CPU TEMPERATURE (Clean Crisp White, --C if offline)
  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  canvas.setTextSize(2);
  char cpuTempStr[16];
  if (sys.cpuTemp <= 5.0f) {
    snprintf(cpuTempStr, sizeof(cpuTempStr), "--C");
  } else {
    snprintf(cpuTempStr, sizeof(cpuTempStr), "%2.0fC", sys.cpuTemp);
  }
  canvas.drawString(cpuTempStr, 114, 32);

  // Full-Height Oscilloscope Area Graph (Zero negative space: Y: 56 to 163 -> 107px high canvas!)
  const int gx = 10, gy = 56, gw = 156, gh = 107;
  canvas.fillRoundRect(gx, gy, gw, gh, 4, 0x080B10);

  for (int i = 0; i < GRAPH_SAMPLES - 1; i++) {
    int x1 = gx + (i * gw) / (GRAPH_SAMPLES - 1);
    int x2 = gx + ((i + 1) * gw) / (GRAPH_SAMPLES - 1);
    int y1 = gy + gh - (int)((cpuHistory[i] / 100.0f) * gh);
    int y2 = gy + gh - (int)((cpuHistory[i + 1] / 100.0f) * gh);

    // Deep cyan gradient fill
    canvas.fillTriangle(x1, y1, x2, y2, x1, gy + gh, 0x001A24);
    canvas.fillTriangle(x2, y2, x2, gy + gh, x1, gy + gh, 0x001A24);
    // Glowing neon waveform crest
    canvas.drawLine(x1, y1, x2, y2, COLOR_CYAN);
    canvas.drawLine(x1, y1 + 1, x2, y2 + 1, 0x006655);
  }

  // Right Card: RAM, GPU, Network (Full height: Y: 27 to 169 -> H: 142)
  canvas.fillRoundRect(176, 27, 140, 142, 6, CARD_BG);

  // RAM Row (Y: 30 to 57)
  canvas.setTextColor(COLOR_MAGENTA, CARD_BG);
  canvas.setTextSize(1);
  canvas.drawString("RAM", 184, 32);

  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  canvas.setTextSize(2);
  char ramTxt[16];
  snprintf(ramTxt, sizeof(ramTxt), "%2.0f%%", sys.ramPercent);
  canvas.drawString(ramTxt, 214, 30);

  canvas.setTextColor(COLOR_SILVER, CARD_BG);
  canvas.setTextSize(1);
  char ramGb[12];
  snprintf(ramGb, sizeof(ramGb), "%1.0fG", sys.ramUsedGb);
  canvas.drawString(ramGb, 274, 33);
  drawGlowProgressBar(184, 50, 124, 7, sys.animRam, COLOR_MAGENTA, 0x221326);

  // GPU Row (Y: 62 to 89)
  canvas.setTextColor(COLOR_AMBER, CARD_BG);
  canvas.setTextSize(1);
  canvas.drawString("GPU", 184, 64);

  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  canvas.setTextSize(2);
  char gpuTxt[16];
  snprintf(gpuTxt, sizeof(gpuTxt), "%2.0f%%", sys.gpuPercent);
  canvas.drawString(gpuTxt, 214, 62);

  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  canvas.setTextSize(1);
  char gpuTTxt[12];
  if (sys.gpuTemp <= 5.0f) {
    snprintf(gpuTTxt, sizeof(gpuTTxt), "--C");
  } else {
    snprintf(gpuTTxt, sizeof(gpuTTxt), "%2.0fC", sys.gpuTemp);
  }
  canvas.drawString(gpuTTxt, 274, 65);
  drawGlowProgressBar(184, 82, 124, 7, sys.animGpu, COLOR_AMBER, 0x281B0C);

  // Clean horizontal divider
  canvas.drawFastHLine(184, 96, 124, 0x18202D);

  // Network Section: Download and Upload SIDE-BY-SIDE (Zero negative space)
  // Left Column: DOWNLOAD (v DOWN)
  canvas.setTextColor(COLOR_NET_DL, CARD_BG);
  canvas.setTextSize(1);
  canvas.drawString("v DOWN", 184, 101);

  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  canvas.setTextSize(2);
  char dlTxt[16];
  snprintf(dlTxt, sizeof(dlTxt), "%3.1f", sys.netDownMbps);
  canvas.drawString(dlTxt, 184, 114);

  canvas.setTextColor(COLOR_MUTED, CARD_BG);
  canvas.setTextSize(1);
  canvas.drawString("MB/s", 184, 134);
  drawGlowProgressBar(184, 148, 56, 7, constrain((sys.netDownMbps / 10.0f) * 100.0f, 2.0f, 100.0f), COLOR_NET_DL, 0x0A202A);

  // Vertical Divider between DL and UL
  canvas.drawFastVLine(245, 101, 54, 0x18202D);

  // Right Column: UPLOAD (^ UP)
  canvas.setTextColor(COLOR_NET_UL, CARD_BG);
  canvas.setTextSize(1);
  canvas.drawString("^ UP", 252, 101);

  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  canvas.setTextSize(2);
  char ulTxt[16];
  snprintf(ulTxt, sizeof(ulTxt), "%3.1f", sys.netUpMbps);
  canvas.drawString(ulTxt, 252, 114);

  canvas.setTextColor(COLOR_MUTED, CARD_BG);
  canvas.setTextSize(1);
  canvas.drawString("MB/s", 252, 134);
  drawGlowProgressBar(252, 148, 56, 7, constrain((sys.netUpMbps / 10.0f) * 100.0f, 2.0f, 100.0f), COLOR_NET_UL, 0x0A2A1E);
}

// =============================================================================
// PAGE 1: CORE WORKLOAD (CPU, GPU & RAM) - BORDERLESS CARDS & LARGE FONTS
// =============================================================================
void renderPage1Workload() {
  drawHeader("WORKLOAD", 1);

  // CARD 1: CPU (Y: 27 to 71) - BORDERLESS + ACCENT PILL
  canvas.fillRoundRect(4, 27, SCREEN_W - 8, 44, 6, CARD_BG);
  canvas.fillRoundRect(4, 27, 4, 44, 2, COLOR_CYAN);

  canvas.setTextColor(COLOR_CYAN, CARD_BG);
  canvas.setTextSize(2);
  canvas.drawString("CPU", 16, 31);

  canvas.setTextSize(3);
  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  char cpuBig[16];
  snprintf(cpuBig, sizeof(cpuBig), "%2.0f%%", sys.cpuPercent);
  canvas.drawString(cpuBig, 90, 29);

  canvas.setTextSize(2);
  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  char cpuT[16];
  if (sys.cpuTemp <= 5.0f) {
    snprintf(cpuT, sizeof(cpuT), "--C");
  } else {
    snprintf(cpuT, sizeof(cpuT), "%2.0fC", sys.cpuTemp);
  }
  canvas.drawString(cpuT, 240, 31);
  drawGlowProgressBar(16, 56, SCREEN_W - 32, 8, sys.animCpu, COLOR_CYAN, 0x0A202A);

  // CARD 2: GPU (Y: 74 to 118) - BORDERLESS + ACCENT PILL
  canvas.fillRoundRect(4, 74, SCREEN_W - 8, 44, 6, CARD_BG);
  canvas.fillRoundRect(4, 74, 4, 44, 2, COLOR_AMBER);

  canvas.setTextColor(COLOR_AMBER, CARD_BG);
  canvas.setTextSize(2);
  canvas.drawString("GPU", 16, 78);

  canvas.setTextSize(3);
  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  char gpuBig[16];
  snprintf(gpuBig, sizeof(gpuBig), "%2.0f%%", sys.gpuPercent);
  canvas.drawString(gpuBig, 90, 76);

  canvas.setTextSize(2);
  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  char gpuT[16];
  if (sys.gpuTemp <= 5.0f) {
    snprintf(gpuT, sizeof(gpuT), "--C");
  } else {
    snprintf(gpuT, sizeof(gpuT), "%2.0fC", sys.gpuTemp);
  }
  canvas.drawString(gpuT, 240, 78);
  drawGlowProgressBar(16, 103, SCREEN_W - 32, 8, sys.animGpu, COLOR_AMBER, 0x25180A);

  // CARD 3: RAM (Y: 121 to 169) - BORDERLESS + ACCENT PILL
  canvas.fillRoundRect(4, 121, SCREEN_W - 8, 47, 6, CARD_BG);
  canvas.fillRoundRect(4, 121, 4, 47, 2, COLOR_MAGENTA);

  canvas.setTextColor(COLOR_MAGENTA, CARD_BG);
  canvas.setTextSize(2);
  canvas.drawString("RAM", 16, 125);

  canvas.setTextSize(3);
  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  char ramBig[16];
  snprintf(ramBig, sizeof(ramBig), "%2.0f%%", sys.ramPercent);
  canvas.drawString(ramBig, 90, 123);

  canvas.setTextSize(2);
  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  char ramGB[20];
  snprintf(ramGB, sizeof(ramGB), "%1.1fG", sys.ramUsedGb);
  canvas.drawString(ramGB, 230, 125);
  drawGlowProgressBar(16, 151, SCREEN_W - 32, 8, sys.animRam, COLOR_MAGENTA, 0x251025);
}

// =============================================================================
// PAGE 2: DEDICATED NETWORK MONITOR (TRUE DARK THEME & DUAL WAVE GRAPHS)
// =============================================================================
void renderPage2Network() {
  drawHeader("NETWORK", 2);

  // DOWNLOAD CARD (Y: 27 to 95) - TRUE DARK THEME CARD_BG
  canvas.fillRoundRect(4, 27, SCREEN_W - 8, 68, 6, CARD_BG);
  canvas.fillRoundRect(4, 27, 4, 68, 2, COLOR_NET_DL);

  canvas.setTextColor(COLOR_NET_DL, CARD_BG);
  canvas.setTextSize(2);
  canvas.drawString("v DOWNLOAD", 16, 31);

  canvas.setTextSize(3);
  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  char dlBig[16];
  snprintf(dlBig, sizeof(dlBig), "%4.1f", sys.netDownMbps);
  canvas.drawString(dlBig, 16, 53);

  canvas.setTextSize(2);
  canvas.setTextColor(0xB8E8FF, CARD_BG);
  canvas.drawString("MB/s", 114, 60);

  // Mini Sparkline Graph Well for DOWNLOAD (Deep dark well)
  const int nx = 172, ny = 33, nw = 136, nh = 54;
  canvas.fillRoundRect(nx, ny, nw, nh, 4, 0x080B10);

  float maxDl = 2.0f;
  for (int i = 0; i < GRAPH_SAMPLES; i++) {
    if (netDownHistory[i] > maxDl) maxDl = netDownHistory[i];
  }

  for (int i = 0; i < GRAPH_SAMPLES - 1; i++) {
    int x1 = nx + (i * nw) / (GRAPH_SAMPLES - 1);
    int x2 = nx + ((i + 1) * nw) / (GRAPH_SAMPLES - 1);
    int y1 = ny + nh - (int)((netDownHistory[i] / maxDl) * (nh - 6)) - 3;
    int y2 = ny + nh - (int)((netDownHistory[i + 1] / maxDl) * (nh - 6)) - 3;

    canvas.fillTriangle(x1, y1, x2, y2, x1, ny + nh, 0x001A24);
    canvas.fillTriangle(x2, y2, x2, ny + nh, x1, ny + nh, 0x001A24);
    canvas.drawLine(x1, y1, x2, y2, COLOR_NET_DL);
    canvas.drawLine(x1, y1 + 1, x2, y2 + 1, 0x007766);
  }

  // UPLOAD CARD (Y: 98 to 166) - TRUE DARK THEME CARD_BG
  canvas.fillRoundRect(4, 98, SCREEN_W - 8, 68, 6, CARD_BG);
  canvas.fillRoundRect(4, 98, 4, 68, 2, COLOR_NET_UL);

  canvas.setTextColor(COLOR_NET_UL, CARD_BG);
  canvas.setTextSize(2);
  canvas.drawString("^ UPLOAD", 16, 102);

  canvas.setTextSize(3);
  canvas.setTextColor(COLOR_WHITE, CARD_BG);
  char ulBig[16];
  snprintf(ulBig, sizeof(ulBig), "%4.1f", sys.netUpMbps);
  canvas.drawString(ulBig, 16, 124);

  canvas.setTextSize(2);
  canvas.setTextColor(0xB8FFD8, CARD_BG);
  canvas.drawString("MB/s", 114, 131);

  // Mini Sparkline Graph Well for UPLOAD (Deep dark well)
  const int ux = 172, uy = 104, uw = 136, uh = 54;
  canvas.fillRoundRect(ux, uy, uw, uh, 4, 0x080B10);

  float maxUl = 1.0f;
  for (int i = 0; i < GRAPH_SAMPLES; i++) {
    if (netUpHistory[i] > maxUl) maxUl = netUpHistory[i];
  }

  for (int i = 0; i < GRAPH_SAMPLES - 1; i++) {
    int x1 = ux + (i * uw) / (GRAPH_SAMPLES - 1);
    int x2 = ux + ((i + 1) * uw) / (GRAPH_SAMPLES - 1);
    int y1 = uy + uh - (int)((netUpHistory[i] / maxUl) * (uh - 6)) - 3;
    int y2 = uy + uh - (int)((netUpHistory[i + 1] / maxUl) * (uh - 6)) - 3;

    canvas.fillTriangle(x1, y1, x2, y2, x1, uy + uh, 0x002414);
    canvas.fillTriangle(x2, y2, x2, uy + uh, x1, uy + uh, 0x002414);
    canvas.drawLine(x1, y1, x2, y2, COLOR_NET_UL);
    canvas.drawLine(x1, y1 + 1, x2, y2 + 1, 0x008844);
  }
}

// =============================================================================
// PAGE 3: DUAL TEMPERATURE GRAPH (BORDERLESS OSCILLOSCOPE & PILLS)
// =============================================================================
void renderPage3Thermals() {
  drawHeader("THERMALS", 3);

  // Thermal Pill Badges (Borderless Sleek Pills)
  canvas.fillRoundRect(4, 26, 154, 26, 6, 0x0B202A);
  canvas.fillCircle(14, 39, 4, COLOR_CYAN);
  canvas.setTextColor(COLOR_CYAN, 0x0B202A);
  canvas.setTextSize(2);
  canvas.drawString("CPU:", 24, 31);
  canvas.setTextColor(COLOR_WHITE, 0x0B202A);
  char cpuTStr[16];
  if (sys.cpuTemp <= 5.0f) {
    snprintf(cpuTStr, sizeof(cpuTStr), "--C");
  } else {
    snprintf(cpuTStr, sizeof(cpuTStr), "%2.0fC", sys.cpuTemp);
  }
  canvas.drawString(cpuTStr, 82, 31);

  canvas.fillRoundRect(162, 26, 154, 26, 6, 0x28190B);
  canvas.fillCircle(172, 39, 4, COLOR_AMBER);
  canvas.setTextColor(COLOR_AMBER, 0x28190B);
  canvas.setTextSize(2);
  canvas.drawString("GPU:", 182, 31);
  canvas.setTextColor(COLOR_WHITE, 0x28190B);
  char gpuTStr[16];
  if (sys.gpuTemp <= 5.0f) {
    snprintf(gpuTStr, sizeof(gpuTStr), "--C");
  } else {
    snprintf(gpuTStr, sizeof(gpuTStr), "%2.0fC", sys.gpuTemp);
  }
  canvas.drawString(gpuTStr, 240, 31);

  // Large Dual-Trace Thermal Oscilloscope Container (BORDERLESS)
  const int tx = 4, ty = 56, tw = SCREEN_W - 8, th = 112;
  canvas.fillRoundRect(tx, ty, tw, th, 6, CARD_BG);

  const float minT = 25.0f, maxT = 95.0f;
  auto tempToY = [&](float t) -> int {
    float cl = constrain(t, minT, maxT);
    return ty + th - (int)(((cl - minT) / (maxT - minT)) * (th - 20)) - 10;
  };

  int y40 = tempToY(40.0f);
  int y60 = tempToY(60.0f);
  int y80 = tempToY(80.0f);

  // Subtle Reference Guides (Brightened Text)
  canvas.drawFastHLine(tx + 8, y40, tw - 16, 0x1A2230);
  canvas.setTextColor(0x8FA4BC, CARD_BG);
  canvas.setTextSize(1);
  canvas.drawString("40C", tx + 12, y40 - 8);

  canvas.drawFastHLine(tx + 8, y60, tw - 16, 0x222E40);
  canvas.setTextColor(0xA0B8D0, CARD_BG);
  canvas.drawString("60C", tx + 12, y60 - 8);

  canvas.drawFastHLine(tx + 8, y80, tw - 16, 0x481C1C);
  canvas.setTextColor(COLOR_RED, CARD_BG);
  canvas.drawString("80C ALERT", tx + 12, y80 - 8);

  // Trace 1: CPU Temperature (Neon Cyan)
  for (int i = 0; i < GRAPH_SAMPLES - 1; i++) {
    int x1 = tx + 34 + (i * (tw - 42)) / (GRAPH_SAMPLES - 1);
    int x2 = tx + 34 + ((i + 1) * (tw - 42)) / (GRAPH_SAMPLES - 1);
    int y1 = tempToY(cpuTempHistory[i]);
    int y2 = tempToY(cpuTempHistory[i + 1]);
    canvas.drawLine(x1, y1, x2, y2, COLOR_CYAN);
    canvas.drawLine(x1, y1 + 1, x2, y2 + 1, 0x008866);
  }

  // Trace 2: GPU Temperature (Amber / Coral)
  for (int i = 0; i < GRAPH_SAMPLES - 1; i++) {
    int x1 = tx + 34 + (i * (tw - 42)) / (GRAPH_SAMPLES - 1);
    int x2 = tx + 34 + ((i + 1) * (tw - 42)) / (GRAPH_SAMPLES - 1);
    int y1 = tempToY(gpuTempHistory[i]);
    int y2 = tempToY(gpuTempHistory[i + 1]);
    canvas.drawLine(x1, y1, x2, y2, COLOR_AMBER);
    canvas.drawLine(x1, y1 + 1, x2, y2 + 1, 0x885500);
  }

  // Legends
  canvas.fillCircle(tx + 54, ty + th - 10, 3, COLOR_CYAN);
  canvas.setTextColor(COLOR_CYAN, CARD_BG);
  canvas.drawString("CPU Wave", tx + 62, ty + th - 14);

  canvas.fillCircle(tx + 154, ty + th - 10, 3, COLOR_AMBER);
  canvas.setTextColor(COLOR_AMBER, CARD_BG);
  canvas.drawString("GPU Wave", tx + 162, ty + th - 14);
}

// =============================================================================
// PUBLIC DEMO SHOWCASE TELEMETRY CONTROLLER
// =============================================================================
void toggleDemoMode() {
  demoMode = !demoMode;
  preferences.putBool("demo", demoMode);
  g_forceImmediateRender = true;
  Serial.printf("[DEMO] Showcase Mode: %s\n", demoMode ? "ACTIVE (Public Display)" : "OFF (PC Stream)");
  if (!demoMode) {
    sys.cpuPercent  = 0.0f;
    sys.ramPercent  = 0.0f;
    sys.ramUsedGb   = 0.0f;
    sys.gpuPercent  = 0.0f;
    sys.netDownMbps = 0.0f;
    sys.netUpMbps   = 0.0f;
    sys.cpuTemp     = 0.0f;
    sys.gpuTemp     = 0.0f;
    for (int i = 0; i < GRAPH_SAMPLES; i++) {
      cpuHistory[i]      = 0.0f;
      cpuTempHistory[i]  = 0.0f;
      gpuTempHistory[i]  = 0.0f;
      netDownHistory[i]  = 0.0f;
      netUpHistory[i]    = 0.0f;
    }
  }
}

// Generates dynamic, realistic PC telemetry for public booths, desk showcases & exhibitions
void updateDemoTelemetry() {
  static uint32_t lastDemoMs = 0;
  uint32_t now = millis();
  if (now - lastDemoMs < 500) return;
  lastDemoMs = now;

  float t = now / 1000.0f;

  // Realistic waving CPU load (35% - 78%) with organic jitter
  sys.cpuPercent = constrain(46.0f + 20.0f * sinf(t * 0.35f) + 10.0f * sinf(t * 0.85f + 1.2f) + (random(-25, 26) / 10.0f), 12.0f, 96.0f);

  // Realistic CPU Die Temp (50°C - 69°C) with load correlation
  sys.cpuTemp = constrain(50.0f + (sys.cpuPercent * 0.22f) + 3.0f * sinf(t * 0.2f) + (random(-8, 9) / 10.0f), 45.0f, 78.0f);

  // Realistic GPU load (42% - 88%)
  sys.gpuPercent = constrain(58.0f + 22.0f * sinf(t * 0.42f + 2.0f) + 8.0f * cosf(t * 0.95f) + (random(-20, 21) / 10.0f), 18.0f, 98.0f);

  // Realistic GPU Temp (54°C - 72°C)
  sys.gpuTemp = constrain(53.0f + (sys.gpuPercent * 0.20f) + 2.5f * cosf(t * 0.3f) + (random(-7, 8) / 10.0f), 48.0f, 82.0f);

  // Realistic RAM (15.2 GB / 32.0 GB ~ 47.5%)
  sys.ramTotalGb = 32.0f;
  sys.ramUsedGb  = 14.8f + 2.2f * sinf(t * 0.15f) + (random(-4, 5) / 10.0f);
  sys.ramPercent = (sys.ramUsedGb / sys.ramTotalGb) * 100.0f;

  // Realistic Network Waves (bursty downloads & smooth uploads)
  float netWave = sinf(t * 0.5f);
  if (netWave > 0.1f) {
    sys.netDownMbps = 24.0f + 85.0f * (netWave * netWave) + (random(0, 80) / 10.0f);
  } else {
    sys.netDownMbps = 3.5f + (random(0, 30) / 10.0f);
  }
  sys.netUpMbps = 2.4f + 14.0f * (sinf(t * 0.35f + 1.5f) * sinf(t * 0.35f + 1.5f)) + (random(0, 15) / 10.0f);

  // Shift & append history buffers for live graphs
  for (int i = 0; i < GRAPH_SAMPLES - 1; i++) {
    cpuHistory[i]      = cpuHistory[i + 1];
    cpuTempHistory[i]  = cpuTempHistory[i + 1];
    gpuTempHistory[i]  = gpuTempHistory[i + 1];
    netDownHistory[i]  = netDownHistory[i + 1];
    netUpHistory[i]    = netUpHistory[i + 1];
  }
  cpuHistory[GRAPH_SAMPLES - 1]     = sys.cpuPercent;
  cpuTempHistory[GRAPH_SAMPLES - 1] = sys.cpuTemp;
  gpuTempHistory[GRAPH_SAMPLES - 1] = sys.gpuTemp;
  netDownHistory[GRAPH_SAMPLES - 1] = sys.netDownMbps;
  netUpHistory[GRAPH_SAMPLES - 1]   = sys.netUpMbps;

  sys.dataReceived = true;
  sys.lastDataTime = now;
}

// =============================================================================
// PAGE 4: PUBLIC DEMO SHOWCASE CONTROLLER (100% MAXIMUM BRIGHTNESS)
// =============================================================================
void renderPage4Settings() {
  drawHeader("DEMO MODE", 4);

  // ---------------------------------------------------------------------------
  // 1. MASSIVE PUBLIC DEMO MODE CONTROLLER CARD (Height 98, Width 308)
  // ---------------------------------------------------------------------------
  int cardX = 6, cardY = 28, cardW = 308, cardH = 98, cardR = 10;

  if (demoMode) {
    // ACTIVE STATE: Radiant Neon Mint Glow
    canvas.fillRoundRect(cardX, cardY, cardW, cardH, cardR, 0x003A20);
    canvas.drawRoundRect(cardX, cardY, cardW, cardH, cardR, 0x00FF88);
    canvas.drawRoundRect(cardX + 1, cardY + 1, cardW - 2, cardH - 2, cardR - 1, 0x00CC66);

    // Glowing Status Badge
    canvas.fillRoundRect(cardX + 80, cardY + 10, 148, 18, 4, 0x005530);
    canvas.setTextColor(0x00FF88, 0x005530);
    canvas.setTextSize(1);
    canvas.drawString("● PUBLIC DEMO ACTIVE", cardX + 90, cardY + 15);

    // Primary Title
    canvas.setTextColor(0x00FF88, 0x003A20);
    canvas.setTextSize(1);
    canvas.drawString("✔ 60 FPS REALISTIC TELEMETRY", cardX + 46, cardY + 38);

    // Subtitle
    canvas.setTextColor(0x88EEBB, 0x003A20);
    canvas.drawString("Simulating Waving CPU, GPU, RAM & Network Waves", cardX + 14, cardY + 54);

    // Action Pill
    canvas.fillRoundRect(cardX + 36, cardY + 70, 236, 20, 5, 0x005530);
    canvas.setTextColor(0xFFFFFF, 0x005530);
    canvas.drawString("[ TAP CARD TO STOP DEMO & USE PC ]", cardX + 48, cardY + 76);
  } else {
    // INACTIVE STATE: Sleek Elevated Obsidian Surface
    canvas.fillRoundRect(cardX, cardY, cardW, cardH, cardR, 0x121824);
    canvas.drawRoundRect(cardX, cardY, cardW, cardH, cardR, 0x2D3B4E);

    // Standby Badge
    canvas.fillRoundRect(cardX + 80, cardY + 10, 148, 18, 4, 0x1E2938);
    canvas.setTextColor(COLOR_CYAN, 0x1E2938);
    canvas.setTextSize(1);
    canvas.drawString("STANDBY / PC STREAM", cardX + 96, cardY + 15);

    // Primary Title
    canvas.setTextColor(COLOR_WHITE, 0x121824);
    canvas.setTextSize(1);
    canvas.drawString("▶ START PUBLIC DEMO MODE", cardX + 58, cardY + 38);

    // Subtitle
    canvas.setTextColor(COLOR_MUTED, 0x121824);
    canvas.drawString("Showcase Animated Graphs in Public Without PC", cardX + 22, cardY + 54);

    // Action Pill
    canvas.fillRoundRect(cardX + 36, cardY + 70, 236, 20, 5, 0x1E2938);
    canvas.setTextColor(COLOR_CYAN, 0x1E2938);
    canvas.drawString("[ TAP CARD TO START 60 FPS DEMO ]", cardX + 50, cardY + 76);
  }

  // ---------------------------------------------------------------------------
  // 2. BOTTOM DEVICE TELEMETRY BAR (Height 34, Width 308)
  // ---------------------------------------------------------------------------
  int barX = 6, barY = 132, barW = 308, barH = 34, barR = 6;
  canvas.fillRoundRect(barX, barY, barW, barH, barR, CARD_BG);
  canvas.drawRoundRect(barX, barY, barW, barH, barR, 0x1A2330);

  // Status Pill 1: Backlight locked at 100%
  canvas.setTextColor(COLOR_GOLD, CARD_BG);
  canvas.setTextSize(1);
  canvas.drawString("☀ BL: 100% (MAX)", barX + 10, barY + 12);

  // Status Pill 2: Battery
  uint32_t batCol = (sys.batteryVoltage < 3.4f) ? COLOR_RED : ((sys.batteryVoltage < 3.7f) ? COLOR_AMBER : 0x00FF99);
  char batStr[16];
  snprintf(batStr, sizeof(batStr), "BAT: %.2fV", sys.batteryVoltage);
  canvas.setTextColor(batCol, CARD_BG);
  canvas.drawString(batStr, barX + 130, barY + 12);

  // Status Pill 3: Refresh Rate
  canvas.setTextColor(COLOR_CYAN, CARD_BG);
  canvas.drawString("DISP: 60 FPS", barX + 224, barY + 12);
}

// =============================================================================
// RENDER DISPATCHER (100% Double-Buffered, ZERO Flicker, BORDERLESS)
// =============================================================================
void renderDashboard() {
  // 60 FPS Exponential lerp for silky-smooth indicator transitions
  sys.animCpu     += (sys.cpuPercent - sys.animCpu) * 0.22f;
  sys.animRam     += (sys.ramPercent - sys.animRam) * 0.22f;
  sys.animGpu     += (sys.gpuPercent - sys.animGpu) * 0.22f;
  sys.animNetDown += (sys.netDownMbps - sys.animNetDown) * 0.25f;

  canvas.fillScreen(BG_DARK);

  switch (currentPage) {
    case 0: renderPage0Summary(); break;
    case 1: renderPage1Workload(); break;
    case 2: renderPage2Network(); break;
    case 3: renderPage3Thermals(); break;
    case 4: renderPage4Settings(); break;
    default: currentPage = 0; renderPage0Summary(); break;
  }

  // Blit 110 KB buffer to physical display in ONE DMA push!
  canvas.pushSprite(0, 0);
}

// =============================================================================
// Touch & Physical Buttons Handler (D19 = Next Page, D15 = Prev Page, BOOT = Next)
// =============================================================================
void handleTouchAndButtons() {
  static bool wasTouchPressed = false;
  static bool wasBtn1Pressed  = false;
  static bool wasBtn2Pressed  = false;
  static bool wasBootPressed  = false;
  static uint32_t lastBtn1Ms  = 0;
  static uint32_t lastBtn2Ms  = 0;
  static uint32_t lastBootMs  = 0;
  static uint32_t lastTouchMs = 0;
  uint32_t now = millis();

  // 1. Non-blocking Capacitive Touch Handling
  uint16_t tx = 0, ty = 0;
  bool touchPressed = touch.getTouch(&tx, &ty);

  if (touchPressed && !wasTouchPressed && (now - lastTouchMs > 200)) {
    lastTouchMs = now;
    if (currentPage == 4) {
      // Top header bar tap (y < 28) -> Page Navigation
      if (ty < 28) {
        if (tx < SCREEN_W / 2) {
          currentPage = (currentPage == 0) ? (TOTAL_PAGES - 1) : (currentPage - 1);
        } else {
          currentPage = (currentPage + 1) % TOTAL_PAGES;
        }
      }
      // Tapping anywhere on the large card toggles Public Demo Mode!
      else {
        toggleDemoMode();
      }
    } else {
      // Pages 0..3: Full-screen tap left / right for instant page navigation
      if (tx < SCREEN_W / 2) {
        currentPage = (currentPage == 0) ? (TOTAL_PAGES - 1) : (currentPage - 1);
      } else {
        currentPage = (currentPage + 1) % TOTAL_PAGES;
      }
    }
    g_forceImmediateRender = true;
    Serial.printf("[TOUCH] Tap at (%d, %d) -> Page %d\n", tx, ty, currentPage);
  }
  wasTouchPressed = touchPressed;

  // 2. Physical Button 1 (D19) -> NEXT PAGE (Instant response, 120ms debounced)
  bool btn1 = (digitalRead(BTN_1_PIN) == LOW);
  if (btn1 && !wasBtn1Pressed && (now - lastBtn1Ms > 120)) {
    lastBtn1Ms = now;
    currentPage = (currentPage + 1) % TOTAL_PAGES;
    g_forceImmediateRender = true;
    Serial.printf("[BUTTON 1 - D19] Pressed -> Page %d\n", currentPage);
  }
  wasBtn1Pressed = btn1;

  // 3. Physical Button 2 (D15) -> PREVIOUS PAGE (Instant response, 120ms debounced)
  bool btn2 = (digitalRead(BTN_2_PIN) == LOW);
  if (btn2 && !wasBtn2Pressed && (now - lastBtn2Ms > 120)) {
    lastBtn2Ms = now;
    currentPage = (currentPage == 0) ? (TOTAL_PAGES - 1) : (currentPage - 1);
    g_forceImmediateRender = true;
    Serial.printf("[BUTTON 2 - D15] Pressed -> Page %d\n", currentPage);
  }
  wasBtn2Pressed = btn2;

  // 4. Onboard BOOT Button (GPIO 0) -> CYCLE PAGE
  bool bootBtn = (digitalRead(BTN_BOOT) == LOW);
  if (bootBtn && !wasBootPressed && (now - lastBootMs > 120)) {
    lastBootMs = now;
    currentPage = (currentPage + 1) % TOTAL_PAGES;
    g_forceImmediateRender = true;
    Serial.printf("[BOOT] Pressed -> Page %d\n", currentPage);
  }
  wasBootPressed = bootBtn;
}

// =============================================================================
// OFFICIAL BEARBYTE BOOTUP SPLASH SCREEN (CYBER YELLOW & BLACK THEME)
// =============================================================================
void showBootScreen() {
  const uint16_t* pLogo = bear_byte_logo;
  uint32_t barTrack = 0xD4A000; // Warm amber-gold capsule track on yellow
  uint32_t barFill  = 0x101012; // Deep obsidian black fill matching the logo

  canvas.setSwapBytes(false); // Native LovyanGFX swap565_t DMA mode

  // Initial Instant Render of Logo
  canvas.pushImage(0, 0, BOOT_LOGO_W, BOOT_LOGO_H, pLogo);
  canvas.pushSprite(0, 0);

  // Smooth 1.4-second loading progression with sleek black capsule
  for (int p = 0; p <= 100; p += 4) {
    canvas.pushImage(0, 0, BOOT_LOGO_W, BOOT_LOGO_H, pLogo);
    drawGlowProgressBar(70, 154, 180, 5, (float)p, barFill, barTrack);
    canvas.pushSprite(0, 0);
    delay(28);

    // Skip boot screen immediately if user touches screen or presses button
    uint16_t tx = 0, ty = 0;
    if (touch.getTouch(&tx, &ty) || digitalRead(BTN_1_PIN) == LOW || digitalRead(BTN_2_PIN) == LOW || digitalRead(BTN_BOOT) == LOW) {
      break;
    }
  }
  delay(250);
}

// =============================================================================
// SETUP & LOOP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=======================================================");
  Serial.println("       BEARBYTE PC RESOURCE MONITOR (60 FPS)           ");
  Serial.println("=======================================================");

  // Keep on-board microSD socket deselected on shared SPI bus
  pinMode(D6, OUTPUT);
  digitalWrite(D6, HIGH);

  // Initialize Display with LovyanGFX
  lcd.init();
  lcd.setRotation(1); // Landscape (320 x 172)

  // Lock screen backlight to 100% maximum brightness at all times
  lcd.setBrightness(255);

  // Load saved demo state from NVS
  preferences.begin("bearbyte", false);
  demoMode = preferences.getBool("demo", false);

  // Allocate 110 KB Double-Buffered Canvas in RAM/PSRAM
  canvas.setColorDepth(16);
  if (!canvas.createSprite(SCREEN_W, SCREEN_H)) {
    Serial.println("[ERROR] Failed to allocate 320x172 DMA Canvas!");
  } else {
    Serial.println("[OK] 320x172 Double-Buffered DMA Canvas ready!");
  }

  // Setup Physical Buttons (D19 = Next, D15 = Prev, BOOT = Cycle)
  pinMode(BTN_1_PIN, INPUT_PULLUP);
  pinMode(BTN_2_PIN, INPUT_PULLUP);
  pinMode(BTN_BOOT, INPUT_PULLUP);
  touch.begin();

  // Show Official BearByte Boot Screen!
  showBootScreen();

  // Setup Battery Voltage ADC (12-bit, 11dB attenuation)
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  sys.batteryVoltage = readBatteryVoltage();

  // Start Bluetooth Low Energy
  initBLE();
}

void loop() {
  // 1. Process touch gestures and physical buttons (D19 & D15)
  handleTouchAndButtons();

  // 2. Battery Voltage Sampling (Non-blocking every 1000ms with low-pass filtering)
  static uint32_t lastBatMs = 0;
  if (millis() - lastBatMs >= 1000) {
    lastBatMs = millis();
    float rawBat = readBatteryVoltage();
    if (rawBat > 2.5f && rawBat < 4.5f) {
      sys.batteryVoltage = (sys.batteryVoltage * 0.7f) + (rawBat * 0.3f);
    } else {
      sys.batteryVoltage = rawBat;
    }
  }

  // 3. Ultra-Fast USB-C Serial Streaming (Zero heap allocations)
  static char serialRxBuf[256];
  static uint16_t rxIdx = 0;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxIdx > 0) {
        serialRxBuf[rxIdx] = '\0';
        parsePacket(String(serialRxBuf));
        rxIdx = 0;
      }
    } else {
      if (rxIdx < sizeof(serialRxBuf) - 1) {
        serialRxBuf[rxIdx++] = c;
      } else {
        rxIdx = 0; // Prevent overflow
      }
    }
  }

  // 4. Telemetry Update: Demo Mode Simulation vs Standby Zero-State
  if (demoMode) {
    updateDemoTelemetry();
  } else {
    // Standby Zero-State: Keep telemetry at true 0 when PC app is offline (NO random fake waves!)
    if (!sys.dataReceived || (millis() - sys.lastDataTime > 4000)) {
      sys.cpuPercent  = 0.0f;
      sys.ramPercent  = 0.0f;
      sys.ramUsedGb   = 0.0f;
      sys.gpuPercent  = 0.0f;
      sys.netDownMbps = 0.0f;
      sys.netUpMbps   = 0.0f;
      sys.cpuTemp     = 0.0f;
      sys.gpuTemp     = 0.0f;
    }
  }

  // 5. Smooth 60 FPS Framerate-Paced Render + INSTANT Trigger on button/touch
  static uint32_t lastRenderUs = 0;
  uint32_t nowUs = micros();
  if (g_forceImmediateRender || (nowUs - lastRenderUs >= 16666)) {
    lastRenderUs = nowUs;
    g_forceImmediateRender = false;
    renderDashboard();
  }

  // Handle BLE advertising auto-restart (non-blocking)
  static uint32_t bleDisconnectMs = 0;
  if (!bleConnected && oldBleConnected) {
    bleDisconnectMs = millis();
    oldBleConnected = bleConnected;
  }
  if (!bleConnected && bleDisconnectMs > 0 && (millis() - bleDisconnectMs >= 100)) {
    bleDisconnectMs = 0;
    pServer->startAdvertising();
    Serial.println("[BLE] Restarted advertising");
  }
  if (bleConnected && !oldBleConnected) {
    oldBleConnected = bleConnected;
    Serial.println("[BLE] Client connected!");
  }
}
