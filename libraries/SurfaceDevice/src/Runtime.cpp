#include "SurfaceDevice.h"
#include "RoomConfig.h"
#include "ConsoleWrite.h"
#include "StickPlayback.h"
#include "RuntimeCoordinator.h"
#include "JobNetworkClient.h"
#if defined(SURFACE_WAVESHARE_1_8) && !SURFACE_TOUCH_DIAGNOSTIC
#include "WaveshareArtwork.h"
#endif
#include <SurfaceSonos.h>
#include <surface_json.hpp>
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <map>
#include <set>
#include <esp_timer.h>
#include <esp_system.h>
#include <atomic>
#include <memory>

namespace surface::device {
namespace {
using Json = nlohmann::json;
#if defined(SURFACE_STICK_S3)
constexpr char hardwareTargetName[] = "stick-s3";
#elif defined(SURFACE_WAVESHARE_1_8)
constexpr char hardwareTargetName[] = "ws-1.8";
#else
#error "Build with one hardware target define"
#endif
struct Config {
  std::string ssid, password, appleRegion = "52231";
  bool readOnly = true;
  uint32_t sleepTimeoutSeconds = defaultSleepTimeoutSeconds;
  uint32_t revision = 1;
  RoomConfig rooms;
  SourcePolicy policy;
} config;
Preferences preferences;
SemaphoreHandle_t stateMutex;
QueueHandle_t jobs;
TaskHandle_t workerTask = nullptr;
uint64_t workerRetryAt = 0;
std::atomic<bool> stopping{false};
DevicePower power;
AppState sharedState;
RoomSelection selection;
std::string savedPreference;
std::string notice, serialLine;
// Boot adapter identity, reported by the read-only board command.
std::string adapterNotice;
bool adapterReady = false;
bool transientNotice = false;
bool serialOverflow = false;
RuntimeCoordinator coordinator(sharedState);
uint64_t nowMs() { return esp_timer_get_time() / 1000; }
// Development-only modem power-save override (USB `wifi-sleep`), applied after
// each connection. Never persisted; empty keeps the SDK default.
std::optional<bool> wifiSleepOverride;
bool wifiWasOnline = false;
// Coordinator calls and publication share this mutex. Platform work runs after
// unlocking, except the nonblocking enqueue that commits validated admission.
struct WorkerActivity {
  bool user = false, automatic = false;
};
// Busy, as shown to users, means a user job is running. Automatic reads yield.
WorkerActivity workerActivity() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto worker = coordinator.snapshots().worker;
  xSemaphoreGive(stateMutex);
  return {worker.running && worker.origin == JobOrigin::User,
          worker.running && worker.origin == JobOrigin::Automatic};
}
// USB config and reboot need an Idle worker: a user job rejects them, while a
// running automatic read is preempted to get there.
bool idleForMaintenance() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto worker = coordinator.snapshots().worker;
  const bool idle = !worker.running || (worker.origin == JobOrigin::Automatic &&
                                        coordinator.preemptAutomatic(nowMs()));
  xSemaphoreGive(stateMutex);
  return idle;
}
bool jobActiveLocked(uint64_t id) { return coordinator.jobActive(id, nowMs(), stopping.load()); }
bool jobActive(uint64_t id) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool active = jobActiveLocked(id);
  xSemaphoreGive(stateMutex);
  return active;
}
JobFinish finishJob(uint64_t id, RuntimeCoordinator::JobOutcomeInput outcome = {}) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto finish = coordinator.finishJob(nowMs(), id, outcome);
  xSemaphoreGive(stateMutex);
  return finish;
}
struct Job {
  bool refresh;
  bool cycle = false;
  ResolvedIntent accepted;
  std::optional<PolicyContext> toggleContext = std::nullopt;
  std::optional<std::pair<uint32_t, uint32_t>> queuePage = std::nullopt;
  bool roomChanged = false;
  std::string readTarget;
  uint64_t id = 0;
  uint64_t discoveryId = 0;
  JobOrigin origin = JobOrigin::User;
  const std::string& targetId() const {
    return toggleContext ? toggleContext->targetId : accepted.targetId;
  }
};
void consoleLine(const std::string& message, uint32_t waitMs) {
  const auto line = "[" + std::to_string(millis()) + "] " + message + "\n";
  writeConsoleLine(Serial, line, waitMs, [] { return uint32_t(millis()); }, [] { vTaskDelay(1); });
}
void log(const std::string& message) { consoleLine(message, 0); }
void logResponse(const std::string& message) { consoleLine(message, 250); }
void serviceWifi() {
  const bool observedOnline = WiFi.status() == WL_CONNECTED;
  if (observedOnline && !wifiWasOnline && wifiSleepOverride)
    WiFi.setSleep(*wifiSleepOverride);
  wifiWasOnline = observedOnline;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  coordinator.serviceWifi(nowMs(), observedOnline);
  const auto effects = coordinator.drainWifi();
  xSemaphoreGive(stateMutex);
  if (effects.disconnect)
    WiFi.disconnect(false, false);
  if (effects.connect) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.begin(config.ssid.c_str(), config.password.c_str());
  }
}
void restart(const std::string& message) {
  stopping.store(true);
  logResponse(message);
  // Only restart acknowledgments may drain synchronously. Normal operation
  // never inherits the SDK's repeated per-chunk TX waits.
  Serial.setTxTimeoutMs(20);
  Serial.flush();
  ESP.restart();
}
void publish(uint64_t id, const AppState& state) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto* room = selection.selected();
  const bool published = coordinator.publish(nowMs(), id, state, room ? room->id : "",
                                             selection.warning, stopping.load());
  xSemaphoreGive(stateMutex);
  if (!published)
    return;
  log("request=" + std::to_string(state.requestId) + " status=" + state.status + " " +
      state.detail + " refresh-error=" + state.refreshError);
}
bool parseConfig(const std::string& text, Config& output) {
  Json json;
  if (!parseConfigDocument(text, json))
    return false;
  Config c;
  c.ssid = json.value("wifi_ssid", "");
  c.password = json.value("wifi_password", "");
  if (!parseDeviceRooms(json, c.readOnly, c.rooms, c.policy) ||
      !parseSleepTimeout(json, c.sleepTimeoutSeconds))
    return false;
  c.appleRegion = json.value("apple_region", "52231");
  if (c.ssid.size() > 32 || c.password.size() > 63)
    return false;
  if (c.appleRegion.empty() || c.appleRegion.find_first_not_of("0123456789") != std::string::npos)
    return false;
  output = std::move(c);
  return true;
}
class EspHttp final : public GuardedHttp {
public:
  std::string host;
  uint64_t jobId = 0;
  uint64_t discoveryId = 0;
  bool transportFailed = false;
  bool automatic = false;
  EspHttp() { readOnly = config.readOnly; }
  std::string baseUrl() const override { return "http://" + host + ":1400"; }

protected:
  HttpResponse blocked(const std::string& reason, const std::string& action) override {
    log(reason + " " + action);
    return {0, "", reason, true};
  }
  HttpResponse dispatch(const std::string& path, const std::string& action,
                        const std::string& body) override {
    const auto allowed = [this, &action] {
      if (WiFi.status() != WL_CONNECTED)
        return false;
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      const bool result =
          coordinator.dispatchAllowed(jobId, discoveryId, surface::device::nowMs(), stopping.load(),
                                      !isReadOnlySonosAction(action));
      xSemaphoreGive(stateMutex);
      return result;
    };
    if (!allowed())
      return {0, "", "Job or Sonos authority expired/unavailable", true};
    if (host.empty())
      return {0, "", "No discovered address", true};
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const auto deadline = coordinator.snapshots().worker.deadline;
    xSemaphoreGive(stateMutex);
    const auto started = surface::device::nowMs();
    const auto name = action.empty() ? "GET " + path : action.substr(action.rfind('#') + 1);
    // One line per unanswered request, in the transition-log style. A request
    // ended by preemption, expiry, or shutdown is logged but never recorded.
    const auto failure = [&](const char* phase, int code, const std::string& error) {
      const int number = errno;
      const auto elapsed = surface::device::nowMs() - started;
      const bool cancelled = !allowed();
      log("Sonos failure phase=" + std::string(phase) + " host=" + host + " action=" + name +
          " code=" + std::to_string(code) + " error=\"" + error +
          "\" errno=" + std::to_string(number) + " elapsed-ms=" + std::to_string(elapsed) +
          " automatic=" + std::to_string(int(automatic)) +
          " cancelled=" + std::to_string(int(cancelled)));
      if (cancelled)
        return;
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      coordinator.transportFailure(
          {phase, host, name, error, code, number, elapsed, surface::device::nowMs()});
      xSemaphoreGive(stateMutex);
    };
    JobNetworkClient client(
        deadline, [] { return surface::device::nowMs(); }, allowed, [] { vTaskDelay(1); });
    HTTPClient http;
    const std::string url = "http://" + host + ":1400" + path;
    http.setConnectTimeout(automatic ? RuntimeCoordinator::AutomaticConnectTimeoutMs
                                     : RuntimeCoordinator::UserConnectTimeoutMs);
    http.setTimeout(8000);
    http.setReuse(false);
    if (!http.begin(client, url.c_str())) {
      failure("begin", 0, "HTTP begin failed");
      return {0, "", "HTTP begin failed", true};
    }
    log("Sonos " + host + " " + (action.empty() ? "GET " + path : action));
    // This is the last admission boundary, including identity reads. A request
    // admitted before shutdown can already be on the wire; never replay it.
    if (!allowed()) {
      http.end();
      return {0, "", "Job expired/cancelled", true};
    }
    int status;
    if (action.empty())
      status = http.GET();
    else {
      http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
      http.addHeader("SOAPACTION", ("\"" + action + "\"").c_str());
      status = http.POST(String(body.c_str()));
    }
    HttpResponse result;
    // HTTPClient uses negative transport errors; an answered HTTP/SOAP fault
    // still proves reachability, even if its body cannot be consumed.
    transportFailed = transportFailed || status <= 0;
    result.status = status;
    if (status > 0) {
      // Sonos replies are small; cap even chunked responses before allocating a body.
      class Sink : public Stream {
      public:
        uint64_t jobId;
        explicit Sink(uint64_t id) : jobId(id) {}
        std::string data;
        size_t write(uint8_t c) override { return write(&c, 1); }
        size_t write(const uint8_t* p, size_t n) override {
          if (!jobActive(jobId) || n > 65536 - data.size())
            return 0;
          data.append(reinterpret_cast<const char*>(p), n);
          return n;
        }
        int available() override { return 0; }
        int read() override { return -1; }
        int peek() override { return -1; }
        void flush() override {}
      } sink(jobId);
      const int written = http.getSize() > 65536 ? 0 : http.writeToStream(&sink);
      if (http.getSize() > 65536 || written < 0) {
        result.status = 0;
        result.error = "Incomplete or oversized HTTP response";
        if (written < 0)
          failure("body", written, HTTPClient::errorToString(written).c_str());
      } else
        result.body = std::move(sink.data);
    } else {
      result.error = HTTPClient::errorToString(status).c_str();
      failure(status == HTTPC_ERROR_CONNECTION_REFUSED ? "connect"
              : status == HTTPC_ERROR_SEND_HEADER_FAILED ||
                      status == HTTPC_ERROR_SEND_PAYLOAD_FAILED
                  ? "send"
                  : "headers",
              status, result.error);
    }
    http.end();
    return result;
  }
  uint64_t nowMs() override { return surface::device::nowMs(); }
  void pollWait(uint32_t ms) override {
    if (jobActive(jobId))
      vTaskDelay(pdMS_TO_TICKS(ms));
  }
};
// SSDP discovers a player from which to read household topology. Topology supplies
// current names, addresses, and eligibility; configured room keys select targets.
std::vector<std::string> discoverAddresses(uint64_t id) {
  // A manual status/rooms request must also be safe before configuration.
  if (!jobActive(id) || WiFi.status() != WL_CONNECTED)
    return {};
  std::set<std::string> hosts;
  WiFiUDP udp;
  if (udp.begin(0)) {
    const char* search =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: "
        "1\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
    udp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
    udp.write(reinterpret_cast<const uint8_t*>(search), strlen(search));
    udp.endPacket();
    const auto start = millis();
    while (jobActive(id) && millis() - start < 1800) {
      if (udp.parsePacket()) {
        char packet[2049]{};
        const int n = udp.read(packet, 2048);
        String reply(n > 0 ? packet : "");
        reply.toLowerCase();
        if (reply.indexOf("zoneplayer") >= 0 && hosts.size() < 32)
          hosts.insert(udp.remoteIP().toString().c_str());
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    udp.stop();
  }
  log("SSDP discovered players=" + std::to_string(hosts.size()));
  return {hosts.begin(), hosts.end()};
}
struct Session {
  EspHttp http;
  DirectSonos sonos;
  Application app;
  explicit Session(const Room& room)
      : sonos(http, {room.id, config.appleRegion}, log),
        app(sonos, {room.id, {}, 1},
            [this](const AppState& state) { publish(http.jobId, state); }) {
    http.host = room.address;
    http.target = room.id;
  }
};
// GENA notifications only invalidate topology; all target decisions use a fresh
// full snapshot. No remote event can directly issue a playback command.
class TopologyEvents {
  WiFiServer server{1401};
  bool listening = false;

  void poll() {
    auto client = server.accept();
    if (!client)
      return;
    const auto deadline = nowMs() + 500;
    unsigned bytes = 0;
    // Read bytewise with an absolute deadline and cap, including the first
    // request line. Stream::readStringUntil has only an inactivity timeout.
    auto line = [&]() {
      std::string result;
      while (nowMs() < deadline && bytes < 8192 && client.connected()) {
        if (!client.available()) {
          vTaskDelay(1);
          continue;
        }
        const int c = client.read();
        if (c < 0)
          continue;
        ++bytes;
        if (c == '\n')
          break;
        result += char(c);
      }
      return result;
    };
    const auto first = line();
    std::string sid;
    bool headersComplete = false;
    while (nowMs() < deadline && bytes < 8192 && client.connected()) {
      auto header = line();
      if (header == "\r" || header.empty()) {
        headersComplete = true;
        break;
      }
      auto lower = header;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      if (lower.rfind("sid:", 0) == 0) {
        const auto firstValue = header.find_first_not_of(" \t", 4);
        const auto lastValue = header.find_last_not_of(" \t\r");
        if (firstValue != std::string::npos && lastValue >= firstValue)
          sid = header.substr(firstValue, lastValue - firstValue + 1);
      }
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const bool valid = first.rfind("NOTIFY /topology ", 0) == 0 && headersComplete &&
                       bytes < 8192 && nowMs() < deadline && !stopping.load() &&
                       coordinator.notify(nowMs(), sid, stopping.load());
    xSemaphoreGive(stateMutex);
    const std::string response =
        valid
            ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
            : "HTTP/1.1 412 Precondition Failed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    ::send(client.fd(), response.data(), response.size(), MSG_DONTWAIT);
    client.stop();
  }

public:
  void service() {
    if (stopping.load() || WiFi.status() != WL_CONNECTED) {
      stop();
      return;
    }
    // ESP networking must be initialized before opening the listener.
    if (!listening) {
      server.begin();
      listening = true;
    }
    poll();
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    auto pending = coordinator.drainSubscription();
    const auto deadline = coordinator.snapshots().subscription.deadline;
    xSemaphoreGive(stateMutex);
    if (!pending)
      return;
    const auto active = [id = pending->id] {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      const bool result = coordinator.subscriptionActive(id, nowMs(), stopping.load());
      xSemaphoreGive(stateMutex);
      return result;
    };
    if (!active())
      return;
    JobNetworkClient client(deadline, [] { return nowMs(); }, active, [] { vTaskDelay(1); }, 8192);
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(2000);
    http.setReuse(false);
    const auto url = "http://" + pending->address + ":1400/ZoneGroupTopology/Event";
    int status = 0;
    if (http.begin(client, url.c_str())) {
      const char* headers[] = {"SID", "TIMEOUT"};
      http.collectHeaders(headers, 2);
      http.addHeader("TIMEOUT", "Second-300");
      if (!pending->renewal) {
        http.addHeader("CALLBACK", ("<http://" + std::string(WiFi.localIP().toString().c_str()) +
                                    ":1401/topology>")
                                       .c_str());
        http.addHeader("NT", "upnp:event");
      } else
        http.addHeader("SID", pending->sid.c_str());
      if (active())
        status = http.sendRequest("SUBSCRIBE");
    }
    const std::string sid = http.header("SID").c_str();
    const std::string timeout = http.header("TIMEOUT").c_str();
    http.end();
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    coordinator.subscriptionResult(nowMs(), pending->id, status == 200, sid,
                                   parseSubscriptionLeaseMs(timeout));
    xSemaphoreGive(stateMutex);
  }
  void stop() {
    if (listening)
      server.end();
    listening = false;
    // No blocking UNSUBSCRIBE. The peer's lease expires independently.
  }
};
void worker(void*) {
  TopologyEvents events;
  std::map<std::string, std::unique_ptr<Session>> sessions;
  std::string discoveryHost; // Learned by SSDP in this process; never configured or persisted.
  for (;;) {
    if (stopping.load()) {
      events.stop();
      vTaskSuspend(nullptr); // Deep sleep ends this task; it is never resumed.
    }
    Job* job = nullptr;
    events.service();
    if (xQueueReceive(jobs, &job, pdMS_TO_TICKS(100)) != pdTRUE)
      continue;
    if (!jobActive(job->id)) {
      finishJob(job->id);
      delete job;
      continue;
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const auto binding = coordinator.bindDiscovery(nowMs(), job->id);
    job->discoveryId = binding.discoveryId;
    // Pickup latency: a user job admitted over an automatic read waits here
    // while the preempted read unwinds.
    const auto pickup = coordinator.snapshots().worker;
    const auto waited = pickup.jobId == job->id ? nowMs() - pickup.startedAt : 0;
    // Topology source: the job's own room (bound target, else selected room).
    std::string roomAddress;
    if (job->refresh) {
      if (const auto* room = selection.selected())
        roomAddress = room->address;
    } else
      for (const auto& room : selection.rooms)
        if (room.id == job->targetId())
          roomAddress = room.address;
    xSemaphoreGive(stateMutex);
    if (job->origin == JobOrigin::User)
      log("worker pickup id=" + std::to_string(job->id) + " wait-ms=" + std::to_string(waited));
    if (binding.discardHost)
      discoveryHost.clear();
    if (!job->discoveryId) {
      finishJob(job->id);
      delete job;
      continue;
    }
    std::vector<Room> rooms;
    const auto topology = readHouseholdTopology(
        roomAddress, discoveryHost,
        [&](const TopologyProbe& source, std::vector<Room>& found) {
          EspHttp http;
          http.host = source.host;
          http.jobId = job->id;
          http.discoveryId = job->discoveryId;
          http.automatic = job->origin == JobOrigin::Automatic;
          DirectSonos sonos(http, {"", config.appleRegion}, log);
          return source.verifyIdentity ? sonos.discover(found) : sonos.topology(found);
        },
        [&] { return discoverAddresses(job->id); }, [&] { return jobActive(job->id); }, rooms);
    Result discovered = topology.result;
    // Serial backpressure must never hold the UI's state lock: button polling
    // needs to observe both releases in a double-click even without a monitor.
    if (discovered.ok)
      for (const auto& room : rooms)
        log("discovered room=" + room.name + " roomDisplayId=" + room.displayId +
            " uuid=" + room.id + " eligible=" + std::to_string(room.eligible));
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (!jobActiveLocked(job->id)) {
      xSemaphoreGive(stateMutex);
      finishJob(job->id);
      delete job;
      continue;
    }
    // A result at/after the discovery deadline cannot refresh topology authority.
    const bool discoveryAccepted = coordinator.discoveryResult(nowMs(), job->id, job->discoveryId,
                                                               discovered.ok, topology.host);
    if (discovered.ok && !discoveryAccepted)
      discovered = Result::fail("Discovery expired");
    if (discovered.ok) {
      selection.update(std::move(rooms));
    } else {
      sharedState.observed.stale = true;
      sharedState.queue.reset();
      selection.problems = {"DISCOVERY FAILED: " + discovered.error};
      selection.warning = selection.problems.front();
    }
    if (job->cycle && discovered.ok)
      selection.cycle();
    const Room* selected = selection.selected();
    Room selectedRoom = selected ? *selected : Room{};
    std::string warning = selection.warning;
    const auto selectableRooms = selection.rooms;
    const auto problems = selection.problems;
    const auto resolvedRules = selection.resolvedPolicies;
    const bool changedTarget = selectObservedRoom(sharedState, selectedRoom);
    Room target = selectedRoom;
    if (!job->refresh) {
      target = {};
      for (const auto& room : selection.rooms)
        if (room.id == job->targetId())
          target = room;
    }
    xSemaphoreGive(stateMutex);
    for (const auto& problem : problems)
      log("CONFIG_WARNING " + problem);
    for (const auto& rule : resolvedRules)
      log("resolved room policy uuid=" + rule.first + " " +
          roomConfigJson({{rule.second.displayId, rule.second.policy}}).dump());
    for (const auto& room : selectableRooms)
      log("room=" + room.name + " roomDisplayId=" + room.displayId + " uuid=" + room.id +
          " ip=" + room.address + " group=" + room.group + " coordinator=" + room.coordinator +
          " selectable=" + std::to_string(room.eligible));
    if (job->refresh && !selectedRoom.id.empty())
      log("selected=" + selectedRoom.name + " uuid=" + selectedRoom.id + " " + warning);
    bool queueFailed = false;
    const bool targetUsable = discovered.ok && target.eligible && !target.address.empty();
    Session* jobSession = nullptr;
    std::optional<AppState> retained;
    if (!targetUsable) {
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      AppState unavailable = sharedState;
      xSemaphoreGive(stateMutex);
      unavailable.observed.stale = true;
      unavailable.queue.reset();
      unavailable.refreshError = !warning.empty() ? warning
                                 : discovered.ok  ? "Selected room unavailable/grouped"
                                                  : discovered.error;
      unavailable.detail = warning;
      publish(job->id, unavailable);
    } else {
      auto& session = sessions[target.id];
      if (!session)
        session = std::make_unique<Session>(target);
      jobSession = session.get();
      session->http.jobId = job->id;
      session->http.transportFailed = false;
      session->http.discoveryId = job->discoveryId;
      session->http.host = target.address;
      session->http.automatic = job->origin == JobOrigin::Automatic;
      session->http.targetAllowed = target.eligible;
      session->sonos.confirmTopology(target);
      if (job->refresh) {
        if (changedTarget || job->cycle || job->roomChanged) {
          session->app.invalidateObservation();
          AppState loading;
          loading.observed.room = target.name;
          loading.observed.targetId = target.id;
          loading.detail = job->cycle ? "Room switched; reading state" : warning;
          publish(job->id, loading);
        }
        retained = session->app.state();
        discovered = session->app.reconcile();
        if (job->queuePage && (job->readTarget.empty() || job->readTarget == target.id)) {
          const auto queueStarted = millis();
          const auto result = session->app.queue(job->queuePage->first, job->queuePage->second);
          queueFailed = !result.ok;
          log("queue-fetch start=" + std::to_string(job->queuePage->first) +
              " count=" + std::to_string(job->queuePage->second) +
              " ms=" + std::to_string(millis() - queueStarted) +
              " heap=" + std::to_string(ESP.getFreeHeap()) +
              " psram-free=" + std::to_string(ESP.getFreePsram()));
          if (!result.ok)
            log("queue-error=" + result.error);
          else {
            const auto& page = *session->app.state().queue;
            log("queue-page " + Json{{"target", page.targetId},
                                     {"start", page.start},
                                     {"total", page.total},
                                     {"revision", page.revision},
                                     {"count", page.items.size()}}
                                    .dump());
            for (const auto& item : page.items)
              log("queue-item " +
                  Json{{"index", item.index},
                       {"title", item.title},
                       {"artist", item.artist},
                       {"album", item.album},
                       {"artwork", item.artwork},
                       {"uri", item.uri},
                       {"durationMs", item.durationMs ? Json(*item.durationMs) : Json(nullptr)}}
                      .dump());
          }
        }
      } else {
        retained = session->app.state();
        auto result = job->toggleContext ? session->app.submitToggle(*job->toggleContext)
                                         : session->app.submit(job->accepted);
        discovered = result;
        if (!result.ok)
          log("command rejected/result: " + result.error);
      }
    }
    // Decide terminal acceptance under the same lock as deadline processing.
    // Cleanup occurs on this task before any newer job can reuse its Session.
    const auto finish = finishJob(job->id, {targetUsable, discovered.ok, queueFailed, false,
                                            jobSession && jobSession->http.transportFailed});
    if (jobSession && retained)
      discardJobResult(finish, jobSession->app, *retained, !job->refresh);
    delete job;
  }
}
void ensureWorker() {
  if (workerTask || stopping.load() || nowMs() < workerRetryAt)
    return;
  workerRetryAt = nowMs() + 30000;
  // One user job can wait behind one preempted automatic job not yet picked up.
  if (!jobs)
    jobs = xQueueCreate(2, sizeof(Job*));
  if (!jobs || xTaskCreate(worker, "surface-sonos", 24576, nullptr, 1, &workerTask) != pdPASS) {
    workerTask = nullptr;
    log("worker unavailable; retry in 30000ms");
    return;
  }
  log("worker available");
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  coordinator.workerAvailable();
  xSemaphoreGive(stateMutex);
}
// The queue send and model admission commit together under stateMutex. The
// worker takes that mutex before using a queued job, so it sees the committed ID.
bool enqueueJobLocked(Job* job) {
  return coordinator.enqueueJob(
             nowMs(), job->refresh, job->origin,
             [&](uint64_t id) {
               job->id = id;
               return workerTask && jobs && xQueueSend(jobs, &job, 0) == pdTRUE;
             },
             stopping.load()) != 0;
}
void rejectInput(const std::string& input, const RuntimeCoordinator::Admission& admission) {
  notice = admission.notice;
  transientNotice = true;
  log("input=" + input + " rejected: " + admission.reason);
}
void submit(const std::string& payload, bool refresh = false, bool cycle = false,
            bool announce = true,
            std::optional<std::pair<uint32_t, uint32_t>> queuePage = std::nullopt,
            const BoardEvent* uiEvent = nullptr) {
  const std::string input = cycle ? "room-next" : refresh ? "refresh" : "intent";
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  auto admission = coordinator.admission(nowMs(), refresh, JobOrigin::User, stopping.load());
  xSemaphoreGive(stateMutex);
  if (!admission.allowed) {
    rejectInput(input, admission);
    return;
  }
  MusicIntent intent;
  Result result;
  if (!refresh)
    result =
        uiEvent ? (intent = uiEvent->intent, validateIntent(intent)) : parseIntent(payload, intent);
  if (!result.ok) {
    notice = result.error;
    transientNotice = true;
    log("Rejected: " + result.error);
    return;
  }
  auto job = std::make_unique<Job>(Job{refresh, cycle, {}});
  job->queuePage = queuePage;
  Room acceptedRoom;
  Plan plan;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  admission = coordinator.admission(nowMs(), refresh, JobOrigin::User, stopping.load());
  const auto* selected = selection.selected();
  if (admission.allowed && uiEvent && !runtimeUiMatches(selected, sharedState.observed, *uiEvent))
    result = Result::fail("Room changed - try again");
  if (admission.allowed && result.ok && uiEvent)
    job->readTarget = uiEvent->targetId;
  if (admission.allowed && result.ok && !refresh) {
    if (!selected || !selected->eligible || selected->address.empty())
      result = Result::fail("Selected room unavailable; refresh targets");
    else {
      acceptedRoom = *selected;
      job->accepted = resolvePolicy(
          intent, {selected->id, selection.resolvedPolicies, config.revision, config.policy});
      result = makePlan(job->accepted, plan);
    }
  }
  // Capture diagnostics before transferring ownership to the worker.
  const auto description = !refresh && result.ok ? describeIntent(intent, job->accepted) : "";
  const bool queued = admission.allowed && result.ok && enqueueJobLocked(job.get());
  if (queued)
    job.release();
  xSemaphoreGive(stateMutex);
  if (!admission.allowed) {
    rejectInput(input, admission);
    return;
  }
  if (!result.ok) {
    notice = result.error;
    transientNotice = true;
    log("input=" + input + " rejected: " + result.error);
    return;
  }
  if (!queued) {
    notice = "Worker unavailable";
    transientNotice = true;
    log("input=" + input + " rejected: queue unavailable");
    return;
  }
  if (!refresh) {
    log("accepted room=" + acceptedRoom.name + " roomDisplayId=" + acceptedRoom.displayId + " " +
        description);
    std::string effects;
    for (auto op : plan.operations)
      effects += std::string(operationName(op)) + " ";
    log("planned effects=" + effects);
  }
  if (announce) {
    log("input=" + input + " accepted");
    notice = cycle ? "Switching room" : refresh ? "Refreshing rooms/state" : "Intent accepted";
    transientNotice = true;
  }
}
// Direct room selection uses only the configured/eligible projection. Clear
// the observation only after the validated read has entered the queue.
void selectRoom(const std::string& displayId) {
  auto job = std::make_unique<Job>(Job{true, false, {}});
  job->roomChanged = true;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto admission = coordinator.admission(nowMs(), true, JobOrigin::User, stopping.load());
  const auto it = std::find_if(selection.rooms.begin(), selection.rooms.end(), [&](const Room& r) {
    return r.displayId == displayId && r.eligible && !r.address.empty();
  });
  const bool valid = it != selection.rooms.end();
  const bool queued = admission.allowed && valid && enqueueJobLocked(job.get());
  if (queued) {
    job.release();
    selection.selectedId = it->id;
    selection.preferredId = it->displayId;
    selection.initialized = true;
    selectObservedRoom(sharedState, *it);
  }
  xSemaphoreGive(stateMutex);
  if (!admission.allowed) {
    rejectInput("room-select", admission);
    return;
  }
  if (!queued) {
    notice = valid ? "Worker unavailable" : "Room unavailable - refresh";
    log("input=room-select rejected: " + notice);
  } else {
    notice = "Reading selected room";
    log("room-select accepted displayId=" + displayId +
        " (observation cleared; no playback intent)");
  }
  transientNotice = true;
}
void submitToggle() {
  auto job = std::make_unique<Job>(Job{false, false, {}});
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto admission = coordinator.admission(nowMs(), false, JobOrigin::User, stopping.load());
  const auto* selected = selection.selected();
  const Room room = selected ? *selected : Room{};
  const bool valid = room.eligible && !room.address.empty();
  if (admission.allowed && valid)
    job->toggleContext =
        PolicyContext{room.id, selection.resolvedPolicies, config.revision, config.policy};
  const auto revision = config.revision;
  const bool queued = admission.allowed && valid && enqueueJobLocked(job.get());
  if (queued)
    job.release();
  xSemaphoreGive(stateMutex);
  if (!admission.allowed) {
    rejectInput("toggle", admission);
    return;
  }
  if (!queued) {
    notice = valid ? "Worker unavailable" : "Selected room unavailable; refresh targets";
    log("input=toggle rejected: " + notice);
  } else {
    log("toggle requested room=" + room.name + " roomDisplayId=" + room.displayId +
        " uuid=" + room.id + " policyRevision=" + std::to_string(revision));
    notice = "Reading play/pause state";
  }
  transientNotice = true;
}
std::string command(const char* transport) {
  return Json{{"format", "sonos-surface"}, {"version", 1}, {"intent", {{"transport", transport}}}}
      .dump();
}
// Pure policy/plan diagnostic, including in CONTROL mode. Never queues a job or
// calls Sonos; uses exactly the normal parser, resolver, and planner.
void preview(const std::string& payload) {
  MusicIntent input;
  auto result = parseIntent(payload, input);
  ResolvedIntent accepted;
  Room room;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto* selected = selection.selected();
  if (result.ok && !selected)
    result = Result::fail("Selected room unavailable; refresh targets");
  if (result.ok) {
    room = *selected;
    accepted =
        resolvePolicy(input, {room.id, selection.resolvedPolicies, config.revision, config.policy});
  }
  xSemaphoreGive(stateMutex);
  Plan plan;
  if (result.ok)
    result = makePlan(accepted, plan);
  if (!result.ok) {
    log("PREVIEW_INVALID " + result.error);
    return;
  }
  log("policy-preview roomDisplayId=" + room.displayId + " " + describeIntent(input, accepted));
  std::string effects;
  for (auto op : plan.operations)
    effects += std::string(operationName(op)) + " ";
  log("PREVIEW_ONLY planned effects=" + effects);
}
void lifecycleStatus() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto snapshots = coordinator.snapshots();
  xSemaphoreGive(stateMutex);
  // Build fields incrementally: nested initializer lists enlarge the caller's
  // stack frame and leave too little of the Arduino loop stack for board commands.
  Json status = Json::object();
  auto& worker = status["worker"];
  worker["state"] = snapshots.worker.running ? "Running" : "Idle";
  worker["origin"] = snapshots.worker.running ? originName(snapshots.worker.origin) : "none";
  // busy is a running user job; backgroundActive a running automatic read.
  worker["busy"] = snapshots.worker.running && snapshots.worker.origin == JobOrigin::User;
  worker["backgroundActive"] =
      snapshots.worker.running && snapshots.worker.origin == JobOrigin::Automatic;
  worker["jobId"] = snapshots.worker.jobId;
  worker["startedAt"] = snapshots.worker.startedAt;
  worker["deadline"] = snapshots.worker.deadline;
  worker["lastJobId"] = snapshots.worker.lastJobId;
  worker["lastOutcome"] = outcomeName(snapshots.worker.lastOutcome);
  worker["lastOrigin"] = originName(snapshots.worker.lastOrigin);
  worker["preempted"] = snapshots.worker.preempted;
  worker["ignoredPreempts"] = snapshots.worker.ignoredPreempts;
  worker["accepted"] = snapshots.worker.accepted;
  worker["completed"] = snapshots.worker.completed;
  worker["staleResults"] = snapshots.worker.staleResults;
  worker["taskAvailable"] = workerTask != nullptr;
  auto& wifi = status["wifi"];
  wifi["state"] = wifiStateName(snapshots.wifi.state);
  wifi["since"] = snapshots.wifi.since;
  wifi["deadline"] = snapshots.wifi.deadline;
  wifi["retryAt"] = snapshots.wifi.retryAt;
  wifi["attempts"] = snapshots.wifi.attempts;
  wifi["lastError"] = wifiErrorName(snapshots.wifi.lastError);
  wifi["powerSave"] = WiFi.getSleep() != WIFI_PS_NONE;
  auto& transport = status["transport"];
  if (const auto& failure = snapshots.lastTransportFailure) {
    auto& last = transport["lastFailure"];
    last["phase"] = failure->phase;
    last["host"] = failure->host;
    last["action"] = failure->action;
    last["error"] = failure->error;
    last["code"] = failure->code;
    last["errno"] = failure->errorNumber;
    last["elapsedMs"] = failure->elapsedMs;
    last["at"] = failure->at;
  } else
    transport["lastFailure"] = nullptr;
  auto& sonos = status["sonos"];
  sonos["state"] = sonosStateName(snapshots.sonos.state);
  sonos["since"] = snapshots.sonos.since;
  sonos["deadline"] = snapshots.sonos.deadline;
  sonos["retryAt"] = snapshots.sonos.retryAt;
  sonos["discoveryId"] = snapshots.sonos.discoveryId;
  sonos["lastSuccessfulDiscovery"] = snapshots.sonos.lastSuccessfulDiscovery;
  sonos["lastError"] = sonosErrorName(snapshots.sonos.lastError);
  auto& subscription = status["subscription"];
  subscription["state"] = subscriptionStateName(snapshots.subscription.state);
  subscription["sidPresent"] = snapshots.subscription.sidPresent;
  subscription["requestId"] = snapshots.subscription.requestId;
  subscription["deadline"] = snapshots.subscription.deadline;
  subscription["retryAt"] = snapshots.subscription.retryAt;
  subscription["renewAt"] = snapshots.subscription.renewAt;
  subscription["leaseUntil"] = snapshots.subscription.leaseUntil;
  subscription["lastNotifyAt"] = snapshots.subscription.lastNotifyAt;
  subscription["lastError"] = subscriptionErrorName(snapshots.subscription.lastError);
  logResponse("lifecycles " + status.dump());
}
void handle(const std::string& line) {
  if (stopping.load())
    return;
  if (line == "reboot") {
    if (!idleForMaintenance()) {
      logResponse("REBOOT_BUSY");
      return;
    }
    restart("REBOOTING");
  } else if (line == "board") {
    // Read-only identification for host tooling: no Sonos work, no activity,
    // and safe while the worker is busy.
    logResponse("board target=" + std::string(hardwareTargetName) + " adapter=\"" + adapterNotice +
                "\" ready=" + std::to_string(int(adapterReady)));
  } else if (boardCommand(line))
    return;
  else if (line.compare(0, 7, "config ") == 0) {
    Config next;
    const auto text = line.substr(7);
    if (!parseConfig(text, next)) {
      logResponse("CONFIG_INVALID (values omitted from log)");
      return;
    }
    if (!idleForMaintenance()) {
      logResponse("CONFIG_BUSY");
      return;
    }
    if (config.revision == UINT32_MAX || !preferences.putUInt("config-rev", config.revision + 1)) {
      logResponse("CONFIG_SAVE_FAILED revision");
      return;
    }
    if (!preferences.putString("config", text.c_str())) {
      logResponse("CONFIG_SAVE_FAILED");
      return;
    }
    restart("CONFIG_SAVED rebooting (credentials not logged)");
  } else if (line == "read-only true" || line == "read-only false") {
    if (!idleForMaintenance()) {
      logResponse("CONFIG_BUSY");
      return;
    }
    auto stored = Json::parse(preferences.getString("config", "{}").c_str(), nullptr, false);
    if (!stored.is_object()) {
      logResponse("CONFIG_INVALID; upload config first");
      return;
    }
    stored["read_only"] = line == "read-only true";
    handle("config " + stored.dump());
  } else if (line.rfind("preview ", 0) == 0)
    preview(line.substr(8));
  else if (line == "config-status") {
    logResponse("device-config " + Json{{"read_only", config.readOnly},
                                        {"sleep_timeout_seconds", config.sleepTimeoutSeconds},
                                        {"rooms", roomConfigJson(config.rooms)},
                                        {"policy", sourcePolicyJson(config.policy)},
                                        {"policyRevision", config.revision}}
                                       .dump());
  } else if (line == "lifecycle-status")
    lifecycleStatus();
  else if (line == "wifi-sleep on" || line == "wifi-sleep off" || line == "wifi-sleep default") {
    // Development diagnostic for power-save measurements; never persisted.
    wifiSleepOverride =
        line == "wifi-sleep default" ? std::nullopt : std::optional<bool>(line == "wifi-sleep on");
    if (wifiSleepOverride && WiFi.status() == WL_CONNECTED)
      WiFi.setSleep(*wifiSleepOverride);
    logResponse("wifi-sleep override=" +
                std::string(!wifiSleepOverride   ? "default"
                            : *wifiSleepOverride ? "on"
                                                 : "off") +
                " power-save=" + std::to_string(int(WiFi.getSleep() != WIFI_PS_NONE)));
  } else if (line == "rooms" || line == "room-next")
    submit("", true, line == "room-next");
  else if (line.rfind("room-select ", 0) == 0)
    selectRoom(line.substr(12));
  else if (line.rfind("queue ", 0) == 0) {
    // JSON array avoids permissive integer parsing and unbounded "count=0".
    const auto page = Json::parse(line.substr(6), nullptr, false);
    if (!page.is_array() || page.size() != 2 || !page[0].is_number_integer() ||
        !page[1].is_number_integer() || page[0] < 0 || page[0] > UINT32_MAX || page[1] < 1 ||
        page[1] > maxQueuePageSize)
      log("QUEUE_INVALID: use queue [start,count], count 1..20");
    else
      submit("", true, false, true,
             std::make_pair(page[0].get<uint32_t>(), page[1].get<uint32_t>()));
  } else if (line == "status")
    submit("", true);
  else if (line == "play")
    submit(command("play"));
  else if (line == "pause")
    submit(command("pause"));
  else if (line == "toggle")
    submitToggle();
  else if (line == "next" || line == "previous")
    submit(command(line.c_str()));
  else if (!line.empty() && (line[0] == '{' || line.compare(0, 8, "https://") == 0))
    submit(line);
  else
    log("Commands: board | rooms | room-next | status | queue [start,count] | play | pause | "
        "toggle | next | previous | config-status | read-only true/false | config {JSON} | preview "
        "URL/intent JSON | lifecycle-status | wifi-sleep on/off/default | URL/intent JSON");
}
} // namespace

RuntimeStatus runtimeStatus() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const auto worker = coordinator.snapshots().worker;
  xSemaphoreGive(stateMutex);
  return {worker.running && worker.origin == JobOrigin::User, uint32_t(worker.completed),
          uint32_t(millis())};
}

void begin() {
  // Buffer complete configuration and intent commands while the display task
  // is busy; the native USB default of 256 RX bytes is insufficient.
  const auto usbRxBytes = Serial.setRxBufferSize(8192);
  Serial.setTxBufferSize(16384); // Fits bounded config replies and ordinary diagnostic bursts.
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0); // A closed/non-reading USB monitor must not delay worker or buttons.
  Serial.printf("[usb] RX buffer=%u bytes\n", unsigned(usbRxBytes));
  Serial.printf("[boot] application reached reset-reason=%d\n", int(esp_reset_reason()));
  stateMutex = xSemaphoreCreateMutex();
  if (!stateMutex) {
    log("FATAL worker allocation");
    return;
  }
  const bool boardReady = boardBegin(notice);
  adapterNotice = notice;
  adapterReady = boardReady;
  log("sonos-surface firmware; " + notice);
  Serial.printf("[board] adapter ready=%d heap=%lu psram=%lu\n", boardReady, ESP.getFreeHeap(),
                ESP.getPsramSize());
  if (!preferences.begin("surface", false))
    log("NVS open failed; USB saves unavailable");
  auto stored = preferences.getString("config", "{}");
  if (!parseConfig(stored.c_str(), config))
    log("Stored config invalid; USB config required");
  power = DevicePower(esp_timer_get_time() / 1000, config.sleepTimeoutSeconds);
  config.revision = preferences.getUInt("config-rev", 1);
  log(config.readOnly ? "SONOS_MODE=READ_ONLY (runtime; all mutations blocked before HTTP)"
                      : "SONOS_MODE=CONTROL (configured eligible rooms)");
  handle("config-status");
  savedPreference = preferences.getString("preferred-id", "").c_str();
  selection.configured = config.rooms;
  selection.restorePreference(savedPreference);
  if (selection.preferredId != savedPreference) {
    if (!preferences.remove("preferred-id"))
      log("Preferred room clear failed");
    savedPreference = selection.preferredId;
  }
  if (!config.ssid.empty()) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    coordinator.configAvailable(nowMs());
    xSemaphoreGive(stateMutex);
    serviceWifi();
  } else
    log("No WiFi configuration; NFC/input diagnostics still active");
  ensureWorker();
  log("READY: NFC/touch; USB commands status/play/pause/config; explicit URL/intent JSON");
}
void loop() {
  if (!stateMutex) {
    vTaskDelay(1);
    return;
  }
  ensureWorker();
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  coordinator.service(nowMs());
  auto transitions = coordinator.drainTransitions();
  const auto feedback = coordinator.drainNotice();
  xSemaphoreGive(stateMutex);
  for (const auto& transition : transitions)
    log(transition);
  if (feedback) {
    notice = *feedback;
    transientNotice = true;
  }
  // Sample physical activity before admission or background work. USB commands
  // do not extend appliance uptime (use timeout=0 during development).
  const auto event = boardPoll();
  bool writerActive = false;
#if defined(SURFACE_STICK_S3)
  writerActive = boardWriterActive();
#endif
  if (power.poll(esp_timer_get_time() / 1000, event.activity, writerActive)) {
    stopping.store(true);
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    coordinator.shutdown(nowMs());
    xSemaphoreGive(stateMutex);
    log("SLEEP_REQUESTED: local inactivity; cancelling device work");
#if defined(SURFACE_WAVESHARE_1_8) && !SURFACE_TOUCH_DIAGNOSTIC
    artworkStop();
#endif
    // No forced task deletion while a worker might hold a network/state lock.
    // Close networking and enter deep sleep promptly; in-flight work is
    // abandoned, with no transient state writes or automatic replay on boot.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    boardPrepareSleep();
    boardSleep();
  }
  serviceWifi();
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (serialOverflow)
        log("USB command exceeds 4096 bytes");
      else
        handle(serialLine);
      serialLine.clear();
      serialOverflow = false;
    } else if (c != '\r') {
      if (serialLine.size() < 4096)
        serialLine += c;
      else
        serialOverflow = true;
    }
  }
  switch (event.input) {
  case Input::Play:
    handle("play");
    break;
  case Input::Pause:
    handle("pause");
    break;
  case Input::Toggle:
    submitToggle();
    break;
  case Input::Next:
    submit(command("next"));
    break;
  case Input::StickPrevious: {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const auto previous =
        stickPreviousEvent(sharedState, selection.selectedId, WiFi.status() == WL_CONNECTED,
                           esp_timer_get_time() / 1000);
    xSemaphoreGive(stateMutex);
    log(previous.intent.seekPositionMs ? "btnB action=restart-current" : "btnB action=previous");
    submit("", false, false, true, std::nullopt, &previous);
    break;
  }
  case Input::RoomNext:
    handle("room-next");
    break;
  case Input::Refresh:
    handle("status");
    break;
  case Input::RoomSelect:
    selectRoom(event.text);
    break;
  case Input::QueuePage:
    submit("", true, false, false, std::make_pair(event.start, event.count), &event);
    break;
  case Input::Intent:
    submit("", false, false, true, std::nullopt, &event);
    break;
  case Input::Payload: {
    MusicIntent parsed;
    auto r = parseIntent(event.text, parsed);
    if (r.ok) {
      notice = "NFC parsed; submitted";
      log("NFC parsed " + (parsed.source ? parsed.source->url : "transport/settings"));
      submit(event.text);
    } else {
      notice = r.error;
      log("NFC parse error: " + r.error);
    }
    break;
  }
  case Input::Error:
    notice = event.text;
    transientNotice = false;
    log("Input error: " + notice);
    break;
  case Input::None:
    break;
  }
  static uint32_t lastRender = 0, lastHeartbeat = 0;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool online = coordinator.snapshots().wifi.networkReady;
  if (coordinator.automaticJobDue(nowMs(), workerTask != nullptr, stopping.load())) {
    auto job = std::make_unique<Job>(Job{true, false, {}});
    job->origin = JobOrigin::Automatic;
    if (enqueueJobLocked(job.get()))
      job.release();
  }
  const auto preference = selection.preferredId;
  xSemaphoreGive(stateMutex);
  // Main task owns NVS writes. An expired worker can never persist an older
  // room over a later selection while it unwinds.
  if (preference != savedPreference && !preference.empty()) {
    savedPreference = preference;
    if (!preferences.putString("preferred-id", preference.c_str()))
      log("Preferred room save failed");
  }
  if (millis() - lastRender >= 100) {
    lastRender = millis();
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    auto state = sharedState;
    BoardContext context;
#if defined(SURFACE_WAVESHARE_1_8)
    context.rooms = selection.rooms;
#endif
    xSemaphoreGive(stateMutex);
    const auto inputNotice = notice;
    const auto activity = workerActivity();
    if (transientNotice && !activity.user) {
      notice = "Ready";
      transientNotice = false;
    }
    context.readOnly = config.readOnly;
    context.online = online;
    context.busy = activity.user;
    context.backgroundActive = activity.automatic;
    // Operational feedback is short-lived; diagnostics stay in serial/AppState.
    if (inputNotice == "Busy; input ignored" || inputNotice == "Busy - try again" ||
        inputNotice == "Room changed - try again" || inputNotice == "Room unavailable - refresh" ||
        inputNotice == "Worker unavailable" || inputNotice.find("Sonos recovering;") == 0 ||
        inputNotice == "Sonos unavailable; recovering" ||
        inputNotice == "WiFi not configured; Sonos unavailable")
      context.feedback = inputNotice;
    boardContext(context);
    boardRender(state, inputNotice);
  }
  if (millis() - lastHeartbeat >= 5000) {
    lastHeartbeat = millis();
    // inactivity is the local-activity timer sleep uses: USB commands and
    // injected input must never reset it.
    const auto activity = workerActivity();
    log("heartbeat wifi=" + std::to_string(WiFi.status()) + " busy=" +
        std::to_string(activity.user) + " background=" + std::to_string(activity.automatic) +
        " heap=" + std::to_string(ESP.getFreeHeap()) +
        " psram=" + std::to_string(ESP.getFreePsram()) +
        " inactivity=" + std::to_string(power.inactivityMs(nowMs()) / 1000));
  }
  vTaskDelay(1); // Yield the UI task; no Sonos ordering depends on this scheduler tick.
}
} // namespace surface::device
