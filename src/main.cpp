#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <cmath>

namespace {

constexpr uint8_t kTftCs = 15;
constexpr uint8_t kTftDc = 2;
constexpr int8_t kTftRst = -1;
constexpr uint8_t kTftSclk = 14;
constexpr uint8_t kTftMiso = 12;
constexpr uint8_t kTftMosi = 13;
constexpr uint8_t kBacklightPin = 21;
constexpr uint8_t kTouchCs = 33;
constexpr uint8_t kTouchIrq = 36;
constexpr uint8_t kGrblRxPin = 16;
constexpr uint8_t kGrblTxPin = 17;
constexpr uint32_t kGrblBaud = 115200;
constexpr uint8_t kRotation = 1;

constexpr int16_t kTouchRawMinX = 240;
constexpr int16_t kTouchRawMaxX = 3800;
constexpr int16_t kTouchRawMinY = 200;
constexpr int16_t kTouchRawMaxY = 3850;
constexpr bool kTouchSwapAxes = true;
constexpr bool kTouchInvertX = false;
constexpr bool kTouchInvertY = true;

constexpr uint16_t kBackground = 0x1082;
constexpr uint16_t kPanel = 0x18E3;
constexpr uint16_t kPanelOutline = 0x4208;
constexpr uint16_t kAccent = 0xFC00;
constexpr uint16_t kPositive = 0x2626;
constexpr uint16_t kNegative = 0xB924;
constexpr uint16_t kWarning = 0xFD20;
constexpr uint16_t kText = 0xFFFF;
constexpr uint16_t kMutedText = 0xBDF7;
constexpr uint16_t kDRO = 0x17FF;

constexpr uint32_t kStatusPollMs = 250;
constexpr uint32_t kRedrawMs = 150;
constexpr uint32_t kTouchDebounceMs = 220;

enum class Action : uint8_t {
  None,
  Unlock,
  Home,
  Reset,
  Hold,
  Resume,
  ZeroX,
  ZeroY,
  ZeroZ,
  Step001,
  Step010,
  Step100,
  XMinus,
  XPlus,
  YMinus,
  YPlus,
  ZMinus,
  ZPlus,
  SpindleToggle,
  CoolantToggle,
};

struct Button {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  const char *label;
  Action action;
  uint16_t fill;
  uint16_t text;
  bool latch;
};

struct MachineState {
  String status = "BOOT";
  float mpos[3] = {0.0F, 0.0F, 0.0F};
  float wpos[3] = {0.0F, 0.0F, 0.0F};
  int feed = 0;
  int spindleSpeed = 0;
  bool connected = false;
  bool spindleEnabled = false;
  bool coolantEnabled = false;
  String lastMessage = "Waiting for GRBL";
};

struct UiCache {
  String status;
  float wpos[3] = {NAN, NAN, NAN};
  int feed = INT32_MIN;
  int spindleSpeed = INT32_MIN;
  bool spindleEnabled = false;
  bool coolantEnabled = false;
  String lastMessage;
} uiCache;

SPIClass hspi(HSPI);
HardwareSerial grblSerial(2);
Arduino_DataBus *bus = new Arduino_ESP32SPI(kTftDc, kTftCs, kTftSclk, kTftMosi, kTftMiso, HSPI);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, kTftRst, kRotation, false);
XPT2046_Touchscreen touch(kTouchCs, kTouchIrq);
MachineState state;

float jogStepMm = 1.0F;
uint32_t lastStatusPoll = 0;
uint32_t lastRedraw = 0;
uint32_t lastTouch = 0;

constexpr Button kButtons[] = {
    {205, 34, 50, 28, "UNLK", Action::Unlock, kAccent, kText, false},
    {262, 34, 50, 28, "HOME", Action::Home, kPositive, kText, false},
    {205, 66, 50, 28, "HOLD", Action::Hold, kWarning, kText, false},
    {262, 66, 50, 28, "RUN", Action::Resume, kPositive, kText, false},
    {205, 98, 50, 28, "RST", Action::Reset, kNegative, kText, false},
    {262, 98, 50, 28, "M8", Action::CoolantToggle, kPanel, kText, true},
    {10, 184, 48, 22, "0.01", Action::Step001, kPanel, kText, true},
    {62, 184, 48, 22, "0.10", Action::Step010, kPanel, kText, true},
    {114, 184, 48, 22, "1.00", Action::Step100, kPanel, kText, true},
    {10, 211, 48, 22, "X=0", Action::ZeroX, kPanel, kText, false},
    {62, 211, 48, 22, "Y=0", Action::ZeroY, kPanel, kText, false},
    {114, 211, 48, 22, "Z=0", Action::ZeroZ, kPanel, kText, false},
    {177, 131, 42, 32, "Y+", Action::YPlus, kPositive, kText, false},
    {177, 201, 42, 32, "Y-", Action::YMinus, kPositive, kText, false},
    {132, 166, 42, 32, "X-", Action::XMinus, kPositive, kText, false},
    {222, 166, 42, 32, "X+", Action::XPlus, kPositive, kText, false},
    {267, 131, 42, 32, "Z+", Action::ZPlus, kAccent, kText, false},
    {267, 201, 42, 32, "Z-", Action::ZMinus, kAccent, kText, false},
    {177, 166, 42, 32, "M3", Action::SpindleToggle, kPanel, kText, true},
};

String formatAxis(float value) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%7.3f", static_cast<double>(value));
  return String(buffer);
}

float extractCoordinate(const String &token, uint8_t index) {
  int start = token.indexOf(':');
  if (start < 0) {
    return 0.0F;
  }

  start += 1;
  for (uint8_t current = 0; current < index; ++current) {
    start = token.indexOf(',', start);
    if (start < 0) {
      return 0.0F;
    }
    start += 1;
  }

  int end = token.indexOf(',', start);
  if (end < 0) {
    end = token.length();
  }
  return token.substring(start, end).toFloat();
}

int extractInt(const String &token, uint8_t index) {
  return static_cast<int>(lroundf(extractCoordinate(token, index)));
}

void sendLine(const String &command) {
  grblSerial.print(command);
  grblSerial.print('\n');
  state.lastMessage = command;
}

void sendRealtime(uint8_t command) {
  grblSerial.write(command);
}

void sendJog(char axis, float distanceMm) {
  char buffer[48];
  snprintf(buffer, sizeof(buffer), "$J=G91 G21 %c%.3f F500", axis, static_cast<double>(distanceMm));
  sendLine(buffer);
}

void parseStatusMessage(const String &line) {
  if (!line.startsWith("<") || !line.endsWith(">")) {
    return;
  }

  const String payload = line.substring(1, line.length() - 1);
  int tokenStart = 0;
  int tokenEnd = payload.indexOf('|');
  if (tokenEnd < 0) {
    tokenEnd = payload.length();
  }

  state.status = payload.substring(0, tokenEnd);
  state.connected = true;

  float workOffset[3] = {0.0F, 0.0F, 0.0F};
  bool hasMpos = false;
  bool hasWpos = false;
  bool hasWco = false;

  while (tokenStart < payload.length()) {
    tokenEnd = payload.indexOf('|', tokenStart);
    if (tokenEnd < 0) {
      tokenEnd = payload.length();
    }
    const String token = payload.substring(tokenStart, tokenEnd);

    if (token.startsWith("MPos:")) {
      for (uint8_t i = 0; i < 3; ++i) {
        state.mpos[i] = extractCoordinate(token, i);
      }
      hasMpos = true;
    } else if (token.startsWith("WPos:")) {
      for (uint8_t i = 0; i < 3; ++i) {
        state.wpos[i] = extractCoordinate(token, i);
      }
      hasWpos = true;
    } else if (token.startsWith("WCO:")) {
      for (uint8_t i = 0; i < 3; ++i) {
        workOffset[i] = extractCoordinate(token, i);
      }
      hasWco = true;
    } else if (token.startsWith("FS:")) {
      state.feed = extractInt(token, 0);
      state.spindleSpeed = extractInt(token, 1);
      state.spindleEnabled = state.spindleSpeed > 0;
    }

    tokenStart = tokenEnd + 1;
  }

  if (!hasWpos && hasMpos) {
    for (uint8_t i = 0; i < 3; ++i) {
      state.wpos[i] = state.mpos[i] - (hasWco ? workOffset[i] : 0.0F);
    }
  }
}

void processLine(const String &line) {
  if (line.isEmpty()) {
    return;
  }

  if (line.startsWith("<")) {
    parseStatusMessage(line);
    return;
  }

  state.connected = true;
  state.lastMessage = line;

  if (line.startsWith("ALARM")) {
    state.status = "ALARM";
  } else if (line.startsWith("error")) {
    state.status = "ERROR";
  } else if (line.startsWith("[MSG:")) {
    const int messageEnd = line.lastIndexOf(']');
    if (messageEnd > 5) {
      state.lastMessage = line.substring(5, messageEnd);
    }
  }
}

void pollGrbl() {
  static String serialBuffer;
  while (grblSerial.available() > 0) {
    const char raw = static_cast<char>(grblSerial.read());
    if (raw == '\r') {
      continue;
    }
    if (raw == '\n') {
      processLine(serialBuffer);
      serialBuffer = "";
      continue;
    }
    serialBuffer += raw;
  }
}

void drawButton(const Button &button) {
  bool isActive = false;
  if (button.action == Action::Step001) {
    isActive = fabsf(jogStepMm - 0.01F) < 0.001F;
  } else if (button.action == Action::Step010) {
    isActive = fabsf(jogStepMm - 0.1F) < 0.001F;
  } else if (button.action == Action::Step100) {
    isActive = fabsf(jogStepMm - 1.0F) < 0.001F;
  } else if (button.action == Action::SpindleToggle) {
    isActive = state.spindleEnabled;
  } else if (button.action == Action::CoolantToggle) {
    isActive = state.coolantEnabled;
  }

  const uint16_t fill = isActive && button.latch ? kAccent : button.fill;
  gfx->fillRoundRect(button.x, button.y, button.w, button.h, 4, fill);
  gfx->drawRoundRect(button.x, button.y, button.w, button.h, 4, kPanelOutline);
  gfx->setTextColor(button.text, fill);
  gfx->setTextSize(button.h >= 30 ? 2 : 1);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(button.label, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor(button.x + ((button.w - static_cast<int16_t>(w)) / 2), button.y + ((button.h + static_cast<int16_t>(h)) / 2) - 2);
  gfx->print(button.label);
}

void drawStaticUi() {
  gfx->fillScreen(kBackground);
  gfx->fillRect(0, 0, 320, 24, 0x0841);
  gfx->setTextColor(kText, 0x0841);
  gfx->setTextSize(2);
  gfx->setCursor(8, 5);
  gfx->print("CYD CNC");
  gfx->setTextColor(kMutedText, 0x0841);
  gfx->setTextSize(1);
  gfx->setCursor(114, 8);
  gfx->print("DMG-Mori-inspired GRBL control");

  gfx->fillRoundRect(8, 32, 190, 138, 6, kPanel);
  gfx->drawRoundRect(8, 32, 190, 138, 6, kPanelOutline);
  gfx->fillRoundRect(172, 125, 140, 108, 6, kPanel);
  gfx->drawRoundRect(172, 125, 140, 108, 6, kPanelOutline);
  gfx->fillRoundRect(8, 176, 160, 57, 6, kPanel);
  gfx->drawRoundRect(8, 176, 160, 57, 6, kPanelOutline);

  gfx->setTextColor(kMutedText, kPanel);
  gfx->setTextSize(1);
  gfx->setCursor(16, 40);
  gfx->print("WORK POSITION");
  gfx->setCursor(16, 179);
  gfx->print("STEP");
  gfx->setCursor(16, 206);
  gfx->print("WORK ZERO");

  for (const Button &button : kButtons) {
    drawButton(button);
  }
}

void drawValueRow(int16_t y, const char *label, const String &value) {
  gfx->fillRect(14, y, 180, 28, kPanel);
  gfx->setTextSize(2);
  gfx->setTextColor(kMutedText, kPanel);
  gfx->setCursor(16, y + 6);
  gfx->print(label);
  gfx->setTextColor(kDRO, kPanel);
  gfx->setCursor(60, y + 6);
  gfx->print(value);
}

void drawDynamicUi(bool force = false) {
  if (!force && millis() - lastRedraw < kRedrawMs) {
    return;
  }
  lastRedraw = millis();

  if (force || uiCache.status != state.status) {
    gfx->fillRect(12, 52, 180, 18, kPanel);
    gfx->setTextSize(2);
    const bool isRunning = state.status.equalsIgnoreCase("run") || state.status.equalsIgnoreCase("jog");
    const bool isFaulted = state.status.equalsIgnoreCase("alarm") || state.status.equalsIgnoreCase("error");
    const uint16_t statusColor =
        isFaulted ? kNegative : (isRunning ? kWarning : kPositive);
    gfx->setTextColor(statusColor, kPanel);
    gfx->setCursor(14, 54);
    gfx->print(state.status);
    uiCache.status = state.status;
  }

  for (uint8_t i = 0; i < 3; ++i) {
    if (force || uiCache.wpos[i] != state.wpos[i]) {
      const String label = i == 0 ? "X" : (i == 1 ? "Y" : "Z");
      drawValueRow(76 + (i * 28), label.c_str(), formatAxis(state.wpos[i]));
      uiCache.wpos[i] = state.wpos[i];
    }
  }

  if (force || uiCache.feed != state.feed || uiCache.spindleSpeed != state.spindleSpeed) {
    gfx->fillRect(12, 146, 180, 18, kPanel);
    gfx->setTextSize(1);
    gfx->setTextColor(kMutedText, kPanel);
    gfx->setCursor(14, 150);
    gfx->printf("FEED %d mm/min   SPINDLE %d rpm", state.feed, state.spindleSpeed);
    uiCache.feed = state.feed;
    uiCache.spindleSpeed = state.spindleSpeed;
  }

  if (force || uiCache.lastMessage != state.lastMessage) {
    gfx->fillRect(0, 224, 320, 16, 0x0841);
    gfx->setTextSize(1);
    gfx->setTextColor(kMutedText, 0x0841);
    gfx->setCursor(8, 228);
    gfx->print(state.connected ? state.lastMessage : "Waiting for GRBL heartbeat on Serial2");
    uiCache.lastMessage = state.lastMessage;
  }

  if (force || uiCache.spindleEnabled != state.spindleEnabled || uiCache.coolantEnabled != state.coolantEnabled) {
    for (const Button &button : kButtons) {
      if (button.latch) {
        drawButton(button);
      }
    }
    uiCache.spindleEnabled = state.spindleEnabled;
    uiCache.coolantEnabled = state.coolantEnabled;
  }
}

bool readTouch(int16_t &x, int16_t &y) {
  if (!touch.touched()) {
    return false;
  }

  const TS_Point p = touch.getPoint();
  int32_t rawX = p.x;
  int32_t rawY = p.y;

  if (kTouchSwapAxes) {
    const int32_t temp = rawX;
    rawX = rawY;
    rawY = temp;
  }

  x = map(rawX, kTouchRawMinX, kTouchRawMaxX, 0, gfx->width());
  y = map(rawY, kTouchRawMinY, kTouchRawMaxY, 0, gfx->height());

  if (kTouchInvertX) {
    x = gfx->width() - x;
  }
  if (kTouchInvertY) {
    y = gfx->height() - y;
  }

  x = constrain(x, 0, gfx->width() - 1);
  y = constrain(y, 0, gfx->height() - 1);
  return true;
}

bool hitButton(const Button &button, int16_t x, int16_t y) {
  return x >= button.x && x < button.x + button.w && y >= button.y && y < button.y + button.h;
}

void performAction(Action action) {
  switch (action) {
    case Action::Unlock:
      sendLine("$X");
      break;
    case Action::Home:
      sendLine("$H");
      break;
    case Action::Reset:
      sendRealtime(0x18);
      state.lastMessage = "Soft reset";
      break;
    case Action::Hold:
      sendRealtime('!');
      state.lastMessage = "Feed hold";
      break;
    case Action::Resume:
      sendRealtime('~');
      state.lastMessage = "Cycle start";
      break;
    case Action::ZeroX:
      sendLine("G10 L20 P1 X0");
      break;
    case Action::ZeroY:
      sendLine("G10 L20 P1 Y0");
      break;
    case Action::ZeroZ:
      sendLine("G10 L20 P1 Z0");
      break;
    case Action::Step001:
      jogStepMm = 0.01F;
      break;
    case Action::Step010:
      jogStepMm = 0.1F;
      break;
    case Action::Step100:
      jogStepMm = 1.0F;
      break;
    case Action::XMinus:
      sendJog('X', -jogStepMm);
      break;
    case Action::XPlus:
      sendJog('X', jogStepMm);
      break;
    case Action::YMinus:
      sendJog('Y', -jogStepMm);
      break;
    case Action::YPlus:
      sendJog('Y', jogStepMm);
      break;
    case Action::ZMinus:
      sendJog('Z', -jogStepMm);
      break;
    case Action::ZPlus:
      sendJog('Z', jogStepMm);
      break;
    case Action::SpindleToggle:
      if (state.spindleEnabled) {
        sendLine("M5");
        state.spindleEnabled = false;
        state.spindleSpeed = 0;
      } else {
        sendLine("M3 S10000");
        state.spindleEnabled = true;
        state.spindleSpeed = 10000;
      }
      break;
    case Action::CoolantToggle:
      if (state.coolantEnabled) {
        sendLine("M9");
      } else {
        sendLine("M8");
      }
      state.coolantEnabled = !state.coolantEnabled;
      break;
    case Action::None:
      break;
  }

  for (const Button &button : kButtons) {
    if (button.action == action || button.action == Action::SpindleToggle || button.action == Action::CoolantToggle ||
        button.action == Action::Step001 || button.action == Action::Step010 || button.action == Action::Step100) {
      drawButton(button);
    }
  }
  drawDynamicUi(true);
}

void handleTouch() {
  int16_t x = 0;
  int16_t y = 0;
  if (!readTouch(x, y)) {
    return;
  }
  if (millis() - lastTouch < kTouchDebounceMs) {
    return;
  }
  lastTouch = millis();

  for (const Button &button : kButtons) {
    if (hitButton(button, x, y)) {
      performAction(button.action);
      break;
    }
  }
}

void configureBacklight() {
  ledcSetup(0, 12000, 8);
  ledcAttachPin(kBacklightPin, 0);
  ledcWrite(0, 220);
}

void initialiseGrbl() {
  grblSerial.begin(kGrblBaud, SERIAL_8N1, kGrblRxPin, kGrblTxPin);
  delay(50);
  grblSerial.print("\r\n\r\n");
  state.lastMessage = "Serial2 ready";
}

}  // namespace

void setup() {
  Serial.begin(115200);
  configureBacklight();

  hspi.begin(kTftSclk, kTftMiso, kTftMosi, -1);
  touch.begin(hspi);
  touch.setRotation(kRotation);

  gfx->begin();
  gfx->fillScreen(BLACK);
  drawStaticUi();
  drawDynamicUi(true);

  initialiseGrbl();
  drawDynamicUi(true);
}

void loop() {
  pollGrbl();
  handleTouch();

  if (millis() - lastStatusPoll >= kStatusPollMs) {
    sendRealtime('?');
    lastStatusPoll = millis();
  }

  drawDynamicUi();
}
