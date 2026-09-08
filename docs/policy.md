# Room configuration and source policy

## Device configuration

`rooms` is one object keyed by canonical `roomDisplayId`. Its keys are the device's
entire room allowlist; each value contains only that room's policy exceptions.
An empty object allows the room without overriding shared source defaults.

```json
{
  "read_only": true,
  "rooms": {
    "office": {},
    "living-room": {"playlist": {"shuffle": true}},
    "sons-room": {
      "album": {"shuffle": true},
      "playlist": {"shuffle": true},
      "track": {"repeat": "one"}
    }
  }
}
```

These are illustrative personalities, not instructions to enroll those rooms.
Configure current household names deliberately. Settings are local to each
controller; there is no automatic policy distribution or general rules language.

`read_only` is the sole Sonos mutation switch: true blocks all mutations at HTTP
dispatch; false permits requested effects for configured, uniquely resolved,
topology-eligible targets. Room configuration controls target availability, not
mutation permission. Missing `read_only` defaults true; missing/empty `rooms`
selects nothing. [Execution](planner-executor.md) owns dispatch/identity safeguards.

## Room identity and selection

The display ID derives from the current Sonos name: lowercase ASCII letters and
digits; whitespace, hyphens, and underscores collapse to one hyphen; other ASCII
punctuation is stripped; edge hyphens are trimmed. `Office` → `office`,
`Living Room` → `living-room`, `Kids' Room` → `kids-room`. Non-ASCII names,
unsupported control characters, empty results, and IDs over 64 characters are
invalid. There is no transliteration; configured keys must already be canonical.

At topology refresh, each config key must match exactly one discovered room across
the entire snapshot, including ineligible players. Missing, invalid, or ambiguous
IDs warn and never bind. Grouped, bonded, invisible, addressless, or otherwise
ineligible targets warn as unavailable. Valid entries continue; discovered but
unconfigured rooms never enter selection. Only selectable UUIDs receive resolved
room policies. There is no separate policy map capable of enrolling another room.

Resolution is `config display ID → discovered room → UUID`. A rename intentionally
breaks the old binding until configuration is edited. UUIDs are internal identities,
never human config keys. Legacy `sonos_uid` is ignored for selection and may be
removed from old files; `sonos_ip` is an optional discovery bootstrap hint only.

Selection sorts by case-insensitive human name then UUID. A room switch persists
its display ID in `surface/preferred-id`. Boot restores it if eligible; otherwise
warn and use the first valid configured room. The same fallback applies when the
selection disappears. A recovered room rejoins without stealing selection.
Accepted work never follows a fallback or a later room switch.

## Code-owned defaults and valid overrides

`sourceDefaults(SourceKind)` in SurfaceCore defines product behavior. Defaults
run only when the incoming intent contains a normalized source, and only fill
omitted fields. They are not stored in NVS, copied into room entries, or written
onto cards. No policy examines the currently playing Sonos media.

| Incoming source | Default shuffle | Default repeat | Allowed room override fields |
| --- | --- | --- | --- |
| Album | false | off | shuffle true/false; repeat off/all |
| Playlist | absent/preserve | off | shuffle true/false; repeat off/all |
| Track | unsupported; absent | off | repeat off/one |
| Station | unsupported; absent | unsupported; absent | None; `station` config key rejects |
| No source | absent/preserve | absent/preserve | None |

A new album plays in order for one pass. Playlists retain shuffle unless explicitly
set or overridden by the room, but clear prior repeat-one/all by default. A track
plays once unless its room or explicit intent requests repeat-one. These defaults
prevent a previous source's repeat mode leaking into a new card's behavior.
The [intent source/mode matrix](intent.md#source-specific-mode-validity) is also
used to validate room overrides; config cannot express extra source capabilities.

## Per-field precedence and provenance

For each field independently: **explicit intent → room override → source default
→ absent/preserve**. Explicit false and repeat off are values, not omission.
Policies never derive source, target, volume, transport, seek, or queue index.

| Request and room | Resolved shuffle / origin | Resolved repeat / origin |
| --- | --- | --- |
| Album, Office | false / `source-default:album` | off / `source-default:album` |
| Album, Sons Room | true / `room-policy:sons-room` | off / `source-default:album` |
| Album, Sons Room, explicit shuffle=false | false / `explicit` | off / `source-default:album` |
| Album, explicit repeat=all | room/default value | all / `explicit` |
| Playlist, Office | absent / `preserve` | off / `source-default:playlist` |
| Playlist, Living Room | true / `room-policy:living-room` | off / `source-default:playlist` |
| Playlist, Living Room, explicit shuffle=false | false / `explicit` | off / `source-default:playlist` |
| Track, Sons Room | absent / `preserve` | one / `room-policy:sons-room` |
| Track, Sons Room, explicit repeat=off | absent / `preserve` | off / `explicit` |
| No-source repeat=one | absent / `preserve` | one / `explicit` |

`PolicyProvenance` is a small map of typed fields (Shuffle, Repeat) to typed
origins (Preserved, Explicit, RoomPolicy, SourceDefault), with a display ID or
source-kind key where applicable. Logs pair each origin with its resolved value:

```json
{
  "shuffle": {"value": true, "origin": "room-policy:living-room"},
  "repeat": {"value": "off", "origin": "source-default:playlist"}
}
```

Absent fields have JSON `null` and origin `preserve`, distinct from false/off.
Accepted diagnostics include room/display ID, frozen UUID, policy revision,
source kind, explicit fields, this per-field `policy` object, preserved fields,
and the static operation plan. Preservation baselines belong to execution reads,
not policy. USB `preview URL` or `preview {v1 envelope}` prints the same policy/plan
without submitting work, preflight reads, or mutations, in either runtime mode.

## Replacement, acceptance, and migration

Configuration validation completes before NVS replacement; invalid updates retain
the prior config. Successful replacement advances the persisted local revision and
reboots. Busy updates reject. Acceptance validates input first and freezes target
UUID, resolved values, provenance, and revision before handing work to the worker.
Execution never reruns policy; later config or selection changes cannot alter it.

Config permits at most 32 rooms, 64 bytes per ID, eight JSON container levels,
and 4,088 UTF-8 bytes. Wrong types, duplicate keys at any depth, unknown fields,
unsupported source/mode policies, and excessive sizes reject atomically. Invalid
ID strings within the size bound remain stored for actionable topology warnings.
Empty source policy objects are allowed and equivalent to omitting that override.
Diagnostic serialization retains exceptions only, never expands defaults.

The former `rooms` array, `playlist_shuffle_rooms`, and `playlist_shuffle_room`
formats now **reject**. Maintaining multiple parsers is unnecessary for the two
owner-controlled development devices. There is no silent legacy reinterpretation.
An old installed config after flashing remains in NVS but fails validation: the
runtime uses read-only/no-target defaults until a valid replacement is uploaded.

For an unmigrated device:

1. Before flashing, query `config-status`; save its current mode, room set, and
   playlist overrides. Keep a private backup of its complete config file.
2. Replace the array with `{ "room-id": {} }` entries. Move each existing playlist
   boolean into that room's `playlist.shuffle`; remove both legacy keys. If a
   policy names a room outside the old allowlist, resolve that discrepancy
   explicitly rather than enrolling it. Preserve `read_only` and private fields.
3. Build/flash current firmware, then upload the converted file using
   `scripts/configure.py` as shown in [README](../README.md#configuration).
   Household replacement leaves the separate `surface/touch` calibration intact.
4. Query `config-status`, `rooms`, and pure `preview` intents. Confirm the same
   selectable rooms, existing playlist override, source defaults, and calibration.
   Do not use playback to test pure policy or assign a new track policy by guess.

[Hardware](hardware.md#configuration-validation) records the development-device
migration result. Future writer controls and omission labels live in
[intent](intent.md#simple-family-writer-contract-future).
