/*
 * Public-header consumer proof for moq_session_close(): includes ONLY the
 * public <moq/session.h> and links moq::core (never the test-internals
 * archive), exactly as an external app -- e.g. a relay binding denying
 * setup auth with nothing but a moq_session_t* in hand -- would. No
 * network, no timing.
 */

#include <moq/session.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            fprintf(stderr, "FAIL: %s:%d: %s\n",                       \
                    __FILE__, __LINE__, #expr);                         \
            failures++;                                                 \
        }                                                               \
    } while (0)

int main(void)
{
    moq_session_cfg_t cfg;
    moq_session_cfg_init(&cfg, moq_alloc_default(), MOQ_PERSPECTIVE_SERVER);
    moq_session_t *session = NULL;
    CHECK(moq_session_create(&cfg, 0, &session) == MOQ_OK);
    if (!session) return 1;

    static const char reason[] = "auth denied";
    CHECK(moq_session_close(session, 0x4442u, reason, 1000) == MOQ_OK);
    CHECK(moq_session_state(session) == MOQ_SESS_CLOSED);

    moq_action_t action;
    CHECK(moq_session_poll_actions(session, &action, 1) == 1);
    CHECK(action.kind == MOQ_ACTION_CLOSE_SESSION);
    CHECK(action.u.close_session.code == 0x4442u);
    CHECK(action.u.close_session.reason.len == sizeof(reason) - 1);

    moq_event_t event;
    CHECK(moq_session_poll_events(session, &event, 1) == 1);
    CHECK(event.kind == MOQ_EVENT_SESSION_CLOSED);
    CHECK(event.u.closed.code == 0x4442u);
    moq_event_cleanup(&event);

    moq_session_destroy(session);

    /* Public consumer proof for moq_session_version(): CALLED and LINKED
     * through <moq/session.h> against moq::core, on a non-default profile so
     * a build that returned the config default would differ. This file is the
     * public-only surface -- it never sees the test-internals archive. */
    {
        moq_session_cfg_t vcfg;
        moq_session_cfg_init_sized(&vcfg, sizeof(vcfg), moq_alloc_default(),
                                   MOQ_PERSPECTIVE_CLIENT);
        vcfg.version = MOQ_VERSION_DRAFT_18;
        moq_session_t *vs = NULL;
        CHECK(moq_session_create(&vcfg, 0, &vs) == MOQ_OK);
        if (vs) {
            CHECK(moq_session_version(vs) == MOQ_VERSION_DRAFT_18);
            CHECK(moq_session_version(vs) != MOQ_VERSION_DRAFT_16);
            moq_session_destroy(vs);
        }
        CHECK(moq_session_version(NULL) == (moq_version_t)0);
    }

    if (failures == 0)
        printf("PASS: session_public_close\n");
    return failures ? 1 : 0;
}
