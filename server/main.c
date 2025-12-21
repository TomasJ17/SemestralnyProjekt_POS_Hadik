#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("SERVER started\n");
    printf("SERVER PID: %d\n", getpid());

    while (1) {
        sleep(1);
    }

    return 0;
}
