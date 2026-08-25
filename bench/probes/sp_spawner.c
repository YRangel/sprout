/* sp_spawner <payload> — fork+exec the payload 50 times (spawn-cost cell). */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
extern char **environ;
int main(int argc, char **argv){
    if (argc < 2) return 1;
    for (int i = 0; i < 50; i++) {
        pid_t p = fork();
        if (p < 0) return 1;
        if (p == 0) { execve(argv[1], (char*[]){argv[1], NULL}, environ); _exit(127); }
        int st = 0; waitpid(p, &st, 0);
        if (!WIFEXITED(st)) return 1;
    }
    return 42;
}
