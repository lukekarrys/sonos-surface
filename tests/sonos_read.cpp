// Read-only hardware diagnostic using the exact Sonos adapter built into both boards.
#include <SurfaceSonos.h>
#include <curl/curl.h>
#include <arpa/inet.h>
#include <chrono>
#include <iostream>
#include <thread>

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
  surface::PlaybackState state;
  auto result = sonos.refresh(state);
  if (!result.ok) { std::cerr << result.error << '\n'; return 1; }
  if (argc > 2) {
    // Preflight only: checks grouping and identity, dispatches no mutation.
    surface::MusicIntent intent;
    intent.transport = surface::TransportCommand::Play;
    result = sonos.prepare({intent, "preserve", 1});
    if (!result.ok) { std::cerr << "Preflight: " << result.error << '\n'; return 1; }
  }
  std::cout << "READ_ONLY_OK " << state.targetId << " " << state.playback << " " << state.title << '\n';
}
