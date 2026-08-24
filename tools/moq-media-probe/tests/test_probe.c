/*
 * Unit tests for moq_media_probe: the pure request handler and the stdin/stdout
 * loop. Uses LibMoQ's vendored JSON parser only to INSPECT the probe's output
 * (never to author expected values by re-serializing the parser result under
 * test -- expected projections are written by hand from the input + spec). The
 * 14 committed MSF fixtures are used as native inputs where useful; they are
 * neither moved nor rewritten. Every allocation is freed so the sanitizer/leak
 * gate is credible.
 */
#include "probe.h"

#include <moq/types.h>
#include "json.h" /* declarations only; the implementation lives in probe.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);  \
            g_fail = 1;                                                      \
        }                                                                    \
    } while (0)

/* ---- helpers ---------------------------------------------------------- */

static char *run(const char *req) {
    char *out = NULL;
    moq_result_t rc = moq_media_probe_handle(moq_alloc_default(), req, strlen(req), &out);
    CHECK(rc == MOQ_OK);
    CHECK(out != NULL);
    return out;
}

/* Escape `cat` and splice it into a catalog request envelope (heap; free it). */
static char *make_req(const char *id, const char *op, const char *profile, const char *cat) {
    size_t cap = strlen(cat) * 2 + strlen(id) + strlen(op) + (profile ? strlen(profile) : 0) + 256;
    char *b = (char *)malloc(cap);
    size_t n = 0;
    n += (size_t)snprintf(b + n, cap - n,
        "{\"protocol\":\"moq-media-probe/1\",\"id\":\"%s\",\"operation\":\"%s\",", id, op);
    if (profile) n += (size_t)snprintf(b + n, cap - n, "\"profile\":\"%s\",", profile);
    n += (size_t)snprintf(b + n, cap - n, "\"input\":{\"utf8\":\"");
    for (const char *s = cat; *s; s++) {
        char c = *s;
        if (c == '"' || c == '\\') { b[n++] = '\\'; b[n++] = c; }
        else if (c == '\n') { b[n++] = '\\'; b[n++] = 'n'; }
        else b[n++] = c;
    }
    n += (size_t)snprintf(b + n, cap - n, "\"}}");
    b[n] = '\0';
    return b;
}

static struct json_value_s *jparse(const char *s) { return json_parse(s, strlen(s)); }

static struct json_value_s *obj_get(struct json_value_s *v, const char *k) {
    struct json_object_s *o = json_value_as_object(v);
    if (!o) return NULL;
    for (struct json_object_element_s *e = o->start; e; e = e->next)
        if (e->name->string_size == strlen(k) && memcmp(e->name->string, k, strlen(k)) == 0)
            return e->value;
    return NULL;
}

static bool str_is(struct json_value_s *v, const char *lit) {
    struct json_string_s *s = v ? json_value_as_string(v) : NULL;
    return s && s->string_size == strlen(lit) && memcmp(s->string, lit, strlen(lit)) == 0;
}

/* Recursively assert every object's element names are strictly ascending. */
static void assert_keys_sorted(struct json_value_s *v) {
    struct json_object_s *o = json_value_as_object(v);
    if (o) {
        const char *prev = NULL; size_t prev_n = 0;
        for (struct json_object_element_s *e = o->start; e; e = e->next) {
            if (prev) {
                size_t m = prev_n < e->name->string_size ? prev_n : e->name->string_size;
                int c = memcmp(prev, e->name->string, m);
                CHECK(c < 0 || (c == 0 && prev_n < e->name->string_size));
            }
            prev = e->name->string; prev_n = e->name->string_size;
            assert_keys_sorted(e->value);
        }
        return;
    }
    struct json_array_s *a = json_value_as_array(v);
    if (a)
        for (struct json_array_element_s *e = a->start; e; e = e->next)
            assert_keys_sorted(e->value);
}

static bool contains_key(struct json_value_s *v, const char *key) {
    struct json_object_s *o = json_value_as_object(v);
    if (o) {
        for (struct json_object_element_s *e = o->start; e; e = e->next) {
            if (e->name->string_size == strlen(key) && memcmp(e->name->string, key, strlen(key)) == 0)
                return true;
            if (contains_key(e->value, key)) return true;
        }
        return false;
    }
    struct json_array_s *a = json_value_as_array(v);
    if (a)
        for (struct json_array_element_s *e = a->start; e; e = e->next)
            if (contains_key(e->value, key)) return true;
    return false;
}

static void copy_str(struct json_value_s *v, char *buf, size_t cap) {
    struct json_string_s *s = v ? json_value_as_string(v) : NULL;
    buf[0] = '\0';
    if (s && s->string_size < cap) { memcpy(buf, s->string, s->string_size); buf[s->string_size] = '\0'; }
}

static void get_status(const char *resp, char *buf, size_t cap) {
    struct json_value_s *v = jparse(resp);
    copy_str(obj_get(v, "status"), buf, cap);
    free(v);
}
static void get_category(const char *resp, char *buf, size_t cap) {
    struct json_value_s *v = jparse(resp);
    struct json_value_s *e = obj_get(v, "error");
    copy_str(e ? obj_get(e, "category") : NULL, buf, cap);
    free(v);
}

#ifdef MOQ_MSF_FIXTURE_DIR
static char *read_fixture(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", MOQ_MSF_FIXTURE_DIR, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, fp);
    buf[got] = '\0';
    fclose(fp);
    return buf;
}
#endif

static void *fail_alloc(size_t s, void *c) { (void)s; (void)c; return NULL; }
static void *fail_realloc(void *p, size_t o, size_t n, void *c) { (void)p; (void)o; (void)n; (void)c; return NULL; }
static void fail_free(void *p, size_t s, void *c) { (void)p; (void)s; (void)c; }

/* Assert a catalog.parse request's status (and category, when error). */
static void expect_parse(const char *profile, const char *cat, const char *status, const char *category) {
    char *req = make_req("m", "catalog.parse", profile, cat);
    char *resp = run(req);
    char st[32], ca[64];
    get_status(resp, st, sizeof st);
    CHECK(strcmp(st, status) == 0);
    if (strcmp(status, "error") == 0) {
        get_category(resp, ca, sizeof ca);
        CHECK(strcmp(ca, category) == 0);
    }
    if (strcmp(st, status) != 0) fprintf(stderr, "  (profile=%s got status=%s cat=%s)\n", profile, st, cat);
    free(req); free(resp);
}

/* ---- tests ------------------------------------------------------------ */

static void test_capabilities_deterministic(void) {
    const char *req = "{\"protocol\":\"moq-media-probe/1\",\"id\":\"x\",\"operation\":\"capabilities\"}";
    char *a = run(req);
    char *b = run(req);
    CHECK(strcmp(a, b) == 0); /* byte-for-byte deterministic */
    struct json_value_s *v = jparse(a);
    CHECK(str_is(obj_get(v, "status"), "ok"));
    assert_keys_sorted(v);
    CHECK(contains_key(v, "operations"));
    CHECK(contains_key(v, "protectedPlayback")); /* cmsf-01 honesty */
    /* The lossy empty-array projection is reported as a capability limitation. */
    CHECK(strstr(a, "empty-array-indistinguishable-from-absent") != NULL);
    free(v);
    free(a); free(b);
}

static void test_msf01_string_one(void) {
    char *req = make_req("1", "catalog.parse", "msf-01",
        "{\"version\":\"1\",\"tracks\":[{\"name\":\"v\",\"packaging\":\"loc\",\"isLive\":true}]}");
    char *resp = run(req);
    struct json_value_s *v = jparse(resp);
    CHECK(str_is(obj_get(v, "status"), "ok"));
    CHECK(str_is(obj_get(obj_get(v, "result"), "version"), "1"));
    assert_keys_sorted(v);
    free(v); free(resp); free(req);
}

static void test_draft_form_characterized(void) {
    /* draft-01 is recognized under the draft profile. */
    char *ok = make_req("d1", "catalog.parse", "msf-01-draft",
        "{\"version\":\"draft-01\",\"tracks\":[{\"name\":\"v\",\"packaging\":\"loc\",\"isLive\":true}]}");
    char *r1 = run(ok);
    char st[32]; get_status(r1, st, sizeof st);
    CHECK(strcmp(st, "ok") == 0);
    /* Another draft matches the profile FORM but the parser does not support it. */
    expect_parse("msf-01-draft",
        "{\"version\":\"draft-99\",\"tracks\":[{\"name\":\"v\",\"packaging\":\"loc\",\"isLive\":true}]}",
        "error", "catalog-invalid");
    free(r1); free(ok);
}

/* Full profile x version-form matrix (finding: profile must be enforced). */
static void test_profile_mismatch_matrix(void) {
    const char *num  = "{\"version\":1,\"tracks\":[{\"name\":\"v\",\"packaging\":\"loc\",\"isLive\":true}]}";
    const char *s1   = "{\"version\":\"1\",\"tracks\":[{\"name\":\"v\",\"packaging\":\"loc\",\"isLive\":true}]}";
    const char *draft= "{\"version\":\"draft-01\",\"tracks\":[{\"name\":\"v\",\"packaging\":\"loc\",\"isLive\":true}]}";
    const char *cmsf = "{\"version\":\"1\",\"tracks\":[{\"name\":\"v\",\"packaging\":\"cmaf\",\"isLive\":true}],\"initDataList\":[{\"id\":\"i\",\"type\":\"inline\",\"data\":\"AAAB\"}]}";

    expect_parse("msf-00", num,   "ok", NULL);
    expect_parse("msf-00", s1,    "error", "profile-mismatch");
    expect_parse("msf-01", s1,    "ok", NULL);
    expect_parse("msf-01", num,   "error", "profile-mismatch");
    expect_parse("msf-01", draft, "error", "profile-mismatch");
    expect_parse("msf-01-draft", draft, "ok", NULL);
    expect_parse("msf-01-draft", s1,  "error", "profile-mismatch");
    expect_parse("msf-01-draft", num, "error", "profile-mismatch");
    expect_parse("cmsf-01", cmsf, "ok", NULL);
    expect_parse("cmsf-01", s1,   "ok", NULL); /* CMSF markers are OPTIONAL: string "1" suffices */
    expect_parse("cmsf-01", num,  "error", "profile-mismatch");
    /* Marker-free CMSF catalogs are valid and must be accepted. */
    expect_parse("cmsf-01", "{\"version\":\"1\",\"tracks\":[]}", "ok", NULL); /* empty */
    expect_parse("cmsf-01",
        "{\"version\":\"1\",\"tracks\":[{\"name\":\"e\",\"packaging\":\"eventtimeline\",\"isLive\":true,\"eventType\":\"org.example.sap\"}]}",
        "ok", NULL); /* eventtimeline-only */
}

/* Independently-authored EXACT projection of a CMSF catalog, incl. nested
 * DRM/init fields and spec field names (laURL, not laUrl). */
static void test_cmsf_exact_projection(void) {
    const char *cat =
        "{\"version\":\"1\",\"generatedAt\":1000,"
        "\"tracks\":[{\"name\":\"v\",\"packaging\":\"cmaf\",\"isLive\":true,\"codec\":\"avc1.4d401f\","
        "\"width\":1920,\"height\":1080,\"initRef\":\"i1\",\"contentProtectionRefIDs\":[\"1\"],\"depends\":[\"base\"]}],"
        "\"contentProtections\":[{\"refID\":\"1\",\"defaultKID\":[\"01234567-89ab-cdef-0123-456789abcdef\"],"
        "\"scheme\":\"cbcs\",\"drmSystem\":{\"systemID\":\"edef8ba9-79d6-4ace-a3c8-27dcd51d21ed\","
        "\"laURL\":{\"url\":\"https://la.example/x\",\"type\":\"EME-1.0\"},\"pssh\":\"AAAB\"}}],"
        "\"initDataList\":[{\"id\":\"i1\",\"type\":\"inline\",\"data\":\"AAAB\"}]}";
    /* Hand-authored canonical projection (keys ascending; ints as strings;
     * spec field names preserved). */
    const char *expected =
        "{\"contentProtections\":[{\"defaultKID\":[\"01234567-89ab-cdef-0123-456789abcdef\"],"
        "\"drmSystem\":{\"laURL\":{\"type\":\"EME-1.0\",\"url\":\"https://la.example/x\"},"
        "\"pssh\":\"AAAB\",\"systemID\":\"edef8ba9-79d6-4ace-a3c8-27dcd51d21ed\"},"
        "\"refID\":\"1\",\"scheme\":\"cbcs\"}],"
        "\"generatedAt\":\"1000\","
        "\"initDataList\":[{\"data\":\"AAAB\",\"id\":\"i1\",\"type\":\"inline\"}],"
        "\"tracks\":[{\"codec\":\"avc1.4d401f\",\"contentProtectionRefIDs\":[\"1\"],\"depends\":[\"base\"],"
        "\"height\":\"1080\",\"initRef\":\"i1\",\"isLive\":true,\"name\":\"v\",\"packaging\":\"cmaf\","
        "\"width\":\"1920\"}],\"version\":\"1\"}";
    char *req = make_req("x", "catalog.parse", "cmsf-01", cat);
    char *resp = run(req);
    CHECK(strstr(resp, expected) != NULL); /* exact projection appears verbatim */
    CHECK(strstr(resp, "laUrl") == NULL);  /* NOT lowercased */
    CHECK(strstr(resp, "playback-unsupported") != NULL);
    if (!strstr(resp, expected)) fprintf(stderr, "  got: %s\n", resp);
    free(req); free(resp);
}

/* Empty-vs-absent arrays: LibMoQ's model cannot distinguish them, so the probe
 * omits present-but-empty arrays -- pinned here as documented behavior. */
static void test_empty_arrays_omitted(void) {
    const char *cat =
        "{\"version\":\"1\",\"tracks\":[{\"name\":\"v\",\"packaging\":\"cmaf\",\"isLive\":true,"
        "\"depends\":[],\"contentProtectionRefIDs\":[]}],\"contentProtections\":[],\"initDataList\":[]}";
    const char *expected = "\"result\":{\"tracks\":[{\"isLive\":true,\"name\":\"v\",\"packaging\":\"cmaf\"}],\"version\":\"1\"}";
    char *req = make_req("e", "catalog.parse", "cmsf-01", cat);
    char *resp = run(req);
    CHECK(strstr(resp, expected) != NULL);
    CHECK(strstr(resp, "depends") == NULL);
    CHECK(strstr(resp, "contentProtectionRefIDs") == NULL);
    CHECK(strstr(resp, "initDataList") == NULL);
    CHECK(strstr(resp, "contentProtections") == NULL);
    if (!strstr(resp, expected)) fprintf(stderr, "  got: %s\n", resp);
    free(req); free(resp);
}

static void test_cmsf_fixture_fields_survive(void) {
#ifdef MOQ_MSF_FIXTURE_DIR
    char *cat = read_fixture("cmsf_clearkey.json");
    CHECK(cat != NULL);
    if (cat) {
        char *req = make_req("cp", "catalog.parse", "cmsf-01", cat);
        char *resp = run(req);
        struct json_value_s *v = jparse(resp);
        CHECK(str_is(obj_get(v, "status"), "ok"));
        struct json_value_s *res = obj_get(v, "result");
        CHECK(contains_key(res, "initDataList"));
        CHECK(contains_key(res, "initRef"));
        CHECK(contains_key(res, "contentProtections"));
        CHECK(contains_key(res, "systemID"));
        CHECK(contains_key(res, "laURL"));
        assert_keys_sorted(v);
        free(v); free(resp); free(req); free(cat);
    }
#endif
}

static void test_unknown_fields_do_not_leak(void) {
#ifdef MOQ_MSF_FIXTURE_DIR
    char *cat = read_fixture("unknown_fields.json"); /* customTop, futureField, anotherUnknown; numeric version */
    CHECK(cat != NULL);
    if (cat) {
        char *req = make_req("u", "catalog.parse", "msf-00", cat);
        char *resp = run(req);
        struct json_value_s *v = jparse(resp);
        struct json_value_s *res = obj_get(v, "result");
        CHECK(str_is(obj_get(v, "status"), "ok"));
        CHECK(!contains_key(res, "customTop"));
        CHECK(!contains_key(res, "futureField"));
        CHECK(!contains_key(res, "anotherUnknown"));
        free(v); free(resp); free(req); free(cat);
    }
#endif
}

static void test_delta_order_preserved_and_reversal_differs(void) {
#ifdef MOQ_MSF_FIXTURE_DIR
    char *cat = read_fixture("delta_add_clone.json");
    CHECK(cat != NULL);
    if (cat) {
        char *req = make_req("d", "catalog.delta.parse", "msf-01", cat);
        char *resp = run(req);
        struct json_value_s *v = jparse(resp);
        CHECK(str_is(obj_get(v, "status"), "ok"));
        struct json_array_s *a = json_value_as_array(obj_get(obj_get(v, "result"), "deltaUpdate"));
        CHECK(a && a->length == 2);
        if (a && a->length == 2) {
            CHECK(str_is(obj_get(a->start->value, "op"), "add"));
            CHECK(str_is(obj_get(a->start->next->value, "op"), "clone"));
        }
        free(v); free(resp); free(req); free(cat);
    }
#endif
    const char *ab = "{\"deltaUpdate\":[{\"op\":\"add\",\"tracks\":[{\"name\":\"a\",\"packaging\":\"loc\",\"isLive\":true}]},{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"a\",\"name\":\"b\"}]}]}";
    const char *ba = "{\"deltaUpdate\":[{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"a\",\"name\":\"b\"}]},{\"op\":\"add\",\"tracks\":[{\"name\":\"a\",\"packaging\":\"loc\",\"isLive\":true}]}]}";
    char *ra_req = make_req("r1", "catalog.delta.parse", "msf-01", ab);
    char *rb_req = make_req("r2", "catalog.delta.parse", "msf-01", ba);
    char *ra = run(ra_req);
    char *rb = run(rb_req);
    CHECK(strstr(ra, "\"add\"") < strstr(ra, "\"clone\""));
    CHECK(strstr(rb, "\"clone\"") < strstr(rb, "\"add\""));
    free(ra); free(rb); free(ra_req); free(rb_req);
}

static void test_error_categories_distinct(void) {
    struct { const char *req; const char *cat; } cases[] = {
        { "this is not json", "malformed-json" },
        { "{\"protocol\":\"x/1\",\"id\":\"a\",\"operation\":\"capabilities\"}", "unsupported-protocol" },
        { "{\"protocol\":\"moq-media-probe/1\",\"id\":\"a\",\"operation\":\"loc.parse\"}", "unknown-operation" },
        { "{\"protocol\":\"moq-media-probe/1\",\"id\":\"a\",\"operation\":\"catalog.parse\",\"profile\":\"zzz\",\"input\":{\"utf8\":\"{}\"}}", "unknown-profile" },
        { "{\"protocol\":\"moq-media-probe/1\",\"id\":\"a\",\"operation\":\"catalog.delta.parse\",\"profile\":\"cmsf-01\",\"input\":{\"utf8\":\"{}\"}}", "unsupported-profile" },
        { "{\"protocol\":\"moq-media-probe/1\",\"id\":\"a\",\"operation\":\"capabilities\",\"bogus\":1}", "unknown-field" },
        { "{\"protocol\":\"moq-media-probe/1\",\"id\":123,\"operation\":\"capabilities\"}", "invalid-field-type" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *resp = run(cases[i].req);
        char ca[64]; get_category(resp, ca, sizeof ca);
        CHECK(strcmp(ca, cases[i].cat) == 0);
        if (strcmp(ca, cases[i].cat) != 0) fprintf(stderr, "  case %zu got %s\n", i, ca);
        free(resp);
    }
    /* Semantic (valid JSON, invalid catalog) vs input-syntax (malformed input JSON). */
    expect_parse("msf-01", "{\"version\":\"1\"}", "error", "catalog-invalid");
    expect_parse("msf-01", "{not json", "error", "malformed-json");
}

/* Prove the RFC 3629 validator DISCRIMINATES: valid multi-byte sequences pass
 * (and round-trip), and each class of ill-formed sequence is rejected -- not a
 * blanket "reject all non-ASCII". The id byte-sequence is isolated by string
 * concatenation so a hex escape can't absorb the following character. */
#define UTF8_REQ(seq) "{\"protocol\":\"moq-media-probe/1\",\"id\":\"" seq "\",\"operation\":\"capabilities\"}"
static void test_utf8_validation(void) {
    struct { const char *req; const char *seq; } good[] = {
        { UTF8_REQ("\xC3\xA9"),         "\xC3\xA9" },         /* U+00E9  2-byte */
        { UTF8_REQ("\xE2\x82\xAC"),     "\xE2\x82\xAC" },     /* U+20AC  3-byte */
        { UTF8_REQ("\xF0\x9D\x84\x9E"), "\xF0\x9D\x84\x9E" }, /* U+1D11E 4-byte */
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        char *out = run(good[i].req);
        char st[32]; get_status(out, st, sizeof st);
        CHECK(strcmp(st, "ok") == 0);                 /* accepted */
        CHECK(strstr(out, good[i].seq) != NULL);      /* echoed verbatim (round-trip) */
        free(out);
    }
    const char *bad[] = {
        UTF8_REQ("\xC0\xAF"),         /* overlong 2-byte ('/') */
        UTF8_REQ("\xE0\x80\xAF"),     /* overlong 3-byte */
        UTF8_REQ("\xF0\x80\x80\xAF"), /* overlong 4-byte */
        UTF8_REQ("\xED\xA0\x80"),     /* encoded surrogate U+D800 */
        UTF8_REQ("\xF4\x90\x80\x80"), /* above U+10FFFF */
        UTF8_REQ("\xF5\x80\x80\x80"), /* invalid lead byte */
        UTF8_REQ("\xC3\x28"),         /* bad continuation */
        UTF8_REQ("\xE2\x82"),         /* truncated 3-byte */
        UTF8_REQ("\xFF"),             /* lone 0xff */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char *out = NULL;
        CHECK(moq_media_probe_handle(moq_alloc_default(), bad[i], strlen(bad[i]), &out) == MOQ_OK);
        char ca[64]; get_category(out, ca, sizeof ca);
        CHECK(strcmp(ca, "malformed-json") == 0);
        for (char *p = out; *p; p++) CHECK((unsigned char)*p < 0x80); /* error output is pure ASCII */
        free(out);
    }
}
#undef UTF8_REQ

/* Inject a failing I/O seam and prove read failure, short write (of the body
 * and of the newline), and flush failure each return nonzero. */
typedef struct {
    const char *in; size_t pos, len; bool in_err;
    int put_calls; int fail_put_at; bool fail_flush;
} tio_t;
static int  tio_get(void *c)      { tio_t *t = c; return t->pos < t->len ? (unsigned char)t->in[t->pos++] : -1; }
static bool tio_in_error(void *c) { return ((tio_t *)c)->in_err; }
static bool tio_write(void *c, const char *s, size_t n) {
    (void)s; (void)n; tio_t *t = c; t->put_calls++;
    return !(t->fail_put_at && t->put_calls == t->fail_put_at);
}
static bool tio_flush(void *c)    { return !((tio_t *)c)->fail_flush; }
static int run_tio(tio_t *t) {
    moq_media_probe_io_t io = { tio_get, tio_in_error, tio_write, tio_flush, t };
    return moq_media_probe_run_io(moq_alloc_default(), &io);
}
static void test_io_failures(void) {
    const char *req = "{\"protocol\":\"moq-media-probe/1\",\"id\":\"a\",\"operation\":\"capabilities\"}\n";
    size_t n = strlen(req);
    tio_t control    = { req, 0, n, false, 0, 0, false };
    tio_t read_err   = { req, 0, n, true,  0, 0, false };
    tio_t short_body = { req, 0, n, false, 0, 1, false }; /* first write (body) fails */
    tio_t short_nl   = { req, 0, n, false, 0, 2, false }; /* second write (newline) fails */
    tio_t flush_err  = { req, 0, n, false, 0, 0, true };
    CHECK(run_tio(&control) == 0);    /* clean success */
    CHECK(run_tio(&read_err) != 0);   /* input read error is not clean EOF */
    CHECK(run_tio(&short_body) != 0); /* short write */
    CHECK(run_tio(&short_nl) != 0);   /* short write of the newline */
    CHECK(run_tio(&flush_err) != 0);  /* flush failure */
}

static void test_oversized_input(void) {
    size_t big = MOQ_MEDIA_PROBE_MAX_LINE + 8;
    char *buf = (char *)malloc(big + 1);
    memset(buf, 'x', big);
    buf[big] = '\0';
    char *out = NULL;
    CHECK(moq_media_probe_handle(moq_alloc_default(), buf, big, &out) == MOQ_OK);
    char ca[64]; get_category(out, ca, sizeof ca);
    CHECK(strcmp(ca, "oversized-input") == 0);
    free(out); free(buf);
}

static void test_large_integer_exact(void) {
    char *req = make_req("n", "catalog.parse", "msf-01",
        "{\"version\":\"1\",\"generatedAt\":18446744073709551615,"
        "\"tracks\":[{\"name\":\"v\",\"packaging\":\"loc\",\"isLive\":true,\"bitrate\":9007199254740993}]}");
    char *resp = run(req);
    CHECK(strstr(resp, "\"generatedAt\":\"18446744073709551615\"") != NULL);
    CHECK(strstr(resp, "\"bitrate\":\"9007199254740993\"") != NULL);
    free(resp); free(req);
}

static void test_alloc_failure_no_crash(void) {
    const moq_alloc_t failing = { NULL, fail_alloc, fail_realloc, fail_free };
    char *req = make_req("f", "catalog.parse", "msf-01",
        "{\"version\":\"1\",\"tracks\":[{\"name\":\"v\",\"packaging\":\"loc\",\"isLive\":true}]}");
    char *out = NULL;
    CHECK(moq_media_probe_handle(&failing, req, strlen(req), &out) == MOQ_OK);
    char ca[64]; get_category(out, ca, sizeof ca);
    CHECK(strcmp(ca, "internal") == 0);
    free(out); free(req);
}

static void test_all_fixtures(void) {
#ifdef MOQ_MSF_FIXTURE_DIR
    struct { const char *name; const char *op; const char *profile; } fx[] = {
        { "av_single.json",          "catalog.parse",       "msf-00" },
        { "empty_tracks.json",       "catalog.parse",       "msf-00" },
        { "minimal.json",            "catalog.parse",       "msf-00" },
        { "unknown_fields.json",     "catalog.parse",       "msf-00" },
        { "with_init_data.json",     "catalog.parse",       "msf-00" },
        { "mediatimeline.json",      "catalog.parse",       "msf-01" },
        { "template.json",           "catalog.parse",       "msf-01" },
        { "termination.json",        "catalog.parse",       "msf-01" },
        { "vod.json",                "catalog.parse",       "msf-01" },
        { "cmsf_clearkey.json",      "catalog.parse",       "cmsf-01" },
        { "cmsf_cmaf_simulcast.json","catalog.parse",       "cmsf-01" },
        { "cmsf_drm_cbcs.json",      "catalog.parse",       "cmsf-01" },
        { "delta_add_clone.json",    "catalog.delta.parse", "msf-01" },
        { "delta_remove.json",       "catalog.delta.parse", "msf-01" },
    };
    for (size_t i = 0; i < sizeof(fx) / sizeof(fx[0]); i++) {
        char *cat = read_fixture(fx[i].name);
        CHECK(cat != NULL);
        if (!cat) continue;
        char *req = make_req("fx", fx[i].op, fx[i].profile, cat);
        char *resp = run(req);
        struct json_value_s *v = jparse(resp);
        CHECK(v != NULL);
        CHECK(str_is(obj_get(v, "status"), "ok")); /* correct profile => parses */
        if (!str_is(obj_get(v, "status"), "ok")) fprintf(stderr, "  fixture %s not ok\n", fx[i].name);
        assert_keys_sorted(v);
        free(v); free(resp); free(req); free(cat);
    }
#endif
}

static void test_run_loop_malformed_then_valid(void) {
    FILE *in = tmpfile();
    FILE *out = tmpfile();
    CHECK(in && out);
    fputs("garbage not json\n", in);
    fputs("{\"protocol\":\"moq-media-probe/1\",\"id\":\"ok\",\"operation\":\"capabilities\"}\n", in);
    rewind(in);
    CHECK(moq_media_probe_run(moq_alloc_default(), in, out) == 0);
    rewind(out);
    char buf[8192];
    size_t got = fread(buf, 1, sizeof(buf) - 1, out);
    buf[got] = '\0';
    int lines = 0;
    for (size_t i = 0; i < got; i++) if (buf[i] == '\n') lines++;
    CHECK(lines == 2); /* the malformed line did not poison the valid one */
    CHECK(strstr(buf, "\"category\":\"malformed-json\"") != NULL);
    CHECK(strstr(buf, "\"id\":\"ok\"") != NULL);
    CHECK(strstr(buf, "\"status\":\"ok\"") != NULL);
    fclose(in); fclose(out);
}

int main(void) {
    test_capabilities_deterministic();
    test_msf01_string_one();
    test_draft_form_characterized();
    test_profile_mismatch_matrix();
    test_cmsf_exact_projection();
    test_empty_arrays_omitted();
    test_cmsf_fixture_fields_survive();
    test_unknown_fields_do_not_leak();
    test_delta_order_preserved_and_reversal_differs();
    test_error_categories_distinct();
    test_utf8_validation();
    test_io_failures();
    test_oversized_input();
    test_large_integer_exact();
    test_alloc_failure_no_crash();
    test_all_fixtures();
    test_run_loop_malformed_then_valid();
    if (g_fail) { fprintf(stderr, "SOME TESTS FAILED\n"); return 1; }
    printf("all moq_media_probe tests passed\n");
    return 0;
}
