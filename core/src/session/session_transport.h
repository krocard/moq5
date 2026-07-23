#ifndef MOQ_SESSION_TRANSPORT_H
#define MOQ_SESSION_TRANSPORT_H

/*
 * Session <-> transport-bridge internal contract.
 *
 * Declarations the transport bridge needs from the session core that are not
 * part of the public application API (moq/session.h). Kept out of
 * session_internal.h so the bridge does not pull in the full session layout.
 */

#include "moq/session.h"
#include "profile.h"   /* moq_uni_class_t */

/*
 * True if the bound profile carries control on a local/peer pair of
 * unidirectional control channels (so the bridge runs in uni-control-pair
 * mode). False for profiles that use a single bidirectional control channel
 * (draft-16), which keep the bridge in its default mode.
 */
bool moq_session_uses_uni_control(const moq_session_t *s);

/*
 * Classify an inbound peer unidirectional stream by its leading bytes. If the
 * profile has no unidirectional control channel (e.g. draft-16), returns
 * MOQ_UNI_CLASS_DATA so every peer unidirectional stream is treated as data.
 */
moq_uni_class_t moq_session_classify_peer_uni(const moq_session_t *s,
                                              const uint8_t *data, size_t len);

/*
 * Opaque monotonic event-progress token. Advances (by an unspecified amount)
 * on every event ENQUEUE and every event DEQUEUE; stable across calls that move
 * no event. A coalesced-doorbell adapter snapshots it BEFORE the app pump and
 * compares for EQUALITY after the post-pump service pass: inequality means the
 * whole cycle moved events — the bounded pump dequeued some (draining a backlog)
 * and/or service enqueued some (refilling the queue). Paired with has_events it
 * drives a bounded re-pump. Never reset; unsigned wrap is acceptable
 * (equality-only). Not part of the public API.
 */
uint64_t moq_session_event_progress_token(const moq_session_t *s);

/*
 * True while the session's event queue holds at least one undelivered event.
 * The bridge pairs this with the progress token: a re-pump is owed only when
 * the cycle made progress AND events still remain to drain. Not public API.
 */
bool moq_session_has_events(const moq_session_t *s);

#endif /* MOQ_SESSION_TRANSPORT_H */
