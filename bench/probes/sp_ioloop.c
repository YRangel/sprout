/* sp_ioloop — open+read+close x20k on a fixed tmpfile in the guest. */
#include <fcntl.h>
#include <unistd.h>
int main(void){
    char buf[64];
    int fd = open("/etc/hostname", O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) return 1;
    write(fd, "io\n", 3); close(fd);
    for (int i = 0; i < 20000; i++) {
        fd = open("/etc/hostname", O_RDONLY);
        if (fd < 0) return 1;
        (void)read(fd, buf, sizeof buf);
        close(fd);
    }
    return 42;
}
