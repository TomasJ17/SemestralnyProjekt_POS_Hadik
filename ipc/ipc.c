#include "ipc.h"

#include <asm-generic/errno-base.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

int ipc_create_fifos(void){
  //zmazanie starych
  unlink(FIFO_C2S);
  unlink(FIFO_S2C);

  if(mkfifo(FIFO_C2S, 0666) < 0){
    perror("mkfifo c2s");
    return -1;
  }
  if(mkfifo(FIFO_S2C, 0666) < 0){
    perror("mkfifo s2c");
    unlink(FIFO_C2S);
    return -1;
  }
  return 0;
}

void ipc_cleanup_fifos(void){
  unlink(FIFO_C2S);
  unlink(FIFO_S2C);
}

int ipc_open_c2s_read(void){
  return open(FIFO_C2S, O_RDONLY);
}
int ipc_open_c2s_write(void){
  return open(FIFO_C2S, O_WRONLY);
}
int ipc_open_s2c_read(void){
  return open(FIFO_S2C, O_RDONLY);
}
int ipc_open_s2c_write(void){
  return open(FIFO_S2C, O_WRONLY);
}

ssize_t ipc_write_line(int fd, const char *line){
  //posiela sa len riadok ukonceny \n
  size_t len = strlen(line);
  if(write(fd, line, len) != (ssize_t)len) return -1;
  if(write(fd, "\n", 1) != 1) return -1;
  return (ssize_t)(len + 1);
}

ssize_t ipc_read_line(int fd, char *buf, size_t cap){
  if(cap == 0) return -1;
  size_t i = 0;

  while(i + 1 < cap){
    char c;
    ssize_t r = read(fd, &c, 1);
    if(r == 0){ //EOF
      buf[i] = '\0';
      return 0;
    }
    if (r < 0){
      if(errno == EINTR) continue;
      return -1;
    }
    if (c == '\n') break;
    buf[i++] = c;
  }
  buf[i] = '\0';
  return (ssize_t)i;
}
