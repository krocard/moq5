#include "config.h"

#include "snapshot.h"

#include <moqrelay/capacity.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Vendored single-header JSON parser (same one media/msf ships; the CLI
 * points its include path at that vendor directory — one copy in tree). */
#include "json.h"

#define CFG_MAX_ENTRIES  (1u << 20)          /* per-pool sanity cap      */
#define CFG_MAX_BYTES    (UINT64_C(1) << 40) /* per-budget sanity cap    */

static void
cfg_err(char *err, size_t err_len, const char *msg)
{
    if (err != NULL && err_len > 0) {
        snprintf(err, err_len, "%s", msg);
    }
}

static bool
num_u64(struct json_value_s *v, uint64_t *out)
{
    struct json_number_s *n = json_value_as_number(v);
    if (n == NULL) {
        return false;
    }
    /* strtoull silently accepts a leading '-' (wrapping) and skips leading
     * whitespace; reject anything but a plain non-negative integer, and
     * catch overflow via ERANGE. */
    const char *p = n->number;
    if (!isdigit((unsigned char)p[0])) {
        return false;   /* no sign, no space, no leading dot */
    }
    errno = 0;
    char *end = NULL;
    unsigned long long x = strtoull(p, &end, 10);
    if (end == NULL || *end != '\0' || errno == ERANGE) {
        return false;
    }
    *out = (uint64_t)x;
    return true;
}

static bool
copy_string(struct json_value_s *v, char *dst, size_t cap)
{
    struct json_string_s *s = json_value_as_string(v);
    if (s == NULL || s->string_size >= cap) {
        return false;
    }
    memcpy(dst, s->string, s->string_size);
    dst[s->string_size] = '\0';
    return true;
}

static moqr_result_t
parse_listener(struct json_object_s *o, moqr_cli_config_t *out,
               char *err, size_t err_len)
{
    bool have_port = false;
    for (struct json_object_element_s *e = o->start; e != NULL;
         e = e->next) {
        const char *k = e->name->string;
        if (strcmp(k, "transport") == 0) {
            char t[32];
            if (!copy_string(e->value, t, sizeof(t)) ||
                strcmp(t, "msquic") != 0) {
                cfg_err(err, err_len,
                        "listener.transport: only \"msquic\" is supported");
                return MOQR_ERR_INVAL;
            }
        } else if (strcmp(k, "host") == 0) {
            if (!copy_string(e->value, out->host, sizeof(out->host))) {
                cfg_err(err, err_len, "listener.host: invalid string");
                return MOQR_ERR_INVAL;
            }
        } else if (strcmp(k, "port") == 0) {
            uint64_t p = 0;
            if (!num_u64(e->value, &p) || p < 1 || p > 65535) {
                cfg_err(err, err_len, "listener.port: need 1..65535");
                return MOQR_ERR_INVAL;
            }
            out->port = (int)p;
            have_port = true;
        } else if (strcmp(k, "cert") == 0) {
            if (!copy_string(e->value, out->cert, sizeof(out->cert))) {
                cfg_err(err, err_len, "listener.cert: invalid string");
                return MOQR_ERR_INVAL;
            }
        } else if (strcmp(k, "key") == 0) {
            if (!copy_string(e->value, out->key, sizeof(out->key))) {
                cfg_err(err, err_len, "listener.key: invalid string");
                return MOQR_ERR_INVAL;
            }
        } else if (strcmp(k, "versions") == 0) {
            /* An ORDERED, non-empty set of supported drafts, most preferred
             * first. One entry keeps the exact-version listener; several offer
             * several ALPNs on one listener and ALPN performs the version
             * negotiation (draft-18 Section 3.1/3.1.1). Everything malformed
             * fails closed: empty, over-bound, duplicate, unknown, boolean,
             * fractional or non-numeric. */
            struct json_array_s *arr = json_value_as_array(e->value);
            if (arr == NULL || arr->length == 0) {
                cfg_err(err, err_len,
                        "listener.versions: a non-empty array is required");
                return MOQR_ERR_INVAL;
            }
            if (arr->length > MOQR_CLI_MAX_VERSIONS) {
                cfg_err(err, err_len, "listener.versions: too many entries");
                return MOQR_ERR_INVAL;
            }
            size_t n = 0;
            for (struct json_array_element_s *ae = arr->start; ae != NULL;
                 ae = ae->next) {
                uint64_t v = 0;
                if (!num_u64(ae->value, &v) || (v != 16 && v != 18)) {
                    cfg_err(err, err_len,
                            "listener.versions: only 16 and 18 are known");
                    return MOQR_ERR_INVAL;
                }
                moq_version_t mv = (v == 16) ? MOQ_VERSION_DRAFT_16
                                             : MOQ_VERSION_DRAFT_18;
                for (size_t j = 0; j < n; j++) {
                    if (out->versions[j] == mv) {
                        cfg_err(err, err_len,
                                "listener.versions: duplicate entry");
                        return MOQR_ERR_INVAL;
                    }
                }
                snprintf(out->alpn_buf[n], sizeof(out->alpn_buf[n]),
                         "moqt-%" PRIu64, v);
                out->alpns[n] = out->alpn_buf[n];
                out->versions[n] = mv;
                if (n == 0) {
                    out->version = (uint32_t)v;   /* the preferred entry */
                }
                n++;
            }
            out->alpn_count = n;
            out->version_count = n;
        } else if (strcmp(k, "insecure_skip_verify") == 0) {
            if (json_value_is_true(e->value)) {
                out->insecure_skip_verify = true;
            } else if (json_value_is_false(e->value)) {
                out->insecure_skip_verify = false;
            } else {
                cfg_err(err, err_len,
                        "listener.insecure_skip_verify: need true/false");
                return MOQR_ERR_INVAL;
            }
        } else if (strcmp(k, "lanes") == 0) {
            uint64_t n = 0;
            if (!num_u64(e->value, &n) || n < 1 || n > MOQR_CLI_MAX_LANES) {
                cfg_err(err, err_len, "listener.lanes: need 1..64");
                return MOQR_ERR_INVAL;
            }
            out->lanes = (uint32_t)n;
        } else {
            cfg_err(err, err_len, "listener: unknown key");
            return MOQR_ERR_INVAL;
        }
    }
    if (!have_port) {
        cfg_err(err, err_len, "listener.port is required");
        return MOQR_ERR_INVAL;
    }
    return MOQR_OK;
}

static moqr_result_t
budget_entry_u32(struct json_value_s *v, uint32_t *dst,
                 const char *what, char *err, size_t err_len)
{
    uint64_t x = 0;
    if (!num_u64(v, &x) || x > CFG_MAX_ENTRIES) {
        snprintf(err, err_len, "budgets.%s: need 0..%u", what,
                 CFG_MAX_ENTRIES);
        return MOQR_ERR_INVAL;
    }
    *dst = (uint32_t)x;
    return MOQR_OK;
}

static moqr_result_t
parse_budgets(struct json_object_s *o, moqr_cli_config_t *out,
              char *err, size_t err_len)
{
    for (struct json_object_element_s *e = o->start; e != NULL;
         e = e->next) {
        const char *k = e->name->string;
        moqr_result_t rc = MOQR_OK;
        if (strcmp(k, "max_tracks") == 0) {
            rc = budget_entry_u32(e->value, &out->core.max_tracks, k, err,
                                  err_len);
        } else if (strcmp(k, "max_subs") == 0) {
            rc = budget_entry_u32(e->value, &out->core.max_subs, k, err,
                                  err_len);
        } else if (strcmp(k, "max_bindings") == 0) {
            rc = budget_entry_u32(e->value, &out->core.max_bindings, k, err,
                                  err_len);
        } else if (strcmp(k, "max_ns_nodes") == 0) {
            rc = budget_entry_u32(e->value, &out->core.max_ns_nodes, k, err,
                                  err_len);
        } else if (strcmp(k, "max_ns_subs") == 0) {
            rc = budget_entry_u32(e->value, &out->core.max_ns_subs, k, err,
                                  err_len);
        } else if (strcmp(k, "max_intents") == 0) {
            rc = budget_entry_u32(e->value, &out->core.max_intents, k, err,
                                  err_len);
        } else if (strcmp(k, "name_intern_bytes") == 0) {
            rc = budget_entry_u32(e->value, &out->core.name_intern_bytes, k,
                                  err, err_len);
        } else if (strcmp(k, "log_max_subgroups") == 0) {
            rc = budget_entry_u32(e->value, &out->core.log_max_subgroups, k,
                                  err, err_len);
        } else if (strcmp(k, "log_max_objects") == 0) {
            rc = budget_entry_u32(e->value,
                                  &out->core.log_max_objects_per_group, k,
                                  err, err_len);
        } else if (strcmp(k, "log_max_cursors") == 0) {
            rc = budget_entry_u32(e->value, &out->core.log_max_cursors, k,
                                  err, err_len);
        } else if (strcmp(k, "log_max_chunk_nodes") == 0) {
            rc = budget_entry_u32(e->value, &out->core.log_max_chunk_nodes, k,
                                  err, err_len);
        } else if (strcmp(k, "log") == 0) {
            struct json_object_s *lo = json_value_as_object(e->value);
            if (lo == NULL) {
                cfg_err(err, err_len, "budgets.log: need an object");
                return MOQR_ERR_INVAL;
            }
            for (struct json_object_element_s *le = lo->start;
                 le != NULL; le = le->next) {
                const char *lk = le->name->string;
                uint64_t x = 0;
                if (!num_u64(le->value, &x)) {
                    cfg_err(err, err_len, "budgets.log: need numbers");
                    return MOQR_ERR_INVAL;
                }
                if (strcmp(lk, "max_groups") == 0) {
                    if (x > CFG_MAX_ENTRIES) {
                        cfg_err(err, err_len,
                                "budgets.log.max_groups: oversized");
                        return MOQR_ERR_INVAL;
                    }
                    out->core.log_budget.max_groups = (uint32_t)x;
                } else if (strcmp(lk, "max_bytes") == 0) {
                    if (x > CFG_MAX_BYTES) {
                        cfg_err(err, err_len,
                                "budgets.log.max_bytes: oversized");
                        return MOQR_ERR_INVAL;
                    }
                    out->core.log_budget.max_bytes = x;
                } else if (strcmp(lk, "max_age_us") == 0) {
                    out->core.log_budget.max_age_us = x;
                } else {
                    cfg_err(err, err_len, "budgets.log: unknown key");
                    return MOQR_ERR_INVAL;
                }
            }
        } else if (strcmp(k, "cross_shard") == 0) {
            struct json_object_s *xo = json_value_as_object(e->value);
            if (xo == NULL) {
                cfg_err(err, err_len, "budgets.cross_shard: need an object");
                return MOQR_ERR_INVAL;
            }
            for (struct json_object_element_s *xe = xo->start; xe != NULL;
                 xe = xe->next) {
                const char *xk = xe->name->string;
                /* Key whitelist FIRST: a rejected key reports "unknown key"
                 * loudly, never a misleading type complaint. */
                uint32_t *dst = NULL;
                bool is_bytes = false;
                if (strcmp(xk, "journal_entries") == 0) {
                    dst = &out->cross_shard.journal_entries;
                } else if (strcmp(xk, "mailbox_entries") == 0) {
                    dst = &out->cross_shard.mailbox_entries;
                } else if (strcmp(xk, "demand_channel_entries") == 0) {
                    dst = &out->cross_shard.demand_channel_entries;
                } else if (strcmp(xk, "pending_demands") == 0) {
                    dst = &out->cross_shard.pending_demands;
                } else if (strcmp(xk, "subgroup_slots") == 0) {
                    dst = &out->cross_shard.subgroup_slots;
                } else if (strcmp(xk, "demand_channel_bytes") == 0) {
                    is_bytes = true;
                } else {
                    cfg_err(err, err_len,
                            "budgets.cross_shard: unknown key");
                    return MOQR_ERR_INVAL;
                }
                uint64_t x = 0;
                if (!num_u64(xe->value, &x)) {
                    cfg_err(err, err_len,
                            "budgets.cross_shard: need non-negative numbers");
                    return MOQR_ERR_INVAL;
                }
                if (is_bytes) {
                    if (x > CFG_MAX_BYTES) {
                        cfg_err(err, err_len,
                                "budgets.cross_shard.demand_channel_bytes: "
                                "oversized");
                        return MOQR_ERR_INVAL;
                    }
                    out->cross_shard.demand_channel_bytes = x;
                } else {
                    if (x > CFG_MAX_ENTRIES) {
                        cfg_err(err, err_len,
                                "budgets.cross_shard: entry count oversized");
                        return MOQR_ERR_INVAL;
                    }
                    *dst = (uint32_t)x;
                }
            }
        } else {
            cfg_err(err, err_len, "budgets: unknown key");
            return MOQR_ERR_INVAL;
        }
        if (rc != MOQR_OK) {
            return rc;
        }
    }
    return MOQR_OK;
}

static moqr_result_t
parse_telemetry(struct json_object_s *o, moqr_cli_config_t *out, char *err,
                size_t err_len)
{
    for (struct json_object_element_s *e = o->start; e != NULL;
         e = e->next) {
        const char *k = e->name->string;
        if (strcmp(k, "trace_ring_records") == 0) {
            uint64_t x = 0;
            if (!num_u64(e->value, &x) || x > CFG_MAX_ENTRIES) {
                snprintf(err, err_len,
                         "telemetry.trace_ring_records: need 0..%u",
                         CFG_MAX_ENTRIES);
                return MOQR_ERR_INVAL;
            }
            out->telemetry.trace_ring_records = (uint32_t)x;
        } else {
            cfg_err(err, err_len, "telemetry: unknown key");
            return MOQR_ERR_INVAL;
        }
    }
    return MOQR_OK;
}

static bool
auth_action_from_str(const char *s, moqr_auth_action_t *out)
{
    static const struct {
        const char        *name;
        moqr_auth_action_t val;
    } tab[] = {
        { "client_setup", MOQR_AUTH_CLIENT_SETUP },
        { "server_setup", MOQR_AUTH_SERVER_SETUP },
        { "publish_namespace", MOQR_AUTH_PUBLISH_NAMESPACE },
        { "subscribe_namespace", MOQR_AUTH_SUBSCRIBE_NAMESPACE },
        { "subscribe", MOQR_AUTH_SUBSCRIBE },
        { "request_update", MOQR_AUTH_REQUEST_UPDATE },
        { "publish", MOQR_AUTH_PUBLISH },
        { "fetch", MOQR_AUTH_FETCH },
        { "track_status", MOQR_AUTH_TRACK_STATUS },
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (strcmp(s, tab[i].name) == 0) {
            *out = tab[i].val;
            return true;
        }
    }
    return false;
}

/* "allow" or "deny" only — "defer" and unknowns are rejected: the toy policy
 * is ALLOW/DENY, and DEFER must never be authorable from config. */
static bool
auth_decision_from_str(const char *s, moqr_auth_decision_t *out)
{
    if (strcmp(s, "allow") == 0) {
        *out = MOQR_AUTH_ALLOW;
        return true;
    }
    if (strcmp(s, "deny") == 0) {
        *out = MOQR_AUTH_DENY;
        return true;
    }
    return false;
}

static bool
auth_reason_from_str(const char *s, moqr_auth_reason_t *out)
{
    static const struct {
        const char        *name;
        moqr_auth_reason_t val;
    } tab[] = {
        { "ok", MOQR_AUTH_REASON_OK },
        { "no_token", MOQR_AUTH_REASON_NO_TOKEN },
        { "unscoped", MOQR_AUTH_REASON_UNSCOPED },
        { "expired", MOQR_AUTH_REASON_EXPIRED },
        { "not_yet_valid", MOQR_AUTH_REASON_NOT_YET_VALID },
        { "bad_issuer", MOQR_AUTH_REASON_BAD_ISSUER },
        { "bad_audience", MOQR_AUTH_REASON_BAD_AUDIENCE },
        { "bad_signature", MOQR_AUTH_REASON_BAD_SIGNATURE },
        { "policy", MOQR_AUTH_REASON_POLICY },
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (strcmp(s, tab[i].name) == 0) {
            *out = tab[i].val;
            return true;
        }
    }
    return false;
}

/* Parse one toy rule object into rules[idx] / ns[idx] (both caller-owned). */
static moqr_result_t
parse_auth_rule(struct json_object_s *ro, moqr_cli_auth_t *auth, size_t idx,
                char *err, size_t err_len)
{
    moqr_auth_toy_rule_t *rule = &auth->rules[idx];
    moqr_cli_auth_ns_t   *ns = &auth->ns[idx];
    bool have_action = false;
    bool have_decision = false;
    rule->reason = MOQR_AUTH_REASON_POLICY; /* default deny reason */
    ns->count = 0;
    for (struct json_object_element_s *e = ro->start; e != NULL; e = e->next) {
        const char *k = e->name->string;
        if (strcmp(k, "action") == 0) {
            char s[32];
            moqr_auth_action_t a;
            if (!copy_string(e->value, s, sizeof(s)) ||
                !auth_action_from_str(s, &a)) {
                cfg_err(err, err_len, "auth.rules[].action: unknown action");
                return MOQR_ERR_INVAL;
            }
            rule->action = a;
            have_action = true;
        } else if (strcmp(k, "decision") == 0) {
            char s[16];
            moqr_auth_decision_t d;
            if (!copy_string(e->value, s, sizeof(s)) ||
                !auth_decision_from_str(s, &d)) {
                cfg_err(err, err_len,
                        "auth.rules[].decision: need \"allow\" or \"deny\"");
                return MOQR_ERR_INVAL;
            }
            rule->decision = d;
            have_decision = true;
        } else if (strcmp(k, "reason") == 0) {
            char s[24];
            moqr_auth_reason_t r;
            if (!copy_string(e->value, s, sizeof(s)) ||
                !auth_reason_from_str(s, &r)) {
                cfg_err(err, err_len, "auth.rules[].reason: unknown reason");
                return MOQR_ERR_INVAL;
            }
            rule->reason = r;
        } else if (strcmp(k, "namespace_prefix") == 0) {
            struct json_array_s *arr = json_value_as_array(e->value);
            if (arr == NULL || arr->length > MOQR_CLI_MAX_NS_PARTS) {
                cfg_err(err, err_len,
                        "auth.rules[].namespace_prefix: need 0..8 parts");
                return MOQR_ERR_INVAL;
            }
            size_t j = 0;
            for (struct json_array_element_s *ae = arr->start; ae != NULL;
                 ae = ae->next, j++) {
                struct json_string_s *ps = json_value_as_string(ae->value);
                if (ps == NULL || ps->string_size > MOQR_CLI_MAX_NS_PART_LEN) {
                    cfg_err(err, err_len,
                            "auth.rules[].namespace_prefix: part must be a "
                            "string of <= 64 bytes");
                    return MOQR_ERR_INVAL;
                }
                memcpy(ns->bytes[j], ps->string, ps->string_size);
                ns->parts[j].data = ns->bytes[j];
                ns->parts[j].len = ps->string_size;
            }
            ns->count = j;
        } else {
            cfg_err(err, err_len, "auth.rules[]: unknown key");
            return MOQR_ERR_INVAL;
        }
    }
    if (!have_action || !have_decision) {
        cfg_err(err, err_len,
                "auth.rules[]: action and decision are required");
        return MOQR_ERR_INVAL;
    }
    rule->ns_prefix.parts = ns->parts;
    rule->ns_prefix.count = ns->count;
    return MOQR_OK;
}

static moqr_result_t
parse_auth(struct json_object_s *o, moqr_cli_config_t *out, char *err,
           size_t err_len)
{
    moqr_cli_auth_t     *auth = &out->auth;
    bool                 have_mode = false;
    bool                 have_default = false;
    bool                 have_rules = false;
    moqr_auth_decision_t defd = MOQR_AUTH_DENY;
    struct json_array_s *rules_arr = NULL;

    for (struct json_object_element_s *e = o->start; e != NULL; e = e->next) {
        const char *k = e->name->string;
        if (strcmp(k, "mode") == 0) {
            char s[16];
            if (!copy_string(e->value, s, sizeof(s))) {
                cfg_err(err, err_len, "auth.mode: invalid string");
                return MOQR_ERR_INVAL;
            }
            if (strcmp(s, "allow_all") == 0) {
                auth->mode = MOQR_CLI_AUTH_ALLOW_ALL;
            } else if (strcmp(s, "toy") == 0) {
                auth->mode = MOQR_CLI_AUTH_TOY;
            } else {
                cfg_err(err, err_len,
                        "auth.mode: need \"allow_all\" or \"toy\"");
                return MOQR_ERR_INVAL;
            }
            have_mode = true;
        } else if (strcmp(k, "default") == 0) {
            char s[16];
            if (!copy_string(e->value, s, sizeof(s)) ||
                !auth_decision_from_str(s, &defd)) {
                cfg_err(err, err_len,
                        "auth.default: need \"allow\" or \"deny\"");
                return MOQR_ERR_INVAL;
            }
            have_default = true;
        } else if (strcmp(k, "rules") == 0) {
            rules_arr = json_value_as_array(e->value);
            if (rules_arr == NULL ||
                rules_arr->length > MOQR_CLI_MAX_AUTH_RULES) {
                cfg_err(err, err_len, "auth.rules: need an array of 0..32");
                return MOQR_ERR_INVAL;
            }
            have_rules = true;
        } else {
            cfg_err(err, err_len, "auth: unknown key");
            return MOQR_ERR_INVAL;
        }
    }
    if (!have_mode) {
        cfg_err(err, err_len, "auth.mode is required");
        return MOQR_ERR_INVAL;
    }
    if (auth->mode == MOQR_CLI_AUTH_ALLOW_ALL) {
        if (have_default || have_rules) {
            cfg_err(err, err_len,
                    "auth: default/rules are only valid with mode \"toy\"");
            return MOQR_ERR_INVAL;
        }
        return MOQR_OK;
    }
    /* mode == toy: a default is mandatory (fail-open vs fail-closed is never
     * implicit), rules are optional. */
    if (!have_default) {
        cfg_err(err, err_len, "auth.default is required for mode \"toy\"");
        return MOQR_ERR_INVAL;
    }
    size_t i = 0;
    for (struct json_array_element_s *ae =
             rules_arr != NULL ? rules_arr->start : NULL;
         ae != NULL; ae = ae->next, i++) {
        struct json_object_s *ro = json_value_as_object(ae->value);
        if (ro == NULL) {
            cfg_err(err, err_len, "auth.rules[]: need an object");
            return MOQR_ERR_INVAL;
        }
        moqr_result_t rc = parse_auth_rule(ro, auth, i, err, err_len);
        if (rc != MOQR_OK) {
            return rc;
        }
    }
    /* Wire the self-consistent toy context (borrows into auth->rules/ns). */
    auth->toy.rules = auth->rules;
    auth->toy.rule_count = i;
    auth->toy.default_decision = defd;
    auth->toy.default_reason = MOQR_AUTH_REASON_POLICY;
    return MOQR_OK;
}

moqr_result_t
moqr_cli_config_parse(const char *json, size_t len, moqr_cli_config_t *out,
                      char *err, size_t err_len)
{
    if (json == NULL || out == NULL) {
        return MOQR_ERR_INVAL;
    }
    memset(out, 0, sizeof(*out));
    moqr_core_relay_cfg_init_sized(&out->core, sizeof(out->core), NULL);
    snprintf(out->host, sizeof(out->host), "0.0.0.0");
    out->lanes = 1;   /* default single-lane; listener.lanes overrides */
    cfg_err(err, err_len, "ok");

    struct json_value_s *root = json_parse(json, len);
    if (root == NULL) {
        cfg_err(err, err_len, "not valid JSON");
        return MOQR_ERR_INVAL;
    }
    struct json_object_s *o = json_value_as_object(root);
    moqr_result_t rc = MOQR_OK;
    bool have_listener = false;
    if (o == NULL) {
        cfg_err(err, err_len, "top level must be an object");
        rc = MOQR_ERR_INVAL;
    }
    for (struct json_object_element_s *e = o != NULL ? o->start : NULL;
         e != NULL && rc == MOQR_OK; e = e->next) {
        const char *k = e->name->string;
        if (strcmp(k, "listener") == 0) {
            struct json_object_s *lo = json_value_as_object(e->value);
            if (lo == NULL) {
                cfg_err(err, err_len, "listener: need an object");
                rc = MOQR_ERR_INVAL;
            } else {
                rc = parse_listener(lo, out, err, err_len);
                have_listener = rc == MOQR_OK;
            }
        } else if (strcmp(k, "budgets") == 0) {
            struct json_object_s *bo = json_value_as_object(e->value);
            if (bo == NULL) {
                cfg_err(err, err_len, "budgets: need an object");
                rc = MOQR_ERR_INVAL;
            } else {
                rc = parse_budgets(bo, out, err, err_len);
            }
        } else if (strcmp(k, "telemetry") == 0) {
            struct json_object_s *to = json_value_as_object(e->value);
            if (to == NULL) {
                cfg_err(err, err_len, "telemetry: need an object");
                rc = MOQR_ERR_INVAL;
            } else {
                rc = parse_telemetry(to, out, err, err_len);
            }
        } else if (strcmp(k, "auth") == 0) {
            struct json_object_s *ao = json_value_as_object(e->value);
            if (ao == NULL) {
                cfg_err(err, err_len, "auth: need an object");
                rc = MOQR_ERR_INVAL;
            } else {
                rc = parse_auth(ao, out, err, err_len);
            }
        } else if (strcmp(k, "linger_us") == 0) {
            uint64_t x = 0;
            if (!num_u64(e->value, &x)) {
                cfg_err(err, err_len, "linger_us: need a number");
                rc = MOQR_ERR_INVAL;
            } else {
                out->core.linger_us = x;
            }
        } else {
            cfg_err(err, err_len, "unknown top-level key");
            rc = MOQR_ERR_INVAL;
        }
    }
    if (rc == MOQR_OK && !have_listener) {
        cfg_err(err, err_len, "listener is required");
        rc = MOQR_ERR_INVAL;
    }
    if (rc == MOQR_OK && out->alpn_count == 0) {
        /* Default: exactly the newest draft, a single offer. */
        snprintf(out->alpn_buf[0], sizeof(out->alpn_buf[0]), "moqt-18");
        out->alpns[0] = out->alpn_buf[0];
        out->alpn_count = 1;
        out->version = 18;
        out->versions[0] = MOQ_VERSION_DRAFT_18;
        out->version_count = 1;
    }
    if (rc == MOQR_OK) {
        /* One deterministic label for the whole offered set: the single ALPN
         * when there is one -- preserving exact-listener output byte for byte
         * -- else the ordered ALPNs joined by '+'. */
        size_t o = 0;
        out->alpn_set[0] = '\0';
        for (size_t i = 0; i < out->alpn_count; i++) {
            int w = snprintf(out->alpn_set + o, sizeof(out->alpn_set) - o,
                             "%s%s", i == 0 ? "" : "+", out->alpns[i]);
            if (w < 0 || (size_t)w >= sizeof(out->alpn_set) - o) {
                cfg_err(err, err_len, "listener.versions: label overflow");
                free(root);
                return MOQR_ERR_INVAL;
            }
            o += (size_t)w;
        }
    }
    free(root);
    return rc;
}

#ifdef MOQR_VERIFY_SEAM
/* Blocked-scenario constraint seam (see config.h). Values cached by the ONE
 * load call so every later consumer (describe + compose, any lane) sees the
 * same constraint; 0 = unset = library default. */
static uint32_t g_verify_bind_max_sgs;
static uint32_t g_verify_session_max_sgs;

/* Strict decimal for the seam: 1..65535, no leading zeros, digits only.
 * (The canonical-value rule the blocked bench's --seconds parser pinned:
 * "08" and "8junk" are refusals, not values.) */
static bool
verify_env_u32(const char *name, uint32_t *out, char *err, size_t err_len)
{
    *out = 0;
    const char *v = getenv(name);
    if (v == NULL) {
        return true;   /* unset = default */
    }
    if (*v < '1' || *v > '9') {
        snprintf(err, err_len, "%s: need decimal 1..65535 (no leading "
                               "zeros), got \"%s\"", name, v);
        return false;
    }
    uint32_t acc = 0;
    for (const char *c = v; *c != '\0'; c++) {
        if (*c < '0' || *c > '9' || acc > 6553u ||
            (acc == 6553u && *c > '5')) {
            snprintf(err, err_len, "%s: need decimal 1..65535 (no leading "
                                   "zeros), got \"%s\"", name, v);
            return false;
        }
        acc = acc * 10u + (uint32_t)(*c - '0');
    }
    *out = acc;
    return true;
}

moqr_result_t
moqr_cli_verify_env_load(char *err, size_t err_len)
{
    if (!verify_env_u32("MOQR_VERIFY_BIND_MAX_OPEN_SUBGROUPS",
                        &g_verify_bind_max_sgs, err, err_len) ||
        !verify_env_u32("MOQR_VERIFY_SESSION_MAX_OPEN_SUBGROUPS",
                        &g_verify_session_max_sgs, err, err_len)) {
        g_verify_bind_max_sgs = 0;
        g_verify_session_max_sgs = 0;
        return MOQR_ERR_INVAL;
    }
    return MOQR_OK;
}

uint32_t
moqr_cli_verify_bind_max_sgs(void)
{
    return g_verify_bind_max_sgs;
}

uint32_t
moqr_cli_verify_session_max_sgs(void)
{
    return g_verify_session_max_sgs;
}
#endif /* MOQR_VERIFY_SEAM */

void
moqr_cli_build_shards_cfg(const moqr_cli_config_t *cfg,
                          const moq_alloc_t *alloc, moqr_shards_cfg_t *out)
{
    moqr_shards_cfg_init_sized(out, sizeof(*out), alloc);
    out->shards = (uint16_t)cfg->lanes;
    out->trace_ring_records = cfg->telemetry.trace_ring_records;
    out->live_visibility = true;
    moqr_cli_core_cfg_from(cfg, alloc, &out->core_cfg);
    /* budgets.cross_shard, zero = library default (the shared shard
     * resolver stays authoritative for defaults and cross-field rules). */
    out->journal_entries = cfg->cross_shard.journal_entries;
    out->mailbox_entries = cfg->cross_shard.mailbox_entries;
    out->demand_channel_entries = cfg->cross_shard.demand_channel_entries;
    out->demand_channel_bytes = cfg->cross_shard.demand_channel_bytes;
    out->pending_demand_entries = cfg->cross_shard.pending_demands;
    out->pump_subgroup_slots = cfg->cross_shard.subgroup_slots;
    /* Strict per-lane clamp: at lanes > 1 every shard's manager consumes
     * `lanes` core binding slots, so each lane's binding admits only the
     * external remainder — the shard boundary then fails closed exactly
     * where the model says it will. */
    if (cfg->lanes > 1) {
        moqr_core_limits_t clim;
        if (moqr_core_limits_resolve(&out->core_cfg, &clim) == MOQR_OK &&
            clim.max_bindings > cfg->lanes) {
            out->bind_cfg.max_conns = clim.max_bindings - cfg->lanes;
        }
    }
    /* Production admission is automatically ON exactly when the relay runs
     * multiple lanes: a single-lane relay is the direct M1 path with no
     * cross-shard plane to admit into (and no manager at K == 1), while a
     * multi-lane relay must forward remote-owned demand across the shard
     * boundary for a subscriber to reach a publisher on another lane. This
     * is the ONE place the rule lives — capacity and serve both consume
     * this output. There is no user-facing admission key or toggle. */
    out->admit_remote_demand = cfg->lanes > 1;
#ifdef MOQR_VERIFY_SEAM
    /* Blocked-scenario seam: constrain every shard's bind subgroup slot
     * pool. Applied in THIS builder so describe and serve stay one config —
     * the printed ceiling is the constrained pool, never the default. */
    if (moqr_cli_verify_bind_max_sgs() != 0) {
        out->bind_cfg.max_open_subgroups = moqr_cli_verify_bind_max_sgs();
    }
#endif
}

moqr_result_t
moqr_cli_serve_compose(const moqr_cli_config_t *cfg, const moq_alloc_t *alloc,
                       moqr_shards_cfg_t *out_scfg,
                       uint32_t *out_max_connections)
{
    if (cfg == NULL || out_scfg == NULL || out_max_connections == NULL) {
        return MOQR_ERR_INVAL;
    }
    moqr_shards_cfg_t scfg;
    moqr_cli_build_shards_cfg(cfg, alloc, &scfg);
    moqr_shards_limits_t slim;
    if (moqr_shards_cfg_resolve(&scfg, &slim) != MOQR_OK) {
        return MOQR_ERR_INVAL;
    }
    /* Facade admission cap from the SAME rule the capacity model reports:
     * usable external bindings per shard, times lanes (checked). */
    uint64_t cap64 = (uint64_t)cfg->lanes * slim.usable_bindings;
    *out_max_connections =
        cap64 > UINT32_MAX ? UINT32_MAX : (uint32_t)cap64;
    *out_scfg = scfg;
    return MOQR_OK;
}

moqr_result_t
moqr_cli_config_validate(const moqr_cli_config_t *cfg,
                         const moq_alloc_t *alloc)
{
    if (cfg == NULL) {
        return MOQR_ERR_INVAL;
    }
    moqr_shards_cfg_t scfg;
    moqr_cli_build_shards_cfg(cfg, alloc, &scfg);
    moqr_shards_limits_t lim;
    return moqr_shards_cfg_resolve(&scfg, &lim) == MOQR_OK ? MOQR_OK
                                                           : MOQR_ERR_INVAL;
}

moqr_result_t
moqr_cli_describe_capacity(const moqr_cli_config_t *cfg,
                           const moq_alloc_t *alloc, size_t serve_ctx_bytes,
                           moqr_cli_capacity_t *out)
{
    if (out == NULL) {
        return MOQR_ERR_INVAL;
    }
    memset(out, 0, sizeof(*out));
    if (cfg == NULL) {
        return MOQR_ERR_INVAL;
    }
    if (cfg->lanes <= 1) {
        /* Cross-field validation FIRST, through the same shared resolver
         * lanes>1 uses: "lanes=1 accepts the object" means valid inert
         * settings — an explicit demand_channel_bytes below one resolved
         * log record is invalid at EVERY lane count, never bypassed. */
        if (moqr_cli_config_validate(cfg, alloc) != MOQR_OK) {
            return MOQR_ERR_INVAL;
        }
        /* The direct lanes=1 composition: core + binding + trace — exactly
         * what cmd_serve allocates (no shard container, no CLI rows). */
        moqr_core_relay_cfg_t core = cfg->core;
        core.alloc = alloc;
        moqr_core_capacity_t cap;
        if (moqr_core_capacity_describe(&core, &cap) != MOQR_OK) {
            return MOQR_ERR_INVAL;
        }
        moqr_core_limits_t clim;
        if (moqr_core_limits_resolve(&core, &clim) != MOQR_OK) {
            return MOQR_ERR_INVAL;
        }
        moqr_bind_cfg_t bind_tmpl;
        moqr_bind_cfg_init_sized(&bind_tmpl, sizeof(bind_tmpl), alloc);
        moqr_bind_capacity_t bc;
        if (moqr_bind_capacity_describe(&bind_tmpl, &clim, &bc) != MOQR_OK) {
            return MOQR_ERR_INVAL;
        }
        moqr_bind_limits_t blim;
        (void)moqr_bind_cfg_resolve(&bind_tmpl, &clim, &blim);
        out->core_structure_bytes = cap.structure_bytes;
        out->core_payload_bytes = cap.payload_bytes;
        out->bind_structure_bytes = bc.total_bytes;
        out->trace_bytes =
            moqr_trace_bytes(cfg->telemetry.trace_ring_records);
        out->usable_bindings_per_shard = blim.max_conns < clim.max_bindings
                                             ? blim.max_conns
                                             : clim.max_bindings;
        out->total_bytes = moqr_cap_add(
            moqr_cap_add(out->core_structure_bytes,
                         out->core_payload_bytes),
            moqr_cap_add(out->bind_structure_bytes, out->trace_bytes));
        if (out->total_bytes == UINT64_MAX) {
            memset(out, 0, sizeof(*out));
            return MOQR_ERR_INVAL;
        }
        return MOQR_OK;
    }
    /* lanes > 1: the SAME builder serve consumes, described whole, plus the
     * CLI's own multi-lane context and snapshot rows. */
    moqr_shards_cfg_t scfg;
    moqr_cli_build_shards_cfg(cfg, alloc, &scfg);
    moqr_shards_capacity_t sc;
    moqr_result_t rc = moqr_shards_capacity_describe(&scfg, &sc);
    if (rc != MOQR_OK) {
        return rc;
    }
    out->core_structure_bytes = sc.core_structure_bytes;
    out->core_payload_bytes = sc.core_payload_bytes;
    out->bind_structure_bytes = sc.bind_structure_bytes;
    out->trace_bytes = sc.trace_bytes;
    out->cross_shard_bytes = moqr_cap_add(
        moqr_cap_add(sc.shards_structure_bytes, sc.channel_byte_ceiling),
        moqr_cap_add(sc.canon_byte_ceiling, sc.staging_byte_ceiling));
    out->cli_runtime_bytes = moqr_cap_add(
        serve_ctx_bytes, moqr_cli_snapshot_bytes(cfg->lanes));
    out->usable_bindings_per_shard = sc.usable_bindings_per_shard;
    out->total_bytes =
        moqr_cap_add(sc.relay_alloc_ceiling, out->cli_runtime_bytes);
    if (out->total_bytes == UINT64_MAX) {
        memset(out, 0, sizeof(*out));
        return MOQR_ERR_INVAL;   /* wrapped: refuse, never under-report */
    }
    return MOQR_OK;
}

void
moqr_cli_core_cfg_from(const moqr_cli_config_t *cfg, const moq_alloc_t *alloc,
                       moqr_core_relay_cfg_t *out)
{
    *out = cfg->core;
    out->alloc = alloc;
    /* Install the toy authorizer when configured; allow-all leaves the hook
     * NULL. The ctx borrows cfg->auth (which must outlive the core, and is
     * read-only — safe to share across shard threads); the uintptr_t hop
     * passes the const policy through the void* ctx cleanly. */
    if (cfg->auth.mode == MOQR_CLI_AUTH_TOY) {
        out->authorize = moqr_auth_toy_authorize;
        out->authorize_ctx = (void *)(uintptr_t)&cfg->auth.toy;
    }
}

moqr_result_t
moqr_cli_build_core(const moqr_cli_config_t *cfg, const moq_alloc_t *alloc,
                    moqr_trace_t **trace_out, moqr_core_t **core_out)
{
    if (cfg == NULL || alloc == NULL || trace_out == NULL ||
        core_out == NULL) {
        return MOQR_ERR_INVAL;
    }
    *trace_out = NULL;
    *core_out = NULL;

    moqr_trace_t *trace = NULL;
    moqr_result_t rc =
        moqr_trace_create(alloc, cfg->telemetry.trace_ring_records, &trace);
    if (rc != MOQR_OK) {
        return rc;
    }

    moqr_core_relay_cfg_t core_cfg;
    moqr_cli_core_cfg_from(cfg, alloc, &core_cfg);
    core_cfg.trace = trace;
    moqr_core_t *core = NULL;
    rc = moqr_core_create(&core_cfg, &core);
    if (rc != MOQR_OK) {
        moqr_trace_destroy(trace);
        return rc;
    }

    *trace_out = trace;
    *core_out = core;
    return MOQR_OK;
}

moqr_result_t
moqr_cli_config_load(const char *path, moqr_cli_config_t *out, char *err,
                     size_t err_len)
{
    if (path == NULL) {
        return MOQR_ERR_INVAL;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        cfg_err(err, err_len, "cannot open config file");
        return MOQR_ERR_INVAL;
    }
    char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    bool truncated = fgetc(f) != EOF;
    fclose(f);
    if (truncated) {
        cfg_err(err, err_len, "config file too large (64 KiB max)");
        return MOQR_ERR_INVAL;
    }
    buf[n] = '\0';
    return moqr_cli_config_parse(buf, n, out, err, err_len);
}

/* The single production decision point for config -> transport versions. Both
 * serve compositions route through it, so they cannot drift apart. Transport
 * neutral on purpose: this translation unit is also linked into ungated test
 * binaries that must not depend on an adapter header. */
void
moqr_cli_version_plan(const moqr_cli_config_t *cfg,
                      moqr_cli_version_plan_t *out)
{
    out->exact = 0;
    out->list = NULL;
    out->count = 0;
    if (cfg == NULL || cfg->version_count == 0) {
        return;
    }
    if (cfg->version_count == 1) {
        /* The exact plan: a non-zero version and NO list, which is how the
         * managed adapter spells "exact-version listener". count stays 0. */
        out->exact = cfg->versions[0];
        return;
    }
    out->list = cfg->versions;
    out->count = cfg->version_count;
}
