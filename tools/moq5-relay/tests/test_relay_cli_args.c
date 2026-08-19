/*
 * The moq5-relay argument parser, driven directly.
 *
 * These pin the shape of the command line as a pure function of argv: which
 * spellings mean the same thing, which are refused, and — the property the
 * whole design rests on — that nothing here reads a file or the environment.
 */

#include "../cli/cliargs.h"

#include "../../../tests/unit/test_support.h"

#include <moq/version.h>

#include <stdarg.h>
#include <string.h>

static moqr_cli_args_t
parse(int argc, const char **argv, bool *ok)
{
    moqr_cli_args_t a;

    *ok = moqr_cli_args_parse(argc, (char **)argv, &a);
    return a;
}

static int
check_bad(int *failures, const char *label, ...)
{
    const char *av[16];
    int n = 0;
    va_list ap;
    bool ok = false;

    av[n++] = "moq5-relay";
    va_start(ap, label);
    for (;;) {
        const char *t = va_arg(ap, const char *);

        if (t == NULL || n >= (int)(sizeof(av) / sizeof(av[0]))) {
            break;
        }
        av[n++] = t;
    }
    va_end(ap);

    moqr_cli_args_t a = parse(n, av, &ok);

    if (ok || a.cmd != MOQR_CLI_CMD_INVALID || a.error == NULL ||
        a.config != NULL) {
        printf("FAIL: %s: not refused cleanly\n", label);
        (*failures)++;
    }
    return 0;
}

#define BAD1(...) check_bad(&failures, #__VA_ARGS__, __VA_ARGS__, NULL)

/* Parse a command line written as a NULL-terminated list of tokens after the
 * program name. Statement-expression macros would be a GNU extension, and this
 * file compiles with -Wpedantic. */
static moqr_cli_args_t
parse_ok(bool *ok, ...)
{
    const char *av[16];
    int n = 0;
    va_list ap;

    av[n++] = "moq5-relay";
    va_start(ap, ok);
    for (;;) {
        const char *t = va_arg(ap, const char *);

        if (t == NULL || n >= (int)(sizeof(av) / sizeof(av[0]))) {
            break;
        }
        av[n++] = t;
    }
    va_end(ap);
    return parse(n, av, ok);
}

#define OK1(...)  parse_ok(&ok, __VA_ARGS__, NULL)

static int
test_help_and_version_spellings(void)
{
    int failures = 0;
    bool ok = false;

    MOQ_TEST_CHECK(OK1("-h").cmd == MOQR_CLI_CMD_HELP);
    MOQ_TEST_CHECK(OK1("--help").cmd == MOQR_CLI_CMD_HELP);
    MOQ_TEST_CHECK(OK1("help").cmd == MOQR_CLI_CMD_HELP);
    MOQ_TEST_CHECK(OK1("help", "serve").cmd == MOQR_CLI_CMD_HELP_SERVE);
    MOQ_TEST_CHECK(OK1("help", "capacity").cmd == MOQR_CLI_CMD_HELP_CAPACITY);
    MOQ_TEST_CHECK(OK1("serve", "-h").cmd == MOQR_CLI_CMD_HELP_SERVE);
    MOQ_TEST_CHECK(OK1("serve", "--help").cmd == MOQR_CLI_CMD_HELP_SERVE);
    MOQ_TEST_CHECK(OK1("capacity", "-h").cmd == MOQR_CLI_CMD_HELP_CAPACITY);
    MOQ_TEST_CHECK(OK1("capacity", "--help").cmd == MOQR_CLI_CMD_HELP_CAPACITY);
    MOQ_TEST_CHECK(OK1("-V").cmd == MOQR_CLI_CMD_VERSION);
    MOQ_TEST_CHECK(OK1("--version").cmd == MOQR_CLI_CMD_VERSION);

    /* Every one of them is answerable without touching the relay. */
    MOQ_TEST_CHECK(moqr_cli_cmd_is_informational(MOQR_CLI_CMD_HELP));
    MOQ_TEST_CHECK(moqr_cli_cmd_is_informational(MOQR_CLI_CMD_HELP_SERVE));
    MOQ_TEST_CHECK(moqr_cli_cmd_is_informational(MOQR_CLI_CMD_HELP_CAPACITY));
    MOQ_TEST_CHECK(moqr_cli_cmd_is_informational(MOQR_CLI_CMD_VERSION));
    MOQ_TEST_CHECK(!moqr_cli_cmd_is_informational(MOQR_CLI_CMD_SERVE));
    MOQ_TEST_CHECK(!moqr_cli_cmd_is_informational(MOQR_CLI_CMD_CAPACITY));
    MOQ_TEST_PASS("cli_help_and_version_spellings");
    return failures;
}

/* The three config spellings are the same instruction. */
static int
test_config_spellings_agree(void)
{
    int failures = 0;
    bool ok = false;
    moqr_cli_args_t a = OK1("serve", "-c", "relay.json");
    moqr_cli_args_t b = OK1("serve", "--config", "relay.json");
    moqr_cli_args_t c = OK1("serve", "--config=relay.json");

    MOQ_TEST_CHECK(a.cmd == MOQR_CLI_CMD_SERVE);
    MOQ_TEST_CHECK(b.cmd == MOQR_CLI_CMD_SERVE);
    MOQ_TEST_CHECK(c.cmd == MOQR_CLI_CMD_SERVE);
    MOQ_TEST_CHECK(strcmp(a.config, "relay.json") == 0);
    MOQ_TEST_CHECK(strcmp(b.config, "relay.json") == 0);
    MOQ_TEST_CHECK(strcmp(c.config, "relay.json") == 0);

    moqr_cli_args_t d = OK1("capacity", "--config=relay.json");

    MOQ_TEST_CHECK(d.cmd == MOQR_CLI_CMD_CAPACITY);
    MOQ_TEST_CHECK(strcmp(d.config, "relay.json") == 0);
    MOQ_TEST_PASS("cli_config_spellings_agree");
    return failures;
}

/* Help asked for alongside a configuration still means help: it is answered
 * from argv, so the named file is never opened. */
static int
test_help_wins_over_config(void)
{
    int failures = 0;
    bool ok = false;
    moqr_cli_args_t a = OK1("serve", "--config", "/nonexistent/relay.json",
                           "--help");

    MOQ_TEST_CHECK(a.cmd == MOQR_CLI_CMD_HELP_SERVE);
    MOQ_TEST_CHECK(a.config == NULL);
    MOQ_TEST_PASS("cli_help_wins_over_config");
    return failures;
}

static int
test_malformed_is_refused(void)
{
    int failures = 0;

    BAD1("bogus");                              /* unknown command */
    BAD1("serve");                              /* missing config */
    BAD1("capacity");                           /* missing config */
    BAD1("serve", "--config");                  /* missing value */
    BAD1("serve", "-c");                        /* missing value */
    BAD1("serve", "--config=");                 /* empty value */
    BAD1("serve", "--nope", "x");               /* unknown option */
    /* Otherwise well-formed: only the unknown option is wrong, so nothing
     * else can account for the refusal. */
    BAD1("serve", "--config", "a", "--nope");
    BAD1("--nope");                             /* unknown global option */
    BAD1("serve", "-c", "a", "--config", "b");  /* duplicate */
    BAD1("serve", "--config", "a", "extra");    /* trailing token */
    BAD1("help", "bogus");                      /* unknown topic */
    BAD1("--version", "extra");                 /* trailing token */

    /* No command at all. */
    const char *av[] = { "moq5-relay" };
    bool ok = false;
    moqr_cli_args_t a = parse(1, av, &ok);

    MOQ_TEST_CHECK(!ok);
    MOQ_TEST_CHECK(a.error != NULL);
    MOQ_TEST_PASS("cli_malformed_is_refused");
    return failures;
}

/* The product name is human-facing; the executable and the structured rows
 * are not renamed with it. */
static int
test_product_identity(void)
{
    int failures = 0;

    MOQ_TEST_CHECK(strcmp(MOQR_CLI_PRODUCT, "MOQ5 Relay") == 0);

    /* One version authority: the rendered line is the product name plus
     * exactly what LibMoQ reports. A second, hand-written number here would
     * drift the moment the library moves. */
    char rendered[128];
    char expect[128];
    FILE *f = tmpfile();

    MOQ_TEST_CHECK(f != NULL);
    if (f != NULL) {
        size_t n;

        moqr_cli_print_version(f);
        rewind(f);
        n = fread(rendered, 1, sizeof(rendered) - 1, f);
        rendered[n] = '\0';
        fclose(f);
        snprintf(expect, sizeof(expect), "%s %s\n", MOQR_CLI_PRODUCT,
                 moq_version_string());
        MOQ_TEST_CHECK(strcmp(rendered, expect) == 0);
        MOQ_TEST_CHECK(strstr(rendered, MOQ_VERSION_STRING) != NULL);
    }
    MOQ_TEST_PASS("cli_product_identity");
    return failures;
}

int
main(void)
{
    int failures = 0;

    failures += test_help_and_version_spellings();
    failures += test_config_spellings_agree();
    failures += test_help_wins_over_config();
    failures += test_malformed_is_refused();
    failures += test_product_identity();
    if (failures == 0) {
        printf("ALL PASS\n");
    }
    return failures != 0;
}
