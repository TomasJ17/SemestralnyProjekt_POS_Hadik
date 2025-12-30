#include "game_state.h"

#include <stddef.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

//casovanie pomocou AI chatgpt

static int64_t now_ms(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int points_equal(point_t a, point_t b){
  return a.x == b.x && a.y == b.y;
}

static int in_bounds(const game_state_t  *g, point_t p){
  return p.x >= 0 && p.x < g->width && p.y >= 0 && p.y < g->height; 
}

static point_t wrap_point(const game_state_t *g, point_t p){
  if(p.x < 0) p.x = g->width - 1;
  else if(p.x >= g->width) p.x = 0;
  if(p.y < 0) p.y = g->height -1;
  else if(p.y >= g->height) p.y = 0;
  return p;
}

static int snake_contains(const game_state_t *g, point_t p, size_t upto_exclusive){
  if(upto_exclusive > g->snake_len) upto_exclusive = g->snake_len;
  for(size_t i = 0; i < upto_exclusive; i++){
    if(points_equal(g->snake[i], p)){
      return 1;
    }
  }
  return 0;
}

static point_t next_head(const game_state_t *g){
  point_t head = g->snake[0];
  switch(g->direction){
    case DIR_UP:
      head.y--;
      break;
    case DIR_DOWN:
      head.y++;
      break;
    case DIR_LEFT:
      head.x--;
      break;
    case DIR_RIGHT:
      head.x++;
      break;
  }
  return head;
}

static void spawn_food(game_state_t *g){
    point_t p;
    //pomocka keby bola mapa plna hada
    int max_tries = g->width * g->height;
    //nahodny spawn na mape ale mimo hada
    do{
      p.x = rand() % g->width;
      p.y = rand() % g->height;
      max_tries--;
    } while(snake_contains(g,p, g->snake_len) && max_tries > 0);
    if(max_tries <= 0){
      g->status = GAME_OVER;
      g->alive = 0;
      return;
    }
    g->food = p;
  }

  void game_init(game_state_t *g, int width, int height, game_mode_t mode, world_type_t world, int time_limit_ms){
    srand(time(NULL));
    //premazanie game_state_t a inicializacia
    memset(g, 0, sizeof(*g));
    g->width = width;
    g->height = height;
    g->mode = mode;
    g->world = world;
    g->status = GAME_RUNNING;
    g->alive = 1;
    g->score = 0;
    g->direction = DIR_RIGHT;
    //pociatocna dlzka hada
    g->snake_len = 3;
    int sx = width/2;
    int sy = height/2;
    g->snake[0] = (point_t){sx,sy};
    g->snake[1] = (point_t){sx-1,sy};
    g->snake[2] = (point_t){sx-2,sy};
    
    //casovanie
    g->start_ms = now_ms();
    g->time_limit_ms = (mode == MODE_TIME) ? time_limit_ms : 0;
    g->elapsed_ms = 0;

    spawn_food(g); 
  }

  static int is_opposite(direction_t a, direction_t b){
    return (a == DIR_UP && b == DIR_DOWN) ||
           (a == DIR_DOWN && b == DIR_UP) ||
           (a == DIR_RIGHT && b == DIR_LEFT) ||
           (a == DIR_LEFT && b == DIR_RIGHT);  
  }

  
  void game_set_direction(game_state_t *g, direction_t direction){
    if(is_opposite(g->direction, direction)) return;
    g->direction = direction;
  }

  
  void game_tick(game_state_t *g, int dt_ms){
    if(g->status == GAME_OVER || !g->alive) return;
    //ak sa hra na cas napocitavaj kazdy tick az kym neuplnynie
    if(g->mode == MODE_TIME){
      g->elapsed_ms += dt_ms;
      if(g->elapsed_ms >= g->time_limit_ms){
        g->status = GAME_OVER;
        g->alive = 0;
        return;
      }
    }
  point_t new_head = next_head(g);
  //ci je povolene wrapovanie
  if(g->world == WORLD_WRAP){
    new_head = wrap_point(g, new_head);
  }  else{
      if(!in_bounds(g, new_head)){
        g->status = GAME_OVER;
        g->alive = 0;
        return;
      }
    }
  //ci zjedol jedlo a bude rast
  int will_grow = points_equal(new_head, g->food);

  size_t check = g->snake_len;
  //ak nerastie tak po posunuti ci po checku sa dam dostat lebo posledny segment s a posunie a na jeho miesto sa posunie hlava
  if(!will_grow && g->snake_len > 0) check = g->snake_len - 1;

  //narazil sam do seba
  if(snake_contains(g, new_head, check)){
    g->status = GAME_OVER;
    g->alive = 0;
    return;
  }
  //ak rastie zvacsi
  if(will_grow){
    if(g->snake_len < MAX_SNAKE_LEN){
      g->snake_len++;
    } else{
      //dosiahol maximalnu moznu velkost, vyhra
      g->status = GAME_OVER;
      g->alive =  0;
      return;
    }
  }
  
  //posuniutie
  for(size_t i = g->snake_len - 1; i > 0; --i){
    g->snake[i] = g->snake[i - 1];
  }
  g->snake[0] = new_head;

  //ak zje jedlo zvysime skore
  if(points_equal(new_head, g->food)){
    g->score++;
    spawn_food(g);
    } 
  }

void game_make_state_line(const game_state_t *g, char *out, size_t cap){
  snprintf(out,
           cap,
            "STATE status=%d alive=%d score=%d time=%d/%d head=[%d,%d] food=[%d,%d]",g->status,
           g->alive,
           g->score,
           g->elapsed_ms,
           g->time_limit_ms,
           g->snake[0].x,
           g->snake[0].y,
           g->food.x,
           g->food.y
           );
}
