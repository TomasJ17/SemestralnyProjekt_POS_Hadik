#with help of artificial intelligence chatgpt
CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2

CLIENT_BIN = client/client
SERVER_BIN = server/server

CLIENT_SRC = client/main.c ipc/ipc.c
SERVER_SRC = server/main.c ipc/ipc.c
#in case of files named as this system targets
.PHONY: all client server clean
#default targets
all: client server

client: $(CLIENT_BIN)

server: $(SERVER_BIN)

$(CLIENT_BIN): $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(SERVER_BIN): $(SERVER_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(CLIENT_BIN) $(SERVER_BIN)
