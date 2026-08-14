#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/sem.h>
#include <unistd.h>

union semun { int val; struct semid_ds *buf; unsigned short *array; };

/* task #100 cross-ABI: two ARCHITECTURES mixing on one SysV semaphore —
 * argv[1] = "waiter" (i386 runs this and blocks in P), argv[1] = "poster"
 * (x86_64 runs this later and raises V). Different ELFs, different shim
 * DSOs, same backing file → steam's amd64-supervisor × i386-client
 * pairing. */
int main(int argc, char **argv) {
    key_t k = (key_t)0xAB1E; /* "abi×e" */
    int id = semget(k, 1, IPC_CREAT | 0666);
    if (id < 0) { perror("semget"); return 10; }

    if (argc < 2 || strcmp(argv[1], "waiter") == 0) {
        union semun u = { .val = 0 };
        if (semctl(id, 0, SETVAL, u) != 0) { perror("semctl-SETVAL"); return 11; }
        printf("[waiter] P(-1) ...\n"); fflush(stdout);
        struct sembuf p = { .sem_num = 0, .sem_op = -1, .sem_flg = 0 };
        if (semop(id, &p, 1) != 0) { perror("[waiter] semop"); return 12; }
        printf("[waiter] P acquired\n"); fflush(stdout);
        return 0;
    }

    printf("[poster] V(+1)\n"); fflush(stdout);
    struct sembuf v = { .sem_num = 0, .sem_op = +1, .sem_flg = 0 };
    if (semop(id, &v, 1) != 0) { perror("[poster] semop"); return 13; }
    if (semctl(id, 0, IPC_RMID) != 0) { perror("rmid"); return 14; }
    return 0;
}
