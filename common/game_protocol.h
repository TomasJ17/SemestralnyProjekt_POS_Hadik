#ifndef GAME_PROTOCOL_H
#define GAME_PROTOCOL_H

#define MSG_READY "READY"
#define MSG_QUIT  "QUIT"
#define MSG_INPUT "INPUT"
#define MSG_STATE "STATE"
#define MSG_GAME_OVER "GAME_OVER"

// wasd INPUT, inline vlozi sa ako kod a nevola sa
// rychla kontrola ci je sprava input, resp zacina na I
static inline int is_input_line(const char *line){
  return line && (line[0] == 'I');
}
//vseobecna kontrola
static inline int starts_with(const char *s, const char *prefix){
  while(*prefix){
    if(*s++ != *prefix++) return 0;
  }
  return 1;
}

#endif /* ifndef  GAME_PROTOCOL_H*/
