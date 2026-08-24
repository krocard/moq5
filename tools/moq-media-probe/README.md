# moq_media_probe

A small, self-contained black-box executable that exposes LibMoQ's MSF/CMSF
catalog parser through a deterministic line-oriented JSON protocol on
stdin/stdout. It is a generic LibMoQ tool: it knows nothing about any external
caller, corpus, or schema, and depends only on `moq::msf` (and its vendored JSON
parser). External projects may drive it as a subprocess for differential
testing; this tool has no knowledge of them.

## Protocol

`moq-media-probe/1`. One request object per input line; one response object per
output line. Machine output goes only to **stdout**; nothing is written to
stderr on the normal path.

### Request

```json
{
  "protocol": "moq-media-probe/1",
  "id": "caller-opaque-id",
  "operation": "catalog.parse",
  "profile": "msf-01",
  "input": { "utf8": "{ ...catalog JSON... }" }
}
```

- `protocol`, `id`, `operation` are required. `profile` and `input.utf8` are
  required for the catalog operations, unused for `capabilities`.
- The whole request line must be valid UTF-8; an invalid sequence is rejected
  (`malformed-json`) before JSON parsing, so no non-UTF-8 byte can survive into
  a response string.
- Inputs are inline only — there are no filesystem paths or network references.
- Unknown protocol versions, operations, profiles, and top-level/`input` fields
  are rejected with typed errors. Unknown fields *inside a parsed catalog* are
  dropped by the parser and never appear in the result.
- The declared `profile` is **enforced** against the document's version form
  (kind and value): `msf-00` a numeric version, `msf-01`/`cmsf-01` the string
  `"1"`, `msf-01-draft` a `"draft-XX"` string. A mismatch is a typed
  `profile-mismatch` error. (CMSF extension structures are optional, so they are
  not required by `cmsf-01`.)

### Response

Success:

```json
{
  "protocol": "moq-media-probe/1",
  "id": "caller-opaque-id",
  "status": "ok",
  "result": { ... },
  "diagnostics": []
}
```

Error:

```json
{
  "protocol": "moq-media-probe/1",
  "id": "caller-opaque-id",
  "status": "error",
  "error": {
    "stage": "syntax|semantic|operation|internal",
    "category": "stable-machine-readable-token",
    "message": "human-readable detail"
  }
}
```

### Result conventions

- Every **integral** value is emitted as a decimal **string**, so it is lossless
  regardless of the consumer's number type. Booleans and strings are emitted
  verbatim (strings JSON-escaped).
- Object keys are emitted in ascending byte order (canonical). **Array order is
  preserved** wherever it is semantically meaningful (tracks, delta operations,
  `defaultKID`, `depends`, `contentProtectionRefIDs`).
- The result projects LibMoQ's typed model, so unrecognized input fields cannot
  leak into output and present-vs-absent is preserved — **with one documented
  exception**: LibMoQ's model stores `defaultKID`, `depends`,
  `contentProtectionRefIDs`, `contentProtections`, and `initDataList` as
  count-only, so an explicitly empty `[]` is indistinguishable from an absent
  field and both are omitted. This is reported under `limitations` in the
  `capabilities` result.
- Field names are the spec/model names verbatim (e.g. `laURL`, `certURL`,
  `authURL`). `framerateMillis` is the frame rate scaled by 1000 (the model
  stores no floats).

## Operations

| Operation | Purpose |
|---|---|
| `capabilities` | Enumerate supported operations/profiles and reasons. |
| `catalog.parse` | Parse an independent MSF/CMSF catalog. |
| `catalog.delta.parse` | Parse an MSF-01 delta document into ordered operations. |

`catalog.parse` profiles: `msf-00` (legacy numeric `version`), `msf-01`
(`"version":"1"`), `msf-01-draft` (only the `"draft-01"` string is recognized),
and `cmsf-01`. CMSF clear-content catalog fields (`initDataList`/`initRef`,
`contentProtections`) are **parsed**; protected **playback** is not supported and
is reported as such (a `capabilities` flag and a `cmsf-content-protection-parsed-
playback-unsupported` diagnostic). `catalog.delta.parse` supports `msf-01` only.

Call `capabilities` for the authoritative, self-describing matrix.

LOC and BMFF operations are intentionally not implemented; the envelope is
versioned so they can be added without a protocol bump.

## Build & test

```sh
cmake -S . -B build/media-probe \
  -DMOQ_BUILD_MSF=ON -DMOQ_BUILD_MEDIA_PROBE=ON
cmake --build build/media-probe --target moq_media_probe
ctest --test-dir build/media-probe -R moq_media_probe   # unit + CLI smoke
```

`MOQ_BUILD_MEDIA_PROBE` is opt-in and requires `MOQ_BUILD_MSF=ON`.

## Example

```sh
printf '%s\n' \
  '{"protocol":"moq-media-probe/1","id":"1","operation":"capabilities"}' \
  | ./build/media-probe/tools/moq-media-probe/moq_media_probe
```
