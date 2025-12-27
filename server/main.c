#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "../ipc/ipc.h"


int main(void) {
    printf("SERVER started\n");
    printf("SERVER PID: %d\n", getpid());

    // server vytvara IPC rozhranie
    if (ipc_create_fifos() != 0) return 1;

    int c2s = ipc_open_c2s_read(); //cakame na klientov write
    if(c2s < 0){
      perror("open c2s read");
      ipc_cleanup_fifos();
      return 2;
    }

    int s2c = ipc_open_s2c_write(); // cakame na klient read
    if(s2c < 0){
      perror("open s2c write");
      close(c2s);
      ipc_cleanup_fifos();
      return 3;
    }

    char line[256];
    int tick = 0;

    //handshake
    ipc_write_line(s2c, "READY");

    while (1) {
      ssize_t n = ipc_read_line(c2s, line, sizeof(line));
      if(n <= 0){
        //koniec klienta alebo EOF
        break;
      }
      if(strcmp(line, "QUIT") == 0){
        ipc_write_line(s2c, "BYE");
        break;
      }
    //tu sa bude neskor spracovavat input
    tick++;
    char out[256];
    snprintf(out, sizeof(out), "STATE tick=%d last='%s'", tick, line);
    ipc_write_line(s2c, out);
    }
    
    close(c2s);
    close(s2c);
    printf("SERVER end\n");
    return 0;
}
