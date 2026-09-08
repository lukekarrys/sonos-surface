#include "SurfaceCore.h"
#include <surface_json.hpp>
#include <algorithm>
#include <cctype>
#include <set>

namespace surface {
namespace {
using Json = nlohmann::json;
bool validUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const auto first = static_cast<uint8_t>(s[i++]);
    if (first < 0x80) continue;
    unsigned extra;
    uint32_t code, minimum;
    if (first >= 0xC2 && first <= 0xDF) { extra = 1; code = first & 0x1F; minimum = 0x80; }
    else if (first >= 0xE0 && first <= 0xEF) { extra = 2; code = first & 0x0F; minimum = 0x800; }
    else if (first >= 0xF0 && first <= 0xF4) { extra = 3; code = first & 7; minimum = 0x10000; }
    else return false;
    if (i + extra > s.size()) return false;
    while (extra--) {
      const auto next = static_cast<uint8_t>(s[i++]);
      if ((next & 0xC0) != 0x80) return false;
      code = (code << 6) | (next & 0x3F);
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return false;
  }
  return true;
}
std::string trim(const std::string& s) {
  auto begin = s.find_first_not_of(" \r\n\t");
  return begin == std::string::npos ? "" : s.substr(begin, s.find_last_not_of(" \r\n\t") - begin + 1);
}
bool digits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}
std::vector<std::string> split(const std::string& s, char separator) {
  std::vector<std::string> result;
  size_t start = 0, end;
  do {
    end = s.find(separator, start);
    result.push_back(s.substr(start, end == std::string::npos ? end : end - start));
    start = end + 1;
  } while (end != std::string::npos);
  return result;
}
int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
bool decode(const std::string& s, std::string& out) {
  out.clear();
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = s[i];
    if (c == '%') {
      if (i + 2 >= s.size() || hex(s[i + 1]) < 0 || hex(s[i + 2]) < 0) return false;
      c = hex(s[i + 1]) * 16 + hex(s[i + 2]);
      i += 2;
    }
    if (c < 32 || c == 127) return false;
    out += static_cast<char>(c);
  }
  return true;
}
bool keys(const Json& object, std::initializer_list<const char*> allowed) {
  if (!object.is_object()) return false;
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (std::none_of(allowed.begin(), allowed.end(), [&](const char* key) { return key == it.key(); })) return false;
  }
  return true;
}
bool boundedJson(const std::string& text) {
  // A lexical nesting guard, not a replacement JSON parser.
  bool quoted = false, escaped = false;
  int depth = 0;
  for (char c : text) {
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
    } else if (c == '"') quoted = true;
    else if (c == '{' || c == '[') { if (++depth > 8) return false; }
    else if (c == '}' || c == ']') { if (--depth < 0) return false; }
  }
  return depth == 0 && !quoted;
}
size_t members(const Json& value) {
  size_t total = 0;
  if (value.is_structured()) {
    total += value.size();
    for (const auto& child : value) total += members(child);
  }
  return total;
}
} // namespace

Result normalizeAppleUrl(const std::string& input, Source& source) {
  const auto url = trim(input);
  const auto bad = Result::fail("Unsupported Apple Music URL: use an album, playlist, track, or station share URL");
  if (url.size() > 4096 || !validUtf8(url) || url.compare(0, 8, "https://") != 0) return bad;
  for (unsigned char c : url) if (c <= 32 || c == 127 || c == '\\') return bad;
  auto slash = url.find('/', 8);
  if (slash == std::string::npos) return bad;
  std::string host = url.substr(8, slash - 8);
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
  if (host != "music.apple.com" && host != "music.apple.com:443") return bad;
  auto fragment = url.find('#', slash);
  std::string pathQuery = url.substr(slash + 1, fragment == std::string::npos ? fragment : fragment - slash - 1);
  auto question = pathQuery.find('?');
  auto path = pathQuery.substr(0, question);
  auto parts = split(path, '/');
  if (parts.size() != 4 || parts[0].empty() || parts[2].empty() || parts[3].empty()) return bad;
  Source parsed;
  if (!decode(parts[0], parsed.storefront) || !decode(parts[3], parsed.catalogId)) return bad;
  std::string slug;
  if (!decode(parts[2], slug)) return bad;
  if (parts[1] == "album") {
    if (!digits(parsed.catalogId)) return bad;
    parsed.kind = SourceKind::Album;
  } else if (parts[1] == "playlist") parsed.kind = SourceKind::Playlist;
  else if (parts[1] == "station") {
    if (parsed.catalogId.compare(0, 3, "ra.") != 0 || parsed.catalogId.size() <= 3 ||
        parsed.catalogId.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos)
      return bad;
    parsed.kind = SourceKind::Station;
  }
  else return bad;
  std::string trackId;
  bool hasTrack = false;
  if (question != std::string::npos) {
    for (const auto& pair : split(pathQuery.substr(question + 1), '&')) {
      const auto equals = pair.find('=');
      std::string name, value;
      if (!decode(pair.substr(0, equals), name) ||
          !decode(equals == std::string::npos ? "" : pair.substr(equals + 1), value)) return bad;
      if (name == "i") {
        if (hasTrack || parts[1] != "album" || !digits(value)) return bad;
        hasTrack = true;
        trackId = value;
      }
    }
  }
  parsed.url = "https://music.apple.com/" + path;
  if (hasTrack) {
    parsed.kind = SourceKind::Track;
    parsed.catalogId = trackId;
    parsed.url += "?i=" + trackId;
  }
  source = std::move(parsed);
  return {};
}

Result validateIntent(const MusicIntent& intent) {
  if (!intent.source && !intent.transport && !intent.shuffle.has_value() && !intent.repeat && !intent.volume &&
      !intent.seekPositionMs && !intent.queueIndex) return Result::fail("Empty intent");
  if (intent.seekPositionMs && (*intent.seekPositionMs < 0 || *intent.seekPositionMs > UINT32_MAX))
    return Result::fail("seek.positionMs must be an integer in 0..4294967295");
  if (intent.queueIndex && (*intent.queueIndex < 0 || *intent.queueIndex >= UINT32_MAX))
    return Result::fail("queueIndex must be an integer in 0..4294967294");
  if ((intent.seekPositionMs || intent.queueIndex) &&
      (intent.source || intent.transport == TransportCommand::Next || intent.transport == TransportCommand::Previous))
    return Result::fail("Seek/queue selection cannot combine with source or next/previous");
  if (intent.seekPositionMs && intent.queueIndex)
    return Result::fail("Seek and queue selection require separate requests");
  if (intent.transport && *intent.transport != TransportCommand::Play && *intent.transport != TransportCommand::Pause &&
      *intent.transport != TransportCommand::Next && *intent.transport != TransportCommand::Previous)
    return Result::fail("Unsupported transport");
  if (intent.volume && (intent.volume->value > 100 || intent.volume->value < (intent.volume->relative ? -100 : 0)))
    return Result::fail("Invalid volume range");
  if (intent.repeat && *intent.repeat != Repeat::Off && *intent.repeat != Repeat::All && *intent.repeat != Repeat::One)
    return Result::fail("Invalid repeat");
  if ((intent.transport == TransportCommand::Next || intent.transport == TransportCommand::Previous) &&
      (intent.source || intent.shuffle.has_value() || intent.repeat)) return Result::fail("Next/previous cannot combine with source or mode");
  if (intent.source) {
    auto modes = validateSourceModes(intent.source->kind, {intent.shuffle, intent.repeat});
    if (!modes.ok) return modes;
    Source normalized;
    auto r = normalizeAppleUrl(intent.source->url, normalized);
    if (!r.ok) return r;
    if (normalized.catalogId != intent.source->catalogId || normalized.kind != intent.source->kind ||
        normalized.storefront != intent.source->storefront) return Result::fail("Inconsistent normalized source");
  }
  return {};
}

Result parseIntent(const std::string& payload, MusicIntent& output) {
  if (payload.size() > 4096) return Result::fail("Card exceeds 4096 bytes");
  const auto text = trim(payload);
  MusicIntent intent;
  if (text.compare(0, 8, "https://") == 0) {
    Source source;
    auto result = normalizeAppleUrl(text, source);
    if (!result.ok) return result;
    intent.source = source;
    intent.transport = TransportCommand::Play;
  } else {
    if (!boundedJson(text)) return Result::fail("Invalid JSON or nesting exceeds eight containers");
    bool duplicate = false;
    std::vector<std::set<std::string>> keyStack;
    auto root = Json::parse(text, [&](int, Json::parse_event_t event, Json& value) {
      if (event == Json::parse_event_t::object_start) keyStack.emplace_back();
      if (event == Json::parse_event_t::key && !keyStack.back().insert(value.get<std::string>()).second) duplicate = true;
      if (event == Json::parse_event_t::object_end) keyStack.pop_back();
      return true;
    }, false);
    if (root.is_discarded() || duplicate || members(root) > 128) return Result::fail("Invalid/duplicate/oversized JSON");
    if (!keys(root, {"format", "version", "intent", "label", "extensions", "requires"}) ||
        !root.contains("format") || root["format"] != "sonos-surface" ||
        !root.contains("version") || !root["version"].is_number_integer() || root["version"] != 1 ||
        !root.contains("intent")) return Result::fail("Expected sonos-surface version 1 envelope");
    if (root.contains("label") && !root["label"].is_string()) return Result::fail("label must be text");
    if (root.contains("extensions")) {
      if (!root["extensions"].is_object()) return Result::fail("extensions must be an object");
      for (auto it = root["extensions"].begin(); it != root["extensions"].end(); ++it)
        if (it.key().find('.') == std::string::npos) return Result::fail("Extension names must be namespaced");
    }
    if (root.contains("requires") && (!root["requires"].is_array() || !root["requires"].empty()))
      return Result::fail("Required extensions are unsupported by this slice");
    const auto& fields = root["intent"];
    if (!keys(fields, {"source", "transport", "shuffle", "repeat", "volume", "seek", "queueIndex"}))
      return Result::fail("Unsupported intent field");
    if (fields.contains("seek")) {
      const auto& seek = fields["seek"];
      if (!keys(seek, {"positionMs"}) || seek.size() != 1 || !seek.contains("positionMs") ||
          !seek["positionMs"].is_number_integer() || seek["positionMs"] < 0 || seek["positionMs"] > UINT32_MAX)
        return Result::fail("Invalid seek.positionMs");
      intent.seekPositionMs = seek["positionMs"].get<int64_t>();
    }
    if (fields.contains("queueIndex")) {
      const auto& index = fields["queueIndex"];
      if (!index.is_number_integer() || index < 0 || index >= UINT32_MAX) return Result::fail("Invalid queueIndex");
      intent.queueIndex = index.get<int64_t>();
    }
    if (fields.contains("source")) {
      const auto& s = fields["source"];
      if (!keys(s, {"service", "url"}) || !s.contains("service") || s["service"] != "apple-music" ||
          !s.contains("url") || !s["url"].is_string()) return Result::fail("Invalid source");
      Source source;
      auto result = normalizeAppleUrl(s["url"].get<std::string>(), source);
      if (!result.ok) return result;
      intent.source = source;
    }
    if (fields.contains("transport")) {
      if (fields["transport"] == "play") intent.transport = TransportCommand::Play;
      else if (fields["transport"] == "pause") intent.transport = TransportCommand::Pause;
      else if (fields["transport"] == "next") intent.transport = TransportCommand::Next;
      else if (fields["transport"] == "previous") intent.transport = TransportCommand::Previous;
      else return Result::fail("Unsupported transport");
    }
    if (fields.contains("repeat")) {
      if (fields["repeat"] == "off") intent.repeat = Repeat::Off;
      else if (fields["repeat"] == "all") intent.repeat = Repeat::All;
      else if (fields["repeat"] == "one") intent.repeat = Repeat::One;
      else return Result::fail("repeat must be off/all/one");
    }
    if (fields.contains("volume")) {
      const auto& v = fields["volume"];
      if (!keys(v, {"set", "delta"}) || v.size() != 1) return Result::fail("volume requires exactly one set/delta");
      const auto& n = v.begin().value();
      if (!n.is_number_integer() || n < -100 || n > 100) return Result::fail("Invalid volume integer");
      intent.volume = Volume{v.contains("delta"), n.get<int>()};
    }
    if (fields.contains("shuffle")) {
      if (!fields["shuffle"].is_boolean()) return Result::fail("shuffle must be true or false");
      intent.shuffle = fields["shuffle"].get<bool>();
    }
  }
  auto result = validateIntent(intent);
  if (result.ok) output = std::move(intent);
  return result;
}

const char* sourceKindName(SourceKind kind) {
  switch (kind) {
    case SourceKind::Album: return "album";
    case SourceKind::Playlist: return "playlist";
    case SourceKind::Track: return "track";
    case SourceKind::Station: return "station";
  }
  return "unknown";
}
const char* repeatName(Repeat repeat) {
  switch (repeat) {
    case Repeat::Off: return "off";
    case Repeat::All: return "all";
    case Repeat::One: return "one";
  }
  return "unknown";
}
Result validateSourceModes(SourceKind kind, const ModePolicy& modes) {
  if (modes.repeat && *modes.repeat != Repeat::Off && *modes.repeat != Repeat::All && *modes.repeat != Repeat::One)
    return Result::fail("Invalid repeat");
  switch (kind) {
    case SourceKind::Album:
    case SourceKind::Playlist:
      if (modes.repeat == Repeat::One) return Result::fail("Repeat one is invalid for album/playlist sources");
      break;
    case SourceKind::Track:
      if (modes.shuffle.has_value()) return Result::fail("Shuffle is unsupported for tracks");
      if (modes.repeat == Repeat::All) return Result::fail("Repeat all is invalid for tracks");
      break;
    case SourceKind::Station:
      if (modes.shuffle.has_value() || modes.repeat) return Result::fail("Shuffle/repeat are unsupported for stations");
      break;
    default: return Result::fail("Invalid source kind");
  }
  return {};
}
ModePolicy sourceDefaults(SourceKind kind) {
  switch (kind) {
    case SourceKind::Album: return {false, Repeat::Off};
    case SourceKind::Playlist:
    case SourceKind::Track: return {std::nullopt, Repeat::Off};
    case SourceKind::Station: return {};
  }
  return {};
}
ModePolicy roomSourcePolicy(const RoomPolicy& policy, SourceKind kind) {
  switch (kind) {
    case SourceKind::Album: return policy.album;
    case SourceKind::Playlist: return policy.playlist;
    case SourceKind::Track: return policy.track;
    case SourceKind::Station: return {};
  }
  return {};
}
std::string describeOrigin(const FieldProvenance& provenance) {
  switch (provenance.origin) {
    case PolicyOrigin::Preserved: return "preserve";
    case PolicyOrigin::Explicit: return "explicit";
    case PolicyOrigin::RoomPolicy: return "room-policy:" + provenance.key;
    case PolicyOrigin::SourceDefault: return "source-default:" + provenance.key;
  }
  return "preserve";
}
ResolvedIntent resolvePolicy(const MusicIntent& intent, const PolicyContext& context) {
  // Callers validate incoming intents first. Execution validates the frozen result
  // again; neither observations nor later configuration participate in resolution.
  ResolvedIntent result{intent, {}, context.revision, context.targetId};
  ModePolicy defaults, overrides;
  std::string displayId;
  if (intent.source) {
    defaults = sourceDefaults(intent.source->kind);
    const auto room = context.rooms.find(context.targetId);
    if (room != context.rooms.end()) {
      overrides = roomSourcePolicy(room->second.policy, intent.source->kind);
      displayId = room->second.displayId;
    }
  }
  auto resolve = [&](auto& value, const auto& overrideValue, const auto& defaultValue, PolicyField field) {
    auto& origin = result.provenance[field];
    if (value.has_value()) origin = {PolicyOrigin::Explicit, ""};
    else if (overrideValue.has_value()) {
      value = overrideValue; origin = {PolicyOrigin::RoomPolicy, displayId};
    } else if (defaultValue.has_value()) {
      value = defaultValue; origin = {PolicyOrigin::SourceDefault, sourceKindName(intent.source->kind)};
    }
  };
  resolve(result.intent.shuffle, overrides.shuffle, defaults.shuffle, PolicyField::Shuffle);
  resolve(result.intent.repeat, overrides.repeat, defaults.repeat, PolicyField::Repeat);
  return result;
}

Result decodeNdefText(const uint8_t* data, size_t size, std::string& text) {
  if (!data || size < 2 || size > 4160 || (data[0] & 0xC0)) return Result::fail("Expected UTF-8 NDEF Text payload");
  size_t language = data[0] & 0x3F;
  if (!language || language + 1 >= size || size - language - 1 > 4096) return Result::fail("Invalid NDEF language/length");
  for (size_t i = 1; i <= language; ++i)
    if (!std::isalnum(data[i]) && data[i] != '-') return Result::fail("Invalid NDEF language");
  text.assign(reinterpret_cast<const char*>(data + 1 + language), size - 1 - language);
  if (!validUtf8(text)) return Result::fail("Malformed UTF-8 NDEF text");
  return {};
}

Result decodeNdefUri(const uint8_t* data, size_t size, std::string& url) {
  if (!data || size < 2 || size > 4097) return Result::fail("Invalid NDEF URI length");
  // NFC URI RTD: 0x00 carries the full URI; 0x04 abbreviates https://.
  // Other prefixes cannot represent an accepted https://music.apple.com URL.
  if (data[0] != 0x00 && data[0] != 0x04) return Result::fail("Unsupported NDEF URI prefix (need Apple Music HTTPS URL)");
  std::string decoded = data[0] == 0x04 ? "https://" : "";
  if (size - 1 > 4096 - decoded.size()) return Result::fail("NDEF URI exceeds 4096 bytes");
  decoded.append(reinterpret_cast<const char*>(data + 1), size - 1);
  if (!validUtf8(decoded)) return Result::fail("Malformed UTF-8 NDEF URI");
  Source source;
  auto result = normalizeAppleUrl(decoded, source);
  if (!result.ok) return result;
  url = std::move(source.url);
  return {};
}

Result decodeNdefRecord(uint8_t tnf, const std::string& type, const uint8_t* data, size_t size, std::string& text) {
  if (tnf == 0 && type.empty() && !size) return Result::fail("Empty NDEF record: no music URL");
  if (tnf != 1) return Result::fail("Unsupported NFC TNF=" + std::to_string(tnf));
  if (type == "T") return decodeNdefText(data, size, text);
  if (type == "U") return decodeNdefUri(data, size, text);
  // Supported household URL cards: well-known TNF, zero type bytes, raw URL.
  // This exception must not turn arbitrary record payloads into JSON intents.
  if (!type.empty()) return Result::fail("Unsupported NFC type=" + type.substr(0, 32));
  if (!data || !size || size > 4096) return Result::fail("Invalid empty-type NFC URL length");
  Source source;
  auto result = normalizeAppleUrl(std::string(reinterpret_cast<const char*>(data), size), source);
  if (!result.ok) return result;
  text = std::move(source.url);
  return {};
}

const char* operationName(Operation op) {
  switch (op) {
    case Operation::Stop: return "Stop";
    case Operation::ClearQueue: return "ClearQueue";
    case Operation::AddSource: return "AddSource";
    case Operation::SelectQueue: return "SelectQueue";
    case Operation::SelectStation: return "SelectStation";
    case Operation::ApplyMode: return "ApplyMode";
    case Operation::Play: return "Play";
    case Operation::Pause: return "Pause";
    case Operation::Next: return "Next";
    case Operation::Previous: return "Previous";
    case Operation::Seek: return "Seek";
    case Operation::SelectQueueItem: return "SelectQueueItem";
    case Operation::SetVolume: return "SetVolume";
    case Operation::RestoreTransport: return "RestoreTransport";
  }
  return "Unknown";
}
Result makePlan(const ResolvedIntent& resolved, Plan& plan) {
  auto valid = validateIntent(resolved.intent);
  if (!valid.ok) return valid;
  plan = {resolved, {}};
  if (resolved.intent.seekPositionMs) plan.operations.push_back(Operation::Seek);
  if (resolved.intent.queueIndex) plan.operations.push_back(Operation::SelectQueueItem);
  if (resolved.intent.source) {
    if (resolved.intent.source->kind == SourceKind::Station) {
      if (resolved.intent.transport != TransportCommand::Play) plan.operations.push_back(Operation::Stop);
      plan.operations.push_back(Operation::SelectStation);
    }
    else plan.operations = {Operation::Stop, Operation::ClearQueue, Operation::AddSource,
                       Operation::SelectQueue, Operation::ApplyMode};
  } else if (resolved.intent.shuffle.has_value() || resolved.intent.repeat) plan.operations.push_back(Operation::ApplyMode);
  if (resolved.intent.volume) plan.operations.push_back(Operation::SetVolume);
  if (resolved.intent.transport) {
    switch (*resolved.intent.transport) {
      case TransportCommand::Play: plan.operations.push_back(Operation::Play); break;
      case TransportCommand::Pause: plan.operations.push_back(Operation::Pause); break;
      case TransportCommand::Next: plan.operations.push_back(Operation::Next); break;
      case TransportCommand::Previous: plan.operations.push_back(Operation::Previous); break;
    }
  } else if (resolved.intent.source) plan.operations.push_back(Operation::RestoreTransport);
  return {};
}

Application::Application(SonosTransport& transport, PolicyContext context, Changed changed)
    : transport_(transport), context_(std::move(context)), changed_(std::move(changed)) {
  state_.observed.targetId = context_.targetId;
}
bool selectObservedRoom(AppState& state, const Room& room) {
  if (state.observed.targetId == room.id) return false;
  state = {};
  state.observed.targetId = room.id;
  state.observed.room = room.name;
  state.observed.roomDisplayId = room.displayId;
  return true;
}
bool publishSelectedState(AppState& state, const AppState& incoming, const std::string& selectedId) {
  if (incoming.observed.targetId != selectedId) return false;
  state = incoming;
  return true;
}
void Application::publish() { if (changed_) changed_(state_); }
Result Application::refresh() {
  if (busy_) return Result::fail("Busy");
  auto next = state_.observed;
  auto result = transport_.refresh(next);
  if (result.ok && next.targetId != context_.targetId) result = Result::fail("Observation target mismatch");
  // Reconciliation invalidates pages even if the queue appears unchanged.
  state_.queue.reset();
  if (result.ok) {
    state_.observed = std::move(next);
    state_.refreshError.clear();
  } else {
    state_.observed.stale = true;
    state_.refreshError = result.error;
  }
  publish();
  return result;
}
void Application::invalidateObservation() {
  state_.observed = {};
  state_.observed.targetId = context_.targetId;
  state_.queue.reset(); state_.queueError.clear();
  publish();
}
Result Application::queue(uint32_t start, uint32_t count) {
  if (busy_) return Result::fail("Busy");
  state_.queue.reset();
  QueuePage page;
  auto result = count == 0 || count > maxQueuePageSize ? Result::fail("Queue count must be 1..20") :
      transport_.queue(start, count, page);
  if (result.ok && page.targetId != context_.targetId) result = Result::fail("Queue target mismatch");
  if (result.ok) {
    if (state_.observed.queueRevision != page.revision) {
      state_.observed.stale = true;
      state_.observed.queueIndex.reset();
    }
    if (state_.observed.queueBacked == true) state_.observed.queueTotal = page.total;
    state_.observed.queueRevision = page.revision;
    state_.queue = std::move(page);
  }
  state_.queueError = result.ok ? "" : result.error;
  publish();
  return result;
}
Result Application::submitToggle(const PolicyContext& bound) {
  if (busy_) return Result::fail("Busy");
  if (state_.recoveryRequired) return Result::fail("Uncertain previous effect; inspect speaker before toggling");
  auto reject = [&](const std::string& error) {
    state_.status = "failed"; state_.detail = error; publish();
    return Result::fail(error);
  };
  if (bound.targetId.empty() || bound.targetId != context_.targetId) return reject("Bound toggle target mismatch");
  auto result = refresh();
  if (!result.ok) return result;
  const auto& current = state_.observed;
  if (!current.known || current.stale || current.targetId != bound.targetId)
    return reject("Cannot toggle without fresh state for bound room");
  MusicIntent intent;
  if (current.playback == "PLAYING") intent.transport = TransportCommand::Pause;
  else if (current.playback == "PAUSED_PLAYBACK" || current.playback == "STOPPED") intent.transport = TransportCommand::Play;
  else return reject("Cannot toggle playback state: " + current.playback);
  state_.detail = "Toggle resolved: " + current.playback + " -> " +
      (*intent.transport == TransportCommand::Play ? "play" : "pause") + " target=" + bound.targetId +
      " policy-revision=" + std::to_string(bound.revision);
  publish();
  // Resolve once after normalization, then reuse ordinary planning/execution.
  return submit(resolvePolicy(intent, bound));
}
Result Application::submit(const std::string& payload) {
  if (busy_) return Result::fail("Busy");
  if (state_.recoveryRequired) return Result::fail("Uncertain previous effect; inspect speaker and reboot before a deliberate retry");
  MusicIntent intent;
  auto result = parseIntent(payload, intent);
  if (!result.ok) {
    state_.status = "failed"; state_.detail = result.error; publish(); return result;
  }
  return submit(resolvePolicy(intent, context_));
}
Result Application::submit(const ResolvedIntent& accepted) {
  if (busy_) return Result::fail("Busy");
  if (state_.recoveryRequired) return Result::fail("Uncertain previous effect; inspect speaker and reboot before a deliberate retry");
  if (accepted.targetId != context_.targetId) return Result::fail("Accepted target differs from executor target");
  ++state_.requestId;
  Plan plan;
  auto result = makePlan(accepted, plan);
  if (!result.ok) {
    state_.status = "failed";
    state_.detail = result.error;
    publish();
    return result;
  }
  busy_ = true;
  state_.queue.reset();
  state_.status = "pending";
  state_.detail = "Preflight";
  state_.provenance = plan.resolved.provenance;
  publish();
  result = transport_.prepare(plan.resolved);
  size_t completed = 0;
  if (result.ok) {
    for (auto op : plan.operations) {
      state_.detail = operationName(op);
      publish();
      result = transport_.execute(op);
      if (!result.ok) break;
      ++completed;
    }
  }
  if (result.ok) {
    state_.detail = "Verify playback";
    publish();
    result = transport_.verify(plan.resolved, state_.observed);
  }
  if (!result.ok) {
    PlaybackState next = state_.observed;
    const auto refreshed = transport_.refresh(next);
    if (refreshed.ok) {
      state_.observed = std::move(next);
      state_.refreshError.clear();
    } else {
      state_.observed.stale = true;
      state_.refreshError = refreshed.error;
    }
  } else {
    state_.refreshError.clear();
  }
  state_.recoveryRequired = result.uncertain;
  state_.status = result.ok ? "succeeded" : result.uncertain ? "uncertain" : completed ? "partial" : "failed";
  state_.detail = result.ok ? "Observed requested result" : result.error;
  busy_ = false;
  publish();
  return result;
}
} // namespace surface
