/*
 * Negotiated-profile CLOSURE DIRECTION C: the codec itself, corpus-driven.
 *
 * Directions A and B close the loop through a real media_sender and a real
 * media_receiver, which is what proves the SERVICE carries the negotiated
 * draft. This third direction closes it one layer down, on the LOC codec
 * alone, and it is what completes the four-role structure: the SCRIPTED
 * BUILDER (role 2) and the PRODUCT ADAPTER (role 3) are driven against each
 * other over the ONE authoritative corpus, by a driver that itself touches
 * no LOC, key-value-pair or integer codec.
 *
 * Why it is not redundant with the service directions: those exercise one
 * value pair (33 and 33333) through a great deal of machinery. This one
 * exercises EVERY value the corpus declares, at both drafts, against bytes
 * the corpus itself fixes -- so a codec defect at, say, the 16383/16384
 * boundary is caught here even though no service test carries such a value.
 *
 * Three independent things must agree for a record to pass:
 *
 *   the CORPUS's declared value bytes, checked in and digest-pinned;
 *   the BUILDER's block, re-derived from the drafts through the pure oracle;
 *   the PRODUCT's block, produced by moq_loc_encode through the adapter.
 *
 * Two of the three agreeing would prove nothing about the third.
 */
#include "np_corpus.h"
#include "np_oracle.h"
#include "np_product_adapter.h"
#include "np_wire_builder.h"
#include "test_support.h"

#include <moq/session.h>

#include <stdio.h>
#include <string.h>

#ifndef NP_CORPUS_PATH
#error "NP_CORPUS_PATH must be defined at configure time"
#endif

static int failures = 0;

static void dump(const char *label, const uint8_t *b, size_t n)
{
    fprintf(stderr, "  %s (%zu):", label, n);
    for (size_t i = 0; i < n; i++) fprintf(stderr, " %02x", b[i]);
    fprintf(stderr, "\n");
}

/* The transport version a corpus record names, or 0 if it names none. */
static moq_version_t version_for(const np_corpus_rec_t *r)
{
    if (strcmp(r->transport, "d16") == 0) return MOQ_VERSION_DRAFT_16;
    if (strcmp(r->transport, "d18") == 0) return MOQ_VERSION_DRAFT_18;
    return (moq_version_t)0;
}

/* The encoding each draft's integer fields use. Spelled here rather than
 * borrowed from role 4, so this driver depends on the oracle's vocabulary
 * only -- draft-16 section 1.4 (i), draft-18 section 1.4.1 (vi64). */
static np_enc_t enc_for(moq_version_t v)
{
    if (v == MOQ_VERSION_DRAFT_16) return NP_ENC_QUIC_VARINT;
    if (v == MOQ_VERSION_DRAFT_18) return NP_ENC_VI64;
    return (np_enc_t)0;
}

int main(void)
{
    /* Both roles answer for themselves before either is judged. */
    int oracle = np_oracle_self_check();
    if (oracle != 0) {
        fprintf(stderr, "FAIL: oracle self-check: %d failure(s)\n", oracle);
        failures += oracle;
    }
    int builder = np_wire_builder_self_check();
    if (builder != 0) {
        fprintf(stderr, "FAIL: builder self-check: %d failure(s)\n", builder);
        failures += builder;
    }
    if (failures) return 1;

    static np_corpus_t corpus;
    const char *why = NULL;
    if (np_corpus_load(NP_CORPUS_PATH, &corpus, &why) != 0) {
        fprintf(stderr, "FAIL: corpus: %s\n", why ? why : "(unnamed)");
        return 1;
    }

    size_t exercised = 0, refused = 0;

    for (size_t i = 0; i < corpus.n; i++) {
        const np_corpus_rec_t *r = &corpus.recs[i];
        if (strcmp(r->media, "loc01") != 0) continue;
        if (strcmp(r->property, "timestamp") != 0) continue;

        moq_version_t v = version_for(r);
        MOQ_TEST_CHECK(v != (moq_version_t)0);
        if (v == (moq_version_t)0) continue;
        np_enc_t enc = enc_for(v);
        MOQ_TEST_CHECK(enc != (np_enc_t)0);
        if (enc == (np_enc_t)0) continue;

        /* One logical block, expressed twice: packed for the builder, typed
         * for the product. A RAW video object carries both properties, so
         * this also keeps the second entry's delta in play. */
        np_loc01_block_t b;
        memset(&b, 0, sizeof(b));
        b.has_capture_timestamp = true;
        b.capture_timestamp = r->value;
        b.has_video_frame_marking = true;
        b.video_frame_marking = np_pack_video_frame_marking(
            false, false, true, false, false, 0, false, 0);

        np_adapter_loc01_t a;
        memset(&a, 0, sizeof(a));
        a.has_capture_timestamp = true;
        a.capture_timestamp = r->value;
        a.has_video_frame_marking = true;
        a.vfm_independent = true;

        uint8_t want[NP_LOC01_BLOCK_MAX];
        uint8_t got[NP_LOC01_BLOCK_MAX];
        size_t want_len = np_build_loc01_block(enc, &b, want, sizeof(want));
        size_t got_len  = np_adapter_encode_loc01(v, &a, got, sizeof(got));

        if (want_len == 0) {
            /* The draft cannot represent this value -- draft-16 above its
             * own ceiling. The product must refuse it too, never truncate
             * it into a different value. */
            if (got_len != 0) {
                fprintf(stderr,
                    "FAIL: %s timestamp %llu: the draft cannot encode this "
                    "value, but the product produced a block\n",
                    r->transport, (unsigned long long)r->value);
                dump("produced", got, got_len);
                failures++;
            }
            refused++;
            continue;
        }

        if (got_len != want_len || memcmp(got, want, want_len) != 0) {
            fprintf(stderr,
                "FAIL: %s timestamp %llu: property block differs\n",
                r->transport, (unsigned long long)r->value);
            dump("from the product", got, got_len);
            dump("per the draft", want, want_len);
            failures++;
            continue;
        }

        /* The block really carries the corpus's own declared bytes. The
         * Capture Timestamp is property 0x02 and the first entry, so its
         * Delta Type is one byte and the value follows immediately. This is
         * what ties the agreement above to the checked-in, digest-pinned
         * corpus rather than to the builder alone. */
        MOQ_TEST_CHECK(want_len > 1 + r->n_bytes);
        if (want_len > 1 + r->n_bytes) {
            MOQ_TEST_CHECK_EQ_HEX(want[0], 0x02);
            if (memcmp(want + 1, r->bytes, r->n_bytes) != 0) {
                fprintf(stderr,
                    "FAIL: %s timestamp %llu: the block does not carry the "
                    "corpus's declared value bytes\n",
                    r->transport, (unsigned long long)r->value);
                dump("in the block", want + 1, r->n_bytes);
                dump("in the corpus", r->bytes, r->n_bytes);
                failures++;
            }
        }

        /* The product PARSER reads the scripted bytes back exactly. */
        np_adapter_loc01_t back;
        if (!np_adapter_parse_loc01(v, want, want_len, &back)) {
            fprintf(stderr,
                "FAIL: %s timestamp %llu: the product refused the draft's "
                "own bytes\n", r->transport, (unsigned long long)r->value);
            dump("refused", want, want_len);
            failures++;
        } else {
            MOQ_TEST_CHECK(back.has_capture_timestamp);
            MOQ_TEST_CHECK_EQ_U64(back.capture_timestamp, r->value);
            MOQ_TEST_CHECK(back.has_video_frame_marking);
            MOQ_TEST_CHECK(back.vfm_independent);
        }

        /* Cross-family: the OTHER draft's codec must not reproduce the
         * value. Below 64 the two encodings agree and it legitimately does,
         * so that band is skipped here -- it is the control the service
         * directions already carry. */
        if (r->value >= 64) {
            moq_version_t other = (v == MOQ_VERSION_DRAFT_16)
                                      ? MOQ_VERSION_DRAFT_18
                                      : MOQ_VERSION_DRAFT_16;
            np_adapter_loc01_t wrong;
            bool ok = np_adapter_parse_loc01(other, want, want_len, &wrong);
            if (ok && wrong.has_capture_timestamp &&
                wrong.capture_timestamp == r->value) {
                fprintf(stderr,
                    "FAIL: %s timestamp %llu was reproduced exactly by the "
                    "other draft's codec\n",
                    r->transport, (unsigned long long)r->value);
                failures++;
            }
        }

        exercised++;
    }

    /* The loop must actually have run: a corpus that stopped declaring
     * timestamp records would otherwise pass silently. */
    MOQ_TEST_CHECK(exercised >= 20);

    /* The refusal arm above is only reached by a value the corpus declares
     * for one draft and not the other, and today it declares 2^64-1 for
     * draft-18 alone -- so the arm would sit dead. Exercise it directly:
     * draft-16's integer stops at 2^62-1, and a value past it must be
     * refused by BOTH roles rather than truncated by either. */
    {
        np_loc01_block_t b;
        memset(&b, 0, sizeof(b));
        b.has_capture_timestamp = true;
        b.capture_timestamp = UINT64_MAX;
        b.has_video_frame_marking = true;
        b.video_frame_marking = np_pack_video_frame_marking(
            false, false, true, false, false, 0, false, 0);

        np_adapter_loc01_t a;
        memset(&a, 0, sizeof(a));
        a.has_capture_timestamp = true;
        a.capture_timestamp = UINT64_MAX;
        a.has_video_frame_marking = true;
        a.vfm_independent = true;

        uint8_t buf[NP_LOC01_BLOCK_MAX];
        MOQ_TEST_CHECK_EQ_SIZE(
            np_build_loc01_block(NP_ENC_QUIC_VARINT, &b, buf, sizeof(buf)), 0u);
        MOQ_TEST_CHECK_EQ_SIZE(
            np_adapter_encode_loc01(MOQ_VERSION_DRAFT_16, &a, buf,
                                    sizeof(buf)), 0u);

        /* draft-18 reaches it, so the refusal above is about the codec and
         * not about the value being rejected everywhere. */
        MOQ_TEST_CHECK(np_build_loc01_block(NP_ENC_VI64, &b, buf,
                                            sizeof(buf)) > 0);
        MOQ_TEST_CHECK(np_adapter_encode_loc01(MOQ_VERSION_DRAFT_18, &a, buf,
                                               sizeof(buf)) > 0);
    }
    fprintf(stderr,
        "np_codec_closure: %zu record(s) exercised, %zu correctly refused\n",
        exercised, refused);

    MOQ_TEST_PASS("np_codec_closure");
    return failures ? 1 : 0;
}
