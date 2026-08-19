# MOQ5 Relay

A deterministic Media over QUIC relay. The protocol core is sans-I/O and the
relay serves draft-16 and draft-18 from one listener, translating terminal
codes by meaning rather than forwarding raw numbers between registries.

The installed command is `moq5-relay`.

## Build

The relay is off by default. Enable it, and the managed MsQuic transport it
runs on, at configure time:

```sh
cmake -B build -DMOQ_BUILD_RELAY=ON \
               -DMOQ_BUILD_ADAPTER_MSQUIC=ON \
               -DMOQ_BUILD_MSQUIC_MANAGED=ON
cmake --build build
cmake --install build --prefix /usr/local
```

## Run

```sh
moq5-relay serve --config relay.json
```

`capacity` resolves the same configuration and prints the ceiling the process
would run under, without starting a listener:

```sh
moq5-relay capacity --config relay.json
```

`moq5-relay --help`, `moq5-relay help serve` and `moq5-relay help capacity`
describe the command line; `moq5-relay --version` prints the version.

## Configuration

`examples/relay.json` is a complete, inert example — it names no host-specific
path and contains no credential. `listener.cert` and `listener.key` are
required by `serve` and are read relative to the working directory unless given
as absolute paths.

| key | meaning |
|---|---|
| `listener.host`, `listener.port` | where the relay listens |
| `listener.versions` | ordered draft set; default `[18]`, use `[18, 16]` to serve both drafts from one listener |
| `listener.lanes` | independent shard lanes; more than one enables the cross-shard demand plane |
| `listener.cert`, `listener.key` | server credential, PEM |
| `budgets.*` | pool ceilings resolved up front, so an oversized configuration is refused before the listener starts |
| `telemetry.trace_ring_records` | bounded flight-recorder depth |
| `auth.mode` | `allow_all` or the bundled deterministic `toy` policy |

Machine-readable rows (`RELAY_RUN_CONFIG_V1`, `RELAY_PAIR_STATS_V1`) are the
stable contract for tooling. Human diagnostics are prefixed `MOQ5 Relay:` and
are not a parsing surface.

See `moq5-relay(1)` for the full command reference, and
`docs/architecture.md` for the internal relay design.
