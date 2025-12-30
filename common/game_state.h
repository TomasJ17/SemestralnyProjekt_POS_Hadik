#ifndef GAME_STATE_H
#define GAME_STATE_H

#include <stddef.h>
#include <stdint.h>

typedef enum{
  MODE_STANDARD = 0, //po 10 s bez hraca
  MODE_TIME = 1 //po vyprsani casu
}game_mode_t;

typedef enum{
  WORLD_WRAP = 0, //wrap okraje(prejde a ocitne s na druhej strane)
  WORLD_WALLS = 1 //s prekazkami plus okraje su steny
} world_type_t;

typedef enum{
  GAME_RUNNING = 0,
  GAME_OVER = 1
} game_status_t;

typedef enum{
  DIR_UP,
  DIR_DOWN,
  DIR_LEFT,
  DIR_RIGHT
}direction_t;

typedef struct{
  int x;
  int y;
}point_t;

#define MAX_SNAKE_LEN 1024

typedef struct{
  int width;
  int height;
  game_mode_t mode;
  world_type_t world;
  game_status_t status;
  int alive;
  int score;
  direction_t direction;
  size_t snake_len;
  point_t snake[MAX_SNAKE_LEN];
  point_t food;

  //casovanie
  int64_t start_ms; //timestamp startu
  int32_t time_limit_ms; //limit hry, max dlzka v ms
  int32_t elapsed_ms; //kolko casu preslo
} game_state_t;
//init hry
void game_init(game_state_t *g, int width, int height, game_mode_t mode, world_type_t world, int time_limit_ms);
//zmena smeru, nastavenie smeru
void game_set_direction(game_state_t *g, direction_t direction);
//reprezentuje 1 herny tik: posun, kolizie, jedlo, cas
//dt_ms delta time v ms cas medzi jednotlivymi tikmi
void game_tick(game_state_t *g, int dt_ms);

//pre vystup v jednom riadku
void game_make_state_line(const game_state_t *g, char *out, size_t cap);

#endif
