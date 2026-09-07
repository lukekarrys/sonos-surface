#pragma once
#include <SurfaceCore.h>
namespace surface::device {
// All calls from the UI task. HTTP and decoding run on a separate bounded worker.
bool artworkUpdate(const PlaybackState& state, bool online, uint32_t now);
const uint16_t* artworkPixels();
} // namespace surface::device
