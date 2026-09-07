#if defined(SURFACE_WAVESHARE) && !SURFACE_TOUCH_DIAGNOSTIC
#include "WaveshareArtwork.h"
#include "ArtworkState.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <jpeg_decoder.h>
#include <atomic>
#include <memory>

namespace surface::device {
namespace {
struct FreeBuffer { void operator()(void* p) const { heap_caps_free(p); } };
template<class T> using Buffer = std::unique_ptr<T,FreeBuffer>;
template<class T> Buffer<T> allocate(size_t count) {
  return Buffer<T>(static_cast<T*>(heap_caps_malloc(count*sizeof(T),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
}
struct Request { uint32_t token; std::string url; };
struct Completed { uint32_t token; uint16_t* pixels; };
QueueHandle_t requests = nullptr, completions = nullptr;
std::atomic<uint32_t> wanted{0};
ArtworkState state;
Buffer<uint16_t> visible;
bool running = false, started = false;
uint32_t startRetry = 0;

bool current(uint32_t token) { return wanted.load() == token; }
class Download : public Stream {
  uint8_t* buffer;
  uint32_t token, deadline;
public:
  size_t size = 0;
  bool aborted = false;
  Download(uint8_t* p, uint32_t generation, uint32_t end) : buffer(p), token(generation), deadline(end) {}
  size_t write(uint8_t c) override { return write(&c,1); }
  size_t write(const uint8_t* p, size_t count) override {
    if (!current(token) || int32_t(millis()-deadline) >= 0 || count > maxArtworkBytes-size) {
      aborted = true; return 0;
    }
    memcpy(buffer+size,p,count); size += count; return count;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};
Buffer<uint16_t> load(const Request& request) {
  const auto began = millis();
  const auto beforeHeap = ESP.getFreeHeap(), beforePsram = ESP.getFreePsram();
  const char* error = "download failed";
  unsigned width = 0, height = 0;
  size_t bytes = 0;
  uint32_t fetched = 0, decoded = 0;
  Buffer<uint16_t> thumbnail;
  // These buffers are released before the final memory measurement/publication.
  do {
    if (!current(request.token) || WiFi.status() != WL_CONNECTED) { error = "cancelled/offline"; break; }
    auto input = allocate<uint8_t>(maxArtworkBytes);
    if (!input) { error = "download allocation"; break; }
    HTTPClient http;
    http.setConnectTimeout(1500); http.setTimeout(2000);
    http.setReuse(false);
    // Speaker-proxied HTTP artwork only. No credentials, redirects, or writes.
    // HTTPS/PNG/progressive JPEG fall back to the placeholder in this slice.
    if (!http.begin(request.url.c_str())) { error = "HTTP begin"; break; }
    const auto status = http.GET();
    Download sink(input.get(),request.token,began+6000);
    const int expected = http.getSize();
    const auto read = status == 200 && expected <= int(maxArtworkBytes) ? http.writeToStream(&sink) : -1;
    http.end();
    bytes = sink.size; fetched = millis()-began;
    if (status != 200 || read < 0 || sink.aborted || !bytes || (expected >= 0 && bytes != size_t(expected))) {
      error = "HTTP status/size/timeout/cancel"; break;
    }
    if (!current(request.token)) { error = "superseded"; break; }
    if (bytes < 2 || input.get()[0] != 0xFF || input.get()[1] != 0xD8) { error = "not JPEG"; break; }
    esp_jpeg_image_cfg_t cfg{};
    cfg.indata = input.get(); cfg.indata_size = bytes;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    esp_jpeg_image_output_t info{};
    if (esp_jpeg_get_image_info(&cfg,&info) != ESP_OK || !info.width || !info.height ||
        info.width > 2048 || info.height > 2048) { error = "JPEG dimensions/header"; break; }
    width = info.width; height = info.height;
    unsigned scale = 0;
    while (scale < 3 && (std::max(width,height) >> (scale+1)) >= artworkSize) ++scale;
    cfg.out_scale = static_cast<esp_jpeg_image_scale_t>(scale);
    if (esp_jpeg_get_image_info(&cfg,&info) != ESP_OK || !info.output_len || info.output_len > 128*1024) {
      error = "JPEG output bound"; break;
    }
    auto output = allocate<uint8_t>(info.output_len);
    if (!output) { error = "decode allocation"; break; }
    cfg.outbuf = output.get(); cfg.outbuf_size = info.output_len;
    const auto decodeStart = millis();
    if (esp_jpeg_decode(&cfg,&info) != ESP_OK || !info.width || !info.height) { error = "JPEG decode"; break; }
    if (!current(request.token)) { error = "superseded during decode"; break; }
    thumbnail = allocate<uint16_t>(artworkSize*artworkSize);
    if (!thumbnail) { error = "thumbnail allocation"; break; }
    artworkThumbnail(reinterpret_cast<uint16_t*>(output.get()),info.width,info.height,thumbnail.get());
    decoded = millis()-decodeStart; error = "ok";
    Serial.printf("[artwork] working token=%lu scaled=%ux%u heap=%u psram-free=%u\n",
      request.token,info.width,info.height,ESP.getFreeHeap(),ESP.getFreePsram());
  } while (false);
  Serial.printf("[artwork] token=%lu result=%s bytes=%u source=%ux%u fetch=%lu decode=%lu total=%lu heap-before=%u heap-after=%u psram-before=%u psram-after=%u\n",
    request.token,error,unsigned(bytes),width,height,fetched,decoded,millis()-began,
    beforeHeap,ESP.getFreeHeap(),beforePsram,ESP.getFreePsram());
  return thumbnail;
}
void worker(void*) {
  for (;;) {
    Request* request = nullptr;
    if (xQueueReceive(requests,&request,portMAX_DELAY) != pdTRUE) continue;
    auto pixels = load(*request);
    Completed result{request->token,pixels.release()};
    delete request;
    // One in-flight job and one completion; UI never waits for this worker.
    if (xQueueSend(completions,&result,0) != pdTRUE) heap_caps_free(result.pixels);
  }
}
bool beginWorker(uint32_t now) {
  if (started) return true;
  if (int32_t(now-startRetry) < 0) return false;
  startRetry = now+30000;
  requests = xQueueCreate(1,sizeof(Request*));
  completions = xQueueCreate(1,sizeof(Completed));
  if (requests && completions && xTaskCreate(worker,"surface-art",8192,nullptr,1,nullptr) == pdPASS) {
    started = true; return true;
  }
  if (requests) vQueueDelete(requests);
  if (completions) vQueueDelete(completions);
  requests = completions = nullptr;
  Serial.println("[artwork] worker allocation failed; placeholder retained");
  return false;
}
} // namespace
bool artworkUpdate(const PlaybackState& observed, bool online, uint32_t now) {
  bool changed = state.select(observed);
  if (changed) {
    wanted.store(state.generation);
    visible.reset(); // Clear old-room/track pixels on the UI task immediately.
  }
  if (started) {
    Completed result{};
    if (xQueueReceive(completions,&result,0) == pdTRUE) {
      running = false;
      Buffer<uint16_t> pixels(result.pixels);
      if (state.complete(result.token,bool(pixels),now)) {
        visible = std::move(pixels); changed = true;
        Serial.printf("[artwork] publish token=%lu ready=%d room=%s\n",result.token,state.ready,observed.room.c_str());
      } else Serial.printf("[artwork] discarded stale token=%lu\n",result.token);
    }
  }
  if (!running && state.wantsLoad(online,observed.stale,now) && beginWorker(now)) {
    auto request = new Request{state.generation,state.address()};
    if (xQueueSend(requests,&request,0) == pdTRUE) running = true;
    else { delete request; state.complete(state.generation,false,now); }
  }
  return changed;
}
const uint16_t* artworkPixels() { return visible.get(); }
} // namespace surface::device
#endif
