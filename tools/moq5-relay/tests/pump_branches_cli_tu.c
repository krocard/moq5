/* The production CLI translation unit, compiled into the branch test with its
 * entry point renamed away — the test provides main. An #include of the .c is
 * what keeps this target-scoped: nothing about the real moq5-relay binaries
 * changes, and the pumps under test are the exact production objects. */
#define main moqr_cli_disabled_main
#include "../cli/main.c"
#undef main
