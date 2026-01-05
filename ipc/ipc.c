#include "ipc.h"

#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
/*
 * ipc.c
 * ------
 * Implementatio od FIFO creation/open helpers and line-oriented I/O
 *
 * Key design choices:
 *  - Use simple newline-terminated text messages to keep the protocol debuggable
 *  - Provide both blocking and non-blocking open calls:
 *    * non-blocking for connect/retry logic (avoid deadlocks on open())
 *    * switch to blocking for steady-state read_line() usage
 *
 * AI-assisted consultation:
 *  - Handling of FIFO open semantics (ENXIO/ENONENT), connect retry loops and blocking toggles with AI during troubleshooting 
 *
 * */
int ipc_create_fifos(void){
  // Clearing FIFOs from previous run to ensure they do not break new sessions
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
  //Best-effort cleanup, ignore errors if FIFOs are already gone
  unlink(FIFO_C2S);
  unlink(FIFO_S2C);
}
// Blocking open wrappers
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
/* Non-blocking open wrappers, used mainly by the client to implement
 * connect/retry logic without blocking on open()
 * */
int ipc_open_c2s_read_nb(void){
  return open(FIFO_C2S, O_RDONLY | O_NONBLOCK);
}
int ipc_open_c2s_write_nb(void){
  return open(FIFO_C2S, O_WRONLY | O_NONBLOCK);
}
int ipc_open_s2c_read_nb(void){
  return open(FIFO_S2C, O_RDONLY | O_NONBLOCK);
}
int ipc_open_s2c_write_nb(void){
  return open(FIFO_S2C, O_WRONLY | O_NONBLOCK);
}
// Toggle O_NONBLOCK for steady-state I/O vs. connection probing
int ipc_set_blocking(int fd, int blocking){
  int flags = fcntl(fd, F_GETFL);
  if(flags < 0) return -1;
  if(blocking) flags &= ~O_NONBLOCK;
  else         flags |=  O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags);
}
/*
 * Write the whole buffer (retry on EINTR)
 * This is important for FIFO writes to avoid partial message sends
 * */
static int write_all(int fd, const void *buf, size_t len){
  const char *p = (const char*)buf;
  while(len > 0){
    ssize_t w = write(fd, p, len);
    if(w < 0){
      if(errno == EINTR) continue;
      return -1;
    }
    p += (size_t)w;
    len -= (size_t)w;
  }
  return 0;
}

ssize_t ipc_write_line(int fd, const char *line){
  // Protocol is line based, so line is sent with '\n' terminator
  size_t len = strlen(line);
  if(write_all(fd, line, len) < 0) return -1;
  if(write_all(fd, "\n", 1) < 0) return -1;
  return (ssize_t)(len + 1);
}

ssize_t ipc_read_line(int fd, char *buf, size_t cap){
  /*
   * Reads one byte at time until '\n' or capacity is reached
   * This keeps parsing simple and avoids message framing issues
   *
   * Return value:
   *  - 0 indicates EOF (peer closed its end of the FIFO)
   *  - -1 indicates an error (besides EINTR, that is retried)
   *  - > 0 success, return number of characters saved into buf
   *
   *  EINTR - retries to read if was interrupted during reading
   * */
  if(cap == 0) return -1;
  size_t i = 0;

  while(i + 1 < cap){
    char c;
    ssize_t r = read(fd, &c, 1);
    if(r == 0){ //EOF - peer closed FIFO
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
