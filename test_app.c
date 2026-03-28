#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

int main() {
    int fd = open("dev/my_usb_device0", O_RDONLY);

    if (!fd) {
        printf("Failed to open device\n");
        return -1;
    }

    struct pollfd pdf  = {
        .fd = fd,
        .events = POLLIN
    };

    printf("Waiting for data\n");

    int ret = poll(&pdf, 1, 5000);

    if (ret == 0) {
        printf("Timeout\n");
    } else if (ret > 0) {
        printf("Data available\n");
    } else {
        perror("poll");
    }

    close(fd);

    return 0;
}