#include <stdio.h>
#include <unistd.h>

int main(void) {
    printf("CLIENT started\n");
    printf("CLIENT PID: %d\n", getpid());
    return 0;
}
