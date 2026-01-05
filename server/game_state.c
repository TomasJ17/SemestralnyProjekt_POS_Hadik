/*
 * Server side game logic
 * snake movement and collision detection
 * food spawnig (only on free blocks avoiding snake and obstacles)
 * time based mode
 * */

#define _POSIX_C_SOURCE 200809L

#include "../common/game_state.h"

#include <stddef.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

//timing with help of AI chatgpt
//Monotonic time in ms
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
/*Returns 1 if the cell is outside bounds or collides with obstacles on map(WORLD_WALLS)
 * Indexing is [y][x]
 */
static int is_blocked_cell(const game_state_t *g, point_t p){
  if(g->world == WORLD_WALLS){
    if(p.x < 0 || p.x >= g->width || p.y < 0 || p.y >= g->height) return 1;
    if(g->obstacles[p.y][p.x]) return 1;
  }
  return 0;
}
/* Spawns food randomly, but only on free(non-occupied) block cells(use of is_blocked_cell)
 * Uses a capped number of tries (max_tries) to avoid infinite loop if map is crowded
 * NOTE: AI assisted: loop conditions and max_tries guard were consulted with chatgpt to avoid edge-cases
 * */
static void spawn_food(game_state_t *g){
    point_t p;
    int obstacles = (g->world == WORLD_WALLS) ? g->obstacles_count : 0;
    int free_cells = g->width * g->height - obstacles;

    if((int)g->snake_len >= free_cells){
      g->status = GAME_OVER;
      g->alive = 0;
      return;
    }  

    //helper varible ins case map is full
    int max_tries = g->width * g->height;
    //random spawn on map but avoiding snake body
    do{
      p.x = rand() % g->width;
      p.y = rand() % g->height;
      max_tries--;
    } while((snake_contains(g,p, g->snake_len) || is_blocked_cell(g, p)) && max_tries > 0);
    if(max_tries <= 0){
      g->status = GAME_OVER;
      g->alive = 0;
      return;
    }
    g->food = p;
  }
  /*
   *Initializes a new game
   *NOTE: memset clears obstacles too, caller can load yhem after init if needed
   * */
  void game_init(game_state_t *g, int width, int height, game_mode_t mode, world_type_t world, int time_limit_ms){
    srand(time(NULL));
    //clear of game_state_t params and initialization
    memset(g, 0, sizeof(*g));
    g->width = width;
    g->height = height;
    g->mode = mode;
    g->world = world;
    g->status = GAME_RUNNING;
    g->alive = 1;
    g->score = 0;
    //default direction on start of game
    g->direction = DIR_UP;
    //starting snake length and set body parts into middle of map
    g->snake_len = 3;
    int sx = width/2;
    int sy = height/2;
    g->snake[0] = (point_t){sx,sy};
    g->snake[1] = (point_t){sx,sy+1};
    g->snake[2] = (point_t){sx,sy+2};
    
    //casovanie
    g->start_ms = now_ms();
    g->time_limit_ms = (mode == MODE_TIME) ? time_limit_ms : 0;
    g->elapsed_ms = 0;

    spawn_food(g); 
  }
  // helper function so player cannot turn 180 degrees and automatically collide into himself
  static int is_opposite(direction_t a, direction_t b){
    return (a == DIR_UP && b == DIR_DOWN) ||
           (a == DIR_DOWN && b == DIR_UP) ||
           (a == DIR_RIGHT && b == DIR_LEFT) ||
           (a == DIR_LEFT && b == DIR_RIGHT);  
  }

  // setter: sets game_state_t.direction to direction_t from paramater given
  void game_set_direction(game_state_t *g, direction_t direction){
    if(is_opposite(g->direction, direction)) return;
    g->direction = direction;
  }

  //Core simulation step
  //Handles time limit (MODE_TIME)
  //Compute next head position(wrap or walls) using function next_head
  //Takes care of collision, boundaries if WORLD_WRAP=0, obstacles, self
  //Move body; grow body when food eaten; respawn food
  void game_tick(game_state_t *g, int dt_ms, int move){
    if(g->status == GAME_OVER || !g->alive) return;
    //ig game MODE_TIME count time inbetween ticks until time limit is reached, then end game
    if(g->mode == MODE_TIME){
      g->elapsed_ms += dt_ms;
      if(g->elapsed_ms >= g->time_limit_ms){
        g->status = GAME_OVER;
        g->alive = 0;
        return;
      }
    }
    if(!move){
      return;
    }
  point_t new_head = next_head(g);
  //checks if wrapping is allowed
  if(g->world == WORLD_WRAP){
    new_head = wrap_point(g, new_head);
  }  else{
      if(!in_bounds(g, new_head)){
        g->status = GAME_OVER;
        g->alive = 0;
        return;
      }
    }
  //if wrapping is not allowed so map has walls on borders
  if(g->world == WORLD_WALLS){
    if(g->obstacles[new_head.y][new_head.x]){
      g->status = GAME_OVER;
      g->alive = 0;
      return;
    }
  }
  //if ate food -> will grow
  int will_grow = points_equal(new_head, g->food);

  size_t check = g->snake_len;
  //if doesnt grow then after move after check i can reach because last segment will move so its position can be claimed by head, if he will grow that position will be occupied so there will be collision
  if(!will_grow && g->snake_len > 0) check = g->snake_len - 1;

  //collided with himself
  if(snake_contains(g, new_head, check)){
    g->status = GAME_OVER;
    g->alive = 0;
    return;
  }
  //if grows increase length
  if(will_grow){
    int obstacles = (g->world == WORLD_WALLS) ? g->obstacles_count : 0;
    int free_cells = g->width * g->height - obstacles;
    if((int)g->snake_len < free_cells && g->snake_len < MAX_SNAKE_LEN){
      g->snake_len++;
    } else{
      //reached MAX_SNAKE_LEN, so technically won
      g->status = GAME_OVER;
      g->alive =  0;
      return;
    }
  }
  
  //movement
  for(size_t i = g->snake_len - 1; i > 0; --i){
    g->snake[i] = g->snake[i - 1];
  }
  g->snake[0] = new_head;

  //score increase after food was eaten
  if(points_equal(new_head, g->food)){
    g->score++;
    spawn_food(g);
    } 
  }

void game_make_state_line(const game_state_t *g, char *out, size_t cap){
  //cap total capacity of buffer out
  //cannot write more then cap, because of memory overflow
  //we are tracking used segments so append is safe
  size_t used = 0;
  int n = snprintf(out,
           cap,
            "STATE status=%d alive=%d score=%d time=%d/%d w=%d h=%d head=[%d,%d] food=[%d,%d]",g->status,
           g->alive,
           g->score,
           g->elapsed_ms,
           g->time_limit_ms,
           g->width,
           g->height,
           g->snake[0].x,
           g->snake[0].y,
           g->food.x,
           g->food.y
           );
  //if snprintf failed return
  if(n < 0){
  //sends empty message
    if(cap) out[0] = '\0';
    return;
  }
  used = (size_t)n;
  //if used >= cap there is no space for further appends, checks if head is not "too long"
  //we ensure buffer is still null terminated and return(stop)
  if(used >= cap){
    out[cap - 1] = '\0';
    return;
  }

  //append segments(snake body)
  //in loop each iteration appends "x,y" and optionally ';'
  //so client can parse segments as a lis :"x,y;x,y;x,y"
  for(size_t i = 0; i < g->snake_len; i++){
  //append into out+used, with remaining capacity cap - used
    n = snprintf(out + used, cap - used, "%d,%d%s",
        g->snake[i].x,
        g->snake[i].y,
        (i + 1 < g->snake_len) ? ";" : ""
      );
    if(n < 0) break; // snprintf error, stop appending
    used += (size_t)n;
    //if used >= cap last append got truncated and buffer end is reached
    //truncated ensure termination and stop, check after each body segment add
    if(used >= cap){
      out[cap - 1] = '\0';
      return;
    }
  }
}
