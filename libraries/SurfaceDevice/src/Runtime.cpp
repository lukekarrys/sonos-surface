#include "SurfaceDevice.h"
#include <SurfaceSonos.h>
#include <surface_json.hpp>
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <atomic>
#include <memory>

#ifndef SURFACE_ALLOW_SONOS_MUTATIONS
#define SURFACE_ALLOW_SONOS_MUTATIONS 0
#endif

namespace surface::device {
namespace {
using Json = nlohmann::json;
struct Config {
  std::string ssid, password, host, uid, source, playlistRoom, appleRegion = "52231";
} config;
Preferences preferences;
SemaphoreHandle_t stateMutex;
QueueHandle_t jobs;
std::atomic<bool> busy{false};
AppState sharedState;
std::string notice, serialLine;
bool serialOverflow = false;
struct Job { bool refresh; std::string payload; };
void log(const std::string& message) { Serial.printf("[%lu] %s\n", millis(), message.c_str()); }
void publish(const AppState& state) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sharedState = state;
  xSemaphoreGive(stateMutex);
  log("request=" + std::to_string(state.requestId) + " status=" + state.status + " " + state.detail +
      " shuffle-origin=" + state.shuffleOrigin);
}
bool parseConfig(const std::string& text, Config& output) {
  auto json = Json::parse(text, nullptr, false);
  if (!json.is_object() || json.size() > 7) return false;
  for (auto it = json.begin(); it != json.end(); ++it) {
    if (!it.value().is_string()) return false;
    const auto& k = it.key();
    if (k != "wifi_ssid" && k != "wifi_password" && k != "sonos_ip" && k != "sonos_uid" &&
        k != "source_url" && k != "playlist_shuffle_room" && k != "apple_region") return false;
  }
  Config c;
  c.ssid = json.value("wifi_ssid", ""); c.password = json.value("wifi_password", "");
  c.host = json.value("sonos_ip", ""); c.uid = json.value("sonos_uid", "");
  c.source = json.value("source_url", ""); c.playlistRoom = json.value("playlist_shuffle_room", "");
  c.appleRegion = json.value("apple_region", "52231");
  IPAddress address;
  if (c.ssid.size() > 32 || c.password.size() > 63 || (!c.host.empty() && !address.fromString(c.host.c_str())) ||
      (!c.uid.empty() && c.uid.compare(0, 7, "RINCON_") != 0)) return false;
  Source source;
  if (!c.source.empty() && !normalizeAppleUrl(c.source, source).ok) return false;
  if (c.appleRegion.empty() || c.appleRegion.find_first_not_of("0123456789") != std::string::npos) return false;
  output = std::move(c);
  return true;
}
class EspHttp final : public LocalHttp {
public:
  HttpResponse request(const std::string& path, const std::string& action, const std::string& body) override {
    if (!SURFACE_ALLOW_SONOS_MUTATIONS && !isReadOnlySonosAction(action)) {
      log("READ_ONLY_BLOCKED " + action);
      return {0, "", "Read-only firmware: command not sent", true};
    }
    if (WiFi.status() != WL_CONNECTED) return {0, "", "WiFi disconnected", true};
    if (config.host.empty()) return {0, "", "Configure sonos_ip", true};
    HTTPClient http;
    const std::string url = "http://" + config.host + ":1400" + path;
    http.setConnectTimeout(3000);
    http.setTimeout(8000);
    http.setReuse(false);
    if (!http.begin(url.c_str())) return {0, "", "HTTP begin failed", true};
    log("Sonos " + config.host + " " + (action.empty() ? "GET " + path : action));
    int status;
    if (action.empty()) status = http.GET();
    else {
      http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
      http.addHeader("SOAPACTION", ("\"" + action + "\"").c_str());
      status = http.POST(String(body.c_str()));
    }
    HttpResponse result;
    result.status = status;
    if (status > 0) {
      // Sonos replies are small; cap even chunked responses before allocating a body.
      class Sink : public Stream {
      public:
        std::string data;
        size_t write(uint8_t c) override { return write(&c, 1); }
        size_t write(const uint8_t* p, size_t n) override {
          if (n > 65536 - data.size()) return 0;
          data.append(reinterpret_cast<const char*>(p), n); return n;
        }
        int available() override { return 0; }
        int read() override { return -1; }
        int peek() override { return -1; }
        void flush() override {}
      } sink;
      if (http.getSize() > 65536 || http.writeToStream(&sink) < 0) {
        result.status = 0; result.error = "Incomplete or oversized HTTP response";
      } else result.body = std::move(sink.data);
    } else result.error = HTTPClient::errorToString(status).c_str();
    http.end();
    return result;
  }
  uint64_t nowMs() override { return esp_timer_get_time() / 1000; }
  void pollWait(uint32_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms)); }
};
void worker(void*) {
  EspHttp http;
  DirectSonos sonos(http, {config.uid, config.appleRegion}, log);
  Application app(sonos, {config.uid, config.playlistRoom, 1}, publish);
  for (;;) {
    Job* job = nullptr;
    if (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) {
      if (job->refresh) app.refresh();
      else {
        auto result = app.submit(job->payload);
        if (!result.ok) log("command rejected/result: " + result.error);
      }
      delete job;
      busy.store(false);
    }
  }
}
void submit(const std::string& payload, bool refresh = false) {
  if (busy.exchange(true)) { notice = "Busy; try again after result"; return; }
  auto job = new Job{refresh, payload};
  if (!jobs || xQueueSend(jobs, &job, 0) != pdTRUE) {
    delete job; busy.store(false); notice = "Worker unavailable";
  }
}
std::string command(const char* transport) {
  return Json{{"format", "sonos-surface"}, {"version", 1}, {"intent", {{"transport", transport}}}}.dump();
}
void handle(const std::string& line) {
  if (line.compare(0, 7, "config ") == 0) {
    if (busy.load()) { log("CONFIG_BUSY"); return; }
    Config next;
    const auto text = line.substr(7);
    if (!parseConfig(text, next)) { log("CONFIG_INVALID (values omitted from log)"); return; }
    if (!preferences.putString("config", text.c_str())) { log("CONFIG_SAVE_FAILED"); return; }
    log("CONFIG_SAVED rebooting (credentials not logged)");
    Serial.flush();
    ESP.restart();
  } else if (line == "status") submit("", true);
  else if (line == "play") submit(command("play"));
  else if (line == "pause") submit(command("pause"));
  else if (line == "source") {
    if (config.source.empty()) notice = "Configure source_url first";
    else submit(config.source);
  } else if (!line.empty() && (line[0] == '{' || line.compare(0, 8, "https://") == 0)) submit(line);
  else log("Commands: status | play | pause | source | config {JSON} | intent JSON");
}
} // namespace

void begin() {
  // Native USB defaults to 256 RX bytes. A configuration with a source URL
  // exceeds that and can lose its newline while the display task is busy.
  const auto usbRxBytes = Serial.setRxBufferSize(8192);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  Serial.printf("[usb] RX buffer=%u bytes\n", unsigned(usbRxBytes));
  stateMutex = xSemaphoreCreateMutex();
  jobs = xQueueCreate(1, sizeof(Job*));
  if (!stateMutex || !jobs) { log("FATAL worker allocation"); return; }
  const bool boardReady = boardBegin(notice);
  log("sonos-surface checkpoint firmware; " + notice);
  log(SURFACE_ALLOW_SONOS_MUTATIONS ? "SONOS_MODE=PLAYBACK_ENABLED" : "SONOS_MODE=READ_ONLY (all mutations blocked before HTTP)");
  Serial.printf("[board] adapter ready=%d heap=%u psram=%u\n", boardReady, ESP.getFreeHeap(), ESP.getPsramSize());
  preferences.begin("surface", false);
  auto stored = preferences.getString("config", "{}");
  if (!parseConfig(stored.c_str(), config)) log("Stored config invalid; USB config required");
  log("speaker-ip=" + config.host + " uid=" + config.uid);
  if (!config.ssid.empty()) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(config.ssid.c_str(), config.password.c_str());
    log("WiFi connecting");
  } else log("No WiFi configuration; NFC/input diagnostics still active");
  if (xTaskCreate(worker, "surface-sonos", 24576, nullptr, 1, nullptr) != pdPASS) {
    notice = "Failed to create Sonos worker";
    vQueueDelete(jobs); jobs = nullptr;
  }
  log("READY: NFC/touch; USB commands status/play/pause/source/config");
}
void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (serialOverflow) log("USB command exceeds 4096 bytes");
      else handle(serialLine);
      serialLine.clear(); serialOverflow = false;
    } else if (c != '\r') {
      if (serialLine.size() < 4096) serialLine += c;
      else serialOverflow = true;
    }
  }
  const auto event = boardPoll();
  switch (event.input) {
    case Input::Source: handle("source"); break;
    case Input::Play: handle("play"); break;
    case Input::Pause: handle("pause"); break;
    case Input::Refresh: handle("status"); break;
    case Input::Payload: {
      MusicIntent parsed;
      auto r = parseIntent(event.text, parsed);
      if (r.ok) {
        notice = "NFC parsed; submitted";
        log("NFC parsed " + (parsed.source ? parsed.source->url : "transport/settings"));
        submit(event.text);
      } else { notice = r.error; log("NFC parse error: " + r.error); }
      break;
    }
    case Input::Error: notice = event.text; log("Input error: " + notice); break;
    case Input::None: break;
  }
  static bool connected = false;
  static uint32_t lastRefresh = 0, lastRender = 0, lastHeartbeat = 0;
  const bool online = WiFi.status() == WL_CONNECTED;
  if (online && !connected) {
    log("WiFi connected ip=" + std::string(WiFi.localIP().toString().c_str()));
    lastRefresh = millis(); submit("", true);
  } else if (!online && connected) {
    log("WiFi disconnected");
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    sharedState.observed.stale = true;
    xSemaphoreGive(stateMutex);
  }
  connected = online;
  if (online && !config.host.empty() && !busy.load() && millis() - lastRefresh >= 10000) {
    lastRefresh = millis(); submit("", true);
  }
  if (millis() - lastRender >= 100) {
    lastRender = millis();
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    auto state = sharedState;
    xSemaphoreGive(stateMutex);
    boardRender(state, std::string(online ? "WiFi OK | " : "WiFi offline | ") + notice);
  }
  if (millis() - lastHeartbeat >= 5000) {
    lastHeartbeat = millis();
    log("heartbeat wifi=" + std::to_string(WiFi.status()) + " busy=" + std::to_string(busy.load()) +
        " heap=" + std::to_string(ESP.getFreeHeap()));
  }
  vTaskDelay(1); // Yield the UI task; no Sonos ordering depends on this scheduler tick.
}
} // namespace surface::device
