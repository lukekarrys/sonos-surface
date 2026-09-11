#pragma once
#include "SurfaceCore.h"
#include <surface_json.hpp>

namespace surface {
// Card metadata stays outside MusicIntent, but must survive an editor round trip.
struct CardDocument {
  MusicIntent intent;
  nlohmann::json metadata = nlohmann::json::object();
};
Result parseCardDocument(const std::string& payload, CardDocument& output);
Result serializeCardDocument(const CardDocument& card, std::string& output);
// NFC output: URL, ss1, or structured JSON, choosing the smallest lossless form.
Result encodeCardPayload(const CardDocument& card, std::string& output);
const char* cardEncodingName(const std::string& payload);
bool sameCard(const CardDocument& a, const CardDocument& b);
Result authorCard(const std::string& url, std::optional<bool> shuffle, std::optional<Repeat> repeat,
                  const CardDocument* base, CardDocument& output);
std::vector<uint8_t> cardNdef(const std::string& payload);

// One owner is chosen at presentation, before any tag preparation can fail.
// The board retains its existing removal latch through all terminal states.
enum class CardOwner { Playback, Read, Write, Ignore };
enum class WriterState {
  Idle,
  ArmedRead,
  ArmedWrite,
  Detected,
  Reading,
  Writing,
  Verifying,
  Success,
  Failed,
  TimedOut
};
const char* writerStateName(WriterState state);
class CardWriter {
  WriterState current = WriterState::Idle;
  CardOwner owner = CardOwner::Playback;
  uint64_t deadline = 0;
  bool activity = false;
  uint32_t sequence = 0;
  std::optional<CardDocument> edited;
  std::string editUid, editPayload;
  uint32_t editId = 0;
  bool editing = false;
  CardDocument intended;
  std::string serialized, detail, raw, kind;
  size_t capacity = 0;
  void state(WriterState value, std::string message);

public:
  static constexpr uint64_t armMs = 60000, operationMs = 15000;
  WriterState status() const { return current; }
  bool active() const;
  bool takeActivity();
  void tick(uint64_t now);
  void cancel();
  Result armRead(uint64_t now);
  Result armWrite(const std::string& url, std::optional<bool> shuffle, std::optional<Repeat> repeat,
                  uint32_t baseId, uint64_t now);
  CardOwner present(uint64_t now);
  void fail(const std::string& message);
  void reading();
  void writing();
  void verifying();
  Result read(const std::string& uid, const std::string& payload);
  Result checkEdit(const std::string& uid, const std::string& payload) const;
  Result verify(const std::string& payload);
  void setCapacity(size_t bytes) { capacity = bytes; }
  const std::string& payload() const { return serialized; }
  nlohmann::json snapshot() const;
  // Explicit small LAN API; polling never generates activity. No room/config/Sonos dependency.
  int request(const std::string& method, const std::string& path, const std::string& body,
              uint64_t now, std::string& response);
};
} // namespace surface
