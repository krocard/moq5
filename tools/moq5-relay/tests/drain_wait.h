#ifndef MOQR_TEST_DRAIN_WAIT_H
#define MOQR_TEST_DRAIN_WAIT_H

/*
 * The one drain-wait every relay harness uses. A facade that CLOSES during a
 * drain is a named outcome, never a timeout: managed_wait returns
 * MOQ_ERR_CLOSED on a facade terminal (a stop, or a lane pump that returned
 * nonzero), and a harness that keeps polling the connection count through
 * that signal converts a loud fail-stop into a silent 10-second stall — the
 * exact silence that cost the original stranded-drain failure its
 * attribution. Pending activity notifications are consumed along the way
 * (managed_wait reports them first, by design), so CLOSED here is the
 * terminal fact, not a race.
 */

#include <stdint.h>

#include <moq/msquic_managed.h>

typedef enum {
    MOQR_DRAIN_OK,      /* the count was reached                          */
    MOQR_DRAIN_CLOSED,  /* facade terminal before the count was reached   */
    MOQR_DRAIN_TIMEOUT, /* rounds exhausted with the facade still open    */
} moqr_drain_result_t;

static inline moqr_drain_result_t
moqr_drain_to_count(moq_msquic_managed_t *m, uint32_t want, int rounds)
{
    for (int i = 0; i < rounds; i++) {
        if (moq_msquic_managed_conn_count(m) == want) {
            return MOQR_DRAIN_OK;
        }
        if (moq_msquic_managed_wait(m, 50 * 1000) == MOQ_ERR_CLOSED) {
            return moq_msquic_managed_conn_count(m) == want
                       ? MOQR_DRAIN_OK
                       : MOQR_DRAIN_CLOSED;
        }
    }
    return moq_msquic_managed_conn_count(m) == want ? MOQR_DRAIN_OK
                                                    : MOQR_DRAIN_TIMEOUT;
}

#endif /* MOQR_TEST_DRAIN_WAIT_H */
