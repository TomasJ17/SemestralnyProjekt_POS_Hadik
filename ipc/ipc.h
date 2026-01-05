#ifndef IPC_H
#define IPC_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

/*
 * ipc.h
 * ------
 * Minimal IPC helper for a single-client Snake game using POSIX named FIFOs
 *
 * Responsibilities:
 *  - Defines FIFO paths for duplex communication (client -> server, server -> client)
 *  - Providing wrappers for creating/cleaning FIFOs and opening endpoints
 *  - Provide line-oriented I/O helpers (write_line / read_line)
 *
 * Notes:
 *  - The protocol itself (message strings, semantics) lives in common/game_protocol
 *  - Non-blocking open variants are used to implement connect/retry loops safely
 *
 * AI-assisted consultation:
 *  - Connection lifecycle and FIFO open/close corner cases were consulted with AI 
 *    during debugging (design remained project-owned and integrated by the author)
 * */

/*
 *  Named FIFOs used for a simple duplex client-server protocol:
 * */
#define FIFO_C2S "/tmp/snake_c2s.fifo" //canal for communication client server, client writing server reading
#define FIFO_S2C "/tmp/snake_s2c.fifo" //canal for communication server client server writing client reading

int ipc_create_fifos(void);
void ipc_cleanup_fifos(void);

//blocking open variants
int ipc_open_c2s_read(void);
int ipc_open_c2s_write(void);
int ipc_open_s2c_read(void);
int ipc_open_s2c_write(void);

//non-blocking open variants for connect/retry loop
int ipc_open_c2s_read_nb(void);
int ipc_open_c2s_write_nb(void);
int ipc_open_s2c_read_nb(void);
int ipc_open_s2c_write_nb(void);
//helper function, toggle O_NONBLOCK on an already opened file descriptor
int ipc_set_blocking(int fd, int blocking);

/*
 *  Send a single text line terminated by '\n'
 *  Returns bytes written, or -1 on error
 * */
ssize_t ipc_write_line(int fd, const char *line);
/*
 *  Read a single text line (up to '\n' or buffer capacity)
 *  Returns:
 *    >0  number of bytes stored (without '\0')
 *    0   EOF (peer closed)
 *    -1  error
 * */
ssize_t ipc_read_line(int fd, char *buf, size_t cap);

#endif
