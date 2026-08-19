#include "versions.h"

void
moqr_cli_apply_versions(const moqr_cli_config_t *cfg,
                        moq_msquic_managed_cfg_t *tcfg)
{
    moqr_cli_version_plan_t plan;
    moqr_cli_version_plan(cfg, &plan);
    if (plan.count > 1) {
        tcfg->version = 0;
        tcfg->versions = plan.list;
        tcfg->version_count = plan.count;
    } else {
        tcfg->version = plan.exact;
        tcfg->versions = NULL;
        tcfg->version_count = 0;
    }
}
