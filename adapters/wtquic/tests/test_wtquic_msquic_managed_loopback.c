/*
 * Real loopback smoke for the managed wtquic-MsQuic facade, both perspectives.
 *
 * CLIENT: stands up the EXISTING public wtquic-MsQuic listener and connects the
 * managed facade client to it, proving actual WT-Protocol negotiation and
 * MoQ-session/attach-adapter creation at the negotiated version — for draft-16
 * and draft-18 SEPARATELY. Establishment is observed through the facade's own
 * condition variable (broadcast on establishment).
 *
 * SERVER: stands up the managed facade server and connects a raw wtquic client,
 * proving the server admits AND establishes a child (draft-18 version + attach
 * adapter + retained ws, read under the child's lane guard) and then quiesces it
 * server-first at teardown (env_close must return). This case recompiles the
 * facade with MOQ_WTQ_MM_TESTING for the guarded child observation.
 *
 * Establishment only: no subscribe/object flow (that needs the pump slice), no
 * datagrams, no service integration. The per-call wait timeout and overall
 * deadline are pure hang guards, never timing assertions.
 *
 * Usage: test_wtquic_msquic_managed_loopback <cert.pem> <key.pem>
 */
#include <moq/wtquic_msquic_managed.h>

#include "wtquic_msquic_managed_test_internal.h"

#include <wtquic/wtquic.h>
#include <wtquic/wtquic_msquic.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_fail;
#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);    \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

#define WAIT_SECS 30

static const char *cert_path;
static const char *key_path;

static int dummy_pump(moq_wtquic_msquic_managed_t *m,
                      moq_wtquic_msquic_managed_lane_t *lane, uint64_t now_us,
                      void *user)
{
    (void)m;
    (void)lane;
    (void)now_us;
    (void)user;
    return 0;
}

/* One offer/version pair over a fresh listener + facade client. */
static void run_case(const char *token, moq_version_t expected)
{
    /* server: the public wtquic-MsQuic listener, supporting both MoQ
     * subprotocols. No-op events, no admission, no MoQ session — it only
     * accepts the WebTransport session so the client can establish. */
    wtq_msquic_env_cfg_t secfg = WTQ_MSQUIC_ENV_CFG_INIT;
    wtq_msquic_env_t *senv = NULL;
    CHECK(wtq_msquic_env_open(&secfg, &senv) == WTQ_OK);
    if (senv == NULL)
        return;

    static const char *const server_protos[] = { "moqt-18", "moqt-16" };
    wtq_serve_config_t serve = WTQ_SERVE_CONFIG_INIT;
    serve.path = "/moq";
    serve.subprotocols = server_protos;
    serve.subprotocol_count = 2;
    serve.require_subprotocol = true;

    wtq_session_events_t sev;
    wtq_session_events_init(&sev);
    wtq_msquic_listener_cfg_t lcfg = WTQ_MSQUIC_LISTENER_CFG_INIT;
    lcfg.bind_address = "127.0.0.1";
    lcfg.port = 0;
    lcfg.cert_file = cert_path;
    lcfg.key_file = key_path;
    lcfg.paths = &serve;
    lcfg.path_count = 1;
    lcfg.events = &sev;
    lcfg.user = NULL;
    wtq_msquic_listener_t *listener = NULL;
    CHECK(wtq_msquic_listener_start(senv, &lcfg, &listener) == WTQ_OK);
    if (listener == NULL) {
        wtq_msquic_env_close(senv);
        return;
    }
    uint16_t port = wtq_msquic_listener_port(listener);

    /* client: the managed facade, offering exactly `token` so the negotiated
     * version is unambiguous. */
    const char *offers[] = { token };
    moq_wtquic_msquic_managed_cfg_t cfg;
    moq_wtquic_msquic_managed_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.alloc = moq_alloc_default();
    cfg.perspective = MOQ_PERSPECTIVE_CLIENT;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.insecure_skip_verify = true;
    cfg.wt_path = "/moq";
    cfg.wt_protocols = offers;
    cfg.wt_protocol_count = 1;
    cfg.on_lane_pump = dummy_pump;

    moq_wtquic_msquic_managed_t *m = NULL;
    CHECK(moq_wtquic_msquic_managed_create(&cfg, &m) == MOQ_OK);
    if (m != NULL) {
        /* one bounded wait: establishment latches activity -> MOQ_OK; a client
         * fatal/closed -> MOQ_ERR_CLOSED; the WAIT_SECS bound is only a hang
         * guard (it would surface as MOQ_DONE) */
        moq_result_t wr =
            moq_wtquic_msquic_managed_wait(m, (uint64_t)WAIT_SECS * 1000000u);
        CHECK(wr == MOQ_OK);
        /* exact negotiated-version publication (negotiated_version is set only
         * after the MoQ session + attach adapter are both created) */
        CHECK(moq_wtquic_msquic_managed_negotiated_version(m) == expected);
        CHECK(!moq_wtquic_msquic_managed_is_fatal(m));
        /* clean client stop + teardown */
        CHECK(moq_wtquic_msquic_managed_stop(m) == MOQ_OK);
        moq_wtquic_msquic_managed_destroy(m);
    }

    wtq_msquic_listener_stop(listener);
    wtq_msquic_env_close(senv);
}

/*
 * Server: the managed listener admits + establishes a raw client, then tears
 * down cleanly. This is the diagnostic for the pre-barrier connection quiesce —
 * without it the server's env_close blocks on the established child's session.
 */
struct raw_cli {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int established;
};
static void raw_established(wtq_session_t *s, wtq_str_t sub, void *user)
{
    (void)s;
    (void)sub;
    struct raw_cli *r = user;
    pthread_mutex_lock(&r->mu);
    r->established = 1;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
}
static bool raw_wait_established(struct raw_cli *r)
{
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += WAIT_SECS;
    pthread_mutex_lock(&r->mu);
    bool ok = true;
    while (r->established == 0 && ok)
        ok = pthread_cond_timedwait(&r->cv, &r->mu, &dl) == 0;
    bool set = r->established != 0;
    pthread_mutex_unlock(&r->mu);
    return set;
}

static void run_server_case(void)
{
    const char *offers[] = { "moqt-18", "moqt-16" };
    moq_wtquic_msquic_managed_cfg_t cfg;
    moq_wtquic_msquic_managed_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.alloc = moq_alloc_default();
    cfg.perspective = MOQ_PERSPECTIVE_SERVER;
    cfg.host = "127.0.0.1"; /* bind address */
    cfg.port = 0;           /* ephemeral */
    cfg.cert_path = cert_path;
    cfg.key_path = key_path;
    cfg.wt_path = "/moq";
    cfg.wt_protocols = offers;
    cfg.wt_protocol_count = 2;
    cfg.max_connections = 4;
    cfg.on_lane_pump = dummy_pump;

    moq_wtquic_msquic_managed_t *srv = NULL;
    CHECK(moq_wtquic_msquic_managed_create(&cfg, &srv) == MOQ_OK);
    if (srv == NULL)
        return;
    uint16_t port = moq_wtquic_msquic_managed_port(srv);
    CHECK(port != 0); /* bound-port reporting */

    struct raw_cli rc;
    memset(&rc, 0, sizeof(rc));
    CHECK(pthread_mutex_init(&rc.mu, NULL) == 0);
    CHECK(pthread_cond_init(&rc.cv, NULL) == 0);
    wtq_msquic_env_cfg_t ecfg = WTQ_MSQUIC_ENV_CFG_INIT;
    wtq_msquic_env_t *cenv = NULL;
    CHECK(wtq_msquic_env_open(&ecfg, &cenv) == WTQ_OK);
    /* the raw client's creator reference: released AFTER cenv is closed, per the
     * post-env_close session lifetime rule. Held at function scope so teardown
     * can outlive the connect block. */
    wtq_session_t *cs = NULL;
    if (cenv != NULL && port != 0) {
        static const char *const cprotos[] = { "moqt-18" };
        wtq_session_events_t cev;
        wtq_session_events_init(&cev);
        cev.on_established = raw_established;
        wtq_connect_config_t connect = WTQ_CONNECT_CONFIG_INIT;
        connect.authority = "localhost";
        connect.path = "/moq";
        connect.subprotocols = cprotos;
        connect.subprotocol_count = 1;
        wtq_msquic_client_cfg_t cli = WTQ_MSQUIC_CLIENT_CFG_INIT;
        cli.server_name = "127.0.0.1";
        cli.port = port;
        cli.insecure_skip_verify = true;
        cli.connect = &connect;
        cli.events = &cev;
        cli.user = &rc;
        CHECK(wtq_msquic_client_connect(cenv, &cli, &cs) == WTQ_OK);
        /* the client establishes only if the managed server admitted + accepted
         * the WT session; the server then holds exactly one live child */
        CHECK(raw_wait_established(&rc));
        CHECK(moq_wtquic_msquic_managed_conn_count(srv) == 1);
        /* server negotiation is per-connection: the facade accessor stays 0 */
        CHECK(moq_wtquic_msquic_managed_negotiated_version(srv) == 0);

        /* conn_count == 1 only proves ADMISSION. Prove the managed server also
         * completed ESTABLISHMENT: under the child's lane guard it holds the
         * negotiated draft-18 version, an attach adapter, and the retained ws. */
        moq_version_t child_ver = 0;
        bool child_has_adapter = false, child_has_ws = false;
        CHECK(moq_wtquic_msquic_managed_test_server_child(
            srv, &child_ver, &child_has_adapter, &child_has_ws));
        CHECK(child_ver == MOQ_VERSION_DRAFT_18);
        CHECK(child_has_adapter);
        CHECK(child_has_ws);
    }

    /* Prove SERVER-INITIATED quiescence: keep the client alive and stop the
     * server first, so the server's pre-barrier quiesce is what closes the
     * established child (if we closed the client first the peer could close the
     * child, hiding whether the server can). env_close must still return. */
    CHECK(moq_wtquic_msquic_managed_stop(srv) == MOQ_OK);
    moq_wtquic_msquic_managed_destroy(srv);

    /* now tear down the client: close the env, then release the creator ref */
    if (cenv != NULL)
        wtq_msquic_env_close(cenv);
    if (cs != NULL)
        wtq_session_release(cs);
    pthread_cond_destroy(&rc.cv);
    pthread_mutex_destroy(&rc.mu);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <cert.pem> <key.pem>\n", argv[0]);
        return 2;
    }
    cert_path = argv[1];
    key_path = argv[2];

    run_case("moqt-16", MOQ_VERSION_DRAFT_16);
    run_case("moqt-18", MOQ_VERSION_DRAFT_18);
    run_server_case();

    if (g_fail != 0) {
        fprintf(stderr, "FAILED: test_wtquic_msquic_managed_loopback (%d)\n",
                g_fail);
        return 1;
    }
    printf("PASS: test_wtquic_msquic_managed_loopback\n");
    return 0;
}
