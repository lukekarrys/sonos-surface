#include "CardWriter.h"
#include <set>

namespace surface {
using Json = nlohmann::json;
namespace {
Json fields(const MusicIntent& i) {
  Json j = Json::object();
  if (i.source)
    j["source"] = {{"service", "apple-music"}, {"url", i.source->url}};
  if (i.transport) {
    const char* names[] = {"play", "pause", "next", "previous"};
    j["transport"] = names[static_cast<unsigned>(*i.transport)];
  }
  if (i.shuffle)
    j["shuffle"] = *i.shuffle;
  if (i.repeat)
    j["repeat"] = repeatName(*i.repeat);
  if (i.volume)
    j["volume"] = {{i.volume->relative ? "delta" : "set", i.volume->value}};
  if (i.seekPositionMs)
    j["seek"] = {{"positionMs", *i.seekPositionMs}};
  if (i.queueIndex)
    j["queueIndex"] = *i.queueIndex;
  return j;
}
// API values are flat scalars, never an arbitrary JSON document. Bound nesting
// before parsing, and reject duplicate keys rather than accepting last-wins.
bool apiBody(const std::string& body, Json& out) {
  if (body.size() > 4608)
    return false;
  bool quoted = false, escaped = false;
  int depth = 0;
  for (char c : body) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[') {
      if (++depth > 1)
        return false;
    } else if (c == '}' || c == ']') {
      if (--depth < 0)
        return false;
    }
  }
  bool duplicate = false;
  std::set<std::string> keys;
  out = Json::parse(
      body,
      [&](int, Json::parse_event_t event, Json& value) {
        if (event == Json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
          duplicate = true;
        return true;
      },
      false);
  return !duplicate && out.is_object();
}
Json choices(SourceKind kind) {
  Json j = {{"shuffle", Json::array({"default"})}, {"repeat", Json::array({"default"})}};
  for (bool value : {true, false})
    if (validateSourceModes(kind, {value, {}}).ok)
      j["shuffle"].push_back(value ? "on" : "off");
  for (Repeat value : {Repeat::Off, Repeat::All, Repeat::One})
    if (validateSourceModes(kind, {{}, value}).ok)
      j["repeat"].push_back(repeatName(value));
  return j;
}
} // namespace
Result parseCardDocument(const std::string& payload, CardDocument& output) {
  CardDocument card;
  auto result = parseIntent(payload, card.intent);
  if (!result.ok)
    return result;
  const auto start = payload.find_first_not_of(" \r\n\t");
  if (start != std::string::npos && payload[start] == '{') {
    // Already bounded and fully validated by the normal parser.
    auto document = Json::parse(payload);
    for (const char* key : {"label", "extensions", "requires"})
      if (document.contains(key))
        card.metadata[key] = document[key];
  }
  output = std::move(card);
  return {};
}
Result serializeCardDocument(const CardDocument& card, std::string& output) {
  auto result = validateIntent(card.intent);
  if (!result.ok)
    return result;
  Json document = card.metadata;
  if (!document.is_object())
    return Result::fail("Invalid card metadata");
  document["format"] = "sonos-surface";
  document["version"] = 1;
  document["intent"] = fields(card.intent);
  auto text = document.dump();
  MusicIntent parsed;
  result = parseIntent(text, parsed);
  if (result.ok)
    output = std::move(text);
  return result;
}
Result encodeCardPayload(const CardDocument& card, std::string& output) {
  const auto& intent = card.intent;
  auto result = validateIntent(intent);
  if (!result.ok)
    return result;
  if (!card.metadata.is_object())
    return Result::fail("Invalid card metadata");
  if (!intent.source || intent.transport != TransportCommand::Play || intent.volume ||
      intent.seekPositionMs || intent.queueIndex || !card.metadata.empty())
    return serializeCardDocument(card, output);
  std::string options;
  if (intent.shuffle.has_value())
    options = *intent.shuffle ? "s=1" : "s=0";
  if (intent.repeat) {
    if (!options.empty())
      options += ',';
    options += *intent.repeat == Repeat::Off   ? "r=0"
               : *intent.repeat == Repeat::One ? "r=1"
                                               : "r=a";
  }
  auto payload = (options.empty() ? "" : "ss1:" + options + ";") + intent.source->url;
  MusicIntent parsed;
  result = parseIntent(payload, parsed);
  if (result.ok)
    output = std::move(payload);
  return result;
}
const char* cardEncodingName(const std::string& payload) {
  if (payload.empty())
    return "none";
  if (payload.compare(0, 8, "https://") == 0)
    return "url";
  return payload.compare(0, 4, "ss1:") == 0 ? "ss1" : "json";
}
bool sameCard(const CardDocument& a, const CardDocument& b) {
  if (!validateIntent(a.intent).ok || !validateIntent(b.intent).ok)
    return false;
  return fields(a.intent) == fields(b.intent) && a.metadata == b.metadata;
}
Result authorCard(const std::string& url, std::optional<bool> shuffle, std::optional<Repeat> repeat,
                  const CardDocument* base, CardDocument& output) {
  CardDocument card = base ? *base : CardDocument{};
  Source source;
  auto result = normalizeAppleUrl(url, source);
  if (!result.ok)
    return result;
  card.intent.source = source;
  card.intent.transport = TransportCommand::Play;
  card.intent.shuffle = shuffle;
  card.intent.repeat = repeat;
  result = validateIntent(card.intent);
  if (!result.ok)
    return Result::fail((base ? "Existing advanced setting conflicts with this source: " : "") +
                        result.error);
  output = std::move(card);
  return {};
}
std::vector<uint8_t> cardNdef(const std::string& payload) {
  // URI RTD 0x04 abbreviates https:// using the already-supported reader path.
  // Compact/JSON cards use one UTF-8/en Text record. Both have a Type 2 TLV/terminator.
  const bool uri = payload.compare(0, 8, "https://") == 0;
  const size_t payloadSize = uri ? payload.size() - 7 : payload.size() + 3;
  const bool shortRecord = payloadSize < 256;
  const size_t recordSize = payloadSize + (shortRecord ? 4 : 7);
  std::vector<uint8_t> bytes{0x03};
  if (recordSize < 255)
    bytes.push_back(uint8_t(recordSize));
  else {
    bytes.push_back(0xff);
    bytes.push_back(uint8_t(recordSize >> 8));
    bytes.push_back(uint8_t(recordSize));
  }
  bytes.push_back(shortRecord ? 0xd1 : 0xc1);
  bytes.push_back(1);
  if (!shortRecord) {
    bytes.push_back(uint8_t(payloadSize >> 24));
    bytes.push_back(uint8_t(payloadSize >> 16));
    bytes.push_back(uint8_t(payloadSize >> 8));
  }
  bytes.push_back(uint8_t(payloadSize));
  if (uri) {
    bytes.insert(bytes.end(), {'U', 0x04});
    bytes.insert(bytes.end(), payload.begin() + 8, payload.end());
  } else {
    bytes.insert(bytes.end(), {'T', 2, 'e', 'n'});
    bytes.insert(bytes.end(), payload.begin(), payload.end());
  }
  bytes.push_back(0xfe);
  return bytes;
}
const char* writerStateName(WriterState value) {
  switch (value) {
  case WriterState::Idle:
    return "idle";
  case WriterState::ArmedRead:
    return "armed-read";
  case WriterState::ArmedWrite:
    return "armed-write";
  case WriterState::Detected:
    return "card-detected";
  case WriterState::Reading:
    return "reading";
  case WriterState::Writing:
    return "writing";
  case WriterState::Verifying:
    return "verifying";
  case WriterState::Success:
    return "success";
  case WriterState::Failed:
    return "failed";
  case WriterState::TimedOut:
    return "timed-out";
  }
  return "failed";
}
void CardWriter::state(WriterState value, std::string message) {
  current = value;
  detail = std::move(message);
}
bool CardWriter::active() const {
  return current == WriterState::ArmedRead || current == WriterState::ArmedWrite ||
         current == WriterState::Detected || current == WriterState::Reading ||
         current == WriterState::Writing || current == WriterState::Verifying;
}
bool CardWriter::takeActivity() {
  bool result = activity;
  activity = false;
  return result;
}
void CardWriter::tick(uint64_t now) {
  if (active() && now >= deadline) {
    const bool partial = current == WriterState::Writing || current == WriterState::Verifying;
    state(WriterState::TimedOut, partial
                                     ? "Timed out; card may be incomplete. Remove and arm again."
                                     : "Timed out. Remove card and arm again.");
    owner = CardOwner::Playback;
    // Expiry is not user interaction.
  }
}
void CardWriter::cancel() {
  activity = true;
  const bool partial = current == WriterState::Writing || current == WriterState::Verifying;
  state(WriterState::Idle, partial ? "Cancelled; card may be incomplete. Remove and retry."
                                   : "Cancelled. Remove card before normal playback.");
  owner = CardOwner::Playback;
}
Result CardWriter::armRead(uint64_t now) {
  tick(now);
  if (active())
    return Result::fail("Writer busy; cancel or wait for completion");
  ++sequence;
  activity = true;
  capacity = 0;
  raw.clear();
  serialized.clear();
  deadline = now + armMs;
  owner = CardOwner::Read;
  state(WriterState::ArmedRead,
        "READ / EDIT: present a card within 60 seconds. Playback suppressed.");
  return {};
}
Result CardWriter::armWrite(const std::string& url, std::optional<bool> shuffle,
                            std::optional<Repeat> repeat, uint32_t baseId, uint64_t now) {
  tick(now);
  if (active())
    return Result::fail("Writer busy; cancel or wait for completion");
  if (baseId && (!edited || baseId != editId))
    return Result::fail("Edit expired; read the card again");
  CardDocument card;
  auto result = authorCard(url, shuffle, repeat, baseId ? &*edited : nullptr, card);
  std::string text;
  if (result.ok)
    result = encodeCardPayload(card, text);
  if (!result.ok)
    return result;
  kind = sourceKindName(card.intent.source->kind);
  intended = std::move(card);
  serialized = std::move(text);
  editing = baseId != 0;
  ++sequence;
  activity = true;
  capacity = 0;
  raw.clear();
  deadline = now + armMs;
  owner = CardOwner::Write;
  state(WriterState::ArmedWrite,
        "WRITE: present the card within 60 seconds. Its contents will be replaced.");
  return {};
}
CardOwner CardWriter::present(uint64_t now) {
  tick(now);
  activity = true;
  if (current == WriterState::ArmedRead || current == WriterState::ArmedWrite) {
    state(WriterState::Detected, "Card detected");
    deadline = now + operationMs;
    return owner;
  }
  return active() ? CardOwner::Ignore : CardOwner::Playback;
}
void CardWriter::fail(const std::string& message) {
  if (!active())
    return;
  state(WriterState::Failed, message + ". Remove card; rearm to retry.");
  owner = CardOwner::Playback;
  activity = true;
}
void CardWriter::reading() {
  if (current == WriterState::Detected)
    state(WriterState::Reading, "Reading card; playback suppressed");
}
void CardWriter::writing() {
  if (current == WriterState::Detected && owner == CardOwner::Write)
    state(WriterState::Writing, "Writing; keep card still");
}
void CardWriter::verifying() {
  if (current == WriterState::Writing)
    state(WriterState::Verifying, "Verifying through normal card parser");
}
Result CardWriter::read(const std::string& uid, const std::string& payload) {
  if (owner != CardOwner::Read || current != WriterState::Reading)
    return Result::fail("Read is not armed");
  CardDocument card;
  auto result = parseCardDocument(payload, card);
  raw = payload.substr(0, 4096);
  if (!result.ok) {
    fail(result.error);
    return result;
  }
  kind = card.intent.source ? sourceKindName(card.intent.source->kind) : "none";
  encodeCardPayload(card, serialized);
  edited = std::move(card);
  editUid = uid;
  editPayload = payload;
  editId = sequence;
  state(WriterState::Success, "Card loaded. Advanced settings are preserved; rewriting sets "
                              "transport to Play. Remove card before rearming.");
  owner = CardOwner::Playback;
  activity = true;
  return {};
}
Result CardWriter::checkEdit(const std::string& uid, const std::string& payload) const {
  if (!editing && !payload.empty())
    return Result::fail("Card is not empty; use Read / Edit before replacing it");
  if (editing && (uid != editUid || payload != editPayload))
    return Result::fail("Different or changed card; read it again before editing");
  return {};
}
Result CardWriter::verify(const std::string& payload) {
  if (current != WriterState::Verifying || owner != CardOwner::Write)
    return Result::fail("Write is not armed");
  CardDocument actual;
  auto result = parseCardDocument(payload, actual);
  if (result.ok && !sameCard(intended, actual))
    result = Result::fail("Read-back intent differs from draft");
  if (!result.ok) {
    fail("Verification failed: " + result.error);
    return result;
  }
  // A subsequent edit must bind a fresh read, not the pre-write tag contents.
  edited.reset();
  editId = 0;
  state(WriterState::Success,
        "WRITE OK: semantic read-back verified. Remove card. A later normal tap can play it.");
  owner = CardOwner::Playback;
  activity = true;
  return {};
}
Json CardWriter::snapshot() const {
  Json j = {{"state", writerStateName(current)},
            {"detail", detail},
            {"active", active()},
            {"operation", sequence},
            {"payloadBytes", serialized.size()},
            {"encoding", cardEncodingName(serialized)},
            {"tagBytes", serialized.empty() ? 0 : cardNdef(serialized).size()},
            {"capacity", capacity},
            {"sourceKind", kind},
            {"maxCardBytes", 4096},
            {"raw", raw}};
  if (edited) {
    const auto& i = edited->intent;
    j["editor"] = {{"id", editId},
                   {"url", i.source ? i.source->url : ""},
                   {"shuffle", i.shuffle ? (*i.shuffle ? "on" : "off") : "default"},
                   {"repeat", i.repeat ? repeatName(*i.repeat) : "default"},
                   {"advanced", bool(i.volume || i.seekPositionMs || i.queueIndex ||
                                     !edited->metadata.empty())}};
  }
  return j;
}
int CardWriter::request(const std::string& method, const std::string& path, const std::string& body,
                        uint64_t now, std::string& response) {
  tick(now);
  auto error = [&](int code, const std::string& message) {
    response = Json{{"error", message}}.dump();
    return code;
  };
  if (path == "/api/status") {
    if (method != "GET" || !body.empty())
      return error(405, "GET required");
    response = snapshot().dump();
    return 200;
  }
  if (path != "/api/source" && path != "/api/write" && path != "/api/read" && path != "/api/cancel")
    return error(404, "Unknown path");
  if (method != "POST")
    return error(405, "POST required");
  if (body.size() > 4608)
    return error(413, "Body exceeds 4608 bytes");
  Json j;
  if (!apiBody(body, j))
    return error(400, "Expected a flat JSON object with unique keys");
  for (auto it = j.begin(); it != j.end(); ++it)
    if (!((path == "/api/write" && (it.key() == "url" || it.key() == "shuffle" ||
                                    it.key() == "repeat" || it.key() == "editId")) ||
          (path == "/api/source" && it.key() == "url")))
      return error(400, "Unknown field");
  // This is an explicit browser action, including a rejected arm. Status GETs
  // have already returned and cannot extend inactivity.
  activity = true;
  if (path == "/api/cancel") {
    cancel();
    response = snapshot().dump();
    return 200;
  }
  if (active() && path != "/api/source")
    return error(409, "Writer busy; cancel or wait for completion");
  if (path == "/api/read") {
    auto result = armRead(now);
    if (!result.ok)
      return error(409, result.error);
  } else {
    if (!j.contains("url") || !j["url"].is_string())
      return error(400, "Apple Music URL is required");
    Source source;
    auto result = normalizeAppleUrl(j["url"].get<std::string>(), source);
    if (!result.ok)
      return error(400, result.error);
    if (path == "/api/source") {
      activity = true;
      response = Json{{"url", source.url},
                      {"kind", sourceKindName(source.kind)},
                      {"choices", choices(source.kind)}}
                     .dump();
      return 200;
    }
    std::optional<bool> shuffle;
    std::optional<Repeat> repeat;
    if (j.contains("shuffle")) {
      if (j["shuffle"] == "on")
        shuffle = true;
      else if (j["shuffle"] == "off")
        shuffle = false;
      else if (j["shuffle"] != "default")
        return error(400, "Invalid shuffle choice");
    }
    if (j.contains("repeat")) {
      if (j["repeat"] == "off")
        repeat = Repeat::Off;
      else if (j["repeat"] == "all")
        repeat = Repeat::All;
      else if (j["repeat"] == "one")
        repeat = Repeat::One;
      else if (j["repeat"] != "default")
        return error(400, "Invalid repeat choice");
    }
    uint32_t baseId = 0;
    if (j.contains("editId")) {
      if (!j["editId"].is_number_integer() || j["editId"] < 0 || j["editId"] > UINT32_MAX)
        return error(400, "Invalid edit ID");
      baseId = j["editId"].get<uint32_t>();
    }
    result = armWrite(source.url, shuffle, repeat, baseId, now);
    if (!result.ok)
      return error(400, result.error);
  }
  response = snapshot().dump();
  return 200;
}
} // namespace surface
