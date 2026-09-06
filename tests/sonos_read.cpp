// Read-only hardware diagnostic using the exact Sonos adapter built into both boards.
#include <SurfaceSonos.h>
#include <curl/curl.h>
#include <arpa/inet.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <surface_json.hpp>

class CurlReadOnly : public surface::LocalHttp {
public:
  explicit CurlReadOnly(std::string host) : host_(std::move(host)) {}
  surface::HttpResponse request(const std::string& path, const std::string& action, const std::string& body) override {
    if (!surface::isReadOnlySonosAction(action)) return {0, "", "Read-only diagnostic refuses mutations", true};
    CURL* curl = curl_easy_init();
    if (!curl) return {0, "", "curl initialization failed"};
    surface::HttpResponse response;
    const auto url = "http://" + host_ + ":1400" + path;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* p, size_t size, size_t count, void* data) -> size_t {
      auto& text = *static_cast<std::string*>(data);
      if (size * count > 65536 - text.size()) return 0;
      text.append(p, size * count); return size * count;
    });
    curl_slist* headers = nullptr;
    if (!action.empty()) {
      headers = curl_slist_append(headers, "Content-Type: text/xml; charset=\"utf-8\"");
      headers = curl_slist_append(headers, ("SOAPACTION: \"" + action + "\"").c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    auto status = curl_easy_perform(curl);
    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    response.status = status == CURLE_OK ? static_cast<int>(httpStatus) : 0;
    if (status != CURLE_OK) response.error = curl_easy_strerror(status);
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
    return response;
  }
  std::string baseUrl() const override { return "http://" + host_ + ":1400"; }
  uint64_t nowMs() override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  void pollWait(uint32_t ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
private:
  std::string host_;
};
int main(int argc, char** argv) {
  in_addr ip{};
  if (argc < 2 || inet_pton(AF_INET, argv[1], &ip) != 1) {
    std::cerr << "Usage: sonos_read SPEAKER_IPV4 [EXPECTED_UID]\n"; return 2;
  }
  CurlReadOnly http(argv[1]);
  surface::DirectSonos sonos(http, {argc > 2 ? argv[2] : "", "52231"},
                             [](const std::string& line) { std::cout << line << '\n'; });
  using Json = nlohmann::json;
  auto integer = [&](int arg, uint32_t fallback, uint32_t max) {
    if (argc <= arg) return fallback;
    std::string text = argv[arg];
    if (text.empty() || text.size() > 10 || text.find_first_not_of("0123456789") != std::string::npos ||
        std::stoull(text) > max) { std::cerr << "Invalid numeric argument\n"; std::exit(2); }
    return static_cast<uint32_t>(std::stoull(text));
  };
  const auto start = integer(3, 0, UINT32_MAX), count = integer(4, 2, surface::maxQueuePageSize);
  const auto samples = integer(5, 1, 360), interval = integer(6, 10000, 60000);
  if (!count || !samples) return 2;
  auto optional = [](auto n) { return n ? Json(*n) : Json(nullptr); };
  for (uint32_t sample = 0; sample < samples; ++sample) {
    if (sample) http.pollWait(interval);
    surface::PlaybackState state;
    auto result = sonos.refresh(state);
    if (!result.ok) { std::cerr << result.error << '\n'; return 1; }
    std::cout << "OBSERVATION " << Json{{"target", state.targetId}, {"roomDisplayId", state.roomDisplayId},
        {"atMs", state.observedAtMs}, {"playback", state.playback}, {"source", static_cast<int>(state.source)},
        {"title", state.title}, {"artist", state.artist}, {"album", state.album}, {"artwork", state.artwork},
        {"positionMs", optional(state.positionMs)}, {"durationMs", optional(state.durationMs)},
        {"queueBacked", optional(state.queueBacked)}, {"queueIndex", optional(state.queueIndex)},
        {"queueTotal", optional(state.queueTotal)}, {"queueRevision", optional(state.queueRevision)},
        {"volume", optional(state.volume)}, {"mute", optional(state.mute)}, {"shuffle", optional(state.shuffle)},
        {"repeat", state.repeat ? Json(static_cast<int>(*state.repeat)) : Json(nullptr)}, {"queueError", state.queueError}}.dump() << '\n';
    surface::QueuePage page;
    result = sonos.queue(start, count, page);
    if (!result.ok) { std::cerr << result.error << '\n'; return 1; }
    std::cout << "QUEUE " << Json{{"target", page.targetId}, {"start", page.start}, {"total", page.total},
        {"revision", page.revision}, {"count", page.items.size()}}.dump() << '\n';
    for (const auto& item : page.items)
      std::cout << "ITEM " << Json{{"index", item.index}, {"title", item.title}, {"artist", item.artist},
          {"album", item.album}, {"artwork", item.artwork}, {"uri", item.uri}, {"durationMs", optional(item.durationMs)}}.dump() << '\n';
    std::cout << "READ_ONLY_OK " << state.targetId << '\n' << std::flush;
  }
}
