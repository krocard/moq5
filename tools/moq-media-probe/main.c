/*
 * moq_media_probe -- CLI entry point.
 *
 * Reads one JSON request per line from stdin and writes one JSON response per
 * line to stdout (see probe.h / README.md). Machine output goes ONLY to stdout;
 * this process writes nothing to stderr on the normal path.
 */
#include "probe.h"

#include <moq/types.h>
#include <stdio.h>

int main(void) {
    return moq_media_probe_run(moq_alloc_default(), stdin, stdout);
}
