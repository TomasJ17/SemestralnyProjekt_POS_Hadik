#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>

#include "../ipc/ipc.h"

static void start_server_process(void){
  pid_t pid = fork();
  if( pid < 0){
    perror("fork");
    exit(2);
  }
  if(pid ==  0){
    // potomok spusti server
    execl("./server/server", "./server/server", (char*)NULL);
    perror("execl");
    exit(3);
  }
  //rodic pokracuje ako klient
}

int main(void) {
  printf("CLIENT started\n");
  printf("CLIENT PID: %d\n", getpid());

  printf("Menu:\n1. New game\n2. Exit\n");
  int choice = 0;
  scanf("%d", &choice);
  int c;
  while((c = getchar()) != '\n' && choice != EOF) {}
  if(choice != 1){
    printf("Bye\n");
    return 0;
  }

  start_server_process();

  // klient sa pripoji na FIFO
  int c2s = ipc_open_c2s_write(); //klient pise na server
  if(c2s < 0){
    perror("open c2s write");
    return 4;
  }

  int s2c = ipc_open_s2c_read(); //caka na server read
  if(s2c < 0){
    perror("open s2c read");
    close(c2s);
    return 5;
  }

  //handshake
  char line[256];

  if(ipc_read_line(s2c, line, sizeof(line)) <= 0){
    fprintf(stderr, "Server neposlal READY");
    //zatvaram fifa
    close(c2s);
    close(s2c);
    return 5;
  }

  printf("From server: %s\n", line); //READY

  printf("Controls: w/a/s/d, q = quit\n");

  while(1){
    printf("> ");
    char cmd[32];
    if (!fgets(cmd, sizeof(cmd), stdin)) break;
    cmd[strcspn(cmd, "\n")] = 0;

    if(strcmp(cmd, "q") == 0){
      ipc_write_line(c2s, "QUIT");
      if(ipc_read_line(s2c, line, sizeof(line)) > 0)
        printf("From server: %s\n", line);

      break;
    }else{
      //poslem spravu serveru
      char msg[64];
      snprintf(msg, sizeof(msg), "INPUT %s", cmd);
      ipc_write_line(c2s, msg);

      //precitam odpoved
      if(ipc_read_line(s2c, line, sizeof(line)) > 0){
        printf("From server: %s\n", line);
      } else{
        printf("Server closed connection\n");
        break;
      }
    }
  }

  close(c2s);
  close(s2c);
  //pockame na server
  wait(NULL);

  return 0;
}
