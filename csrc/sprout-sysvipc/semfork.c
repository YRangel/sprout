#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <unistd.h>

union semun { int val; struct semid_ds *buf; unsigned short *array; };

/* task #100: two independent PROCESSES sharing one SysV semaphore across a
 * fork — child holds P(-1) until parent raises V(+1). Across the boundary:
 * no shared address space, only the file-backed map. */
int main(void) {
    int id = semget((key_t)0x70686F6E /*"phon"*/, 1, IPC_CREAT | 0666);
    if (id < 0) { perror("semget"); return 10; }
    union semun u = { .val = 0 };
    if (semctl(id, 0, SETVAL, u) != 0) { perror("semctl-SETVAL"); return 11; }

    pid_t c = fork();
    if (c < 0) { perror("fork"); return 12; }
    if (c == 0) {
        printf("[child] waiting P(-1)...\n"); fflush(stdout);
        struct sembuf p = { .sem_num = 0, .sem_op = -1, .sem_flg = 0 };
        if (semop(id, &p, 1) != 0) { perror("[child] semop"); _exit(13); }
        printf("[child] P acquired\n"); fflush(stdout);
        _exit(0);
    }
    usleep(700000); /* stagger: child blocked first */
    printf("[parent] V(+1)\n"); fflush(stdout);
    struct sembuf v = { .sem_num = 0, .sem_op = +1, .sem_flg = 0 };
    if (semop(id, &v, 1) != 0) { perror("[parent] semop"); return 14; }
    int st = 0;
    waitpid(c, &st, 0);
    printf("[parent] child status=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    if (semctl(id, 0, IPC_RMID, u) != 0) { perror("rmid"); return 15; }
    return 0;
}
