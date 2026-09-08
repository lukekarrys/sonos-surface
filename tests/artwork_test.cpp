#include "ArtworkState.h"
#include <cassert>
#include <iostream>
using namespace surface;
using namespace surface::device;
int main() {
  ArtworkState art;
  PlaybackState a;
  a.targetId = "room-a";
  a.trackUri = "track-a";
  a.artwork = "http://speaker/getaa?a";
  a.known = true;
  assert(art.select(a));
  const auto first = art.generation;
  assert(!art.select(a) && art.wantsLoad(true, false, 0));
  assert(!art.wantsLoad(false, false, 0) && !art.wantsLoad(true, true, 0));
  assert(art.complete(first, true, 1) && art.ready && !art.wantsLoad(true, false, 2));
  auto b = a;
  b.targetId = "room-b";
  assert(art.select(b) && !art.ready);
  assert(!art.complete(first, true, 3) &&
         !art.ready); // Same URL in another room is still new ownership.
  const auto second = art.generation;
  b.trackUri = "track-b";
  assert(art.select(b));
  assert(!art.complete(second, false, 4) &&
         !art.failed); // Old failures cannot delay the new track.
  assert(art.complete(art.generation, false, 100) && !art.wantsLoad(true, false, 30099));
  assert(art.wantsLoad(true, false, 30100));
  b.known = false;
  assert(art.select(b) && !art.wantsLoad(true, false, 60000));
  b.known = true;
  b.artwork = "";
  assert(!art.select(b) && !art.wantsLoad(true, false, 60000));
  b.artwork = "https://speaker/art";
  assert(art.select(b) && !art.wantsLoad(true, false, 60000));
  b.artwork = "http://speaker/" + std::string(2048, 'x');
  assert(art.select(b) && !art.wantsLoad(true, false, 60000));
  assert(art.select(a));
  assert(art.complete(art.generation, false, UINT32_MAX - 10000));
  assert(!art.wantsLoad(true, false, 1000) && art.wantsLoad(true, false, 20000)); // Clock wrap.
  uint16_t target[artworkSize * artworkSize];
  uint16_t square[] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
  artworkThumbnail(square, 2, 2, target);
  assert(target[0] == 0xF800 && target[63] == 0x07E0 && target[63 * 64] == 0x001F &&
         target[4095] == 0xFFFF);
  uint16_t wide[] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
  artworkThumbnail(wide, 4, 1, target);
  assert(target[0] == 0 && target[24 * 64] == 0xF800 && target[39 * 64 + 63] == 0xFFFF &&
         target[40 * 64] == 0);
  artworkThumbnail(nullptr, 0, 0, target);
  for (auto p : target)
    assert(p == 0);
  std::cout << "Artwork checks passed: identity/generation, stale completion, retries, bounds, "
               "offline, thumbnail fit/colors\n";
}
