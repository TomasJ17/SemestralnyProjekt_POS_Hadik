#ifndef IPC_H
#define IPC_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#define FIFO_C2S "/tmp/snake_c2s.fifo" //canal for communication client server, client writing server reading
#define FIFO_S2C "/tmp/snake_s2c.fifo" //canal for communication server client server writing client reading

int ipc_create_fifos(void);
void ipc_cleanup_fifos(void);

int ipc_open_c2s_read(void);
int ipc_open_c2s_write(void);
int ipc_open_s2c_read(void);
int ipc_open_s2c_write(void);

ssize_t ipc_write_line(int fd, const char *line);
ssize_t ipc_read_line(int fd, char *buf, size_t cap);

#endif
