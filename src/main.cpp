#if defined(PLATFORMIO) || defined(ESP32_UART_INCLUDED_FROM_INO)

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <driver/rmt.h>
#include <esp_err.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <ctype.h>
#include <string.h>

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#endif

#ifndef ESP32_TOOLS_STA_SSID
#define ESP32_TOOLS_STA_SSID "YOUR_WIFI_SSID"
#endif

#ifndef ESP32_TOOLS_STA_PASSWORD
#define ESP32_TOOLS_STA_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

static const char *STA_WIFI_SSID = ESP32_TOOLS_STA_SSID;
static const char *STA_WIFI_PASSWORD = ESP32_TOOLS_STA_PASSWORD;

static const uint16_t HTTP_PORT = 80;
static const uint16_t WS_PORT = 81;
static const uint8_t MAX_WS_CLIENTS = 4;
static const size_t WS_MAX_PAYLOAD = 2048;
static const size_t UART_RX_BATCH = 512;

static const int WS2812_STATUS_PIN = 16;
static const rmt_channel_t WS2812_RMT_CHANNEL = RMT_CHANNEL_0;
static const uint8_t STATUS_RED_LEVEL = 48;
static const uint8_t STATUS_GREEN_LEVEL = 32;
static const uint32_t STATUS_LED_BLINK_MS = 1000;

static const int DEFAULT_UART_RX_PIN = 25;
static const int DEFAULT_UART_TX_PIN = 26;
static const uint32_t DEFAULT_BAUD = 115200;
static const char *AP_SSID = "ESP32-Tools";
static const char *AP_PASSWORD = "12345678";

IPAddress STA_STATIC_IP(192, 168, 0, 201);
IPAddress STA_GATEWAY(192, 168, 0, 1);
IPAddress STA_SUBNET(255, 255, 255, 0);
IPAddress STA_DNS1(192, 168, 0, 1);
IPAddress STA_DNS2(8, 8, 8, 8);

WebServer httpServer(HTTP_PORT);
WiFiServer wsServer(WS_PORT);
HardwareSerial BridgeSerial(2);

struct UartSettings {
  uint32_t baud = DEFAULT_BAUD;
  uint8_t dataBits = 8;
  char parity = 'N';
  uint8_t stopBits = 1;
  int rxPin = DEFAULT_UART_RX_PIN;
  int txPin = DEFAULT_UART_TX_PIN;
};

enum WsReadState : uint8_t {
  WS_HEADER,
  WS_EXTENDED_LEN,
  WS_MASK,
  WS_PAYLOAD
};

struct WsClient {
  WiFiClient tcp;
  bool active = false;
  WsReadState state = WS_HEADER;
  uint8_t header[2] = {0, 0};
  uint8_t headerPos = 0;
  uint8_t ext[8] = {0};
  uint8_t extNeeded = 0;
  uint8_t extPos = 0;
  uint8_t mask[4] = {0};
  uint8_t maskPos = 0;
  uint8_t opcode = 0;
  bool masked = false;
  uint64_t payloadLen = 0;
  size_t payloadPos = 0;
  uint8_t payload[WS_MAX_PAYLOAD];
  uint32_t lastSeen = 0;
};

UartSettings uartSettings;
WsClient wsClients[MAX_WS_CLIENTS];

bool apMode = false;
bool staModeRequested = false;
bool wifiFault = false;
bool uartFault = false;
bool statusLedReady = false;
bool statusLedGreenOn = false;
String activeSsid;
String lastFaultText;
uint64_t uartRxBytes = 0;
uint64_t uartTxBytes = 0;
uint32_t lastStatsMs = 0;
uint32_t lastReconnectMs = 0;
uint32_t lastStatusBlinkMs = 0;
uint32_t statusLedColor = 0xFFFFFFFF;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 无线串口调试助手</title>
<style>
:root{
  --bg:#f5f7fb;
  --panel:#ffffff;
  --ink:#1f2937;
  --muted:#667085;
  --line:#d7dee8;
  --field:#f9fafb;
  --accent:#0f766e;
  --accent-weak:#e4f3f0;
  --blue:#2563eb;
  --warn:#d97706;
  --danger:#b42318;
  --ok:#039855;
  --shadow:0 10px 26px rgba(31,41,55,.08);
}
*{box-sizing:border-box}
html,body{height:100%}
body{
  margin:0;
  background:var(--bg);
  color:var(--ink);
  font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","Microsoft YaHei",Arial,sans-serif;
}
button,input,select,textarea{font:inherit}
button{
  border:1px solid var(--line);
  background:#fff;
  color:var(--ink);
  border-radius:8px;
  padding:9px 13px;
  cursor:pointer;
}
button:hover{border-color:#aab4c3}
button.primary{background:var(--accent);border-color:var(--accent);color:#fff}
button.primary:hover{background:#0b655e}
button.warn{border-color:#f3c98b;color:#9a5b00;background:#fff9ee}
button.danger{border-color:#f4b4aa;color:var(--danger);background:#fff7f5}
input,select,textarea{
  width:100%;
  border:1px solid var(--line);
  border-radius:8px;
  background:var(--field);
  color:var(--ink);
  padding:9px 10px;
  outline:none;
}
input:focus,select:focus,textarea:focus{border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-weak)}
label{display:grid;gap:6px;color:var(--muted);font-size:13px}
.app{max-width:1440px;margin:0 auto;padding:16px;display:grid;gap:12px}
.topbar{
  display:flex;
  justify-content:space-between;
  align-items:flex-start;
  gap:16px;
  padding:14px 16px;
  background:var(--panel);
  border:1px solid var(--line);
  border-radius:8px;
  box-shadow:var(--shadow);
}
h1,h2{margin:0;color:var(--ink);letter-spacing:0}
h1{font-size:22px;line-height:1.2}
h2{font-size:16px;line-height:1.3}
.sub{margin-top:6px;color:var(--muted);font-size:13px}
.chips{display:flex;flex-wrap:wrap;gap:8px;justify-content:flex-end}
.chip{
  display:inline-flex;
  align-items:center;
  gap:6px;
  min-height:30px;
  border:1px solid var(--line);
  border-radius:8px;
  background:#fff;
  padding:5px 9px;
  font-size:13px;
  color:var(--muted);
}
.dot{width:8px;height:8px;border-radius:50%;background:#98a2b3}
.dot.ok{background:var(--ok)}
.dot.warn{background:var(--warn)}
.dot.bad{background:var(--danger)}
.panel{
  background:var(--panel);
  border:1px solid var(--line);
  border-radius:8px;
  padding:14px;
  box-shadow:var(--shadow);
}
.panel-head{
  display:flex;
  justify-content:space-between;
  align-items:center;
  gap:12px;
  margin-bottom:12px;
}
.form-grid{
  display:grid;
  grid-template-columns:1.4fr repeat(5,1fr) auto;
  gap:10px;
  align-items:end;
}
.console-grid{
  display:grid;
  grid-template-columns:minmax(0,1.45fr) minmax(320px,.8fr);
  gap:12px;
}
.mode-row,.action-row,.inline-row{
  display:flex;
  flex-wrap:wrap;
  align-items:center;
  gap:8px;
}
.segmented{
  display:inline-flex;
  border:1px solid var(--line);
  border-radius:8px;
  overflow:hidden;
  background:#fff;
}
.segmented button{
  border:0;
  border-radius:0;
  padding:8px 12px;
  color:var(--muted);
}
.segmented button.active{background:var(--accent);color:#fff}
.console{
  height:48vh;
  min-height:340px;
  max-height:680px;
  overflow:auto;
  background:#101828;
  color:#d1fadf;
  border-radius:8px;
  border:1px solid #182230;
  padding:12px;
  white-space:pre-wrap;
  word-break:break-word;
  font:13px/1.55 Consolas,"SFMono-Regular",Menlo,monospace;
}
textarea{
  min-height:180px;
  resize:vertical;
  font:13px/1.5 Consolas,"SFMono-Regular",Menlo,monospace;
}
.check{
  width:auto;
  display:inline-flex;
  grid-template-columns:auto;
  align-items:center;
  gap:6px;
  color:var(--ink);
  font-size:13px;
}
.check input{width:auto}
.stats{
  display:grid;
  grid-template-columns:repeat(3,1fr);
  gap:8px;
  margin-top:10px;
}
.stat{
  border:1px solid var(--line);
  border-radius:8px;
  padding:9px;
  background:#fff;
}
.stat b{display:block;font-size:16px;color:var(--ink)}
.stat span{color:var(--muted);font-size:12px}
.timer{
  display:grid;
  grid-template-columns:auto minmax(90px,1fr) auto;
  gap:8px;
  align-items:center;
}
h3{margin:0;font-size:14px;line-height:1.3;color:var(--ink);letter-spacing:0}
.break-row{
  display:grid;
  grid-template-columns:minmax(120px,1fr) auto;
  gap:8px;
  align-items:end;
  margin-top:12px;
  padding-top:12px;
  border-top:1px solid var(--line);
}
.multi-panel{
  overflow:hidden;
}
.multi-head{
  display:flex;
  justify-content:space-between;
  align-items:center;
  gap:8px;
  margin-bottom:10px;
}
.multi-actions{
  display:flex;
  flex-wrap:wrap;
  gap:8px;
  margin-bottom:10px;
}
.string-scroll{
  width:100%;
  max-width:100%;
  max-height:min(34vh,360px);
  min-height:190px;
  overflow:auto;
  border:1px solid var(--line);
  border-radius:8px;
  background:#fff;
}
.string-list{
  display:grid;
  gap:8px;
  min-width:1580px;
  padding:8px;
}
.string-row{
  display:grid;
  grid-template-columns:34px minmax(180px,.75fr) 110px minmax(900px,2fr) 120px 80px 80px;
  gap:8px;
  align-items:center;
  border:1px solid var(--line);
  border-radius:8px;
  background:#fdfefe;
  padding:8px;
}
.string-row input,.string-row select{padding:7px 8px;font-size:12px}
.string-row button{padding:7px 9px;font-size:12px}
.string-row .row-enable{width:auto;justify-self:center}
.string-row .row-value{font-family:Consolas,"SFMono-Regular",Menlo,monospace}
.row-empty{
  border:1px dashed var(--line);
  border-radius:8px;
  padding:12px;
  color:var(--muted);
  text-align:center;
  background:#fff;
  font-size:13px;
}
.hidden-file{display:none}
.small{font-size:12px;color:var(--muted)}
@media (max-width:900px){
  .topbar{display:grid}
  .chips{justify-content:flex-start}
  .form-grid{grid-template-columns:repeat(2,minmax(0,1fr))}
  .form-grid button{grid-column:1/-1}
  .console-grid{grid-template-columns:1fr}
  .console{height:42vh}
  .string-scroll{max-height:360px}
  .string-list{min-width:1040px}
}
@media (max-width:520px){
  .app{padding:10px}
  .form-grid{grid-template-columns:1fr}
  .stats{grid-template-columns:1fr}
  .break-row{grid-template-columns:1fr}
  .multi-head{align-items:flex-start;display:grid}
  .string-list{min-width:980px}
}
</style>
</head>
<body>
<main class="app">
  <header class="topbar">
    <div>
      <h1>ESP32 无线串口调试助手</h1>
      <div class="sub" id="statusText">正在连接 WebSocket</div>
    </div>
    <div class="chips">
      <span class="chip"><span class="dot" id="connDot"></span><span id="connText">未连接</span></span>
      <span class="chip">IP <b id="ipText">--</b></span>
      <span class="chip">Wi-Fi <b id="wifiText">--</b></span>
      <span class="chip">MODE <b id="modeText">--</b></span>
      <span class="chip">FAULT <b id="faultText">none</b></span>
    </div>
  </header>

  <section class="panel">
    <div class="panel-head">
      <h2>串口参数</h2>
      <span class="small" id="serialText">115200 8N1</span>
    </div>
    <div class="form-grid">
      <label>波特率
        <input id="baud" list="baudList" type="number" min="300" max="2000000" value="115200">
      </label>
      <label>数据位
        <select id="dataBits">
          <option>8</option><option>7</option><option>6</option><option>5</option>
        </select>
      </label>
      <label>校验
        <select id="parity">
          <option value="N">None</option>
          <option value="E">Even</option>
          <option value="O">Odd</option>
        </select>
      </label>
      <label>停止位
        <select id="stopBits">
          <option>1</option><option>2</option>
        </select>
      </label>
      <label>RX GPIO
        <input id="rxPin" type="number" min="0" max="39" value="25">
      </label>
      <label>TX GPIO
        <input id="txPin" type="number" min="0" max="33" value="26">
      </label>
      <button class="primary" id="applyBtn">应用</button>
      <datalist id="baudList">
        <option value="9600"><option value="19200"><option value="38400">
        <option value="57600"><option value="115200"><option value="230400">
        <option value="460800"><option value="921600"><option value="1000000">
      </datalist>
    </div>
  </section>

  <div class="console-grid">
    <section class="panel">
      <div class="panel-head">
        <h2>接收</h2>
        <div class="mode-row">
          <div class="segmented" id="rxModes">
            <button data-rx-mode="ascii" class="active">ASCII</button>
            <button data-rx-mode="hex">HEX</button>
          </div>
          <label class="check"><input type="checkbox" id="autoScroll" checked>自动滚动</label>
          <button class="danger" id="clearBtn">清空</button>
        </div>
      </div>
      <pre class="console" id="rxView"></pre>
      <div class="stats">
        <div class="stat"><b id="rxBytes">0</b><span>RX Bytes</span></div>
        <div class="stat"><b id="txBytes">0</b><span>TX Bytes</span></div>
        <div class="stat"><b id="clients">0</b><span>Clients</span></div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-head">
        <h2>发送</h2>
        <div class="segmented" id="txModes">
          <button data-tx-mode="ascii" class="active">ASCII</button>
          <button data-tx-mode="hex">HEX</button>
        </div>
      </div>
      <textarea id="txInput" spellcheck="false" placeholder="01 03 00 00 00 02 C4 0B"></textarea>
      <div class="action-row" style="margin-top:10px">
        <label class="check"><input type="checkbox" id="appendCR">CR</label>
        <label class="check"><input type="checkbox" id="appendLF">LF</label>
        <button class="primary" id="sendBtn">发送</button>
        <button class="warn" id="stopTimerBtn">停止定时</button>
      </div>
      <div class="timer" style="margin-top:10px">
        <label class="check"><input type="checkbox" id="timerEnable">定时</label>
        <input id="timerMs" type="number" min="20" value="1000">
        <span class="small">ms</span>
      </div>
      <div class="break-row">
        <label>BREAK 时间 ms
          <input id="breakMs" type="number" min="1" max="5000" value="100">
        </label>
        <button class="warn" id="breakBtn">发送 BREAK</button>
      </div>
      <div class="sub" id="sendStatus">就绪</div>
    </section>
  </div>

  <section class="panel multi-panel">
    <div class="multi-head">
      <h2>多字符串发送</h2>
      <div class="inline-row">
        <button id="multiSendBtn" class="primary">发送勾选</button>
        <button id="multiLoopBtn">循环发送</button>
        <button id="multiStopBtn" class="warn">停止</button>
      </div>
    </div>
    <div class="multi-actions">
      <button id="addRowBtn">新增</button>
      <button id="selectAllRowsBtn">全选</button>
      <button id="selectNoneRowsBtn">全不选</button>
      <button id="importRowsBtn">导入</button>
      <button id="exportRowsBtn">导出</button>
    </div>
    <div class="string-scroll">
      <div class="string-list" id="stringList"></div>
    </div>
    <input class="hidden-file" id="importRowsFile" type="file" accept=".json,application/json">
  </section>
</main>

<script>
(() => {
  const $ = id => document.getElementById(id);
  const rxView = $("rxView");
  const enc = new TextEncoder();
  const RX_LIMIT = 262144;
  let ws;
  let reconnectTimer;
  let timerId;
  let mockRxTimer;
  let sequenceTimer;
  let rxMode = "ascii";
  let txMode = "ascii";
  let rxStore = [];
  let stringRows = [];
  let nextRowId = 1;
  let sequenceRunning = false;
  let sequenceLoop = false;
  let shownTxBytes = 0;
  const rowsStorageKey = "esp32_uart_rows_v1";
  const mockMode = location.protocol === "file:" ||
    location.hostname === "localhost" ||
    location.hostname === "127.0.0.1" ||
    new URLSearchParams(location.search).has("mock");

  function setConn(state, text) {
    const dot = $("connDot");
    dot.className = "dot " + (state || "");
    $("connText").textContent = text;
    $("statusText").textContent = text;
  }

  function wsUrl() {
    const host = location.hostname || "192.168.4.1";
    return "ws://" + host + ":81/";
  }

  function buildStatusMessage() {
    return [
      "STATUS",
      "baud=" + Number($("baud").value || 115200),
      "data=" + $("dataBits").value,
      "parity=" + $("parity").value,
      "stop=" + $("stopBits").value,
      "rx=" + Number($("rxPin").value || 25),
      "tx=" + Number($("txPin").value || 26),
      "ip=" + (mockMode ? "local-preview" : location.hostname),
      "wifi=" + (mockMode ? "Mock" : "--"),
      "ssid=" + (mockMode ? "Local Preview" : "--"),
      "select=" + (mockMode ? "PREVIEW" : "--"),
      "fault=none",
      "clients=1"
    ].join("|");
  }

  function startMock() {
    clearTimeout(reconnectTimer);
    clearInterval(mockRxTimer);
    handleText(buildStatusMessage());
    setConn("ok", "本地预览模式");
    $("sendStatus").textContent = "本地模拟已启用";
    mockRxTimer = setInterval(() => {
      const text = "RX " + new Date().toLocaleTimeString() + "  55 AA 01 02 03 04\r\n";
      appendRx(enc.encode(text));
      handleText("STAT|rx=" + rxStore.length + "|tx=" + shownTxBytes + "|clients=1");
    }, 1600);
  }

  function connect() {
    if (mockMode) {
      startMock();
      return;
    }
    clearTimeout(reconnectTimer);
    setConn("warn", "连接中");
    ws = new WebSocket(wsUrl());
    ws.binaryType = "arraybuffer";
    ws.onopen = () => {
      setConn("ok", "已连接");
      sendCommand("GET");
    };
    ws.onclose = () => {
      setConn("bad", "已断开，准备重连");
      reconnectTimer = setTimeout(connect, 1200);
    };
    ws.onerror = () => setConn("bad", "连接错误");
    ws.onmessage = event => {
      if (typeof event.data === "string") {
        handleText(event.data);
      } else if (event.data instanceof Blob) {
        event.data.arrayBuffer().then(handleBinary);
      } else {
        handleBinary(event.data);
      }
    };
  }

  function parseFields(text) {
    const parts = text.split("|");
    const type = parts.shift();
    const data = {};
    for (const part of parts) {
      const idx = part.indexOf("=");
      if (idx > -1) data[part.slice(0, idx)] = part.slice(idx + 1);
    }
    return { type, data };
  }

  function handleText(text) {
    const { type, data } = parseFields(text);
    if (type === "STATUS") {
      $("baud").value = data.baud || $("baud").value;
      $("dataBits").value = data.data || $("dataBits").value;
      $("parity").value = data.parity || $("parity").value;
      $("stopBits").value = data.stop || $("stopBits").value;
      $("rxPin").value = data.rx || $("rxPin").value;
      $("txPin").value = data.tx || $("txPin").value;
      $("ipText").textContent = data.ip || location.hostname;
      $("wifiText").textContent = data.wifi || "--";
      $("modeText").textContent = data.select || "--";
      $("faultText").textContent = data.fault || "none";
      $("clients").textContent = data.clients || "1";
      $("serialText").textContent = `${$("baud").value} ${$("dataBits").value}${$("parity").value}${$("stopBits").value}`;
      setConn("ok", "已连接");
    } else if (type === "STAT") {
      shownTxBytes = Number(data.tx || 0);
      $("rxBytes").textContent = data.rx || "0";
      $("txBytes").textContent = String(shownTxBytes);
      $("clients").textContent = data.clients || "1";
    } else if (type === "ERROR") {
      $("sendStatus").textContent = data.message || "错误";
      setConn("bad", data.message || "错误");
    } else if (type === "INFO") {
      $("sendStatus").textContent = data.message || "就绪";
    }
  }

  function sendCommand(command) {
    if (mockMode) {
      if (command.startsWith("CFG")) {
        handleText(buildStatusMessage());
        setConn("ok", "本地预览模式");
        $("sendStatus").textContent = "参数已应用到本地预览";
      } else if (command.startsWith("BREAK")) {
        const ms = Number((command.match(/ms=(\d+)/) || [0, 100])[1]);
        appendRx(enc.encode("BREAK " + ms + " ms\r\n"));
        $("sendStatus").textContent = "已模拟 BREAK " + ms + " ms";
      }
      return;
    }
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(command);
  }

  function hexByte(value) {
    return value.toString(16).padStart(2, "0").toUpperCase();
  }

  function bytesToAscii(bytes) {
    const out = [];
    for (let i = 0; i < bytes.length; i++) {
      const b = bytes[i];
      if (b === 10) out.push("\n");
      else if (b === 13) out.push("\r");
      else if (b === 9) out.push("\t");
      else if (b >= 32 && b <= 126) out.push(String.fromCharCode(b));
      else out.push("\\x" + hexByte(b));
    }
    return out.join("");
  }

  function bytesToHex(bytes) {
    const out = [];
    for (let i = 0; i < bytes.length; i++) out.push(hexByte(bytes[i]));
    return out.join(" ") + (out.length ? " " : "");
  }

  function formatBytes(bytes) {
    return rxMode === "hex" ? bytesToHex(bytes) : bytesToAscii(bytes);
  }

  function renderRx() {
    rxView.textContent = formatBytes(rxStore);
    if ($("autoScroll").checked) rxView.scrollTop = rxView.scrollHeight;
  }

  function appendRx(bytes) {
    const incoming = Array.from(bytes);
    let trimmed = false;
    if (incoming.length >= RX_LIMIT) {
      rxStore = incoming.slice(incoming.length - RX_LIMIT);
      trimmed = true;
    } else {
      const overflow = rxStore.length + incoming.length - RX_LIMIT;
      if (overflow > 0) {
        rxStore.splice(0, overflow);
        trimmed = true;
      }
      rxStore.push(...incoming);
    }
    if (trimmed) renderRx();
    else {
      rxView.textContent += formatBytes(incoming);
      if ($("autoScroll").checked) rxView.scrollTop = rxView.scrollHeight;
    }
  }

  function handleBinary(buffer) {
    const bytes = new Uint8Array(buffer);
    appendRx(bytes);
  }

  function parseHexInput(text) {
    const cleaned = text.replace(/0x/gi, "").replace(/[^0-9a-fA-F]/g, "");
    if (!cleaned.length) return new Uint8Array(0);
    if (cleaned.length % 2) throw new Error("HEX 字节不完整");
    const bytes = new Uint8Array(cleaned.length / 2);
    for (let i = 0; i < bytes.length; i++) {
      bytes[i] = parseInt(cleaned.slice(i * 2, i * 2 + 2), 16);
    }
    return bytes;
  }

  function parseAsciiEscapes(text) {
    const bytes = [];
    for (let i = 0; i < text.length; i++) {
      const c = text[i];
      if (c !== "\\") {
        const chunk = enc.encode(c);
        for (const b of chunk) bytes.push(b);
        continue;
      }
      const next = text[++i];
      if (next === "r") bytes.push(13);
      else if (next === "n") bytes.push(10);
      else if (next === "t") bytes.push(9);
      else if (next === "0") bytes.push(0);
      else if (next === "\\") bytes.push(92);
      else if (next === "x") {
        const hex = text.slice(i + 1, i + 3);
        if (!/^[0-9a-fA-F]{2}$/.test(hex)) throw new Error("ASCII \\xNN 转义不完整");
        bytes.push(parseInt(hex, 16));
        i += 2;
      } else if (next !== undefined) {
        bytes.push(92);
        const chunk = enc.encode(next);
        for (const b of chunk) bytes.push(b);
      } else {
        bytes.push(92);
      }
    }
    return new Uint8Array(bytes);
  }

  function buildTxBytes() {
    if (txMode === "hex") return parseHexInput($("txInput").value);
    let text = $("txInput").value;
    if ($("appendCR").checked) text += "\r";
    if ($("appendLF").checked) text += "\n";
    return enc.encode(text);
  }

  function bytesPreview(bytes, mode) {
    return mode === "hex" ? bytesToHex(bytes) : bytesToAscii(bytes);
  }

  function sendBytes(bytes, label, displayMode) {
    if (!mockMode && (!ws || ws.readyState !== WebSocket.OPEN)) {
      $("sendStatus").textContent = "WebSocket 未连接";
      return false;
    }
    if (!bytes.length) return false;
    if (!mockMode) {
      for (let i = 0; i < bytes.length; i += 512) {
        ws.send(bytes.slice(i, i + 512));
      }
    }
    shownTxBytes += bytes.length;
    $("txBytes").textContent = String(shownTxBytes);
    $("sendStatus").textContent = label + " 已发送 " + bytes.length + " bytes";
    if (mockMode) {
      const echoLabel = (displayMode || "ascii") === "hex" ? "TX HEX" : "TX ASCII";
      const echo = echoLabel + "  " + bytesPreview(bytes, displayMode || "ascii") + "\r\n";
      appendRx(enc.encode(echo));
      handleText("STAT|rx=" + rxStore.length + "|tx=" + shownTxBytes + "|clients=1");
    }
    return true;
  }

  function sendPacket() {
    let bytes;
    try {
      bytes = buildTxBytes();
    } catch (err) {
      $("sendStatus").textContent = err.message;
      return false;
    }
    return sendBytes(bytes, txMode === "hex" ? "TX HEX" : "TX ASCII", txMode);
  }

  function syncTimer() {
    clearInterval(timerId);
    timerId = null;
    if (!$("timerEnable").checked) return;
    const ms = Math.max(20, Number($("timerMs").value || 1000));
    $("timerMs").value = ms;
    timerId = setInterval(sendPacket, ms);
    $("sendStatus").textContent = "定时发送 " + ms + " ms";
  }

  function clampInt(value, min, max, fallback) {
    const n = Math.round(Number(value));
    if (!Number.isFinite(n)) return fallback;
    return Math.min(max, Math.max(min, n));
  }

  function defaultStringRows() {
    return [
      { enabled: true, name: "AT", mode: "ascii", value: "AT\\r\\n", interval: 300 },
      { enabled: true, name: "版本", mode: "ascii", value: "AT+GMR\\r\\n", interval: 500 },
      { enabled: false, name: "Modbus 示例", mode: "hex", value: "01 03 00 00 00 02 C4 0B", interval: 1000 }
    ];
  }

  function normalizeRow(row, index) {
    return {
      id: Number(row.id) || nextRowId++,
      enabled: row.enabled !== false,
      name: String(row.name || ("条目 " + (index + 1))).slice(0, 32),
      mode: row.mode === "hex" ? "hex" : "ascii",
      value: String(row.value || ""),
      interval: clampInt(row.interval, 0, 600000, 1000)
    };
  }

  function loadStringRows() {
    try {
      const saved = localStorage.getItem(rowsStorageKey);
      const parsed = saved ? JSON.parse(saved) : null;
      const rows = Array.isArray(parsed?.rows) ? parsed.rows : defaultStringRows();
      stringRows = rows.map(normalizeRow);
    } catch (err) {
      stringRows = defaultStringRows().map(normalizeRow);
    }
    nextRowId = Math.max(1, ...stringRows.map(row => row.id)) + 1;
  }

  function saveStringRows() {
    const rows = stringRows.map(({ id, enabled, name, mode, value, interval }) => ({
      id, enabled, name, mode, value, interval
    }));
    try {
      localStorage.setItem(rowsStorageKey, JSON.stringify({ version: 1, rows }));
    } catch (err) {
      $("sendStatus").textContent = "保存条目失败";
    }
  }

  function rowById(id) {
    return stringRows.find(row => row.id === Number(id));
  }

  function setRowField(row, field, value) {
    if (!row) return;
    if (field === "enabled") row.enabled = Boolean(value);
    else if (field === "name") row.name = String(value).slice(0, 32);
    else if (field === "mode") row.mode = value === "hex" ? "hex" : "ascii";
    else if (field === "value") row.value = String(value);
    else if (field === "interval") row.interval = clampInt(value, 0, 600000, 1000);
    saveStringRows();
  }

  function inputFor(row, field, value, type) {
    const input = document.createElement("input");
    input.dataset.field = field;
    input.value = value;
    input.type = type || "text";
    if (field === "enabled") {
      input.checked = row.enabled;
      input.className = "row-enable";
      input.type = "checkbox";
    }
    if (field === "value") input.className = "row-value";
    if (field === "interval") {
      input.className = "row-interval";
      input.min = "0";
      input.max = "600000";
      input.step = "10";
    }
    return input;
  }

  function renderStringRows() {
    const list = $("stringList");
    list.textContent = "";
    if (!stringRows.length) {
      const empty = document.createElement("div");
      empty.className = "row-empty";
      empty.textContent = "暂无条目";
      list.appendChild(empty);
      return;
    }

    for (const row of stringRows) {
      const item = document.createElement("div");
      item.className = "string-row";
      item.dataset.rowId = row.id;

      const enabled = inputFor(row, "enabled", "", "checkbox");
      const name = inputFor(row, "name", row.name);
      const mode = document.createElement("select");
      mode.dataset.field = "mode";
      for (const optionValue of ["ascii", "hex"]) {
        const option = document.createElement("option");
        option.value = optionValue;
        option.textContent = optionValue.toUpperCase();
        option.selected = row.mode === optionValue;
        mode.appendChild(option);
      }
      const value = inputFor(row, "value", row.value);
      const interval = inputFor(row, "interval", row.interval, "number");
      const send = document.createElement("button");
      send.dataset.action = "send";
      send.textContent = "发送";
      const del = document.createElement("button");
      del.dataset.action = "delete";
      del.className = "danger";
      del.textContent = "删除";

      item.append(enabled, name, mode, value, interval, send, del);
      list.appendChild(item);
    }
  }

  function addStringRow() {
    stringRows.push(normalizeRow({
      id: nextRowId++,
      enabled: true,
      name: "新条目",
      mode: "ascii",
      value: "",
      interval: 1000
    }, stringRows.length));
    saveStringRows();
    renderStringRows();
  }

  function buildRowBytes(row) {
    if (row.mode === "hex") return parseHexInput(row.value);
    return parseAsciiEscapes(row.value);
  }

  function sendStringRow(row) {
    if (!row) return false;
    let bytes;
    try {
      bytes = buildRowBytes(row);
    } catch (err) {
      $("sendStatus").textContent = row.name + ": " + err.message;
      return false;
    }
    return sendBytes(bytes, "TX " + row.name, row.mode);
  }

  function checkedStringRows() {
    return stringRows.filter(row => row.enabled);
  }

  function updateSequenceButtons() {
    $("multiSendBtn").disabled = sequenceRunning;
    $("multiLoopBtn").disabled = sequenceRunning;
    $("multiStopBtn").disabled = !sequenceRunning;
  }

  function stopStringSequence(message) {
    clearTimeout(sequenceTimer);
    sequenceTimer = null;
    sequenceRunning = false;
    sequenceLoop = false;
    updateSequenceButtons();
    if (message) $("sendStatus").textContent = message;
  }

  function runSequenceStep(rows, index) {
    if (!sequenceRunning) return;
    if (!rows.length) {
      stopStringSequence("没有勾选条目");
      return;
    }
    if (index >= rows.length) {
      if (sequenceLoop) {
        sequenceTimer = setTimeout(() => runSequenceStep(rows, 0), 0);
      } else {
        stopStringSequence("勾选条目发送完成");
      }
      return;
    }

    const row = rows[index];
    if (!sendStringRow(row)) {
      stopStringSequence("发送已停止");
      return;
    }
    const delayMs = clampInt(row.interval, 0, 600000, 1000);
    sequenceTimer = setTimeout(() => runSequenceStep(rows, index + 1), delayMs);
  }

  function startStringSequence(loop) {
    const rows = checkedStringRows().map(row => ({ ...row }));
    if (!rows.length) {
      $("sendStatus").textContent = "没有勾选条目";
      return;
    }
    stopStringSequence();
    sequenceRunning = true;
    sequenceLoop = loop;
    updateSequenceButtons();
    $("sendStatus").textContent = loop ? "循环发送已启动" : "顺序发送已启动";
    runSequenceStep(rows, 0);
  }

  function exportStringRows() {
    const rows = stringRows.map(({ enabled, name, mode, value, interval }) => ({
      enabled, name, mode, value, interval
    }));
    const blob = new Blob([JSON.stringify({ version: 1, rows }, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = "esp32-uart-strings.json";
    document.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 800);
    $("sendStatus").textContent = "条目已导出";
  }

  function importStringRows(file) {
    if (!file) return;
    const reader = new FileReader();
    reader.onload = () => {
      try {
        const parsed = JSON.parse(String(reader.result || "{}"));
        const rows = Array.isArray(parsed) ? parsed : parsed.rows;
        if (!Array.isArray(rows)) throw new Error("文件格式不正确");
        stringRows = rows.map(normalizeRow);
        nextRowId = Math.max(1, ...stringRows.map(row => row.id)) + 1;
        saveStringRows();
        renderStringRows();
        $("sendStatus").textContent = "已导入 " + stringRows.length + " 条";
      } catch (err) {
        $("sendStatus").textContent = "导入失败: " + err.message;
      }
    };
    reader.readAsText(file);
  }

  function sendBreak() {
    const ms = clampInt($("breakMs").value, 1, 5000, 100);
    $("breakMs").value = ms;
    sendCommand("BREAK ms=" + ms);
  }

  $("applyBtn").addEventListener("click", () => {
    const cmd = [
      "CFG",
      "baud=" + Number($("baud").value || 115200),
      "data=" + $("dataBits").value,
      "parity=" + $("parity").value,
      "stop=" + $("stopBits").value,
      "rx=" + Number($("rxPin").value || 25),
      "tx=" + Number($("txPin").value || 26)
    ].join(" ");
    sendCommand(cmd);
  });

  $("sendBtn").addEventListener("click", sendPacket);
  $("clearBtn").addEventListener("click", () => {
    rxStore = [];
    rxView.textContent = "";
  });
  $("timerEnable").addEventListener("change", syncTimer);
  $("timerMs").addEventListener("change", syncTimer);
  $("stopTimerBtn").addEventListener("click", () => {
    $("timerEnable").checked = false;
    syncTimer();
  });
  $("breakBtn").addEventListener("click", sendBreak);
  $("addRowBtn").addEventListener("click", addStringRow);
  $("selectAllRowsBtn").addEventListener("click", () => {
    stringRows.forEach(row => row.enabled = true);
    saveStringRows();
    renderStringRows();
  });
  $("selectNoneRowsBtn").addEventListener("click", () => {
    stringRows.forEach(row => row.enabled = false);
    saveStringRows();
    renderStringRows();
  });
  $("multiSendBtn").addEventListener("click", () => startStringSequence(false));
  $("multiLoopBtn").addEventListener("click", () => startStringSequence(true));
  $("multiStopBtn").addEventListener("click", () => stopStringSequence("多字符串发送已停止"));
  $("exportRowsBtn").addEventListener("click", exportStringRows);
  $("importRowsBtn").addEventListener("click", () => $("importRowsFile").click());
  $("importRowsFile").addEventListener("change", event => {
    importStringRows(event.target.files && event.target.files[0]);
    event.target.value = "";
  });
  $("stringList").addEventListener("input", event => {
    const item = event.target.closest(".string-row");
    const row = item ? rowById(item.dataset.rowId) : null;
    if (!row || !event.target.dataset.field) return;
    setRowField(row, event.target.dataset.field, event.target.value);
  });
  $("stringList").addEventListener("change", event => {
    const item = event.target.closest(".string-row");
    const row = item ? rowById(item.dataset.rowId) : null;
    if (!row || !event.target.dataset.field) return;
    const value = event.target.type === "checkbox" ? event.target.checked : event.target.value;
    setRowField(row, event.target.dataset.field, value);
  });
  $("stringList").addEventListener("click", event => {
    const action = event.target.dataset.action;
    if (!action) return;
    const item = event.target.closest(".string-row");
    const row = item ? rowById(item.dataset.rowId) : null;
    if (action === "send") {
      sendStringRow(row);
    } else if (action === "delete" && row) {
      stringRows = stringRows.filter(item => item.id !== row.id);
      saveStringRows();
      renderStringRows();
    }
  });

  document.querySelectorAll("[data-rx-mode]").forEach(button => {
    button.addEventListener("click", () => {
      rxMode = button.dataset.rxMode;
      document.querySelectorAll("[data-rx-mode]").forEach(b => b.classList.toggle("active", b === button));
      renderRx();
    });
  });

  document.querySelectorAll("[data-tx-mode]").forEach(button => {
    button.addEventListener("click", () => {
      txMode = button.dataset.txMode;
      document.querySelectorAll("[data-tx-mode]").forEach(b => b.classList.toggle("active", b === button));
    });
  });

  loadStringRows();
  renderStringRows();
  updateSequenceButtons();
  connect();
})();
</script>
</body>
</html>
)HTML";

String sanitized(String value) {
  value.replace("|", "_");
  value.replace("=", "-");
  return value;
}

String u64ToString(uint64_t value) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)value);
  return String(buffer);
}

String ipString() {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

bool systemFaultActive() {
  return wifiFault || uartFault;
}

uint8_t activeClientCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < MAX_WS_CLIENTS; i++) {
    if (wsClients[i].active) count++;
  }
  return count;
}

String makeStatusMessage() {
  String msg;
  msg.reserve(180);
  msg += "STATUS|baud=";
  msg += uartSettings.baud;
  msg += "|data=";
  msg += uartSettings.dataBits;
  msg += "|parity=";
  msg += uartSettings.parity;
  msg += "|stop=";
  msg += uartSettings.stopBits;
  msg += "|rx=";
  msg += uartSettings.rxPin;
  msg += "|tx=";
  msg += uartSettings.txPin;
  msg += "|ip=";
  msg += ipString();
  msg += "|wifi=";
  msg += apMode ? "AP" : "STA";
  msg += "|ssid=";
  msg += sanitized(activeSsid);
  msg += "|select=";
  msg += staModeRequested ? "STA" : "AP";
  msg += "|fault=";
  msg += systemFaultActive() ? sanitized(lastFaultText.length() ? lastFaultText : "fault") : "none";
  msg += "|clients=";
  msg += activeClientCount();
  return msg;
}

String makeStatsMessage() {
  String msg;
  msg.reserve(80);
  msg += "STAT|rx=";
  msg += u64ToString(uartRxBytes);
  msg += "|tx=";
  msg += u64ToString(uartTxBytes);
  msg += "|clients=";
  msg += activeClientCount();
  return msg;
}

uint32_t serialConfigValue(uint8_t dataBits, char parity, uint8_t stopBits) {
  parity = (char)toupper((unsigned char)parity);
  if (dataBits == 5) {
    if (parity == 'E') return stopBits == 2 ? SERIAL_5E2 : SERIAL_5E1;
    if (parity == 'O') return stopBits == 2 ? SERIAL_5O2 : SERIAL_5O1;
    return stopBits == 2 ? SERIAL_5N2 : SERIAL_5N1;
  }
  if (dataBits == 6) {
    if (parity == 'E') return stopBits == 2 ? SERIAL_6E2 : SERIAL_6E1;
    if (parity == 'O') return stopBits == 2 ? SERIAL_6O2 : SERIAL_6O1;
    return stopBits == 2 ? SERIAL_6N2 : SERIAL_6N1;
  }
  if (dataBits == 7) {
    if (parity == 'E') return stopBits == 2 ? SERIAL_7E2 : SERIAL_7E1;
    if (parity == 'O') return stopBits == 2 ? SERIAL_7O2 : SERIAL_7O1;
    return stopBits == 2 ? SERIAL_7N2 : SERIAL_7N1;
  }
  if (parity == 'E') return stopBits == 2 ? SERIAL_8E2 : SERIAL_8E1;
  if (parity == 'O') return stopBits == 2 ? SERIAL_8O2 : SERIAL_8O1;
  return stopBits == 2 ? SERIAL_8N2 : SERIAL_8N1;
}

bool isFlashPin(int pin) {
  return pin >= 6 && pin <= 11;
}

bool reservedBoardPin(int pin) {
  return pin == WS2812_STATUS_PIN || pin == 34 || pin == 35;
}

bool validRxPin(int pin) {
  return pin >= 0 && pin <= 39 && !isFlashPin(pin) && !reservedBoardPin(pin);
}

bool validTxPin(int pin) {
  return pin >= 0 && pin <= 33 && !isFlashPin(pin) && !reservedBoardPin(pin);
}

bool outputCapablePin(int pin) {
  return pin >= 0 && pin <= 33 && !isFlashPin(pin) && !reservedBoardPin(pin);
}

void setWifiFault(const String &message) {
  wifiFault = true;
  lastFaultText = message;
}

void clearWifiFault() {
  wifiFault = false;
  if (!systemFaultActive()) lastFaultText = "";
}

void setUartFault(const String &message) {
  uartFault = true;
  lastFaultText = message;
}

void clearUartFault() {
  uartFault = false;
  if (!systemFaultActive()) lastFaultText = "";
}

void writeWs2812Rgb(uint8_t red, uint8_t green, uint8_t blue) {
  if (!statusLedReady) return;

  uint32_t nextColor = ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
  if (nextColor == statusLedColor) return;
  statusLedColor = nextColor;

  static const uint16_t t0h = 14;  // 0.35 us at 40 MHz RMT clock.
  static const uint16_t t0l = 32;  // 0.80 us.
  static const uint16_t t1h = 28;  // 0.70 us.
  static const uint16_t t1l = 24;  // 0.60 us.

  uint8_t grb[3] = {green, red, blue};
  rmt_item32_t items[24];
  uint8_t item = 0;
  for (uint8_t colorIndex = 0; colorIndex < 3; colorIndex++) {
    for (int8_t bit = 7; bit >= 0; bit--) {
      bool one = (grb[colorIndex] & (1 << bit)) != 0;
      items[item].level0 = 1;
      items[item].duration0 = one ? t1h : t0h;
      items[item].level1 = 0;
      items[item].duration1 = one ? t1l : t0l;
      item++;
    }
  }

  rmt_write_items(WS2812_RMT_CHANNEL, items, 24, true);
  rmt_wait_tx_done(WS2812_RMT_CHANNEL, pdMS_TO_TICKS(10));
}

void setupStatusLed() {
  rmt_config_t config = {};
  config.rmt_mode = RMT_MODE_TX;
  config.channel = WS2812_RMT_CHANNEL;
  config.gpio_num = (gpio_num_t)WS2812_STATUS_PIN;
  config.mem_block_num = 1;
  config.clk_div = 2;
  config.tx_config.loop_en = false;
  config.tx_config.carrier_en = false;
  config.tx_config.idle_output_en = true;
  config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
  config.tx_config.carrier_level = RMT_CARRIER_LEVEL_HIGH;
  config.tx_config.carrier_duty_percent = 33;

  esp_err_t result = rmt_config(&config);
  if (result == ESP_OK) result = rmt_driver_install(WS2812_RMT_CHANNEL, 0, 0);
  statusLedReady = result == ESP_OK;
  if (!statusLedReady) {
    Serial.print("WS2812B status LED init failed: ");
    Serial.println((int)result);
    return;
  }
  writeWs2812Rgb(0, 0, 0);
}

void serviceIndicators() {
  if (!statusLedReady) return;
  if (wifiFault) {
    statusLedGreenOn = false;
    writeWs2812Rgb(STATUS_RED_LEVEL, 0, 0);
    return;
  }

  if (millis() - lastStatusBlinkMs >= STATUS_LED_BLINK_MS) {
    lastStatusBlinkMs = millis();
    statusLedGreenOn = !statusLedGreenOn;
    writeWs2812Rgb(0, statusLedGreenOn ? STATUS_GREEN_LEVEL : 0, 0);
  }
}

bool validateSettings(const UartSettings &settings, String &error) {
  if (settings.baud < 300 || settings.baud > 2000000) {
    error = "baud out of range";
    return false;
  }
  if (settings.dataBits < 5 || settings.dataBits > 8) {
    error = "data bits out of range";
    return false;
  }
  if (settings.parity != 'N' && settings.parity != 'E' && settings.parity != 'O') {
    error = "parity must be N/E/O";
    return false;
  }
  if (settings.stopBits != 1 && settings.stopBits != 2) {
    error = "stop bits must be 1 or 2";
    return false;
  }
  if (!validRxPin(settings.rxPin)) {
    error = "invalid RX GPIO";
    return false;
  }
  if (!validTxPin(settings.txPin)) {
    error = "invalid TX GPIO";
    return false;
  }
  return true;
}

bool applyUartSettings(const UartSettings &settings, String &error) {
  if (!validateSettings(settings, error)) return false;
  uartSettings = settings;
  BridgeSerial.end();
  delay(20);
  BridgeSerial.setRxBufferSize(4096);
  BridgeSerial.begin(
      uartSettings.baud,
      serialConfigValue(uartSettings.dataBits, uartSettings.parity, uartSettings.stopBits),
      uartSettings.rxPin,
      uartSettings.txPin);
  while (BridgeSerial.available()) BridgeSerial.read();
  return true;
}

bool sendUartBreakMs(uint32_t durationMs, String &error) {
  if (durationMs < 1 || durationMs > 5000) {
    error = "break ms must be 1..5000";
    return false;
  }
  if (!validTxPin(uartSettings.txPin)) {
    error = "invalid TX GPIO";
    return false;
  }

  BridgeSerial.flush();
  BridgeSerial.end();
  delay(2);
  pinMode(uartSettings.txPin, OUTPUT);
  digitalWrite(uartSettings.txPin, HIGH);
  delayMicroseconds(200);
  digitalWrite(uartSettings.txPin, LOW);
  delay(durationMs);
  digitalWrite(uartSettings.txPin, HIGH);
  delayMicroseconds(200);

  BridgeSerial.setRxBufferSize(4096);
  BridgeSerial.begin(
      uartSettings.baud,
      serialConfigValue(uartSettings.dataBits, uartSettings.parity, uartSettings.stopBits),
      uartSettings.rxPin,
      uartSettings.txPin);
  return true;
}

String getParamValue(const String &command, const char *key, const String &fallback) {
  String needle = String(key) + "=";
  int pos = command.indexOf(needle);
  if (pos < 0) return fallback;
  int start = pos + needle.length();
  int end = start;
  while (end < command.length()) {
    char c = command.charAt(end);
    if (c == ' ' || c == '|' || c == ';' || c == '\r' || c == '\n') break;
    end++;
  }
  return command.substring(start, end);
}

void resetWsFrame(WsClient &client) {
  client.state = WS_HEADER;
  client.header[0] = 0;
  client.header[1] = 0;
  client.headerPos = 0;
  memset(client.ext, 0, sizeof(client.ext));
  client.extNeeded = 0;
  client.extPos = 0;
  memset(client.mask, 0, sizeof(client.mask));
  client.maskPos = 0;
  client.opcode = 0;
  client.masked = false;
  client.payloadLen = 0;
  client.payloadPos = 0;
}

void dropWsClient(uint8_t index) {
  if (index >= MAX_WS_CLIENTS) return;
  if (wsClients[index].active) {
    wsClients[index].tcp.stop();
    wsClients[index].active = false;
  }
  resetWsFrame(wsClients[index]);
}

bool writeAll(WiFiClient &client, const uint8_t *data, size_t len) {
  size_t written = 0;
  uint32_t started = millis();
  while (written < len && client.connected()) {
    size_t chunk = client.write(data + written, len - written);
    if (chunk > 0) {
      written += chunk;
      started = millis();
    } else {
      if (millis() - started > 120) return false;
      delay(1);
    }
  }
  return written == len;
}

bool sendWsFrame(WsClient &client, uint8_t opcode, const uint8_t *payload, size_t len) {
  if (!client.active || !client.tcp.connected()) return false;
  uint8_t header[10];
  size_t headerLen = 0;
  header[0] = 0x80 | (opcode & 0x0F);
  if (len <= 125) {
    header[1] = (uint8_t)len;
    headerLen = 2;
  } else if (len <= 65535) {
    header[1] = 126;
    header[2] = (uint8_t)((len >> 8) & 0xFF);
    header[3] = (uint8_t)(len & 0xFF);
    headerLen = 4;
  } else {
    header[1] = 127;
    uint64_t n = len;
    for (uint8_t i = 0; i < 8; i++) {
      header[2 + i] = (uint8_t)((n >> (56 - i * 8)) & 0xFF);
    }
    headerLen = 10;
  }

  if (!writeAll(client.tcp, header, headerLen)) return false;
  if (len > 0 && payload != nullptr && !writeAll(client.tcp, payload, len)) return false;
  return true;
}

bool sendWsText(WsClient &client, const String &message) {
  return sendWsFrame(client, 0x1, (const uint8_t *)message.c_str(), message.length());
}

void broadcastText(const String &message) {
  for (uint8_t i = 0; i < MAX_WS_CLIENTS; i++) {
    if (wsClients[i].active && !sendWsText(wsClients[i], message)) {
      dropWsClient(i);
    }
  }
}

void broadcastBinary(const uint8_t *data, size_t len) {
  if (len == 0) return;
  for (uint8_t i = 0; i < MAX_WS_CLIENTS; i++) {
    if (wsClients[i].active && !sendWsFrame(wsClients[i], 0x2, data, len)) {
      dropWsClient(i);
    }
  }
}

String headerValue(const String &request, const String &headerName) {
  String lowerRequest = request;
  lowerRequest.toLowerCase();
  String lowerName = headerName;
  lowerName.toLowerCase();
  lowerName += ":";
  int pos = lowerRequest.indexOf(lowerName);
  if (pos < 0) return "";
  int start = pos + lowerName.length();
  while (start < request.length()) {
    char c = request.charAt(start);
    if (c != ' ' && c != '\t') break;
    start++;
  }
  int end = request.indexOf('\r', start);
  if (end < 0) end = request.indexOf('\n', start);
  if (end < 0) end = request.length();
  String value = request.substring(start, end);
  value.trim();
  return value;
}

String readHttpRequest(WiFiClient &client) {
  String request;
  request.reserve(1800);
  uint32_t deadline = millis() + 1200;
  while (millis() < deadline && client.connected()) {
    while (client.available()) {
      char c = (char)client.read();
      request += c;
      if (request.endsWith("\r\n\r\n") || request.length() > 2400) return request;
    }
    delay(1);
  }
  return request;
}

String websocketAcceptKey(const String &key) {
  const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  String source = key + guid;
  unsigned char sha[20];
  mbedtls_sha1_ret((const unsigned char *)source.c_str(), source.length(), sha);

  unsigned char encoded[64];
  size_t encodedLen = 0;
  mbedtls_base64_encode(encoded, sizeof(encoded), &encodedLen, sha, sizeof(sha));
  encoded[encodedLen] = '\0';
  return String((const char *)encoded);
}

void acceptWsClient() {
  WiFiClient incoming = wsServer.available();
  if (!incoming) return;
  incoming.setTimeout(2);
  incoming.setNoDelay(true);

  String request = readHttpRequest(incoming);
  String key = headerValue(request, "Sec-WebSocket-Key");
  if (key.length() == 0) {
    incoming.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
    incoming.stop();
    return;
  }

  int slot = -1;
  for (uint8_t i = 0; i < MAX_WS_CLIENTS; i++) {
    if (!wsClients[i].active) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    incoming.print("HTTP/1.1 503 Busy\r\nConnection: close\r\n\r\n");
    incoming.stop();
    return;
  }

  String accept = websocketAcceptKey(key);
  incoming.print("HTTP/1.1 101 Switching Protocols\r\n");
  incoming.print("Upgrade: websocket\r\n");
  incoming.print("Connection: Upgrade\r\n");
  incoming.print("Sec-WebSocket-Accept: ");
  incoming.print(accept);
  incoming.print("\r\n\r\n");

  wsClients[slot].tcp = incoming;
  wsClients[slot].active = true;
  wsClients[slot].lastSeen = millis();
  resetWsFrame(wsClients[slot]);
  sendWsText(wsClients[slot], "INFO|message=WebSocket connected");
  sendWsText(wsClients[slot], makeStatusMessage());
  broadcastText(makeStatsMessage());
}

void sendWsError(uint8_t clientIndex, const String &error) {
  if (clientIndex >= MAX_WS_CLIENTS || !wsClients[clientIndex].active) return;
  String msg = "ERROR|message=" + sanitized(error);
  sendWsText(wsClients[clientIndex], msg);
}

void handleTextCommand(uint8_t clientIndex, const uint8_t *payload, size_t len) {
  String command;
  command.reserve(len + 1);
  for (size_t i = 0; i < len; i++) command += (char)payload[i];
  command.trim();

  if (command == "GET") {
    sendWsText(wsClients[clientIndex], makeStatusMessage());
    sendWsText(wsClients[clientIndex], makeStatsMessage());
    return;
  }

  if (command.startsWith("CFG")) {
    UartSettings next = uartSettings;
    next.baud = (uint32_t)getParamValue(command, "baud", String(next.baud)).toInt();
    next.dataBits = (uint8_t)getParamValue(command, "data", String(next.dataBits)).toInt();
    String parityText = getParamValue(command, "parity", String(next.parity));
    parityText.toUpperCase();
    next.parity = parityText.length() ? parityText.charAt(0) : 'N';
    next.stopBits = (uint8_t)getParamValue(command, "stop", String(next.stopBits)).toInt();
    next.rxPin = getParamValue(command, "rx", String(next.rxPin)).toInt();
    next.txPin = getParamValue(command, "tx", String(next.txPin)).toInt();

    String error;
    if (!applyUartSettings(next, error)) {
      sendWsError(clientIndex, error);
      return;
    }
    sendWsText(wsClients[clientIndex], "INFO|message=UART updated");
    broadcastText(makeStatusMessage());
    return;
  }

  if (command.startsWith("BREAK")) {
    uint32_t durationMs = (uint32_t)getParamValue(command, "ms", "100").toInt();
    String error;
    if (!sendUartBreakMs(durationMs, error)) {
      sendWsError(clientIndex, error);
      return;
    }
    sendWsText(wsClients[clientIndex], "INFO|message=BREAK sent");
    broadcastText(makeStatsMessage());
    return;
  }

  sendWsError(clientIndex, "unknown command");
}

void handleWsFrame(uint8_t clientIndex) {
  WsClient &client = wsClients[clientIndex];
  if (!client.active) return;

  switch (client.opcode) {
    case 0x1:
      handleTextCommand(clientIndex, client.payload, client.payloadPos);
      break;
    case 0x2:
      if (client.payloadPos > 0) {
        size_t written = BridgeSerial.write(client.payload, client.payloadPos);
        uartTxBytes += written;
      }
      break;
    case 0x8:
      sendWsFrame(client, 0x8, nullptr, 0);
      dropWsClient(clientIndex);
      break;
    case 0x9:
      sendWsFrame(client, 0xA, client.payload, client.payloadPos);
      break;
    case 0xA:
      break;
    default:
      sendWsError(clientIndex, "unsupported frame");
      dropWsClient(clientIndex);
      break;
  }
}

void preparePayloadState(WsClient &client, uint8_t clientIndex) {
  if (client.payloadLen > WS_MAX_PAYLOAD) {
    sendWsError(clientIndex, "websocket payload too large");
    dropWsClient(clientIndex);
    return;
  }
  client.payloadPos = 0;
  if (client.masked) {
    client.maskPos = 0;
    client.state = WS_MASK;
  } else if (client.payloadLen == 0) {
    handleWsFrame(clientIndex);
    if (client.active) resetWsFrame(client);
  } else {
    client.state = WS_PAYLOAD;
  }
}

void serviceWsClient(uint8_t clientIndex) {
  WsClient &client = wsClients[clientIndex];
  if (!client.active) return;
  if (!client.tcp.connected()) {
    dropWsClient(clientIndex);
    return;
  }

  while (client.active && client.tcp.available()) {
    int raw = client.tcp.read();
    if (raw < 0) break;
    uint8_t b = (uint8_t)raw;
    client.lastSeen = millis();

    if (client.state == WS_HEADER) {
      client.header[client.headerPos++] = b;
      if (client.headerPos == 2) {
        bool fin = (client.header[0] & 0x80) != 0;
        client.opcode = client.header[0] & 0x0F;
        client.masked = (client.header[1] & 0x80) != 0;
        uint8_t len = client.header[1] & 0x7F;
        if (!fin) {
          sendWsError(clientIndex, "fragmented frames unsupported");
          dropWsClient(clientIndex);
          return;
        }
        if (!client.masked) {
          sendWsError(clientIndex, "client frame must be masked");
          dropWsClient(clientIndex);
          return;
        }
        if (len == 126) {
          client.extNeeded = 2;
          client.extPos = 0;
          client.payloadLen = 0;
          client.state = WS_EXTENDED_LEN;
        } else if (len == 127) {
          client.extNeeded = 8;
          client.extPos = 0;
          client.payloadLen = 0;
          client.state = WS_EXTENDED_LEN;
        } else {
          client.payloadLen = len;
          preparePayloadState(client, clientIndex);
        }
      }
    } else if (client.state == WS_EXTENDED_LEN) {
      client.ext[client.extPos++] = b;
      if (client.extPos == client.extNeeded) {
        uint64_t len = 0;
        for (uint8_t i = 0; i < client.extNeeded; i++) {
          len = (len << 8) | client.ext[i];
        }
        client.payloadLen = len;
        preparePayloadState(client, clientIndex);
      }
    } else if (client.state == WS_MASK) {
      client.mask[client.maskPos++] = b;
      if (client.maskPos == 4) {
        if (client.payloadLen == 0) {
          handleWsFrame(clientIndex);
          if (client.active) resetWsFrame(client);
        } else {
          client.state = WS_PAYLOAD;
        }
      }
    } else if (client.state == WS_PAYLOAD) {
      client.payload[client.payloadPos] = b ^ client.mask[client.payloadPos % 4];
      client.payloadPos++;
      if (client.payloadPos >= client.payloadLen) {
        handleWsFrame(clientIndex);
        if (client.active) resetWsFrame(client);
      }
    }
  }
}

void serviceWsClients() {
  acceptWsClient();
  for (uint8_t i = 0; i < MAX_WS_CLIENTS; i++) {
    serviceWsClient(i);
  }
}

void handleSerialToWeb() {
  static uint8_t buffer[UART_RX_BATCH];
  static size_t len = 0;
  static uint32_t lastByteMs = 0;

  while (BridgeSerial.available() && len < sizeof(buffer)) {
    int value = BridgeSerial.read();
    if (value < 0) break;
    buffer[len++] = (uint8_t)value;
    lastByteMs = millis();
  }

  if (len > 0 && (len == sizeof(buffer) || millis() - lastByteMs >= 5)) {
    uartRxBytes += len;
    broadcastBinary(buffer, len);
    len = 0;
  }
}

String apName() {
  return String(AP_SSID);
}

bool hasConfiguredWifi() {
  return strlen(STA_WIFI_SSID) > 0 && strcmp(STA_WIFI_SSID, "YOUR_WIFI_SSID") != 0;
}

void startAccessPoint() {
  apMode = true;
  activeSsid = apName();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(activeSsid.c_str(), AP_PASSWORD);
  Serial.print("AP SSID: ");
  Serial.println(activeSsid);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void setupWifi() {
  WiFi.persistent(false);
  WiFi.setSleep(false);

  staModeRequested = hasConfiguredWifi();

  Serial.print("WiFi mode: ");
  Serial.println(staModeRequested ? "STA preferred" : "AP only, no STA credentials");

  if (staModeRequested && hasConfiguredWifi()) {
    Serial.print("Connecting to WiFi: ");
    Serial.println(STA_WIFI_SSID);
    WiFi.mode(WIFI_STA);
    if (!WiFi.config(STA_STATIC_IP, STA_GATEWAY, STA_SUBNET, STA_DNS1, STA_DNS2)) {
      Serial.println("Warning: static IP config failed.");
    }
    WiFi.begin(STA_WIFI_SSID, STA_WIFI_PASSWORD);
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
      serviceIndicators();
      delay(300);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      apMode = false;
      activeSsid = STA_WIFI_SSID;
      clearWifiFault();
      Serial.print("STA IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("Gateway: ");
      Serial.println(WiFi.gatewayIP());
      return;
    }

    setWifiFault("STA connect failed, fallback AP");
    Serial.println("STA connect failed, fallback to AP.");
  } else if (staModeRequested) {
    setWifiFault("STA selected but WiFi credentials missing");
    Serial.println("STA selected but WiFi credentials are missing.");
  }

  if (!staModeRequested) clearWifiFault();
  startAccessPoint();
}

void maintainWifi() {
  if (apMode || !staModeRequested || !hasConfiguredWifi()) return;
  if (WiFi.status() == WL_CONNECTED) {
    clearWifiFault();
    return;
  }
  if (!wifiFault) setWifiFault("STA disconnected");
  if (millis() - lastReconnectMs < 10000) return;
  lastReconnectMs = millis();
  Serial.println("WiFi disconnected, reconnecting...");
  WiFi.disconnect();
  WiFi.begin(STA_WIFI_SSID, STA_WIFI_PASSWORD);
}

void handleRoot() {
  httpServer.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleStatusHttp() {
  httpServer.send(200, "text/plain; charset=utf-8", makeStatusMessage());
}

void handleNotFound() {
  httpServer.send(404, "text/plain; charset=utf-8", "Not found");
}

void setupHttp() {
  httpServer.on("/", HTTP_GET, handleRoot);
  httpServer.on("/api/status", HTTP_GET, handleStatusHttp);
  httpServer.onNotFound(handleNotFound);
  httpServer.begin();
  wsServer.begin();
}

void setupMdns() {
  if (MDNS.begin("esp32-uart")) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    Serial.println("mDNS: http://esp32-uart.local/");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("ESP32 Wireless Web UART");
  setupStatusLed();

  String error;
  if (!applyUartSettings(uartSettings, error)) {
    setUartFault(error);
    Serial.print("UART setup error: ");
    Serial.println(error);
  } else {
    clearUartFault();
  }

  setupWifi();
  setupMdns();
  setupHttp();

  Serial.print("Open: http://");
  Serial.print(ipString());
  Serial.println("/");
  Serial.print("WebSocket port: ");
  Serial.println(WS_PORT);
}

void loop() {
  httpServer.handleClient();
  serviceWsClients();
  handleSerialToWeb();
  maintainWifi();
  serviceIndicators();

  if (millis() - lastStatsMs >= 1000) {
    lastStatsMs = millis();
    broadcastText(makeStatsMessage());
  }
}

#endif
