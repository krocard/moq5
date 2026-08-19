#ifndef MOQR_CLI_VERSIONS_H
#define MOQR_CLI_VERSIONS_H

/*
 * The one production mapping from relay configuration to managed-transport
 * versions. Both serve compositions (single-lane and multi-lane) call it, so a
 * change cannot reach one path and miss the other, and tests exercise the same
 * function the relay runs rather than a copy of its logic.
 */

#include <moq/msquic_managed.h>

#include "config.h"

/*
 * One configured version keeps the legacy exact-version listener: cfg.version
 * is populated and the list fields stay empty. Several versions set
 * cfg.version = 0 and hand over the ordered list, which is what makes ALPN
 * select each connection's draft (draft-18 Section 3.1/3.1.1).
 */
void moqr_cli_apply_versions(const moqr_cli_config_t *cfg,
                             moq_msquic_managed_cfg_t *tcfg);

#endif /* MOQR_CLI_VERSIONS_H */
