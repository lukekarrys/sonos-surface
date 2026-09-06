# Policy resolution specification

## Purpose and boundaries

The first slice implements the two required rules below, with the playlist room
configured over USB. A general configuration/rule editor is not implemented.
Policy rule-set revision is currently fixed at 1; runtime configuration revision
tracking remains a later extension alongside policy editing.

Policies fill missing MusicIntent fields using trusted context. They MUST NOT
overwrite explicit input, including `false`, zero, or an explicit value equal
to current state. An unfilled field continues to mean preserve; the resolver
MUST NOT replace absence with a Sonos snapshot value.
These are deliberate application rules, not inherited Sonos or reference-server
defaults; they can be tested independently of hardware.

V1 policies only derive `shuffle`. No source, volume, repeat, transport, room
routing, schedules, scripts, or dynamic expressions are configurable policy
outputs yet. Extending this requires new examples and validation rules, not a
generic rules language. Preservation reads and relative-volume calculations
belong to planning, not policy.

## Inputs, configuration, and precedence

Resolution consumes an immutable normalized intent, a bound stable speaker ID,
and a validated policy configuration revision. Source kind comes from the
incoming URL normalizer, never a card-supplied hint or whatever is already
playing. If the intent omits source, source-kind policies do not match.

V1 configuration has this shape:

```json
{
  "version": 1,
  "revision": 3,
  "rules": [
    {
      "id": "albums-in-order",
      "scope": "household",
      "sourceKind": "album",
      "defaults": { "shuffle": false }
    },
    {
      "id": "playlist-room-shuffle",
      "scope": "room",
      "roomId": "speaker-example-room",
      "sourceKind": "playlist",
      "defaults": { "shuffle": true }
    }
  ]
}
```

`revision` is a positive integer increased on local replacement; request logs
also identify the controller/configuration, since revisions are not global.
`roomId` is a stable target speaker ID, required for room scope and forbidden
for household scope. `sourceKind` is exactly `album`, `playlist`, or `track`.
Station intents receive no shuffle policy; station shuffle is unsupported, so
`station` is not a valid selector in this shuffle-only configuration format.
Rule IDs are unique nonempty strings, and defaults contain only boolean
`shuffle`. Reject unknown keys, wrong versions/types, and duplicate keys rather
than partially applying a malformed configuration.

The household album rule above is the initial required default. The playlist
rule becomes active once the family configures its real target; the placeholder
MUST NOT select a room by inference. Users may subsequently edit/remove rules
through configuration. “Household” applies to every eligible target known to
that controller; it does not imply Sonos grouping or automatic config sync.

Resolution order, independently for each supported output field:

1. Retain an explicitly present intent field.
2. Otherwise select a matching room rule.
3. Otherwise select a matching household rule.
4. Otherwise leave the field absent.

Rule array order has no meaning. For each source kind and output field, allow
at most one household rule and at most one rule per room. Reject equal-scope
duplicates even when they assign the same value. Configuration changes are
validated atomically; retain the previous valid revision on failure. Do not
silently select the first rule or use arbitrary numeric priorities.

## Deterministic algorithm and provenance

Copy explicit fields and annotate their origin. Match rules once against the
original context; fill eligible absent fields by the precedence above. Derived
values MUST NOT trigger a second rule pass. Return a resolved intent with the
same field semantics plus a separate explanation map, policy revision, and
target context. Do not mutate the input or trust caller-supplied provenance.

For a playlist without shuffle in the configured room, a diagnostic excerpt is:

```json
{
  "resolvedFields": { "shuffle": true },
  "provenance": {
    "source": { "origin": "explicit" },
    "shuffle": {
      "origin": "policy",
      "ruleId": "playlist-room-shuffle",
      "scope": "room",
      "configRevision": 3,
      "reason": "Playlist requested in configured room"
    }
  },
  "preservedFields": ["volume", "repeat", "transport"]
}
```

This is an explanation excerpt, not a card or a second wire intent schema.
The full resolved intent retains source and all other original properties.
Provenance is recorded for each present top-level intent field. `volume` remains
explicit when its relative target is later computed; the planner records that
baseline calculation separately. Preserved fields have no fabricated value or
policy origin. Explanations SHOULD identify applicable rules skipped because
explicit input or a more-specific rule won.

Freeze target, original intent, and policy revision when a request is accepted;
resolve exactly once. Queued work retains that revision when configuration is
edited. Transport retries MUST NOT re-resolve under new policy or reclassify
the source. A policy preview is not a guarantee of future preservation values,
which are read at execution; the UI must show the revision and target used.

## Acceptance matrix

Assume the two example rules and no others. “Preserve” means absent in the
resolved intent, regardless of the currently observed value.

| Incoming source | Explicit shuffle | Target | Resolved shuffle / origin |
| --- | --- | --- | --- |
| Album | absent | Any room | `false`, household policy |
| Album | `true` | Any room | `true`, explicit |
| Album | `false` | Any room | `false`, explicit |
| Playlist | absent | Configured playlist room | `true`, room policy |
| Playlist | absent | Another room | Preserve |
| Playlist | `false` | Configured playlist room | `false`, explicit |
| Track from album URL with `i` | absent | Any room | Preserve |
| No source; volume/pause only | absent | Room currently playing album | Preserve |

Additional required tests:

- Add a household playlist `false` rule: the configured room still derives
  `true`; other rooms derive `false`. Removing the room rule exposes the
  household fallback.
- Renaming a room changes no matches; changing the selected stable target does.
- Permuting rule order changes neither values nor winning provenance.
- Invalid duplicate rules reject the update and retain the old configuration.
- Explicit `false` cannot be mistaken for absence; no rule sets transport or
  repeat as a side effect of deriving shuffle.
- Repeated resolution of identical inputs/context/revision is identical and
  does not modify the input. Resolution is input-origin independent.
