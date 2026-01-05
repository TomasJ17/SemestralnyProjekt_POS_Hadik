/*
 * Shared game state used by server and client
 * Server periodically sends a serialized STATE line; client parses it into this struct 
 * */

#ifndef GAME_STATE_H
#define GAME_STATE_H

#include <stddef.h>
#include <stdint.h>

typedef enum{
  MODE_STANDARD = 0, //game ends after collision
  MODE_TIME = 1 //game ends after limited time
}game_mode_t;

typedef enum{
  WORLD_WRAP = 0, //wrap borders(continue through edge and spawns on other side of map)
  WORLD_WALLS = 1 //with obstacles in map and cannot wrap boorders, world defined by map1 in folder maps
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
#define MAP_W 50
#define MAP_H 30

typedef struct{
  int width;
  int height;
  game_mode_t mode;
  world_type_t world;
  //Obstacle grid for WORLD_WALLS indexing is [y][x]
  uint8_t obstacles[MAP_H][MAP_W]; //0/1
  int obstacles_count;
  game_status_t status;
  int alive;
  int score;
  direction_t direction;
  size_t snake_len;
  point_t snake[MAX_SNAKE_LEN];
  point_t food;

  //timing
  int64_t start_ms; //timestamp of start game
  int32_t time_limit_ms; //limit of game, max length in ms
  int32_t elapsed_ms; //how much time elapsed in ms
} game_state_t;
//init of game
void game_init(game_state_t *g, int width, int height, game_mode_t mode, world_type_t world, int time_limit_ms);
//change and set direction from parameter
void game_set_direction(game_state_t *g, direction_t direction);
//represents 1 game tick: movement, collisions, food, time
//dt_ms delta time in ms between ticks
//time limit handling (MODE_TIME)
//determines one game tick when actions happen
void game_tick(game_state_t *g, int dt_ms, int move);

//for oneline output about game state for client-server info
void game_make_state_line(const game_state_t *g, char *out, size_t cap);

#endif
