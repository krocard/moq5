/*
 * Demand-edge discriminator for the automatic catalog refresh.
 *
 * PR #11 re-arms the refresh clock on the catalog's no-demand -> demand edge.
 * A push consumer accepts the catalog PUBLISH with FORWARD 0 (accepted, but no
 * demand: draft-18 10.2.12), stays that way well past the refresh interval, and
 * only then raises FORWARD to 1 with a REQUEST_UPDATE. That is the edge.
 *
 * Without the re-arm the refresh deadline armed at catalog install is long past
 * when demand appears, so a refresh generation stages immediately. With it, the
 * first refresh must wait a full interval from the EDGE. The test measures the
 * elapsed time from the edge to the first group ABOVE the initial generation --
 * the initial catalog (group 0) legitimately arrives at once, and is not a
 * refresh.
 *
 * Real managed pico/pq_threaded endpoint, shipping moq::service.
 */
#include <moq/media_sender.h>
#include <moq/endpoint.h>
#include <moq/picoquic_threaded.h>
#include <moq/session.h>
#include <moq/msf.h>
#include "test_support.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

static int failures = 0;

/* -- Minimal push-only server that records catalog groups ------------- */

#define CAP_MAX 256
typedef struct {
    pthread_mutex_t   mu;
    bool              pub_cat_ok;
    moq_publication_t pub_cat;
    int               n;
    uint64_t          groups[CAP_MAX];
    uint64_t          at_us[CAP_MAX];   /* wall clock of each catalog object */
    atomic_bool       raise_forward;    /* main thread asks for the edge */
    bool              raised;           /* pump issued the REQUEST_UPDATE */
    uint64_t          raised_at_us;     /* wall clock of the edge */
    uint64_t          first_group;      /* the initial generation */
    bool              have_first;
} push_srv_t;

static uint64_t now_wall_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

static push_srv_t g_ps;

static int psrv_pump(moq_pq_threaded_t *t, moq_pq_threaded_lane_t *lane,
                     uint64_t now_us, void *ctx)
{
    (void)lane;
    push_srv_t *ps = (push_srv_t *)ctx;
    moq_session_t *session = moq_pq_threaded_session(t);
    if (!session || moq_session_state(session) != MOQ_SESS_ESTABLISHED)
        return 0;
    moq_event_t ev;
    while (moq_session_poll_events(session, &ev, 1) == 1) {
        if (ev.kind == MOQ_EVENT_NAMESPACE_PUBLISHED) {
            moq_accept_namespace_cfg_t acc;
            moq_accept_namespace_cfg_init(&acc);
            moq_session_accept_namespace(session,
                ev.u.namespace_published.ann, &acc, now_us);
        } else if (ev.kind == MOQ_EVENT_PUBLISH_REQUEST) {
            const moq_publish_request_event_t *pr = &ev.u.publish_request;
            const bool is_catalog =
                pr->track_name.len == strlen(MOQ_MSF_CATALOG_TRACK_NAME) &&
                memcmp(pr->track_name.data, MOQ_MSF_CATALOG_TRACK_NAME,
                       pr->track_name.len) == 0;
            const moq_publication_t cand = pr->pub;
            /* Sized init: has_forward is an APPENDED block; the pointer init's
             * frozen v0 struct_size would leave it unread and the effective
             * state would default to forwarding. */
            moq_accept_publish_cfg_t acc;
            moq_accept_publish_cfg_init_sized(&acc, sizeof(acc));
            acc.has_forward = true;
            /* Only the CATALOG starts without demand. Media keeps flowing so
             * the server's lane pump stays scheduled -- with no traffic at all
             * the pump would not run and the edge could never be issued. */
            acc.forward = !is_catalog;
            if (moq_session_accept_publish(session, cand, &acc,
                                           now_us) == MOQ_OK && is_catalog) {
                pthread_mutex_lock(&ps->mu);
                ps->pub_cat = cand;
                ps->pub_cat_ok = true;
                pthread_mutex_unlock(&ps->mu);
            }
        } else if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED) {
            const moq_object_received_event_t *o = &ev.u.object_received;
            pthread_mutex_lock(&ps->mu);
            if (ps->pub_cat_ok && moq_publication_eq(o->pub, ps->pub_cat) &&
                ps->n < CAP_MAX) {
                if (!ps->have_first) {
                    ps->first_group = o->group_id;
                    ps->have_first = true;
                }
                ps->at_us[ps->n] = now_wall_us();
                ps->groups[ps->n++] = o->group_id;
            }
            pthread_mutex_unlock(&ps->mu);
        }
        moq_event_cleanup(&ev);
    }

    /* The edge: raise FORWARD 0 -> 1 on the accepted catalog publication. */
    if (atomic_load(&ps->raise_forward)) {
        pthread_mutex_lock(&ps->mu);
        bool go = ps->pub_cat_ok && !ps->raised;
        moq_publication_t p = ps->pub_cat;
        pthread_mutex_unlock(&ps->mu);
        if (go) {
            moq_publication_update_cfg_t uc;
            memset(&uc, 0, sizeof(uc));
            uc.struct_size = sizeof(uc);
            uc.has_forward = true;
            uc.forward = true;
            if (moq_session_update_publication(session, p, &uc, now_us)
                    == MOQ_OK) {
                pthread_mutex_lock(&ps->mu);
                ps->raised = true;
                ps->raised_at_us = now_wall_us();
                pthread_mutex_unlock(&ps->mu);
            }
        }
    }
    return 0;
}

static moq_pq_threaded_t *psrv_start(const char *cert, const char *key,
                                     int *out_port)
{
    int base = 20600 + (int)(getpid() % 991);
    for (int attempt = 0; attempt < 8; attempt++) {
        int port = base + attempt * 13;
        moq_pq_threaded_cfg_t cfg;
        moq_pq_threaded_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.alloc = moq_alloc_default();
        cfg.perspective = MOQ_PERSPECTIVE_SERVER;
        cfg.cert_path = cert;
        cfg.key_path = key;
        cfg.port = port;
        cfg.send_request_capacity = true;
        cfg.initial_request_capacity = 16;
        cfg.on_lane_pump = psrv_pump;
        cfg.on_lane_pump_ctx = &g_ps;
        moq_pq_threaded_t *t = NULL;
        if (moq_pq_threaded_create(&cfg, &t) == MOQ_OK) {
            *out_port = port;
            return t;
        }
    }
    return NULL;
}

static int distinct_groups(void)
{
    pthread_mutex_lock(&g_ps.mu);
    int d = 0;
    for (int i = 0; i < g_ps.n; i++) {
        bool seen = false;
        for (int j = 0; j < i; j++)
            if (g_ps.groups[j] == g_ps.groups[i]) { seen = true; break; }
        if (!seen) d++;
    }
    pthread_mutex_unlock(&g_ps.mu);
    return d;
}

static bool wait_ready(moq_media_sender_t *s, int ticks)
{
    for (int i = 0; i < ticks; i++) {
        if (moq_media_sender_is_ready(s)) return true;
        usleep(20000);
    }
    return false;
}

/* One tiny media object; keeps the server's pump scheduled. */
static void write_media(moq_media_sender_t *s, moq_media_track_t *v,
                        bool starts_group, uint64_t pts)
{
    uint8_t d[64];
    memset(d, 0x5a, sizeof(d));
    moq_rcbuf_t *b = NULL;
    if (moq_rcbuf_create(moq_alloc_default(), d, sizeof(d), &b) != MOQ_OK)
        return;
    moq_media_send_object_t o;
    memset(&o, 0, sizeof(o));
    o.struct_size = sizeof(o);
    o.payload = b;
    o.is_sync = starts_group;
    o.starts_group = starts_group;
    o.presentation_time_us = pts;
    if (moq_media_sender_write(s, v, &o) != MOQ_OK)
        moq_rcbuf_decref(b);
}

int main(int argc, char **argv)
{
    const char *cert = NULL, *key = NULL;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--cert") == 0) cert = argv[++i];
        else if (strcmp(argv[i], "--key") == 0) key = argv[++i];
    }
    if (!cert || !key) { printf("usage: --cert <pem> --key <pem>\n"); return 2; }

    memset(&g_ps, 0, sizeof(g_ps));
    pthread_mutex_init(&g_ps.mu, NULL);
    atomic_store(&g_ps.raise_forward, false);

    int port = 0;
    moq_pq_threaded_t *srv = psrv_start(cert, key, &port);
    MOQ_TEST_CHECK(srv != NULL);
    if (!srv) return 1;

    char url[64];
    snprintf(url, sizeof(url), "moqt://127.0.0.1:%d", port);
    moq_endpoint_cfg_t ec;
    moq_endpoint_cfg_init(&ec);
    ec.url = (moq_bytes_t){ (const uint8_t *)url, strlen(url) };
    ec.insecure_skip_verify = true;

    moq_bytes_t parts[2] = { MOQ_BYTES_LITERAL("svc"),
                             MOQ_BYTES_LITERAL("demo") };
    moq_media_sender_cfg_t cfg;
    moq_media_sender_cfg_init_live_sized(&cfg, sizeof(cfg));
    cfg.namespace_ = (moq_namespace_t){ parts, 2 };
    cfg.endpoint = &ec;
    cfg.publish_tracks = true;
    const uint64_t interval_us = 300000;   /* 300 ms */
    cfg.catalog_refresh_interval_us = interval_us;
    moq_media_sender_t *s = NULL;
    MOQ_TEST_CHECK_EQ_INT((int)moq_media_sender_create(&cfg, &s), (int)MOQ_OK);

    moq_media_track_t *v = NULL;
    moq_media_track_cfg_t tc;
    moq_media_track_cfg_init(&tc);
    tc.name = (moq_bytes_t){ (const uint8_t *)"v", 1 };
    tc.media_type = MOQ_MEDIA_TYPE_VIDEO;
    tc.packaging = MOQ_MEDIA_PACKAGING_RAW;
    tc.codec = (moq_bytes_t){ (const uint8_t *)"av01", 4 };
    tc.bitrate = 1500000;
    tc.is_live = true;
    MOQ_TEST_CHECK_EQ_INT((int)moq_media_sender_add_track(s, &tc, &v),
                          (int)MOQ_OK);

    MOQ_TEST_CHECK(wait_ready(s, 400));

    /* NO-DEMAND WINDOW: forward is 0, so no refresh may stage and no catalog
     * object may arrive. Hold it for several intervals so a deadline that was
     * never re-armed is unambiguously in the past. */
    uint64_t pts = 0;
    for (int i = 0; i < 24; i++) {                /* ~1.2 s of media traffic */
        write_media(s, v, i == 0, pts);
        pts += 33000;
        usleep(50000);
    }
    pthread_mutex_lock(&g_ps.mu);
    int during_nodemand = g_ps.n;
    pthread_mutex_unlock(&g_ps.mu);
    MOQ_TEST_CHECK_EQ_INT(during_nodemand, 0);    /* no demand -> nothing */

    /* THE EDGE. */
    atomic_store(&g_ps.raise_forward, true);
    uint64_t edge_us = 0;
    for (int i = 0; i < 200; i++) {
        pthread_mutex_lock(&g_ps.mu);
        edge_us = g_ps.raised ? g_ps.raised_at_us : 0;
        pthread_mutex_unlock(&g_ps.mu);
        if (edge_us) break;
        write_media(s, v, false, pts); pts += 33000;
        usleep(10000);
    }
    MOQ_TEST_CHECK(edge_us != 0);

    /* Wait for the first generation ABOVE the initial one -- that is the
     * refresh. The initial catalog (group 0) is not one and may arrive at
     * once, which is correct. */
    uint64_t refresh_at = 0, refresh_group = 0, first_group = 0;
    for (int i = 0; i < 400 && !refresh_at; i++) {
        pthread_mutex_lock(&g_ps.mu);
        if (g_ps.have_first) first_group = g_ps.first_group;
        for (int k = 0; k < g_ps.n; k++) {
            if (g_ps.have_first && g_ps.groups[k] > g_ps.first_group) {
                refresh_at = g_ps.at_us[k];
                refresh_group = g_ps.groups[k];
                break;
            }
        }
        pthread_mutex_unlock(&g_ps.mu);
        if (!refresh_at) {
            write_media(s, v, false, pts); pts += 33000;
            usleep(10000);
        }
    }

    MOQ_TEST_CHECK(refresh_at != 0);
    if (refresh_at && edge_us) {
        MOQ_TEST_CHECK_EQ_U64(first_group, 0);
        MOQ_TEST_CHECK_EQ_U64(refresh_group, first_group + 1);
        uint64_t delay_us = refresh_at > edge_us ? refresh_at - edge_us : 0;
        /* The clock was re-armed AT the edge: the first refresh cannot precede
         * edge + interval. A generous floor (2/3 of the interval) keeps the
         * assertion robust while still separating it from the un-re-armed
         * behaviour, which fires within a few milliseconds of the edge. */
        printf("[demand-edge] first refresh %llu us after the edge "
               "(interval %llu us)\n",
               (unsigned long long)delay_us, (unsigned long long)interval_us);
        MOQ_TEST_CHECK(delay_us >= (interval_us * 2) / 3);
    }

    moq_media_sender_destroy(s);
    moq_pq_threaded_stop(srv);
    moq_pq_threaded_destroy(srv);
    pthread_mutex_destroy(&g_ps.mu);

    MOQ_TEST_PASS("demand_edge_rearms_refresh_clock");
    printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
