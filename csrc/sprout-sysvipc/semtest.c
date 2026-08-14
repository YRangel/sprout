#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sem.h>

union semun { int val; struct semid_ds *buf; unsigned short *array; };

int main(void) {
    union semun u;

    int id = semget((key_t)0x2ABC1234, 1, IPC_CREAT | 0666);
    if (id < 0) { perror("semget-create"); return 10; }

    u.val = 5;
    if (semctl(id, 0, SETVAL, u) != 0) { perror("semctl-SETVAL"); return 11; }

    printf("GETVAL=%d\n", semctl(id, 0, GETVAL, u));

    struct sembuf p = { .sem_num = 0, .sem_op = -2, .sem_flg = 0 };
    if (semop(id, &p, 1) != 0) { perror("semop-P2"); return 12; }
    printf("after-P=%d\n", semctl(id, 0, GETVAL, u));

    struct sembuf v = { .sem_num = 0, .sem_op = +1, .sem_flg = 0 };
    if (semop(id, &v, 1) != 0) { perror("semop-V1"); return 13; }
    printf("after-V=%d\n", semctl(id, 0, GETVAL, u));

    if (semctl(id, 0, IPC_RMID) != 0) { perror("semctl-RMID"); return 14; }
    printf("RMID-ok\n");

    int id2 = semget(IPC_PRIVATE, 2, 0666);
    if (id2 < 0) { perror("semget-private"); return 15; }
    if (semctl(id2, 0, IPC_RMID) != 0) { perror("semctl-RMID-private"); return 16; }
    printf("private-ok\n");
    return 0;
}
