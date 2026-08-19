#ifndef MOQR_CLI_CLIARGS_H
#define MOQR_CLI_CLIARGS_H

/*
 * The moq5-relay command line.
 *
 * Parsing is a pure function of argv: it opens nothing, reads no environment
 * and allocates nothing, so an informational or malformed invocation can be
 * answered before the relay touches a configuration file, the verify-seam
 * environment, or the transport. That ordering is the point — `--help` must
 * work on a host where the configuration is missing and the environment is
 * junk.
 *
 * Written against plain C rather than getopt_long: the parser is exercised
 * directly by tests, and platform getopt carries global state, permutes argv,
 * and prints to stderr on its own.
 */

#include <stdbool.h>
#include <stdio.h>

typedef enum moqr_cli_cmd {
    MOQR_CLI_CMD_INVALID = 0,
    MOQR_CLI_CMD_SERVE,
    MOQR_CLI_CMD_CAPACITY,
    MOQR_CLI_CMD_HELP,           /* global help */
    MOQR_CLI_CMD_HELP_SERVE,
    MOQR_CLI_CMD_HELP_CAPACITY,
    MOQR_CLI_CMD_VERSION,
} moqr_cli_cmd_t;

typedef struct moqr_cli_args {
    moqr_cli_cmd_t cmd;
    /* BORROWED from argv; NULL unless cmd is SERVE or CAPACITY. */
    const char    *config;
    /* On MOQR_CLI_CMD_INVALID: a one-line reason, never NULL. */
    const char    *error;
} moqr_cli_args_t;

/* true when argv named a command this relay can carry out or answer. On false,
 * out->cmd is MOQR_CLI_CMD_INVALID and out->error says why. */
bool moqr_cli_args_parse(int argc, char **argv, moqr_cli_args_t *out);

/* Is this a command answered by printing, with no relay state at all? */
bool moqr_cli_cmd_is_informational(moqr_cli_cmd_t cmd);

/* Human-facing product name; the executable stays `moq5-relay`. */
#define MOQR_CLI_PRODUCT "MOQ5 Relay"

void moqr_cli_print_help(FILE *f, moqr_cli_cmd_t which);
void moqr_cli_print_version(FILE *f);
/* The conventional pointer to --help, after a refusal. */
void moqr_cli_print_try_help(FILE *f);

#endif /* MOQR_CLI_CLIARGS_H */
