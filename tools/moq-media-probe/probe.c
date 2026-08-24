/*
 * moq_media_probe implementation. See probe.h.
 *
 * Design notes:
 *  - Requests and the inline catalog input are parsed ONLY with LibMoQ's
 *    vendored JSON parser (json.h) and its MSF/CMSF catalog parser (moq/msf.h).
 *    This tool adds no parser of its own.
 *  - Every integral value in a result is emitted as a decimal STRING so it is
 *    lossless regardless of the consumer's number type. Booleans and strings are
 *    emitted verbatim (strings JSON-escaped). Object keys are emitted in
 *    ascending byte order (canonical); array element order is preserved because
 *    it is semantically meaningful (tracks, delta ops, defaultKID, etc.).
 *  - The result projects LibMoQ's typed model, so fields the parser does not
 *    recognize cannot leak into output, and present-vs-absent is preserved via
 *    the model's has_* flags -- EXCEPT for count-only arrays (defaultKID,
 *    depends, contentProtectionRefIDs, contentProtections, initDataList), where
 *    the model cannot distinguish an empty [] from an absent field, so both are
 *    omitted. This one lossy case is reported under capabilities.limitations.
 *  - Output is deterministic: no pointer/address/hash/locale-dependent text.
 */

#define JSON_H_IMPLEMENTATION
#include "../../media/msf/vendor/json.h"

#include "probe.h"

#include <moq/msf.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Strict UTF-8 validation (RFC 3629): rejects overlong forms, surrogates, and
 * out-of-range bytes, so a response can never echo non-UTF-8 bytes inside a JSON
 * string. Validating the whole request up front also covers every string field
 * the response may echo (e.g. id). */
static bool is_valid_utf8(const char *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)p[i];
        size_t extra;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c < 0x80) { i++; continue; }
        else if (c >= 0xC2 && c <= 0xDF) extra = 1;
        else if (c == 0xE0) { extra = 2; lo = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) extra = 2;
        else if (c == 0xED) { extra = 2; hi = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) extra = 2;
        else if (c == 0xF0) { extra = 3; lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) extra = 3;
        else if (c == 0xF4) { extra = 3; hi = 0x8F; }
        else return false; /* 0x80-0xC1, 0xF5-0xFF: never a valid lead byte */
        if (i + extra >= n) return false;
        unsigned char c1 = (unsigned char)p[i + 1];
        if (c1 < lo || c1 > hi) return false;
        for (size_t k = 2; k <= extra; k++) {
            unsigned char cc = (unsigned char)p[i + k];
            if (cc < 0x80 || cc > 0xBF) return false;
        }
        i += extra + 1;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Growable output buffer with a sticky out-of-memory flag.           */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    bool   err; /* sticky: once set, every append is a no-op */
} sb_t;

static void sb_reserve(sb_t *sb, size_t extra) {
    if (sb->err) return;
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t ncap = sb->cap ? sb->cap * 2 : 256;
    while (ncap < sb->len + extra + 1) {
        if (ncap > (SIZE_MAX / 2)) { sb->err = true; return; }
        ncap *= 2;
    }
    char *n = (char *)realloc(sb->data, ncap);
    if (!n) { sb->err = true; return; }
    sb->data = n;
    sb->cap = ncap;
}

static void sb_putn(sb_t *sb, const char *s, size_t n) {
    sb_reserve(sb, n);
    if (sb->err) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_puts(sb_t *sb, const char *s) { sb_putn(sb, s, strlen(s)); }
static void sb_putc(sb_t *sb, char c) { sb_putn(sb, &c, 1); }

/* Append a JSON string literal, escaping per RFC 8259. */
static void sb_json_string(sb_t *sb, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    sb_putc(sb, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  sb_puts(sb, "\\\""); break;
            case '\\': sb_puts(sb, "\\\\"); break;
            case '\b': sb_puts(sb, "\\b");  break;
            case '\f': sb_puts(sb, "\\f");  break;
            case '\n': sb_puts(sb, "\\n");  break;
            case '\r': sb_puts(sb, "\\r");  break;
            case '\t': sb_puts(sb, "\\t");  break;
            default:
                if (c < 0x20) {
                    char u[6] = { '\\', 'u', '0', '0', hex[(c >> 4) & 0xf], hex[c & 0xf] };
                    sb_putn(sb, u, 6);
                } else {
                    sb_putc(sb, (char)c); /* pass UTF-8 bytes through verbatim */
                }
        }
    }
    sb_putc(sb, '"');
}

static void sb_bytes_string(sb_t *sb, moq_bytes_t b) {
    sb_json_string(sb, b.data ? (const char *)b.data : "", b.data ? b.len : 0);
}

/* Emit an integer as a JSON string of its decimal form (lossless). */
static void sb_u64_string(sb_t *sb, uint64_t v) {
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
    sb_putc(sb, '"');
    if (n > 0) sb_putn(sb, tmp, (size_t)n);
    sb_putc(sb, '"');
}
static void sb_i64_string(sb_t *sb, int64_t v) {
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%" PRId64, v);
    sb_putc(sb, '"');
    if (n > 0) sb_putn(sb, tmp, (size_t)n);
    sb_putc(sb, '"');
}

/* Object member key helper: emits `,"key":` (or `"key":` for the first). */
static void key(sb_t *sb, bool *first, const char *k) {
    if (!*first) sb_putc(sb, ',');
    *first = false;
    sb_json_string(sb, k, strlen(k));
    sb_putc(sb, ':');
}

/* ------------------------------------------------------------------ */
/* Model projection (canonical: keys ascending; arrays keep order).   */
/* ------------------------------------------------------------------ */

static void put_string_array(sb_t *sb, const moq_bytes_t *items, size_t n) {
    sb_putc(sb, '[');
    for (size_t i = 0; i < n; i++) {
        if (i) sb_putc(sb, ',');
        sb_bytes_string(sb, items[i]);
    }
    sb_putc(sb, ']');
}

static void put_template(sb_t *sb, const moq_msf_media_template_t *t) {
    bool f = true;
    sb_putc(sb, '{');
    key(sb, &f, "deltaGroup");       sb_u64_string(sb, t->delta_group);
    key(sb, &f, "deltaMediaMs");     sb_u64_string(sb, t->delta_media_ms);
    key(sb, &f, "deltaObject");      sb_u64_string(sb, t->delta_object);
    key(sb, &f, "deltaWallclockMs"); sb_u64_string(sb, t->delta_wallclock_ms);
    key(sb, &f, "startGroup");       sb_u64_string(sb, t->start_group);
    key(sb, &f, "startMediaMs");     sb_u64_string(sb, t->start_media_ms);
    key(sb, &f, "startObject");      sb_u64_string(sb, t->start_object);
    key(sb, &f, "startWallclockMs"); sb_u64_string(sb, t->start_wallclock_ms);
    sb_putc(sb, '}');
}

/* Project one track. Keys are emitted in ascending byte order; only present
 * fields (per the model's has_* flags) appear, preserving omitted-vs-present. */
static void put_track(sb_t *sb, const moq_msf_track_t *tr) {
    bool f = true;
    sb_putc(sb, '{');
    if (tr->has_alt_group)     { key(sb, &f, "altGroup");     sb_i64_string(sb, tr->alt_group); }
    if (tr->has_bitrate)       { key(sb, &f, "bitrate");      sb_u64_string(sb, tr->bitrate); }
    if (tr->has_channel_config){ key(sb, &f, "channelConfig");sb_bytes_string(sb, tr->channel_config); }
    if (tr->has_codec)         { key(sb, &f, "codec");        sb_bytes_string(sb, tr->codec); }
    if (tr->cp_ref_id_count)   { key(sb, &f, "contentProtectionRefIDs"); put_string_array(sb, tr->cp_ref_ids, tr->cp_ref_id_count); }
    if (tr->depends_count)     { key(sb, &f, "depends");      put_string_array(sb, tr->depends, tr->depends_count); }
    if (tr->has_event_type)    { key(sb, &f, "eventType");    sb_bytes_string(sb, tr->event_type); }
    if (tr->has_framerate)     { key(sb, &f, "framerateMillis"); sb_u64_string(sb, tr->framerate_millis); }
    if (tr->has_height)        { key(sb, &f, "height");       sb_u64_string(sb, tr->height); }
    if (tr->has_init_data)     { key(sb, &f, "initData");     sb_bytes_string(sb, tr->init_data); }
    if (tr->has_init_ref)      { key(sb, &f, "initRef");      sb_bytes_string(sb, tr->init_ref); }
    if (tr->has_init_track)    { key(sb, &f, "initTrack");    sb_bytes_string(sb, tr->init_track); }
    if (tr->has_is_live)       { key(sb, &f, "isLive");       sb_puts(sb, tr->is_live ? "true" : "false"); }
    if (tr->has_label)         { key(sb, &f, "label");        sb_bytes_string(sb, tr->label); }
    if (tr->has_lang)          { key(sb, &f, "lang");         sb_bytes_string(sb, tr->lang); }
    if (tr->has_max_grp_sap)   { key(sb, &f, "maxGrpSapStartingType"); sb_u64_string(sb, tr->max_grp_sap); }
    if (tr->has_max_obj_sap)   { key(sb, &f, "maxObjSapStartingType"); sb_u64_string(sb, tr->max_obj_sap); }
    if (tr->has_mime_type)     { key(sb, &f, "mimeType");     sb_bytes_string(sb, tr->mime_type); }
    /* name is required and always present. */
    key(sb, &f, "name"); sb_bytes_string(sb, tr->name);
    if (tr->has_namespace)     { key(sb, &f, "namespace");    sb_bytes_string(sb, tr->namespace_); }
    if (tr->has_packaging)     { key(sb, &f, "packaging");    sb_bytes_string(sb, tr->packaging); }
    if (tr->has_parent_name)   { key(sb, &f, "parentName");   sb_bytes_string(sb, tr->parent_name); }
    if (tr->has_parent_namespace){ key(sb, &f, "parentNamespace"); sb_bytes_string(sb, tr->parent_namespace); }
    if (tr->has_render_group)  { key(sb, &f, "renderGroup");  sb_i64_string(sb, tr->render_group); }
    if (tr->has_role)          { key(sb, &f, "role");         sb_bytes_string(sb, tr->role); }
    if (tr->has_samplerate)    { key(sb, &f, "samplerate");   sb_u64_string(sb, tr->samplerate); }
    if (tr->has_target_latency){ key(sb, &f, "targetLatency");sb_u64_string(sb, tr->target_latency); }
    if (tr->has_template)      { key(sb, &f, "template");     put_template(sb, &tr->template_); }
    if (tr->has_timescale)     { key(sb, &f, "timescale");    sb_u64_string(sb, tr->timescale); }
    if (tr->has_track_duration){ key(sb, &f, "trackDuration");sb_u64_string(sb, tr->track_duration_ms); }
    if (tr->has_width)         { key(sb, &f, "width");        sb_u64_string(sb, tr->width); }
    sb_putc(sb, '}');
}

static void put_url(sb_t *sb, const moq_cmsf_url_t *u) {
    bool f = true;
    sb_putc(sb, '{');
    if (u->has_type) { key(sb, &f, "type"); sb_bytes_string(sb, u->type); }
    key(sb, &f, "url"); sb_bytes_string(sb, u->url);
    sb_putc(sb, '}');
}

static void put_content_protection(sb_t *sb, const moq_cmsf_content_protection_t *cp) {
    bool f = true;
    sb_putc(sb, '{');
    if (cp->default_kid_count) { key(sb, &f, "defaultKID"); put_string_array(sb, cp->default_kids, cp->default_kid_count); }
    /* drmSystem (required). */
    {
        const moq_cmsf_drm_system_t *d = &cp->drm_system;
        key(sb, &f, "drmSystem");
        bool df = true;
        sb_putc(sb, '{');
        /* Spec field names are preserved verbatim (CMSF §4.1.1.4.2-.4). */
        if (d->auth_url.present) { key(sb, &df, "authURL"); put_url(sb, &d->auth_url); }
        if (d->cert_url.present) { key(sb, &df, "certURL"); put_url(sb, &d->cert_url); }
        if (d->la_url.present)   { key(sb, &df, "laURL");   put_url(sb, &d->la_url); }
        if (d->has_pssh)         { key(sb, &df, "pssh");    sb_bytes_string(sb, d->pssh); }
        if (d->has_robustness)   { key(sb, &df, "robustness"); sb_bytes_string(sb, d->robustness); }
        if (d->system_id.data)   { key(sb, &df, "systemID"); sb_bytes_string(sb, d->system_id); }
        sb_putc(sb, '}');
    }
    if (cp->ref_id.data) { key(sb, &f, "refID"); sb_bytes_string(sb, cp->ref_id); }
    if (cp->scheme.data) { key(sb, &f, "scheme"); sb_bytes_string(sb, cp->scheme); }
    sb_putc(sb, '}');
}

/* Project an independent catalog (catalog.parse). */
static void put_catalog(sb_t *sb, const moq_msf_catalog_t *cat) {
    bool f = true;
    sb_putc(sb, '{');
    if (cat->content_protection_count) {
        key(sb, &f, "contentProtections");
        sb_putc(sb, '[');
        for (size_t i = 0; i < cat->content_protection_count; i++) {
            if (i) sb_putc(sb, ',');
            put_content_protection(sb, &cat->content_protections[i]);
        }
        sb_putc(sb, ']');
    }
    if (cat->has_generated_at) { key(sb, &f, "generatedAt"); sb_u64_string(sb, cat->generated_at); }
    if (cat->init_data_count) {
        key(sb, &f, "initDataList");
        sb_putc(sb, '[');
        for (size_t i = 0; i < cat->init_data_count; i++) {
            const moq_msf_init_data_entry_t *e = &cat->init_data_list[i];
            if (i) sb_putc(sb, ',');
            bool ef = true;
            sb_putc(sb, '{');
            if (e->data.data) { key(sb, &ef, "data"); sb_bytes_string(sb, e->data); }
            if (e->id.data)   { key(sb, &ef, "id");   sb_bytes_string(sb, e->id); }
            if (e->type.data) { key(sb, &ef, "type"); sb_bytes_string(sb, e->type); }
            sb_putc(sb, '}');
        }
        sb_putc(sb, ']');
    }
    if (cat->is_complete) { key(sb, &f, "isComplete"); sb_puts(sb, "true"); }
    key(sb, &f, "tracks");
    sb_putc(sb, '[');
    for (size_t i = 0; i < cat->track_count; i++) {
        if (i) sb_putc(sb, ',');
        put_track(sb, &cat->tracks[i]);
    }
    sb_putc(sb, ']');
    key(sb, &f, "version"); sb_i64_string(sb, cat->version);
    sb_putc(sb, '}');
}

static const char *delta_op_name(moq_msf_delta_op_kind_t k) {
    switch (k) {
        case MOQ_MSF_DELTA_OP_ADD:    return "add";
        case MOQ_MSF_DELTA_OP_REMOVE: return "remove";
        case MOQ_MSF_DELTA_OP_CLONE:  return "clone";
    }
    return "unknown";
}

/* Project a delta catalog (catalog.delta.parse): ordered operations. */
static void put_delta(sb_t *sb, const moq_msf_catalog_t *cat) {
    bool f = true;
    sb_putc(sb, '{');
    key(sb, &f, "deltaUpdate");
    sb_putc(sb, '[');
    for (size_t i = 0; i < cat->delta_update_count; i++) {
        const moq_msf_delta_op_t *op = &cat->delta_update[i];
        if (i) sb_putc(sb, ',');
        bool of = true;
        sb_putc(sb, '{');
        key(sb, &of, "op"); sb_json_string(sb, delta_op_name(op->op), strlen(delta_op_name(op->op)));
        key(sb, &of, "tracks");
        sb_putc(sb, '[');
        for (size_t t = 0; t < op->track_count; t++) {
            if (t) sb_putc(sb, ',');
            put_track(sb, &op->tracks[t]);
        }
        sb_putc(sb, ']');
        sb_putc(sb, '}');
    }
    sb_putc(sb, ']');
    if (cat->has_generated_at) { key(sb, &f, "generatedAt"); sb_u64_string(sb, cat->generated_at); }
    sb_putc(sb, '}');
}

/* ------------------------------------------------------------------ */
/* Capabilities (honest, enumerated per operation/profile).           */
/* ------------------------------------------------------------------ */

static void cap_profile(sb_t *sb, bool *first, const char *profile, bool supported,
                        bool has_protected, bool protected_playback, const char *reason) {
    if (!*first) sb_putc(sb, ',');
    *first = false;
    bool f = true;
    sb_putc(sb, '{');
    key(sb, &f, "profile"); sb_json_string(sb, profile, strlen(profile));
    if (has_protected) { key(sb, &f, "protectedPlayback"); sb_puts(sb, protected_playback ? "true" : "false"); }
    if (reason) { key(sb, &f, "reason"); sb_json_string(sb, reason, strlen(reason)); }
    key(sb, &f, "supported"); sb_puts(sb, supported ? "true" : "false");
    sb_putc(sb, '}');
}

static void put_capabilities(sb_t *sb) {
    bool f = true;
    sb_putc(sb, '{');
    /* Honestly report the one lossy aspect of the projection: LibMoQ's model
     * records these arrays as count-only (a pointer + length), so an explicitly
     * empty [] is indistinguishable from an absent field and both are omitted. */
    key(sb, &f, "limitations");
    sb_putc(sb, '[');
    {
        bool lf = true;
        sb_putc(sb, '{');
        key(sb, &lf, "detail");
        sb_puts(sb, "\"LibMoQ's model stores these as count-only, so an explicitly empty array is "
                    "indistinguishable from an absent field; both are omitted from the projection\"");
        key(sb, &lf, "fields");
        sb_puts(sb, "[\"contentProtectionRefIDs\",\"contentProtections\",\"defaultKID\",\"depends\",\"initDataList\"]");
        key(sb, &lf, "limitation"); sb_puts(sb, "\"empty-array-indistinguishable-from-absent\"");
        sb_putc(sb, '}');
    }
    sb_putc(sb, ']');
    key(sb, &f, "operations");
    sb_putc(sb, '[');
    /* capabilities */
    {
        bool of = true;
        sb_putc(sb, '{');
        key(sb, &of, "operation"); sb_puts(sb, "\"capabilities\"");
        key(sb, &of, "supported"); sb_puts(sb, "true");
        sb_putc(sb, '}');
    }
    sb_putc(sb, ',');
    /* catalog.delta.parse */
    {
        bool of = true;
        sb_putc(sb, '{');
        key(sb, &of, "operation"); sb_puts(sb, "\"catalog.delta.parse\"");
        key(sb, &of, "profiles");
        sb_putc(sb, '[');
        bool pf = true;
        cap_profile(sb, &pf, "msf-00", false, false, false, "delta updates are MSF-01 only (5.1.6)");
        cap_profile(sb, &pf, "msf-01", true, false, false, NULL);
        cap_profile(sb, &pf, "cmsf-01", false, false, false, "delta updates are MSF-01 only (5.1.6)");
        sb_putc(sb, ']');
        key(sb, &of, "supported"); sb_puts(sb, "true");
        sb_putc(sb, '}');
    }
    sb_putc(sb, ',');
    /* catalog.parse */
    {
        bool of = true;
        sb_putc(sb, '{');
        key(sb, &of, "operation"); sb_puts(sb, "\"catalog.parse\"");
        key(sb, &of, "profiles");
        sb_putc(sb, '[');
        bool pf = true;
        cap_profile(sb, &pf, "cmsf-01", true, true, false,
                    "clear-content catalog fields (initDataList/initRef, contentProtections) are parsed; protected playback is unsupported");
        cap_profile(sb, &pf, "msf-00", true, false, false, "legacy numeric version 1");
        cap_profile(sb, &pf, "msf-01", true, false, false, NULL);
        cap_profile(sb, &pf, "msf-01-draft", true, false, false,
                    "only the \"draft-01\" version string is recognized");
        sb_putc(sb, ']');
        key(sb, &of, "supported"); sb_puts(sb, "true");
        sb_putc(sb, '}');
    }
    sb_putc(sb, ']');
    key(sb, &f, "protocol"); sb_puts(sb, "\"" MOQ_MEDIA_PROBE_PROTOCOL "\"");
    sb_putc(sb, '}');
}

/* ------------------------------------------------------------------ */
/* Request envelope handling.                                         */
/* ------------------------------------------------------------------ */

typedef enum { OP_UNKNOWN, OP_CAPABILITIES, OP_CATALOG_PARSE, OP_CATALOG_DELTA_PARSE } op_t;

static bool jstr_eq(const json_string_t *s, const char *lit) {
    size_t n = strlen(lit);
    return s && s->string_size == n && memcmp(s->string, lit, n) == 0;
}

/* Known catalog.parse profiles. Returns true if the token is recognized. */
static bool profile_known_parse(const char *p, size_t n) {
    static const char *ok[] = { "msf-00", "msf-01", "msf-01-draft", "cmsf-01" };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (strlen(ok[i]) == n && memcmp(ok[i], p, n) == 0) return true;
    return false;
}
/* catalog.parse supports every known profile. */
static bool profile_supported_parse(const char *p, size_t n) { return profile_known_parse(p, n); }
/* catalog.delta.parse recognizes the same catalog profiles but supports only msf-01. */
static bool profile_supported_delta(const char *p, size_t n) {
    return n == 6 && memcmp(p, "msf-01", 6) == 0;
}

typedef struct {
    const char *stage;
    const char *category;
    const char *message;
} probe_err_t;

/* Assemble the final response envelope and return it as a malloc'd line. */
static char *finish(sb_t *result_or_null, const char *id, size_t id_len, bool id_present,
                    const probe_err_t *err, const char *const *diags, size_t ndiags) {
    sb_t sb = { 0 };
    bool f = true;
    sb_putc(&sb, '{');
    if (err) {
        key(&sb, &f, "error");
        bool ef = true;
        sb_putc(&sb, '{');
        key(&sb, &ef, "category"); sb_json_string(&sb, err->category, strlen(err->category));
        key(&sb, &ef, "message");  sb_json_string(&sb, err->message, strlen(err->message));
        key(&sb, &ef, "stage");    sb_json_string(&sb, err->stage, strlen(err->stage));
        sb_putc(&sb, '}');
        key(&sb, &f, "id");
        if (id_present) sb_json_string(&sb, id, id_len); else sb_puts(&sb, "null");
        key(&sb, &f, "protocol"); sb_puts(&sb, "\"" MOQ_MEDIA_PROBE_PROTOCOL "\"");
        key(&sb, &f, "status");   sb_puts(&sb, "\"error\"");
    } else {
        key(&sb, &f, "diagnostics");
        sb_putc(&sb, '[');
        for (size_t i = 0; i < ndiags; i++) {
            if (i) sb_putc(&sb, ',');
            sb_json_string(&sb, diags[i], strlen(diags[i]));
        }
        sb_putc(&sb, ']');
        key(&sb, &f, "id");
        if (id_present) sb_json_string(&sb, id, id_len); else sb_puts(&sb, "null");
        key(&sb, &f, "protocol"); sb_puts(&sb, "\"" MOQ_MEDIA_PROBE_PROTOCOL "\"");
        key(&sb, &f, "result");
        if (result_or_null && !result_or_null->err && result_or_null->data)
            sb_putn(&sb, result_or_null->data, result_or_null->len);
        else
            sb.err = true;
        key(&sb, &f, "status"); sb_puts(&sb, "\"ok\"");
    }
    sb_putc(&sb, '}');
    if (sb.err) { free(sb.data); return NULL; }
    return sb.data;
}

/* The version form a catalog.parse profile requires, read from the ORIGINAL
 * JSON (LibMoQ normalizes the version away, so the declared profile is enforced
 * against the source document, not the parsed model). The profile is
 * caller-selected; only the version form -- the one reliably declared marker --
 * is enforced. CMSF extension structures (contentProtections/initDataList) are
 * OPTIONAL in a valid CMSF catalog, so they are NOT required by the profile. */
typedef struct {
    int  version_kind;     /* 0 absent, 1 number, 2 string */
    bool version_is_1;     /* string version == "1" */
    bool version_is_draft; /* string version begins "draft-" */
} raw_shape_t;

static void inspect_raw(struct json_value_s *root, raw_shape_t *s) {
    memset(s, 0, sizeof(*s));
    struct json_object_s *o = json_value_as_object(root);
    if (!o) return;
    for (struct json_object_element_s *e = o->start; e; e = e->next) {
        if (jstr_eq(e->name, "version")) {
            if (json_value_as_number(e->value)) s->version_kind = 1;
            else {
                json_string_t *str = json_value_as_string(e->value);
                if (str) {
                    s->version_kind = 2;
                    s->version_is_1 = (str->string_size == 1 && str->string[0] == '1');
                    s->version_is_draft = (str->string_size >= 6 && memcmp(str->string, "draft-", 6) == 0);
                }
            }
            break; /* version is the only field the profile check needs */
        }
    }
}

/* Does the declared catalog.parse profile match the document's version form?
 * `msf-01` and `cmsf-01` share the MSF-01 string version "1"; the caller's
 * choice between them selects how the (identical) parse is labeled/interpreted. */
static bool profile_matches(const char *profile, size_t plen, const raw_shape_t *s) {
    if (plen == 6 && memcmp(profile, "msf-00", 6) == 0)
        return s->version_kind == 1;                        /* legacy numeric version */
    if (plen == 6 && memcmp(profile, "msf-01", 6) == 0)
        return s->version_kind == 2 && s->version_is_1;     /* "version":"1" */
    if (plen == 12 && memcmp(profile, "msf-01-draft", 12) == 0)
        return s->version_kind == 2 && s->version_is_draft; /* "version":"draft-XX" */
    if (plen == 7 && memcmp(profile, "cmsf-01", 7) == 0)
        return s->version_kind == 2 && s->version_is_1;     /* MSF-01 form; CMSF markers optional */
    return false;
}

/* Run the catalog parser and project. `want_delta` selects the operation's
 * shape check; for an independent catalog the declared `profile` is enforced
 * against the document form. On success builds *out_result (caller frees .data). */
static bool do_catalog(const moq_alloc_t *alloc, const char *json, size_t json_len,
                       const char *profile, size_t profile_len, bool want_delta,
                       sb_t *out_result, probe_err_t *out_err, const char **out_diag) {
    *out_diag = NULL;

    /* Parse once up front: validity (syntax) and, for catalog.parse, the version
     * form used to enforce the declared profile. */
    struct json_value_s *iv = json_parse(json, json_len);
    if (!iv) {
        out_err->stage = "syntax"; out_err->category = "malformed-json";
        out_err->message = "input is not well-formed JSON";
        return false;
    }
    raw_shape_t shape;
    inspect_raw(iv, &shape);
    free(iv);

    moq_bytes_t in = { (const uint8_t *)json, json_len };
    moq_msf_catalog_t cat;
    moq_result_t rc = moq_msf_catalog_parse(alloc, in, &cat);
    if (rc == MOQ_ERR_NOMEM || rc == MOQ_ERR_INVAL) {
        out_err->stage = "internal"; out_err->category = "internal";
        out_err->message = "the catalog parser could not run (allocation/argument failure)";
        return false;
    }
    if (rc == MOQ_ERR_PROTO) {
        /* JSON was already confirmed well-formed above, so this is semantic. */
        out_err->stage = "semantic"; out_err->category = "catalog-invalid";
        out_err->message = "well-formed JSON but not a valid/supported catalog "
                           "(missing required field or unsupported version)";
        return false;
    }
    /* rc == MOQ_OK */
    bool is_delta = cat.delta_update != NULL;
    if (want_delta && !is_delta) {
        moq_msf_catalog_cleanup(alloc, &cat);
        out_err->stage = "semantic"; out_err->category = "not-a-delta";
        out_err->message = "catalog.delta.parse requires a delta document (deltaUpdate)";
        return false;
    }
    if (!want_delta && is_delta) {
        moq_msf_catalog_cleanup(alloc, &cat);
        out_err->stage = "semantic"; out_err->category = "unexpected-delta";
        out_err->message = "catalog.parse requires an independent catalog, not a delta";
        return false;
    }
    /* Enforce the declared profile against the document form (independent only;
     * a delta carries no version). */
    if (!want_delta && !profile_matches(profile, profile_len, &shape)) {
        moq_msf_catalog_cleanup(alloc, &cat);
        out_err->stage = "semantic"; out_err->category = "profile-mismatch";
        out_err->message = "document version form does not match the declared profile";
        return false;
    }

    if (want_delta) put_delta(out_result, &cat);
    else            put_catalog(out_result, &cat);
    if (!want_delta && cat.content_protection_count)
        *out_diag = "cmsf-content-protection-parsed-playback-unsupported";
    moq_msf_catalog_cleanup(alloc, &cat);
    if (out_result->err) {
        out_err->stage = "internal"; out_err->category = "internal";
        out_err->message = "allocation failure while projecting the result";
        return false;
    }
    return true;
}

moq_result_t moq_media_probe_handle(const moq_alloc_t *alloc,
                                    const char *line, size_t len, char **out) {
    *out = NULL;
    const char *id = NULL; size_t id_len = 0; bool id_present = false;
    probe_err_t err;

    /* Oversized input: refuse before parsing. */
    if (len > MOQ_MEDIA_PROBE_MAX_LINE) {
        err.stage = "operation"; err.category = "oversized-input";
        err.message = "request line exceeds the maximum size";
        *out = finish(NULL, id, id_len, id_present, &err, NULL, 0);
        return *out ? MOQ_OK : MOQ_ERR_NOMEM;
    }

    /* The request must be valid UTF-8 before it is parsed, so no non-UTF-8 byte
     * can survive into a JSON-string field of the response. */
    if (line == NULL || !is_valid_utf8(line, len)) {
        err.stage = "syntax"; err.category = "malformed-json";
        err.message = "request line is not valid UTF-8";
        *out = finish(NULL, id, id_len, id_present, &err, NULL, 0);
        return *out ? MOQ_OK : MOQ_ERR_NOMEM;
    }

    struct json_value_s *root = json_parse(line, len);
    if (!root) {
        err.stage = "syntax"; err.category = "malformed-json";
        err.message = "request line is not well-formed JSON";
        *out = finish(NULL, id, id_len, id_present, &err, NULL, 0);
        return *out ? MOQ_OK : MOQ_ERR_NOMEM;
    }
    struct json_object_s *obj = json_value_as_object(root);
    if (!obj) {
        free(root);
        err.stage = "syntax"; err.category = "malformed-json";
        err.message = "request must be a JSON object";
        *out = finish(NULL, id, id_len, id_present, &err, NULL, 0);
        return *out ? MOQ_OK : MOQ_ERR_NOMEM;
    }

    /* Walk the envelope: exact key set, correct types. */
    const char *protocol = NULL; size_t protocol_len = 0; bool have_protocol = false;
    op_t op = OP_UNKNOWN; bool have_op = false; bool op_known = true;
    const char *profile = NULL; size_t profile_len = 0; bool have_profile = false;
    const char *input_utf8 = NULL; size_t input_len = 0; bool have_input = false;
    bool bad_field = false;      /* unknown top-level field */
    bool bad_type = false;       /* a known field with the wrong JSON type */

    for (struct json_object_element_s *e = obj->start; e; e = e->next) {
        const json_string_t *nm = e->name;
        if (jstr_eq(nm, "protocol")) {
            json_string_t *s = json_value_as_string(e->value);
            if (!s) { bad_type = true; } else { protocol = s->string; protocol_len = s->string_size; have_protocol = true; }
        } else if (jstr_eq(nm, "id")) {
            json_string_t *s = json_value_as_string(e->value);
            if (!s) { bad_type = true; } else { id = s->string; id_len = s->string_size; id_present = true; }
        } else if (jstr_eq(nm, "operation")) {
            json_string_t *s = json_value_as_string(e->value);
            if (!s) { bad_type = true; }
            else {
                have_op = true;
                if (jstr_eq(s, "capabilities")) op = OP_CAPABILITIES;
                else if (jstr_eq(s, "catalog.parse")) op = OP_CATALOG_PARSE;
                else if (jstr_eq(s, "catalog.delta.parse")) op = OP_CATALOG_DELTA_PARSE;
                else { op = OP_UNKNOWN; op_known = false; }
            }
        } else if (jstr_eq(nm, "profile")) {
            json_string_t *s = json_value_as_string(e->value);
            if (!s) { bad_type = true; } else { profile = s->string; profile_len = s->string_size; have_profile = true; }
        } else if (jstr_eq(nm, "input")) {
            struct json_object_s *io = json_value_as_object(e->value);
            if (!io) { bad_type = true; }
            else {
                bool io_bad = false;
                for (struct json_object_element_s *ie = io->start; ie; ie = ie->next) {
                    if (jstr_eq(ie->name, "utf8")) {
                        json_string_t *s = json_value_as_string(ie->value);
                        if (!s) io_bad = true; else { input_utf8 = s->string; input_len = s->string_size; have_input = true; }
                    } else {
                        bad_field = true; /* unknown key inside input */
                    }
                }
                if (io_bad) bad_type = true;
            }
        } else {
            bad_field = true;
        }
    }

    /* Validate the envelope in a fixed precedence so categories are stable. */
    const char *diag = NULL; const char *diags[1]; size_t ndiags = 0;
    sb_t result = { 0 };
    bool have_err = true;

    if (bad_type) {
        err.stage = "operation"; err.category = "invalid-field-type";
        err.message = "a request field has the wrong JSON type";
    } else if (bad_field) {
        err.stage = "operation"; err.category = "unknown-field";
        err.message = "request contains an unrecognized field";
    } else if (!have_protocol) {
        err.stage = "operation"; err.category = "missing-field";
        err.message = "request is missing the protocol field";
    } else if (protocol_len != strlen(MOQ_MEDIA_PROBE_PROTOCOL) ||
               memcmp(protocol, MOQ_MEDIA_PROBE_PROTOCOL, protocol_len) != 0) {
        err.stage = "operation"; err.category = "unsupported-protocol";
        err.message = "unsupported protocol version";
    } else if (!id_present) {
        err.stage = "operation"; err.category = "missing-field";
        err.message = "request is missing the id field";
    } else if (!have_op) {
        err.stage = "operation"; err.category = "missing-field";
        err.message = "request is missing the operation field";
    } else if (!op_known) {
        err.stage = "operation"; err.category = "unknown-operation";
        err.message = "unrecognized operation";
    } else if (op == OP_CAPABILITIES) {
        put_capabilities(&result);
        have_err = false;
    } else {
        /* catalog.parse / catalog.delta.parse: need profile + input.utf8. */
        if (!have_profile) {
            err.stage = "operation"; err.category = "missing-field";
            err.message = "operation requires a profile";
        } else if (!profile_known_parse(profile, profile_len)) {
            err.stage = "operation"; err.category = "unknown-profile";
            err.message = "unrecognized profile";
        } else if ((op == OP_CATALOG_PARSE && !profile_supported_parse(profile, profile_len)) ||
                   (op == OP_CATALOG_DELTA_PARSE && !profile_supported_delta(profile, profile_len))) {
            err.stage = "operation"; err.category = "unsupported-profile";
            err.message = "profile is not supported for this operation";
        } else if (!have_input) {
            err.stage = "operation"; err.category = "missing-field";
            err.message = "operation requires input.utf8";
        } else {
            have_err = !do_catalog(alloc, input_utf8, input_len, profile, profile_len,
                                   op == OP_CATALOG_DELTA_PARSE, &result, &err, &diag);
            if (!have_err && diag) { diags[0] = diag; ndiags = 1; }
        }
    }

    *out = have_err
        ? finish(NULL, id, id_len, id_present, &err, NULL, 0)
        : finish(&result, id, id_len, id_present, NULL, diags, ndiags);
    free(result.data);
    free(root);
    return *out ? MOQ_OK : MOQ_ERR_NOMEM;
}

/* ------------------------------------------------------------------ */
/* stdin -> stdout line loop.                                         */
/* ------------------------------------------------------------------ */

int moq_media_probe_run_io(const moq_alloc_t *alloc, const moq_media_probe_io_t *io) {
    sb_t line = { 0 };
    bool overflow = false;    /* current line already exceeded the bound */
    bool any = false;         /* saw at least one byte on the current line */

    for (;;) {
        int ch = io->get(io->ctx);
        bool at_eof = (ch < 0);
        if (!at_eof && ch != '\n') {
            any = true;
            if (line.len >= MOQ_MEDIA_PROBE_MAX_LINE) overflow = true;
            else { char c = (char)ch; sb_putn(&line, &c, 1); }
            continue;
        }
        /* End of a request line (newline), or end of input. A blank line
         * (no bytes) is not a request and is skipped. */
        if (any) {
            char *resp = NULL;
            if (overflow)
                moq_media_probe_handle(alloc, NULL, MOQ_MEDIA_PROBE_MAX_LINE + 1, &resp);
            else if (!line.err)
                moq_media_probe_handle(alloc, line.data ? line.data : "", line.len, &resp);
            if (!resp) { free(line.data); return 1; } /* could not allocate a response */
            /* Any output failure means the response was truncated: fail loudly
             * and immediately rather than exiting 0 after partial output. */
            bool ok = io->write(io->ctx, resp, strlen(resp)) &&
                      io->write(io->ctx, "\n", 1) &&
                      io->flush(io->ctx);
            free(resp);
            if (!ok) { free(line.data); return 1; }
        }
        line.len = 0; line.err = false; overflow = false; any = false;
        if (at_eof) {
            /* A read error is NOT a clean EOF. */
            int r = io->in_error(io->ctx) ? 1 : 0;
            free(line.data);
            return r;
        }
    }
}

/* Stdio-backed I/O seam for the process entry point. */
typedef struct { FILE *in; FILE *out; } stdio_io_ctx;
static int  sio_get(void *c)      { return fgetc(((stdio_io_ctx *)c)->in); }
static bool sio_in_error(void *c) { return ferror(((stdio_io_ctx *)c)->in) != 0; }
static bool sio_write(void *c, const char *s, size_t n) { return fwrite(s, 1, n, ((stdio_io_ctx *)c)->out) == n; }
static bool sio_flush(void *c)    { return fflush(((stdio_io_ctx *)c)->out) == 0; }

int moq_media_probe_run(const moq_alloc_t *alloc, FILE *in, FILE *out) {
    stdio_io_ctx sc = { in, out };
    moq_media_probe_io_t io = { sio_get, sio_in_error, sio_write, sio_flush, &sc };
    return moq_media_probe_run_io(alloc, &io);
}
