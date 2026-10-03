/*
 * test_execle_env.c — runtime regression for finding F1 (execle envp
 * mis-parse past 127 arguments).
 *
 * Run under sprout so the interposed execle() is exercised:
 *
 *   gcc -O2 test_execle_env.c -o <guest>/tmp/te_execle
 *   sprout -r <rootfs> /tmp/te_execle
 *
 * The probe calls execle() with EXECLE_FILL variadic filler args (default
 * F300 — argv well past the interposer's 127-slot storage cap) and an env
 * array holding exactly one marker. With the fix, the child shell runs
 * with SP_EXECLE_MARKER=marker_alive; with the bug the trailing envp read
 * lands on a leftover argv pointer and the marker never reaches the child
 * (or the child env is garbage / exec fails).
 *
 * Override the filler count for bisection:
 *   gcc -DEXECLE_FILL_F30  ... (30 args)
 *   gcc -DEXECLE_FILL_F130 ... (130 args)
 *   default: F300 (300 args)
 *
 * Exit 0 = marker observed; nonzero or missing output = FAIL.
 */
#include <stdio.h>
#include <unistd.h>

#define F10   "f0","f1","f2","f3","f4","f5","f6","f7","f8","f9"
#define F20   F10,F10,F10,F10,F10,F10,F10,F10,F10,F10
#define F200  F20,F20,F20,F20,F20,F20,F20,F20,F20,F20
#define F300  F200,F20,F20,F20,F20,F20

#if defined(EXECLE_FILL_F30)
# define EXECLE_FILL F10,F10,F10
#elif defined(EXECLE_FILL_F130)
# define EXECLE_FILL F20,F20,F20,F20,F20,F20,F10
#else
# define EXECLE_FILL F300
#endif

int main(void) {
    static char *envp[2];
    envp[0] = (char *)"SP_EXECLE_MARKER=marker_alive";
    envp[1] = NULL;

    execle("/bin/sh", "sh", "-c",
           "printenv SP_EXECLE_MARKER; test \"$(printenv SP_EXECLE_MARKER)\" = marker_alive",
           EXECLE_FILL, (char *)NULL, envp);
    perror("execle");
    return 111;
}
