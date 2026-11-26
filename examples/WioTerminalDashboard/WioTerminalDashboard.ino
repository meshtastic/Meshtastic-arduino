/*
    Meshtastic Wio Terminal dashboard

    Shows a Meshtastic radio's telemetry on a Seeed Wio Terminal's screen, on three
    pages:
      Radio        the connected radio's battery, voltage, channel use, airtime,
                   online node count and packet counts
      Nodes        every node with its battery, channel use, airtime and when it
                   was last heard, newest first
      Environment  temperature, humidity and pressure from nodes with sensors

    The 5-way switch changes page (left/right) and scrolls (up/down). The three top
    buttons jump to a page: left is Radio, middle is Nodes, right is Environment.

    Wiring, from the Wio's 40-pin header to the radio:
      pin 8  (TX)   to the radio's serial module RX pin
      pin 10 (RX)   to the radio's serial module TX pin
      pin 6  (GND)  to GND
    Both sides are 3.3 V. Power each board from its own USB.
    The header has no pin 1 mark. With the Wio's back facing you and the top
    buttons up, pins 1 and 2 are at the right-hand end and the even pins are on
    the row nearer the top edge, so pins 6, 8 and 10 are the 3rd, 4th and 5th
    from the right on that row.

    On the radio, turn on the serial module in PROTO mode at 19200 baud on the pins
    you wired, for example on a RAK4631 on a RAK19007 base board:
      meshtastic --set serial.enabled true --set serial.mode PROTO \
        --set serial.baud BAUD_19200 --set serial.rxd 15 --set serial.txd 16
    Pin numbers for other radios, and wiring diagrams, are on the serial module page:
    https://meshtastic.org/docs/configuration/module/serial/

    Needs the Seeed SAMD boards package (board: Seeeduino Wio Terminal), which
    includes the screen library. On any other board this sketch builds a stub that
    prints a note on the USB serial port.

    Created November 2026
    By Garth Vander Houwen
*/

#if defined(WIO_TERMINAL)

#include <TFT_eSPI.h>
#include <Meshtastic.h>
#include <meshtastic/telemetry.pb.h>
#include "powered_logo.h"

// ---- Settings ------------------------------------------------------------------

// Must match the radio's serial.baud setting.
static constexpr uint32_t kRadioBaud = 19200;
static constexpr bool kFahrenheit = true;
// Ask the radio for its full node list this often; telemetry packets update it in between.
static constexpr uint32_t kNodeReportIntervalMs = 10UL * 60 * 1000;
// Until the first node list arrives, ask again this often.
static constexpr uint32_t kNodeReportRetryMs = 30UL * 1000;
static constexpr size_t kMaxNodes = 120;

// ---- Data ----------------------------------------------------------------------

struct NodeRow {
  uint32_t num;
  char shortName[8];
  char longName[40];
  uint32_t lastHeard;   // epoch seconds, 0 = unknown
  int battery;          // percent, >100 = external power, -1 = unknown
  float voltage;        // NAN = unknown
  float chUtil;
  float airUtil;
  bool hasEnv;
  float temperature;    // Celsius, NAN = not reported
  float humidity;
  float pressure;       // hPa
  uint32_t envTime;     // epoch seconds of the last environment reading
};

struct LocalStats {
  bool valid = false;
  float chUtil = NAN;
  float airUtil = NAN;
  uint32_t packetsTx = 0;
  uint32_t packetsRx = 0;
  uint32_t packetsRxBad = 0;
  uint16_t online = 0;
  uint16_t total = 0;
  uint32_t uptime = 0;
  int32_t noiseFloor = 0;
};

static NodeRow nodes[kMaxNodes];
static size_t nodeCount = 0;
static LocalStats localStats;

// The Wio has no clock. Track the radio's time from the epoch values it sends and
// count forward with millis() in between.
static uint32_t clockEpoch = 0;
static uint32_t clockMillis = 0;

static bool syncing = false;
static bool haveNodeReport = false;
static uint32_t lastReportRequest = 0;
static uint32_t lastReportDone = 0;
static uint32_t lastRadioActivity = 0;  // millis() of the last frame handled, 0 = never
static bool dirty = true;

static uint32_t nowEpoch() {
  if (clockEpoch == 0) return 0;
  return clockEpoch + (millis() - clockMillis) / 1000;
}

// Only ever moves forward, so one stale timestamp can't drag the clock back.
static void noteTime(uint32_t epoch) {
  if (epoch < 1700000000UL) return;   // unset or nonsense
  if (epoch > nowEpoch()) {
    clockEpoch = epoch;
    clockMillis = millis();
  }
}

static void defaultNames(NodeRow &row) {
  snprintf(row.shortName, sizeof(row.shortName), "%04lx", (unsigned long)(row.num & 0xFFFF));
  snprintf(row.longName, sizeof(row.longName), "Meshtastic %04lx", (unsigned long)(row.num & 0xFFFF));
}

static NodeRow *findNode(uint32_t num) {
  for (size_t i = 0; i < nodeCount; i++) {
    if (nodes[i].num == num) return &nodes[i];
  }
  return nullptr;
}

static NodeRow *upsertNode(uint32_t num) {
  if (NodeRow *row = findNode(num)) return row;
  size_t slot = nodeCount;
  if (nodeCount < kMaxNodes) {
    nodeCount++;
  } else {
    // Full: reuse the node heard longest ago (never the connected radio).
    slot = 0;
    for (size_t i = 1; i < nodeCount; i++) {
      if (nodes[i].num == my_node_num) continue;
      if (nodes[slot].num == my_node_num || nodes[i].lastHeard < nodes[slot].lastHeard) slot = i;
    }
  }
  NodeRow &row = nodes[slot];
  row = NodeRow{};
  row.num = num;
  row.battery = -1;
  row.voltage = row.chUtil = row.airUtil = NAN;
  row.temperature = row.humidity = row.pressure = NAN;
  defaultNames(row);
  return &row;
}

// The library's name buffers aren't always NUL-terminated, so copy with a bound.
static void copyName(char *dest, size_t destSize, const char *src, size_t srcSize) {
  size_t len = strnlen(src, srcSize);
  if (len == 0) return;
  if (len >= destSize) len = destSize - 1;
  memcpy(dest, src, len);
  dest[len] = '\0';
}

static void touch(NodeRow *row) {
  uint32_t now = nowEpoch();
  if (now) row->lastHeard = now;
}

static void markActivity() {
  lastRadioActivity = millis();
  if (lastRadioActivity == 0) lastRadioActivity = 1;
  dirty = true;
}

// ---- Radio callbacks -------------------------------------------------------------

static void onNodeReport(mt_node_t *node, mt_nr_progress_t progress) {
  markActivity();
  if (node == nullptr) {
    syncing = false;
    if (progress == MT_NR_DONE) {
      haveNodeReport = true;
      lastReportDone = millis();
    }
    return;
  }
  NodeRow *row = upsertNode(node->node_num);
  if (node->has_user) {
    copyName(row->shortName, sizeof(row->shortName), node->short_name, sizeof(node->short_name));
    copyName(row->longName, sizeof(row->longName), node->long_name, sizeof(node->long_name));
  }
  if (node->last_heard_from) {
    row->lastHeard = node->last_heard_from;
    noteTime(node->last_heard_from);
  }
  // The library reports missing device metrics as a NAN voltage.
  if (!isnan(node->voltage)) {
    row->battery = node->battery_level;
    row->voltage = node->voltage;
    row->chUtil = node->channel_utilization;
    row->airUtil = node->air_util_tx;
  }
}

static void applyTelemetry(NodeRow *row, const meshtastic_Telemetry &telemetry) {
  switch (telemetry.which_variant) {
  case meshtastic_Telemetry_device_metrics_tag: {
    const meshtastic_DeviceMetrics &m = telemetry.variant.device_metrics;
    if (m.has_battery_level) row->battery = m.battery_level;
    if (m.has_voltage) row->voltage = m.voltage;
    if (m.has_channel_utilization) row->chUtil = m.channel_utilization;
    if (m.has_air_util_tx) row->airUtil = m.air_util_tx;
    break;
  }
  case meshtastic_Telemetry_environment_metrics_tag: {
    const meshtastic_EnvironmentMetrics &m = telemetry.variant.environment_metrics;
    if (!(m.has_temperature || m.has_relative_humidity || m.has_barometric_pressure)) break;
    row->hasEnv = true;
    row->temperature = m.has_temperature ? m.temperature : NAN;
    row->humidity = m.has_relative_humidity ? m.relative_humidity : NAN;
    row->pressure = m.has_barometric_pressure ? m.barometric_pressure : NAN;
    row->envTime = nowEpoch();
    break;
  }
  case meshtastic_Telemetry_local_stats_tag:
    if (row->num != my_node_num) break;
    {
      const meshtastic_LocalStats &s = telemetry.variant.local_stats;
      localStats.valid = true;
      localStats.chUtil = s.channel_utilization;
      localStats.airUtil = s.air_util_tx;
      localStats.packetsTx = s.num_packets_tx;
      localStats.packetsRx = s.num_packets_rx;
      localStats.packetsRxBad = s.num_packets_rx_bad;
      localStats.online = s.num_online_nodes;
      localStats.total = s.num_total_nodes;
      localStats.uptime = s.uptime_seconds;
      localStats.noiseFloor = s.noise_floor;
    }
    break;
  default:
    break;
  }
}

static void onPacket(uint32_t from, uint32_t, uint8_t, meshtastic_PortNum port, meshtastic_Data_payload_t *payload) {
  markActivity();
  NodeRow *row = upsertNode(from);
  if (port == meshtastic_PortNum_TELEMETRY_APP) {
    meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload->bytes, payload->size);
    if (pb_decode(&stream, meshtastic_Telemetry_fields, &telemetry)) {
      // Only trust the connected radio's clock, not every sender's.
      if (from == my_node_num) noteTime(telemetry.time);
      applyTelemetry(row, telemetry);
    }
  } else if (port == meshtastic_PortNum_NODEINFO_APP) {
    meshtastic_User user = meshtastic_User_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload->bytes, payload->size);
    if (pb_decode(&stream, meshtastic_User_fields, &user)) {
      copyName(row->shortName, sizeof(row->shortName), user.short_name, sizeof(user.short_name));
      copyName(row->longName, sizeof(row->longName), user.long_name, sizeof(user.long_name));
    }
  }
  touch(row);
}

static void onText(uint32_t from, uint32_t, uint8_t, const char *) {
  markActivity();
  touch(upsertNode(from));
}

// ---- Display -------------------------------------------------------------------

static TFT_eSPI tft;
static TFT_eSprite body(&tft);

static constexpr int kWidth = 320;
static constexpr int kHeaderHeight = 30;
static constexpr int kFooterHeight = 20;
static constexpr int kBodyHeight = 240 - kHeaderHeight - kFooterHeight;
static constexpr int kRowHeight = 22;
static constexpr int kVisibleRows = (kBodyHeight - kRowHeight) / kRowHeight;  // minus column header

static const uint16_t kGreen = tft.color565(0x67, 0xEA, 0x94);   // Meshtastic green
static const uint16_t kDim = tft.color565(0x8E, 0x8E, 0x93);
static const uint16_t kPanel = tft.color565(0x1C, 0x1C, 0x1E);
static const uint16_t kWarn = tft.color565(0xFF, 0xB0, 0x20);
static const uint16_t kBad = tft.color565(0xFF, 0x45, 0x3A);

enum Page { PageRadio, PageNodes, PageEnvironment, PageCount };
static Page page = PageRadio;
static int scroll = 0;

static uint16_t nodeColor(uint32_t num) {
  // Same color as the node's pin in the iOS app: the low 24 bits as RGB.
  return tft.color565((num >> 16) & 0xFF, (num >> 8) & 0xFF, num & 0xFF);
}

static String age(uint32_t epoch) {
  uint32_t now = nowEpoch();
  if (epoch == 0 || now == 0) return "-";
  if (epoch >= now) return "now";
  uint32_t s = now - epoch;
  if (s < 60) return "now";
  if (s < 3600) return String(s / 60) + "m";
  if (s < 86400) return String(s / 3600) + "h";
  return String(s / 86400) + "d";
}

static String fmt(float value, int decimals, const char *unit) {
  if (isnan(value)) return "-";
  return String(value, decimals) + unit;
}

static String batteryText(const NodeRow &row) {
  if (row.battery > 100) return "Pwr";
  if (row.battery < 0) return "-";
  return String(row.battery) + "%";
}

static uint16_t batteryColor(int battery) {
  if (battery < 0 || battery > 100) return TFT_WHITE;
  if (battery <= 15) return kBad;
  if (battery <= 35) return kWarn;
  return TFT_WHITE;
}

// Nodes newest first, connected radio on top.
static void sortedIndexes(size_t *out, size_t &count, bool envOnly) {
  count = 0;
  for (size_t i = 0; i < nodeCount; i++) {
    if (envOnly && !nodes[i].hasEnv) continue;
    out[count++] = i;
  }
  for (size_t i = 1; i < count; i++) {
    size_t key = out[i];
    size_t j = i;
    while (j > 0) {
      const NodeRow &a = nodes[out[j - 1]];
      const NodeRow &b = nodes[key];
      bool bFirst = (b.num == my_node_num && a.num != my_node_num) ||
        (a.num != my_node_num && b.lastHeard > a.lastHeard);
      if (!bFirst) break;
      out[j] = out[j - 1];
      j--;
    }
    out[j] = key;
  }
}

static void drawHeader() {
  tft.fillRect(0, 0, kWidth, kHeaderHeight, kPanel);
  tft.setTextFont(4);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(kGreen, kPanel);
  const char *titles[] = {"Radio", "Nodes", "Environment"};
  tft.drawString(titles[page], 8, kHeaderHeight / 2 + 1);

  // Status on the right: a dot plus a word.
  uint16_t dot;
  String status;
  if (lastRadioActivity == 0) {
    dot = kBad;
    status = "No radio";
  } else if (syncing) {
    dot = kWarn;
    status = "Syncing " + String(nodeCount);
  } else if (millis() - lastRadioActivity > 20UL * 60 * 1000) {
    dot = kWarn;
    status = "Quiet";
  } else {
    dot = kGreen;
    NodeRow *mine = findNode(my_node_num);
    status = mine ? String(mine->shortName) : "Live";
  }
  tft.setTextFont(2);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_WHITE, kPanel);
  tft.drawString(status, kWidth - 8, kHeaderHeight / 2);
  int textWidth = tft.textWidth(status);
  tft.fillCircle(kWidth - 8 - textWidth - 10, kHeaderHeight / 2, 5, dot);
}

// The arrays are stored byte-swapped to suit the screen (see tools/make_logo_header.py).
static void drawBitmap(int x, int y, int w, int h, const uint16_t *pixels) {
  tft.pushImage(x, y, w, h, pixels);
}

static void drawSplash() {
  tft.fillScreen(TFT_BLACK);
  drawBitmap((kWidth - kPoweredSquareWidth) / 2, (240 - kPoweredSquareHeight) / 2,
    kPoweredSquareWidth, kPoweredSquareHeight, kPoweredSquare);
}

static void drawFooter() {
  int y = 240 - kFooterHeight;
  tft.fillRect(0, y, kWidth, kFooterHeight, TFT_BLACK);
  drawBitmap(6, y + (kFooterHeight - kPoweredBadgeHeight) / 2, kPoweredBadgeWidth, kPoweredBadgeHeight, kPoweredBadge);
  for (int i = 0; i < PageCount; i++) {
    int x = kWidth / 2 + (i - 1) * 16;
    if (i == page) tft.fillCircle(x, y + kFooterHeight / 2, 4, kGreen);
    else tft.drawCircle(x, y + kFooterHeight / 2, 4, kDim);
  }
}

static void drawTile(int x, int y, int w, int h, const char *label, const String &value, uint16_t color) {
  body.fillRoundRect(x, y, w, h, 8, kPanel);
  body.setTextDatum(TL_DATUM);
  body.setTextFont(2);
  body.setTextColor(kDim, kPanel);
  body.drawString(label, x + 10, y + 6);
  body.setTextFont(4);
  body.setTextColor(color, kPanel);
  body.drawString(value, x + 10, y + h - 30);
}

static String uptimeText(uint32_t seconds) {
  if (seconds == 0) return "-";
  if (seconds < 3600) return String(seconds / 60) + "m";
  if (seconds < 86400) return String(seconds / 3600) + "h " + String((seconds % 3600) / 60) + "m";
  return String(seconds / 86400) + "d " + String((seconds % 86400) / 3600) + "h";
}

static void drawRadioPage() {
  NodeRow *mine = findNode(my_node_num);
  if (mine == nullptr) {
    body.setTextDatum(MC_DATUM);
    body.setTextFont(2);
    body.setTextColor(kDim, TFT_BLACK);
    body.drawString(lastRadioActivity ? "Waiting for the radio's node list" : "Waiting for the radio", kWidth / 2, kBodyHeight / 2 - 10);
    body.drawString(String(kRadioBaud) + " baud, serial module in PROTO mode", kWidth / 2, kBodyHeight / 2 + 10);
    return;
  }
  float chUtil = localStats.valid ? localStats.chUtil : mine->chUtil;
  float airUtil = localStats.valid ? localStats.airUtil : mine->airUtil;

  // Three rows of tiles plus the status line below them have to fit in kBodyHeight (190).
  const int gap = 6, w = (kWidth - gap * 3) / 2, h = 52;
  int x1 = gap, x2 = gap * 2 + w;
  int y = 4;
  drawTile(x1, y, w, h, "Battery", batteryText(*mine), batteryColor(mine->battery));
  drawTile(x2, y, w, h, "Voltage", fmt(mine->voltage, 2, " V"), TFT_WHITE);
  y += h + gap;
  drawTile(x1, y, w, h, "Channel use", fmt(chUtil, 1, "%"), chUtil > 25 ? kWarn : TFT_WHITE);
  drawTile(x2, y, w, h, "Airtime TX", fmt(airUtil, 1, "%"), airUtil > 10 ? kWarn : TFT_WHITE);
  y += h + gap;
  String online = localStats.valid ? String(localStats.online) + " / " + String(localStats.total) : String(nodeCount);
  String packets = localStats.valid ? String(localStats.packetsRx) + " / " + String(localStats.packetsTx) : "-";
  drawTile(x1, y, w, h, localStats.valid ? "Nodes online" : "Nodes", online, TFT_WHITE);
  drawTile(x2, y, w, h, "Packets RX / TX", packets, TFT_WHITE);

  body.setTextFont(2);
  body.setTextDatum(TL_DATUM);
  body.setTextColor(kDim, TFT_BLACK);
  String line = String(mine->longName);
  if (localStats.valid) {
    line += "  up " + uptimeText(localStats.uptime);
    if (localStats.noiseFloor != 0) line += "  noise " + String(localStats.noiseFloor) + " dBm";
  }
  body.drawString(line, gap, y + h + 2);
}

static void drawColumnHeader(const char *const *labels, const int *xs, int count) {
  body.setTextFont(2);
  body.setTextColor(kDim, TFT_BLACK);
  for (int i = 0; i < count; i++) {
    body.setTextDatum(i == 0 ? TL_DATUM : TR_DATUM);
    body.drawString(labels[i], xs[i], 3);
  }
  body.drawFastHLine(0, kRowHeight - 1, kWidth, kPanel);
}

static void drawNameCell(const NodeRow &row, int y) {
  body.fillCircle(14, y + kRowHeight / 2, 7, nodeColor(row.num));
  body.drawCircle(14, y + kRowHeight / 2, 7, TFT_WHITE);
  body.setTextDatum(TL_DATUM);
  body.setTextColor(row.num == my_node_num ? kGreen : TFT_WHITE, TFT_BLACK);
  body.drawString(row.shortName, 28, y + 3);
}

static void drawEmpty(const char *line1, const char *line2) {
  body.setTextDatum(MC_DATUM);
  body.setTextFont(2);
  body.setTextColor(kDim, TFT_BLACK);
  body.drawString(line1, kWidth / 2, kBodyHeight / 2 - 10);
  body.drawString(line2, kWidth / 2, kBodyHeight / 2 + 10);
}

static void clampScroll(size_t count) {
  int maxScroll = max(0, (int)count - kVisibleRows);
  scroll = constrain(scroll, 0, maxScroll);
}

static void drawNodesPage() {
  static size_t order[kMaxNodes];
  size_t count;
  sortedIndexes(order, count, false);
  if (count == 0) {
    drawEmpty("No nodes yet", "They appear once the radio sends its node list");
    return;
  }
  clampScroll(count);
  const char *labels[] = {"Node", "Battery", "Ch use", "Air TX", "Heard"};
  const int xs[] = {8, 140, 200, 256, 312};
  drawColumnHeader(labels, xs, 5);
  for (int i = 0; i < kVisibleRows && scroll + i < (int)count; i++) {
    const NodeRow &row = nodes[order[scroll + i]];
    int y = kRowHeight * (i + 1);
    drawNameCell(row, y);
    body.setTextDatum(TR_DATUM);
    body.setTextColor(batteryColor(row.battery), TFT_BLACK);
    body.drawString(batteryText(row), xs[1], y + 3);
    body.setTextColor(TFT_WHITE, TFT_BLACK);
    body.drawString(fmt(row.chUtil, 1, "%"), xs[2], y + 3);
    body.drawString(fmt(row.airUtil, 1, "%"), xs[3], y + 3);
    body.setTextColor(kDim, TFT_BLACK);
    body.drawString(age(row.lastHeard), xs[4], y + 3);
  }
}

static void drawEnvironmentPage() {
  static size_t order[kMaxNodes];
  size_t count;
  sortedIndexes(order, count, true);
  if (count == 0) {
    drawEmpty("No environment readings yet", "Nodes with sensors send them every 30 min");
    return;
  }
  clampScroll(count);
  const char *labels[] = {"Node", "Temp", "Humidity", "Pressure", "Age"};
  const int xs[] = {8, 140, 210, 278, 312};
  drawColumnHeader(labels, xs, 5);
  for (int i = 0; i < kVisibleRows && scroll + i < (int)count; i++) {
    const NodeRow &row = nodes[order[scroll + i]];
    int y = kRowHeight * (i + 1);
    drawNameCell(row, y);
    float temp = kFahrenheit && !isnan(row.temperature) ? row.temperature * 9 / 5 + 32 : row.temperature;
    body.setTextDatum(TR_DATUM);
    body.setTextColor(TFT_WHITE, TFT_BLACK);
    body.drawString(fmt(temp, 1, kFahrenheit ? " F" : " C"), xs[1], y + 3);
    body.drawString(fmt(row.humidity, 0, "%"), xs[2], y + 3);
    body.drawString(fmt(row.pressure, 0, ""), xs[3], y + 3);
    body.setTextColor(kDim, TFT_BLACK);
    body.drawString(age(row.envTime), xs[4], y + 3);
  }
}

static void drawAll() {
  drawHeader();
  body.fillSprite(TFT_BLACK);
  switch (page) {
  case PageRadio: drawRadioPage(); break;
  case PageNodes: drawNodesPage(); break;
  case PageEnvironment: drawEnvironmentPage(); break;
  default: break;
  }
  body.pushSprite(0, kHeaderHeight);
  drawFooter();
}

// ---- Buttons -------------------------------------------------------------------

struct Button {
  uint8_t pin;
  bool wasDown;
  uint32_t changedAt;
};

static Button buttons[] = {
  {WIO_5S_LEFT, false, 0}, {WIO_5S_RIGHT, false, 0}, {WIO_5S_UP, false, 0}, {WIO_5S_DOWN, false, 0},
  {WIO_KEY_C, false, 0}, {WIO_KEY_B, false, 0}, {WIO_KEY_A, false, 0},
};

static void setPage(int newPage) {
  page = (Page)((newPage + PageCount) % PageCount);
  scroll = 0;
  dirty = true;
}

static void onPress(uint8_t pin) {
  switch (pin) {
  case WIO_5S_LEFT: setPage(page - 1); break;
  case WIO_5S_RIGHT: setPage(page + 1); break;
  case WIO_5S_UP: scroll = max(0, scroll - kVisibleRows / 2); dirty = true; break;
  case WIO_5S_DOWN: scroll += kVisibleRows / 2; dirty = true; break;  // clamped when drawn
  case WIO_KEY_C: setPage(PageRadio); break;
  case WIO_KEY_B: setPage(PageNodes); break;
  case WIO_KEY_A: setPage(PageEnvironment); break;
  }
}

static void pollButtons() {
  uint32_t now = millis();
  for (Button &b : buttons) {
    bool down = digitalRead(b.pin) == LOW;
    if (down != b.wasDown && now - b.changedAt > 30) {
      b.wasDown = down;
      b.changedAt = now;
      if (down) onPress(b.pin);
    }
  }
}

// ---- Arduino -------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  for (Button &b : buttons) pinMode(b.pin, INPUT_PULLUP);

  tft.begin();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  body.setColorDepth(8);
  body.createSprite(kWidth, kBodyHeight);
  drawSplash();
  delay(2500);
  tft.fillScreen(TFT_BLACK);

  // Pins are fixed to Serial1 on SAMD boards; the arguments are ignored.
  mt_serial_init(-1, -1, kRadioBaud);
  set_portnum_callback(onPacket);
  set_text_message_callback(onText);
  drawAll();
}

void loop() {
  uint32_t now = millis();
  mt_loop(now);
  pollButtons();
  // A report can finish inside mt_loop and set lastReportDone past the time read above.
  now = millis();

  uint32_t interval = haveNodeReport ? kNodeReportIntervalMs : kNodeReportRetryMs;
  uint32_t since = haveNodeReport ? lastReportDone : lastReportRequest;
  bool due = lastReportRequest == 0 ||
    (now - since > interval && now - lastReportRequest > kNodeReportRetryMs / 2);
  if (due) {
    lastReportRequest = now;
    if (mt_request_node_report(onNodeReport)) {
      syncing = true;
      dirty = true;
    }
  }

  // Redraw on change, at most twice a second (every 2 s while the node list is
  // streaming in, so drawing doesn't starve the serial buffer), and every 30 s so
  // the ages tick over.
  static uint32_t lastDraw = 0;
  uint32_t minGap = syncing ? 2000 : 500;
  if ((dirty && now - lastDraw > minGap) || now - lastDraw > 30000) {
    dirty = false;
    lastDraw = now;
    drawAll();
  }
}

#else

// Not a Wio Terminal: nothing to drive.
void setup() {
  Serial.begin(115200);
}

void loop() {
  Serial.println("The WioTerminalDashboard example needs a Seeed Wio Terminal.");
  delay(5000);
}

#endif
