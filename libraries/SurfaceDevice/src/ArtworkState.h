#pragma once
#include <SurfaceCore.h>
#include <algorithm>
#include <cstdint>

namespace surface::device {
constexpr unsigned artworkSize = 64;
constexpr size_t maxArtworkBytes = 256 * 1024;
// Main-task lifecycle only. A generation belongs to one room/track/URL tuple,
// including an empty/loading selection. Old worker results can never reappear.
class ArtworkState {
  std::string room, track, url;
  uint32_t retryAt = 0;
public:
  uint32_t generation = 0;
  bool ready = false, failed = false;
  bool select(const PlaybackState& state) {
    const auto nextUrl = state.known ? state.artwork : std::string{};
    if (room == state.targetId && track == state.trackUri && url == nextUrl) return false;
    room = state.targetId; track = state.trackUri; url = nextUrl;
    ++generation; ready = failed = false; retryAt = 0;
    return true;
  }
  const std::string& address() const { return url; }
  bool wantsLoad(bool online, bool stale, uint32_t now) const {
    return online && !stale && !ready && !url.empty() && url.size() <= 2048 &&
      url.rfind("http://",0) == 0 && (!failed || int32_t(now-retryAt) >= 0);
  }
  bool complete(uint32_t token, bool success, uint32_t now) {
    if (token != generation) return false;
    ready = success; failed = !success; retryAt = now + 30000;
    return true;
  }
};
// Nearest-neighbor contain fit; never distort a non-square cover. Black padding.
inline void artworkThumbnail(const uint16_t* source, unsigned width, unsigned height, uint16_t* target) {
  std::fill(target,target+artworkSize*artworkSize,0);
  if (!source || !width || !height) return;
  unsigned w = artworkSize, h = artworkSize;
  if (width >= height) h = std::max(1u,artworkSize*height/width);
  else w = std::max(1u,artworkSize*width/height);
  const unsigned left = (artworkSize-w)/2, top = (artworkSize-h)/2;
  for (unsigned y = 0; y < h; ++y)
    for (unsigned x = 0; x < w; ++x)
      target[(top+y)*artworkSize+left+x] = source[(uint64_t(y)*height/h)*width + uint64_t(x)*width/w];
}
} // namespace surface::device
