/* sp_execdepth N — self-exec N times (static->static depth cell). */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv){
    if (argc < 2) return 1;
    int n = atoi(argv[1]);
    if (n <= 0) return 42;
    char nb[16]; snprintf(nb, sizeof nb, "%d", n - 1);
    execv("/proc/self/exe", (char*[]){"/proc/self/exe", nb, NULL});
    return 1;
}
