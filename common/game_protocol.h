#ifndef GAME_PROTOCOL_H
#define GAME_PROTOCOL_H

#include <stddef.h>
#include <string.h>
#define MSG_READY "READY"
#define MSG_START "START"
#define MSG_QUIT  "QUIT"
#define MSG_BYE   "BYE"
#define MSG_INPUT "INPUT"
#define MSG_STATE "STATE"
#define MSG_DISCONNECT "DISCONNECT"
#define MSG_PAUSE "PAUSE"
#define MSG_GAME_OVER "GAME_OVER"


//vseobecna kontrola
static inline int starts_with(const char *s, const char *prefix){
  if(!s || !prefix) return 0;
  size_t n = strlen(prefix);
  return strncmp(s, prefix, n) == 0 && (s[n] == '\n' || s[n] == ' ');
}

// wasd INPUT, inline vlozi sa ako kod a nevola sa
// rychla kontrola ci je sprava input, resp zacina na I
static inline int is_input_line(const char *line){
  return line && starts_with(line, MSG_INPUT);
}


#endif /* ifndef  GAME_PROTOCOL_H*/
