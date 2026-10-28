// Host-side test for the persistent-mode crash guard (../src/T2AudioCrashGuard.h).
//
//   cl /nologo /W4 /Fe:tests\T2AudioCrashGuardTest.exe tests\T2AudioCrashGuardTest.c
//   gcc -Wall -Wextra -Werror -o T2AudioCrashGuardTest tests/T2AudioCrashGuardTest.c

#include <stdio.h>
#include "../src/T2AudioCrashGuard.h"

static int g_failures = 0;

static void check(const char *name, int condition)
{
    printf("  [%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        g_failures++;
    }
}

int main(void)
{
    unsigned int s = 99;

    printf("Crash guard\n");
    check("clean previous session: start, counter stays 0",
          T2AudioCrashGuardAllowStart(0, 0, &s) && s == 0);
    check("first unclean end: start again, one strike",
          T2AudioCrashGuardAllowStart(1, 0, &s) && s == 1);
    check("second unclean end in a row: refuse, two strikes",
          !T2AudioCrashGuardAllowStart(1, 1, &s) && s == 2);
    check("after refusal (not armed): still refused until reset",
          !T2AudioCrashGuardAllowStart(0, 2, &s) && s == 2);
    check("reset by the installer: start",
          T2AudioCrashGuardAllowStart(0, 0, &s) && s == 0);
    check("NULL out pointer tolerated", T2AudioCrashGuardAllowStart(0, 0, 0));
    check("counter saturates", (T2AudioCrashGuardAllowStart(1, 0xFFFFu, &s), s == 0xFFFFu));
    if (g_failures != 0) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
