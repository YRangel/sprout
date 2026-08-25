/* sp_statloop — newfstatat x20k on an existing path in the guest. */
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
int main(void){
    struct stat st;
    for (int i = 0; i < 20000; i++) {
        if (syscall(SYS_newfstatat, AT_FDCWD, "/etc/hostname", &st, 0) < 0) return 1;
    }
    return 42;
}
