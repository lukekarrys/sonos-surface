# Policy resolution specification

## Runtime configuration and identity

The narrow shuffle model uses `playlist_shuffle_rooms` in USB/NVS config:

```json
{
  "read_only": true,
  "rooms": ["office", "living-room", "bedroom"],
  "playlist_shuffle_rooms": {"living-room": true, "bedroom": false}
}
```

Keys are canonical room display IDs, using the single normalization contract in
[product](product.md#stick-room-and-policy-milestone). At topology refresh, each
configured policy ID must resolve to exactly one discovered UUID. Missing, invalid,
or ambiguous IDs produce config warnings and do not install a runtime rule.
Valid rules continue. A policy entry does not add a room to the device's selectable
room list. A false rule derives false; omit the entry to preserve playlist shuffle.
Up to 32 entries are supported. These settings are local to each physical device.

The policy resolver receives the selected UUID plus the resolved UUID-to-boolean
map. It never compares mutable names at execution. A room rename invalidates the
old config ID on the next topology refresh. Requests accepted before that refresh
retain their original UUID and policy result; they never target a different room.

## Precedence and narrow provenance

Policies fill missing fields only. Explicit false is as authoritative as true.
Only shuffle is derived; no generic policy expression engine or per-field origin
framework is implemented. Preserve means absent, never a guessed snapshot value.

| Incoming source | Explicit shuffle | Room rule | Result / shuffle origin |
| --- | --- | --- | --- |
| Album | absent | Any | false / `albums-in-order` (household) |
| Playlist | absent | true | true / `playlist-room-shuffle` (room) |
| Playlist | absent | false | false / `playlist-room-shuffle` (room) |
| Playlist | absent | Missing | absent / `preserve` |
| Any supported input | true or false | Any | explicit value / `explicit` |
| Track, station, or no source | absent | Any | absent / `preserve` |

Explicit station shuffle/repeat is rejected as unsupported. Rules never set
source, volume, repeat, transport, or target. If a second field later derives
policy, refactor provenance then rather than adding another field-specific origin.

## Acceptance, revision, and diagnostics

Parse and validate the original intent; bind the current configured room's resolved
UUID; resolve exactly once and freeze the result and configuration revision before
handing work to the worker. NVS configuration replacement advances a persisted
local revision. Busy updates reject. Retries/execution never re-resolve policy or
reclassify source kind. A static plan preview cannot predict future preservation
baselines, which preflight reads at execution.

Logs include `room`, `roomDisplayId`, UUID, `policyRevision`, normalized `sourceKind`,
recognized explicit fields, `shuffleInput`, `shuffleResolved`, `shuffleOrigin`,
preserved fields, and planned operations. Adapter logs include fresh mode/volume
baselines and operation results. Credentials and arbitrary extensions are omitted.
Read-only mode runs this same parser/resolver/planner path, then blocks effects at
HTTP dispatch with `READ_ONLY_BLOCKED`. It does not pretend playback succeeded.

## Validation and migration

Structural errors (wrong types, duplicate keys/room-list entries, oversized maps,
both singular and plural policy keys) reject atomically and retain saved config.
Malformed ID strings remain stored for actionable topology-resolution warnings;
no invalid string ever binds a target. Unknown configuration keys reject.

Legacy `playlist_shuffle_room` is readable as one true entry, or an empty map for
an empty string. A canonical display ID works; an old UUID key is reported invalid
and must be replaced by the current display ID. There is deliberately no UUID
fallback or automatic name migration. Legacy `sonos_uid` and old saved UUID
preferences cannot restore selection. Set `rooms` explicitly during migration;
an omitted list targets nothing. Missing `read_only` defaults true.

Tests cover precedence, explicit false, resolved identity, collision/rename handling,
config replacement, frozen acceptance, and the real dispatch guard. Hardware
observations and pending physical checks live in [hardware](hardware.md).
