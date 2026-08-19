#include "cliargs.h"

#include <moq/version.h>

#include <string.h>

static bool
arg_is(const char *a, const char *s, const char *l)
{
    return (s != NULL && strcmp(a, s) == 0) || strcmp(a, l) == 0;
}

static bool
refuse(moqr_cli_args_t *out, const char *why)
{
    out->cmd = MOQR_CLI_CMD_INVALID;
    out->config = NULL;
    out->error = why;
    return false;
}

bool
moqr_cli_cmd_is_informational(moqr_cli_cmd_t cmd)
{
    return cmd == MOQR_CLI_CMD_HELP || cmd == MOQR_CLI_CMD_HELP_SERVE ||
           cmd == MOQR_CLI_CMD_HELP_CAPACITY || cmd == MOQR_CLI_CMD_VERSION;
}

/* Options accepted after `serve` / `capacity`. Returns false having set the
 * refusal reason. A repeated --config is refused rather than resolved by
 * last-one-wins: two configurations named on one line is a mistake, and
 * quietly serving one of them is the worst available answer. */
static bool
parse_command_options(int argc, char **argv, int i, moqr_cli_cmd_t scoped_help,
                      moqr_cli_args_t *out)
{
    for (; i < argc; i++) {
        const char *a = argv[i];

        if (arg_is(a, "-h", "--help")) {
            out->cmd = scoped_help;   /* help wins wherever it appears */
            out->config = NULL;
            out->error = NULL;
            return true;
        }
        if (arg_is(a, "-c", "--config")) {
            if (i + 1 >= argc) {
                return refuse(out, "option '--config' needs a file");
            }
            if (out->config != NULL) {
                return refuse(out, "option '--config' given more than once");
            }
            out->config = argv[++i];
            continue;
        }
        if (strncmp(a, "--config=", 9) == 0) {
            if (a[9] == '\0') {
                return refuse(out, "option '--config' needs a file");
            }
            if (out->config != NULL) {
                return refuse(out, "option '--config' given more than once");
            }
            out->config = a + 9;
            continue;
        }
        if (a[0] == '-') {
            return refuse(out, "unknown option");
        }
        return refuse(out, "unexpected argument");
    }
    if (out->config == NULL) {
        return refuse(out, "this command needs --config <FILE>");
    }
    return true;
}

bool
moqr_cli_args_parse(int argc, char **argv, moqr_cli_args_t *out)
{
    if (out == NULL) {
        return false;
    }
    out->cmd = MOQR_CLI_CMD_INVALID;
    out->config = NULL;
    out->error = NULL;
    if (argc < 2 || argv == NULL || argv[1] == NULL) {
        return refuse(out, "no command given");
    }

    const char *cmd = argv[1];

    if (arg_is(cmd, "-h", "--help") || strcmp(cmd, "help") == 0) {
        if (argc == 2) {
            out->cmd = MOQR_CLI_CMD_HELP;
            return true;
        }
        if (argc > 3) {
            return refuse(out, "unexpected argument");
        }
        if (strcmp(argv[2], "serve") == 0) {
            out->cmd = MOQR_CLI_CMD_HELP_SERVE;
            return true;
        }
        if (strcmp(argv[2], "capacity") == 0) {
            out->cmd = MOQR_CLI_CMD_HELP_CAPACITY;
            return true;
        }
        return refuse(out, "unknown help topic");
    }
    if (arg_is(cmd, "-V", "--version")) {
        if (argc > 2) {
            return refuse(out, "unexpected argument");
        }
        out->cmd = MOQR_CLI_CMD_VERSION;
        return true;
    }
    if (strcmp(cmd, "serve") == 0) {
        out->cmd = MOQR_CLI_CMD_SERVE;
        return parse_command_options(argc, argv, 2, MOQR_CLI_CMD_HELP_SERVE,
                                     out);
    }
    if (strcmp(cmd, "capacity") == 0) {
        out->cmd = MOQR_CLI_CMD_CAPACITY;
        return parse_command_options(argc, argv, 2, MOQR_CLI_CMD_HELP_CAPACITY,
                                     out);
    }
    if (cmd[0] == '-') {
        return refuse(out, "unknown option");
    }
    return refuse(out, "unknown command");
}

void
moqr_cli_print_version(FILE *f)
{
    /* One authority for the number: LibMoQ's. */
    fprintf(f, "%s %s\n", MOQR_CLI_PRODUCT, moq_version_string());
}

void
moqr_cli_print_try_help(FILE *f)
{
    fprintf(f, "Try 'moq5-relay --help' for more information.\n");
}

static void
print_command_help(FILE *f, const char *name, const char *summary,
                   const char *example)
{
    fprintf(f,
            "%s - %s\n"
            "\n"
            "Usage:\n"
            "  moq5-relay %s [OPTIONS]\n"
            "\n"
            "Options:\n"
            "  -c, --config <FILE>    Relay configuration (JSON)\n"
            "  -h, --help             Print help\n"
            "\n"
            "Example:\n"
            "  %s\n",
            MOQR_CLI_PRODUCT, summary, name, example);
}

void
moqr_cli_print_help(FILE *f, moqr_cli_cmd_t which)
{
    switch (which) {
    case MOQR_CLI_CMD_HELP_SERVE:
        print_command_help(f, "serve", "run the relay",
                           "moq5-relay serve --config relay.json");
        return;
    case MOQR_CLI_CMD_HELP_CAPACITY:
        print_command_help(f, "capacity",
                           "validate configuration and print the capacity "
                           "ceiling",
                           "moq5-relay capacity --config relay.json");
        return;
    default:
        break;
    }
    fprintf(f,
            "%s - deterministic Media over QUIC relay\n"
            "\n"
            "Usage:\n"
            "  moq5-relay <COMMAND> [OPTIONS]\n"
            "\n"
            "Commands:\n"
            "  serve       Run the relay\n"
            "  capacity    Validate configuration and print the capacity "
            "ceiling\n"
            "  help        Print help for a command\n"
            "\n"
            "Options:\n"
            "  -h, --help       Print help\n"
            "  -V, --version    Print version\n",
            MOQR_CLI_PRODUCT);
}
