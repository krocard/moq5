#ifndef MOQ_WTQUIC_ADAPTER_INTERNAL_H
#define MOQ_WTQUIC_ADAPTER_INTERNAL_H

/*
 * Private, non-installed lockstep SPI between the wtquic attach adapter and the
 * managed wtquic facades (which are SEPARATE DSOs). NOT part of the installed
 * public API (moq/wtquic.h) and NOT an application ABI.
 */

#include <moq/wtquic.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Read the attached session's coalesced-doorbell re-drive inputs in ONE call
 * across the DSO boundary: returns the opaque monotonic event-progress token
 * (see moq_transport_bridge_event_progress_token) and writes, through
 * out_has_events, whether the session still holds an undelivered event. The
 * managed facade snapshots the token before its app pump and, after the
 * post-pump service pass, re-arms a pump iff the token moved AND events remain.
 * This avoids exposing the bridge pointer or any application-facing session API.
 * Caller-serialized; token is equality-only (it wraps).
 */
MOQ_API uint64_t moq_wtquic_conn_event_progress(const moq_wtquic_conn_t *conn,
                                                bool *out_has_events);

#endif /* MOQ_WTQUIC_ADAPTER_INTERNAL_H */
