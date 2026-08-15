/*
 * Managed MsQuic settings policy: the listener's pinned path MTU.
 *
 * A client may open with an Initial that carries no ClientHello, so the
 * server's first reply goes out before any peer transport parameter has been
 * received. At MsQuic's default that reply measured 1260 bytes of UDP payload;
 * the observed peer sent exclusively 1252-byte datagrams, did not process the
 * reply, and retransmitted until it gave up. Capping server datagrams at 1252
 * in a proxy took it from 0/6 to 6/6. The managed LISTENER therefore pins both
 * MTU bounds so the same cap holds at the source; the managed CLIENT declares
 * no MTU policy at all.
 *
 * RFC 9000 §18.2 defines max_udp_payload_size as the largest UDP payload an
 * endpoint is willing to receive and notes larger datagrams are unlikely to be
 * processed; §14 permits a receiver to discard datagrams exceeding its size
 * constraints. The peer's discard was permitted, not mandated -- these
 * assertions pin our own conservative listener, not an obligation on it.
 *
 * This exercises `mgd_build_settings()` -- the same construction
 * `moq_msquic_managed_create()` hands to ConfigurationOpen() -- rather than
 * rebuilding the desired settings, which would assert nothing. Every case
 * selects its behavior solely through the configuration's own perspective. It
 * performs no I/O and opens no transport.
 */
#include <moq/msquic.h>
#include <moq/msquic_managed.h>

#include <msquic.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* the managed settings builder (msquic_managed.c, MOQ_MSQUIC_TESTING only) --
 * the SAME construction moq_msquic_managed_create() hands to
 * ConfigurationOpen(); static and unreachable in production builds. It takes
 * the configuration alone, so the perspective it acts on is the one the
 * configuration declares -- this test cannot pair a perspective with a config
 * that does not carry it. */
extern void mgd_build_settings(const moq_msquic_managed_cfg_t *cfg,
                               QUIC_SETTINGS *out);

/* Expected values, owned by this test and written from the interop evidence
 * rather than read back from the product's own constants. */
#define TS_PATH_MTU        1280u
#define TS_IPV4_HEADER       20u
#define TS_IPV6_HEADER       40u
#define TS_UDP_HEADER         8u
#define TS_IPV4_PAYLOAD    1252u   /* what the observed peer sent, and accepted */
#define TS_IPV6_PAYLOAD    1232u

static int failures;

#define CHECK(cond) do {                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_EQ_U32(got_, want_) do {                                       \
        uint32_t g_ = (uint32_t)(got_), w_ = (uint32_t)(want_);              \
        if (g_ != w_) {                                                      \
            fprintf(stderr, "FAIL: %s:%d: %s == %u, expected %s == %u\n",    \
                    __FILE__, __LINE__, #got_, g_, #want_, w_);              \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* A minimally valid managed config for each perspective. The perspective is
 * the only thing that selects the MTU policy, so it is the only axis these
 * cases vary. */
static void cfg_init(moq_msquic_managed_cfg_t *cfg, moq_perspective_t persp,
                     uint64_t idle_ms)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->struct_size = (uint32_t)sizeof(*cfg);
    cfg->alloc = moq_alloc_default();
    cfg->perspective = persp;
    cfg->idle_timeout_ms = idle_ms;
}

int main(void)
{
    /* Base settings both perspectives inherit, established independently of
     * the builder so a regression in moq_msquic_settings_init() is visible
     * here rather than silently adopted. */
    QUIC_SETTINGS base;
    moq_msquic_settings_init(&base);
    CHECK(base.IsSet.SendBufferingEnabled == TRUE);
    CHECK(base.SendBufferingEnabled == FALSE);
    CHECK(base.IsSet.MinimumMtu == FALSE);
    CHECK(base.IsSet.MaximumMtu == FALSE);

    /* -- a SERVER configuration pins both bounds ---------------------- */
    {
        moq_msquic_managed_cfg_t cfg;
        cfg_init(&cfg, MOQ_PERSPECTIVE_SERVER, 0);
        QUIC_SETTINGS s;
        mgd_build_settings(&cfg, &s);

        CHECK(s.IsSet.MinimumMtu == TRUE);
        CHECK(s.IsSet.MaximumMtu == TRUE);
        CHECK_EQ_U32(s.MinimumMtu, TS_PATH_MTU);
        CHECK_EQ_U32(s.MaximumMtu, TS_PATH_MTU);
        /* the two bounds are mutually compatible -- a maximum below the
         * minimum is the failure mode a maximum-only change would have
         * introduced on this MsQuic revision */
        CHECK(s.MinimumMtu <= s.MaximumMtu);

        /* The UDP payloads this path MTU actually caps, computed from the
         * protocol header sizes rather than restated. Both subtractions are
         * guarded so an implausible MTU cannot wrap. */
        const uint32_t mtu = s.MaximumMtu;
        CHECK(mtu > TS_IPV4_HEADER + TS_UDP_HEADER);
        CHECK(mtu > TS_IPV6_HEADER + TS_UDP_HEADER);
        if (mtu > TS_IPV4_HEADER + TS_UDP_HEADER)
            CHECK_EQ_U32(mtu - TS_IPV4_HEADER - TS_UDP_HEADER, TS_IPV4_PAYLOAD);
        if (mtu > TS_IPV6_HEADER + TS_UDP_HEADER)
            CHECK_EQ_U32(mtu - TS_IPV6_HEADER - TS_UDP_HEADER, TS_IPV6_PAYLOAD);
        /* ... and the IPv4 cap does not exceed what the observed peer used,
         * which is what the oversized first reply did. */
        if (mtu > TS_IPV4_HEADER + TS_UDP_HEADER)
            CHECK(mtu - TS_IPV4_HEADER - TS_UDP_HEADER <= TS_IPV4_PAYLOAD);

        /* the base policy survives the MTU addition */
        CHECK(s.IsSet.SendBufferingEnabled == TRUE);
        CHECK(s.SendBufferingEnabled == FALSE);
        /* no idle override was configured */
        CHECK(s.IsSet.IdleTimeoutMs == base.IsSet.IdleTimeoutMs);
        CHECK(s.IsSet.HandshakeIdleTimeoutMs ==
              base.IsSet.HandshakeIdleTimeoutMs);
    }

    /* -- a CLIENT configuration declares no MTU policy ---------------- */
    {
        moq_msquic_managed_cfg_t cfg;
        cfg_init(&cfg, MOQ_PERSPECTIVE_CLIENT, 0);
        QUIC_SETTINGS s;
        mgd_build_settings(&cfg, &s);

        /* Assert the DECLARATION bits, not MsQuic's runtime defaults: the
         * client is unchanged by this fix precisely because LibMoQ sets
         * neither bound. */
        CHECK(s.IsSet.MinimumMtu == FALSE);
        CHECK(s.IsSet.MaximumMtu == FALSE);
        CHECK(s.IsSet.SendBufferingEnabled == TRUE);
        CHECK(s.SendBufferingEnabled == FALSE);
    }

    /* -- the configured idle timeout still bounds both phases, for both
     *    perspectives, and does not disturb the MTU declarations ------- */
    {
        const uint64_t idle = 4321u;
        moq_msquic_managed_cfg_t cfg;
        QUIC_SETTINGS s;

        cfg_init(&cfg, MOQ_PERSPECTIVE_SERVER, idle);
        mgd_build_settings(&cfg, &s);
        CHECK(s.IsSet.IdleTimeoutMs == TRUE);
        CHECK(s.IsSet.HandshakeIdleTimeoutMs == TRUE);
        CHECK(s.IdleTimeoutMs == idle);
        CHECK(s.HandshakeIdleTimeoutMs == idle);
        CHECK(s.IsSet.MinimumMtu == TRUE);
        CHECK(s.IsSet.MaximumMtu == TRUE);

        cfg_init(&cfg, MOQ_PERSPECTIVE_CLIENT, idle);
        mgd_build_settings(&cfg, &s);
        CHECK(s.IsSet.IdleTimeoutMs == TRUE);
        CHECK(s.IsSet.HandshakeIdleTimeoutMs == TRUE);
        CHECK(s.IdleTimeoutMs == idle);
        CHECK(s.HandshakeIdleTimeoutMs == idle);
        CHECK(s.IsSet.MinimumMtu == FALSE);
        CHECK(s.IsSet.MaximumMtu == FALSE);
    }

    /* -- a prefix-sized config is handled exactly as before ----------- */
    {
        moq_msquic_managed_cfg_t cfg;
        cfg_init(&cfg, MOQ_PERSPECTIVE_SERVER, 777u);
        cfg.struct_size =
            (uint32_t)offsetof(moq_msquic_managed_cfg_t, version);
        QUIC_SETTINGS s;
        mgd_build_settings(&cfg, &s);
        /* idle_timeout_ms and perspective both precede `version`, so a
         * prefix-sized caller still supplies them, and the listener MTU is a
         * policy of the perspective rather than of the config size. */
        CHECK(s.IsSet.IdleTimeoutMs == TRUE);
        CHECK(s.IdleTimeoutMs == 777u);
        CHECK(s.IsSet.MinimumMtu == TRUE);
        CHECK(s.IsSet.MaximumMtu == TRUE);
        CHECK_EQ_U32(s.MinimumMtu, TS_PATH_MTU);
        CHECK_EQ_U32(s.MaximumMtu, TS_PATH_MTU);
    }

    if (failures != 0) {
        fprintf(stderr, "msquic_settings: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: msquic_settings\n");
    return 0;
}
