/*
 * Negotiated-profile substrate: the C++17 READER.
 *
 * It deliberately does NOT reimplement the grammar. C owns the parser; this
 * target links the same np_corpus.c object and asserts that a C++17
 * translation unit reaches the same record count and the same FNV-1a 64
 * digest, plus the same known answers. A second parser would be a second
 * source of truth, which is exactly what the one-reader rule forbids.
 */
extern "C" {
#include "np_corpus.h"
}

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

#ifndef NP_CORPUS_PATH
#error "NP_CORPUS_PATH must be defined at configure time"
#endif
#ifndef NP_CORPUS_COUNT
#error "NP_CORPUS_COUNT must be defined at configure time"
#endif
#ifndef NP_CORPUS_DIGEST
#error "NP_CORPUS_DIGEST must be defined at configure time"
#endif

static int failures = 0;

static void check(bool ok, const char *what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

int main()
{
    /* FNV known answers, independently spelled in this language. */
    check(np_fnv1a64("", 0) == NP_FNV1A64_OFFSET, "fnv empty");
    check(np_fnv1a64("abc", 3) == UINT64_C(0xe71fa2190541574b), "fnv abc");
    check(np_fnv1a64("ab", 2) != np_fnv1a64("abc", 3), "fnv truncation");

    np_corpus_t c;
    const char *why = nullptr;
    check(np_corpus_load(NP_CORPUS_PATH, &c, &why) == 0, "corpus load");
    if (why) std::fprintf(stderr, "  corpus rejected: %s\n", why);

    /* the checked-in corpus count and digest, pinned at configure time and
     * agreed with the C and Swift readers */
    check(c.n == static_cast<std::size_t>(NP_CORPUS_COUNT), "corpus count");
    check(c.digest == static_cast<std::uint64_t>(NP_CORPUS_DIGEST),
          "corpus digest");

    /* truncation is rejected by the shared reader from C++ too */
    {
        static const char kTrunc[] =
            "np-corpus 1\ncount 1\nd16 loc01 timestamp 64 4040\nend";
        np_corpus_t t;
        const char *w = nullptr;
        check(np_corpus_parse(kTrunc, std::strlen(kTrunc), &t, &w) != 0,
              "truncation rejected");
    }

    std::printf("%s: %d failures (count=%zu digest=0x%016llx)\n",
                failures ? "FAIL" : "PASS", failures, c.n,
                static_cast<unsigned long long>(c.digest));
    return failures ? 1 : 0;
}
