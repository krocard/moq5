# Scenario ownership map

Which registered test owns each behavioural claim, and — for claims whose
original carrier has been retired — which tests inherited it.

`check_test_inventory.sh` reads both tables mechanically:

- nothing in RETIRED may be registered again, and its build target must be
  gone unless the row says `retained` — which it may only say because another
  registered test runs that same executable;
- every successor named in RETIRED must still be a registered CTest;
- every test named in REGISTERED OWNERS must still be registered, so a claim
  cannot lose its last owner silently.

## Retired scenarios

| retired scenario | build target | claim it carried | successor owners |
|---|---|---|---|
| `relay_loopback_diag` | removed | a lane binding drains to zero connections before the relay stops, and a frozen drain can name whether the transport terminal or the application acknowledgment is missing | `relay_lane_lifecycle` `relay_pump_branches` `relay_lane_boundary` |
| `relay_loopback` | retained | the full 35-scenario real-QUIC sweep: forwarding on both drafts, lane placement, drain-to-zero over real transport | `relay_loopback_same_d18` `relay_loopback_cross_d18` `relay_lane_lifecycle` `relay_pump_branches` `relay_mixed_draft` |
| `relay_terminal_ack` | removed | an admission-refused orphan is drained, acknowledged and reclaimed by the retirement pass alone, and an acknowledgment attempted before the terminal transfers is refused | `relay_lane_lifecycle` `relay_lane_boundary` `relay_pump_branches` |

### Why each retirement holds

**`relay_loopback_diag`** was a same-source twin of `relay_loopback` linked
against the managed adapter's testing gate. It classified a stall rather than
pinning a behaviour, so it could only ever report; it never failed for a reason
another test could not state.

**`relay_loopback`** (the unselected registration) ran every scenario in the
binary in one 143-second process. The executable is retained: the two thin
selectors run individual scenarios out of it through the same functions with
the same arguments, and `run_lanes_case` ends by stopping both peers,
`moqr_drain_to_count(relay, 0, 200)` and asserting `conn_count == 0` — so the
physical drain-to-zero claim still runs, under a 60-second bound, on real
transport. What the monolith added beyond that was repetition.

**`relay_terminal_ack`** carried two scenarios and neither is now unique:

- *orphan reclaimed by the retirement pass alone* is
  `relay_lane_lifecycle::t_orphan_refused_then_reclaimed`, sans-I/O and with
  the arm decision visible between stages;
- *poll-then-ack ordering is load-bearing* is
  `relay_lane_lifecycle::t_orphan_terminal_before_first_pump`, which pins that
  an acknowledgment attempted before that pass's own poll strands the child;
- *drain to zero, classified* is the retired diag's claim, owned as above;
- the adapter seam it read (`moq_msq_test_lane_reapable`) is exercised
  LibMoQ-side by `test_msquic_terminal_ack.c` and
  `test_msquic_reap_fairness.c`, which own the acknowledgment contract at its
  source.

## Registered owners

Named here so the checker fails if a claim's last owner disappears.

| claim | owner |
|---|---|
| lane/child reclamation, orphan retirement, poll-then-ack ordering | `relay_lane_lifecycle` |
| every production nonzero-return branch of the lane pumps | `relay_pump_branches` |
| fail-closed operands of the retirement pass | `relay_lane_boundary` |
| real-transport forwarding and drain-to-zero, same-lane and cross-lane | `relay_loopback_same_d18` `relay_loopback_cross_d18` |
| cross-draft terminal translation on decoded peer wire | `relay_mixed_draft` |
| the retirement pass is wired into every pump | `relay_reap_wiring` |
| namespace advertisement to a live prefix subscriber, both arrival orders, same-shard and cross-shard | `relay_ns_propagation` |

## Boundary fixture

The three thin real-transport cells (`relay_loopback_same_d18`,
`relay_loopback_cross_d18`, `relay_multiversion`) read LibMoQ's committed
test-only certificate and key:

```
adapters/msquic/tests/test_only_loopback_cert.pem
adapters/msquic/tests/test_only_loopback_key.pem
```

Nothing is minted at run time. The retired `relay_gen_certs` fixture shelled
out to `openssl req`, which made the physical cells depend on the host's
tooling and on the clock for a credential no test inspects.
`check_test_inventory.sh` refuses the return of `openssl`/`MOQ_OPENSSL` or
`relay_gen_certs`, and requires both committed paths to exist and to be named
by the relay CMake.

## Lane labels

| label | meaning |
|---|---|
| `sansio` | deterministic, no transport; the merge lane |
| `seeded` | the committed seed set |
| `boundary` | thin, bounded real-transport reachability |
| `tooling` | scripts and their self-tests |

There is no soak or qualification lane. A scenario that needs repetition or
wall-time to say anything has no owner and is not registered.
