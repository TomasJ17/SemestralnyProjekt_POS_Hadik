/*
 * game_protocol.h
 * ---------------
 *
 * Game protocol constants used in FIFO IPC messages between client and server.
 *
 * Design:
 *  - The protocol is line-based: each message is single '\n' terminated line
 *  - Lines are read using ipc_read_line() and then matched/parsing is done by prefix
 *  
 *  Typical lifecycle (simplified):
 *    1)  server -> client : READY
 *    2)  client -> server : START ... (new game) or JOIN (attach to existing game)
 *    3)  client -> server : INPUT w/a/s/d, PAUSE(p), DISCONNECT(x), QUIT(q)
 *    4)  server -> client : STATE (periodic snapshots)
 *    5)  server -> client : BYE   (server shutting down / game ended)
 *  
 *  Note:
 *    - This header intentionally contains only string constants and small inline helpers
 *    - The state payload format is produced by server/game_state.c (game_make_state_line) and parsed on the client side
 * */

#ifndef GAME_PROTOCOL_H
#define GAME_PROTOCOL_H

#include <stddef.h>
#include <string.h>

/* Handshake / session control*/
#define MSG_READY "READY" // server is ready and waitin for START/JOIN
#define MSG_START "START" // client requests new game with parameters
#define MSG_JOIN  "JOIN"  // client requests to join an existing running game
#define MSG_BYE   "BYE"   // server is terminating / game ended

/* Client commands during game*/
#define MSG_INPUT "INPUT" // movement input: INPUT w/a/s/d
#define MSG_QUIT  "QUIT"  // terminate game and stop the server
#define MSG_DISCONNECT "DISCONNECT" // client leaves, server keeps running for possible re-join
#define MSG_PAUSE "PAUSE" // toggle pause/resume (server adds resume delay)

/* Server -> client periodic snapshot */
#define MSG_STATE "STATE" // serialized snapshot: status/alive/score/time/w/h/food/snake...

/*Help function which checks prefix match and allows either end-of-line or a space after prefix 
 * This prevents false positives (INPUTX != INPUT)*/
static inline int starts_with(const char *s, const char *prefix){
  if(!s || !prefix) return 0;
  size_t n = strlen(prefix);
  return strncmp(s, prefix, n) == 0 && (s[n] == '\n' || s[n] == ' ');
}
/* Convenience helper for validating movement input lines */
static inline int is_input_line(const char *line){
  return line && starts_with(line, MSG_INPUT);
}

#endif /* ifndef  GAME_PROTOCOL_H*/
