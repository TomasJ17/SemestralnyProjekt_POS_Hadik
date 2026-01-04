#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <poll.h>

#include "../ipc/ipc.h"
#include "../common/game_protocol.h"
#include "../common/game_state.h"
//server context thread data type
typedef struct {
  int c2s_fd;   // client->server(read)
  int s2c_fd;   // server->client(write)
  int connected;// if client is connected
  int shutdown; // if server needs to be terminated
  int game_started;
  
  pthread_mutex_t lock;
  game_state_t game;

  int tick_ms;
  int paused;               // 1 - player in menu, snake is freezed
  int resume_delay_active;  // 1 - wait 3s
  int64_t resume_at_ms;     // when snake can move again
  int64_t last_player_left_ms;  
}server_ctx_t;

static int64_t now_ms(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ms_sleep(int ms){
  struct timespec ts;
  ts.tv_sec = ms/1000;
  ts.tv_nsec = (long)(ms%1000)*1000000L;
  nanosleep(&ts, NULL);
}

//tick thread, game is running even without client
static void *tick_thread(void *arg){
  server_ctx_t *ctx = (server_ctx_t *)arg;
  while(1){
    ms_sleep(ctx->tick_ms);
    
    int fd_to_write = -1;
    char out[4096];
    out[0] = '\0';

    pthread_mutex_lock(&ctx->lock);

    if(ctx->shutdown){
      pthread_mutex_unlock(&ctx->lock);
      break;
    }

    int64_t now = now_ms();
  //10s without client ends server in MODE_STANDARD, time mode after time
    if(ctx->game_started && !ctx->connected && ctx->last_player_left_ms != 0){
      int64_t idle = now - ctx->last_player_left_ms;
      if(ctx->game.mode == MODE_STANDARD){
        if(idle >= 10000){
          ctx->shutdown = 1;
          pthread_mutex_unlock(&ctx->lock);
          break;
        }
      }
    }
    
    int freeze =  (ctx->paused || ctx->resume_delay_active && now < ctx->resume_at_ms);
    int move = freeze ? 0 : 1;

    if(ctx->resume_delay_active && now >= ctx->resume_at_ms){
      ctx->resume_delay_active = 0;
    }

    if(ctx->game_started){
      game_tick(&ctx->game, ctx->tick_ms, move);

      if(ctx->game.status == GAME_OVER || !ctx->game.alive){
        ctx->shutdown = 1;
      }
    }

    if(ctx->game_started && ctx->connected && ctx->s2c_fd >= 0){
      game_make_state_line(&ctx->game, out, sizeof(out));
      fd_to_write = ctx->s2c_fd;
      fprintf(stderr, "server tx: %s\n", out);
    }

      pthread_mutex_unlock(&ctx->lock);

      //send message
      if(fd_to_write >= 0){
        if(ipc_write_line(fd_to_write, out) < 0){
          pthread_mutex_lock(&ctx->lock);
          ctx->connected = 0;
          ctx->last_player_left_ms = now_ms();
          if(ctx->c2s_fd >= 0){ 
            close(ctx->c2s_fd);
            ctx->c2s_fd = -1;
          }
          if(ctx->s2c_fd >= 0){
            close(ctx->s2c_fd);
            ctx->s2c_fd = -1;
          }
          pthread_mutex_unlock(&ctx->lock);
        }
      }
    }
  return NULL;
}

static int parse_direction_from_input(const char *line, direction_t *out_dir){
  if(!is_input_line(line)) return 0;
  const char *p = line + strlen(MSG_INPUT);
  while(*p == ' ') p++;
  
  if(*p == '\0') return 0;
  if(*p == 'w') {*out_dir = DIR_UP; return 1;}
  if(*p == 'a') {*out_dir = DIR_LEFT; return 1;}
  if(*p == 's') {*out_dir = DIR_DOWN; return 1;}
  if(*p == 'd') {*out_dir = DIR_RIGHT; return 1;}

  return 0;
}

static const char *find_after(const char*s, const char *needle){
  const char *p = strstr(s, needle);
  if(!p) return NULL;
  return p + strlen(needle);
}

static int parse_int_after(const char *line, const char *key, int *out){
  const char *p = find_after(line, key);
  if(!p) return 0;
  return (sscanf(p, "%d", out) == 1);
}

static int load_map_obstacles(game_state_t *g, const char *path){
  FILE *f = fopen(path, "r");
  if(!f){
    perror("cannot open fiole with map");
    return 0;
  }

  for(int i = 0; i < MAP_H; i++){
    for(int j = 0; j < MAP_W; j++){
      g->obstacles[i][j] = 0;
    }
  }

  char line[256];
  for(int i = 0; i < MAP_H; i++){
    if(!fgets(line, sizeof(line), f)){
      fclose(f);
      fprintf(stderr, "map missing row %d\n", i);
      return 0;
    }
    for(int j = 0; j < MAP_W; j++){
      g->obstacles[i][j] = (line[j] == '#') ? 1 : 0;
    }
  }
  fclose(f);

  //map borders validation
 for(int i = 0; i < MAP_W; i++){
    if(!g->obstacles[0][i] || !g->obstacles[MAP_H - 1][i]){
      fprintf(stderr, "map bordering, cannot place obstacle");
      return 0;
    }
  }

  for(int i = 0; i < MAP_H; i++){
    if(!g->obstacles[i][0] || !g->obstacles[i][MAP_W - 1]){
      fprintf(stderr, "map bordering, cannot place obstacle");
      return 0;
    }
  }
  return 1;
}

static void clear_obstacles(game_state_t *g){
  for(int i = 0; i < MAP_H; i++){
    for(int j = 0; j < MAP_W; j++){
      g->obstacles[i][j] = 0;
    }
  }
}

int main(void) {
    printf("SERVER started\n");
    printf("SERVER PID: %d\n", getpid());

    // server creates IPC interface
    ipc_cleanup_fifos();
    if (ipc_create_fifos() != 0){
      perror("ipc_create_fifos");
      return 1;
    }

    server_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.game.status = GAME_OVER;
    ctx.game.alive = 0;
    ctx.c2s_fd = -1;
    ctx.s2c_fd = -1;
    ctx.connected = 0;
    ctx.shutdown = 0;
    ctx.tick_ms = 150; // speed of game, the greater the slower
    ctx.last_player_left_ms = 0;
    ctx.paused = 0;
    ctx.resume_delay_active = 0;
    ctx.resume_at_ms = 0;
    ctx.game_started = 0;

    pthread_mutex_init(&ctx.lock, NULL);

    //standard limit 0
    //game_init(&ctx.game, 20, 10, MODE_STANDARD, WORLD_WRAP, 0);
    //run of tick thread
    pthread_t t_thread;
    if(pthread_create(&t_thread, NULL, tick_thread, &ctx) != 0){
      perror("Vlakno sa nepodarilo vytvorit");
      ipc_cleanup_fifos();
      return 2;
  }
      int c2s = -1;
      int s2c = -1;
      //main thread processing of clients and messages

      while(1){
        pthread_mutex_lock(&ctx.lock);
        int should_exit = ctx.shutdown;
        pthread_mutex_unlock(&ctx.lock);
        if(should_exit) break;
        
        c2s = -1;
        s2c = -1;

        while(1){
          pthread_mutex_lock(&ctx.lock);
          int should_exit2 = ctx.shutdown;
          pthread_mutex_unlock(&ctx.lock);
          if(should_exit2) break;
          

          if(c2s < 0){
            c2s = open(FIFO_C2S, O_RDONLY | O_NONBLOCK);
            if(c2s < 0){
              if(errno == ENOENT){
                ms_sleep(50);
                continue;
              }
              perror("server open c2s RDONLY nonblock");
              ms_sleep(50);
              continue;
            }
          }
          s2c = open(FIFO_S2C, O_WRONLY | O_NONBLOCK);
          if(s2c < 0){
            if(errno == ENXIO || errno == ENOENT){
              ms_sleep(50);
              continue;
            }
            perror("open s2c nonblock");
            close(c2s);
            c2s = -1;
            ms_sleep(50);
            continue;
        } 
    
      ipc_set_blocking(c2s, 1);
      ipc_set_blocking(s2c, 1);
      break;
    }
    if(c2s < 0 || s2c < 0){
      continue;
    }

    pthread_mutex_lock(&ctx.lock);
    ctx.c2s_fd = c2s;
    ctx.s2c_fd = s2c;
    ctx.connected = 1;
    //stopping timeout
    ctx.last_player_left_ms = 0;
    pthread_mutex_unlock(&ctx.lock);
    
    char line[256];

    //handshake
    ipc_write_line(s2c, MSG_READY);

    //input for game setup from client
    char cfg[256];
    ssize_t cn = ipc_read_line(c2s, cfg, sizeof(cfg));
    if(cn <= 0){
      //fprintf(stderr, "SERVER: expected START, got: %s\n", (cn > 0) ? cfg : "<EOF>");
      //Client didnt start game correctly, close session and wait for next client
      pthread_mutex_lock(&ctx.lock);
      ctx.connected = 0;
      ctx.last_player_left_ms = now_ms();
      if(ctx.c2s_fd >= 0){
        close(ctx.c2s_fd);
        ctx.c2s_fd = -1;
      }
      if(ctx.s2c_fd >= 0){
        close(ctx.s2c_fd);
        ctx.s2c_fd = -1;
      }
      pthread_mutex_unlock(&ctx.lock);
      
      ms_sleep(50);
      continue;
    }
    //JOIN existing game
    if(strcmp(cfg, MSG_JOIN) == 0){
      int can_join = 0;

      pthread_mutex_lock(&ctx.lock);
      can_join = (ctx.game_started && ctx.game.alive && ctx.game.status != GAME_OVER);
      pthread_mutex_unlock(&ctx.lock);

      if(!can_join){
        ipc_write_line(s2c, "ERR no running game");

        pthread_mutex_lock(&ctx.lock);
        ctx.connected = 0;
        ctx.last_player_left_ms = now_ms();
        if(ctx.c2s_fd >= 0){
          close(ctx.c2s_fd);
          ctx.c2s_fd = -1;
        }
        if(ctx.s2c_fd >= 0){
          close(ctx.s2c_fd);
          ctx.s2c_fd = -1;
        }
        pthread_mutex_unlock(&ctx.lock);
        continue;
      }
      //join OK resume with 3s delay
      pthread_mutex_lock(&ctx.lock);
      ctx.paused = 0;
      ctx.resume_delay_active = 1;
      ctx.resume_at_ms = now_ms() + 3000;
      ctx.last_player_left_ms = 0;
      pthread_mutex_unlock(&ctx.lock);
    }
    // START - new game setup
    else if(starts_with(cfg, MSG_START)){
      int mode = MODE_TIME;
      int world = WORLD_WRAP;
      int limit = 30000;
      int w = 20;
      int h = 10; 

      parse_int_after(cfg, "mode=", &mode);
      parse_int_after(cfg, "world=", &world);
      parse_int_after(cfg, "limit=", &limit);
      parse_int_after(cfg, "w=", &w);
      parse_int_after(cfg, "h=", &h);

      pthread_mutex_lock(&ctx.lock);

      if(world == WORLD_WRAP){
        game_init(&ctx.game, w, h, (game_mode_t)mode, WORLD_WRAP, limit);
        clear_obstacles(&ctx.game);
      }else{
        game_init(&ctx.game, MAP_W, MAP_H, MODE_STANDARD, WORLD_WALLS, 0);
        if(!load_map_obstacles(&ctx.game, "maps/map1.txt")){
          fprintf(stderr, "SERVER failed to load map\n");
          ctx.game.status = GAME_OVER;
          ctx.game.alive = 0;
        }
      }

    ctx.paused = 0;
    ctx.resume_delay_active = 0;
    ctx.resume_at_ms = 0;
    ctx.last_player_left_ms = 0;
    ctx.shutdown = 0;
    ctx.game_started = 1;

    pthread_mutex_unlock(&ctx.lock);
    } else {
      fprintf(stderr, "SERVER: expected START/JOIN, got: %s\n", cfg); 
      pthread_mutex_lock(&ctx.lock);
      ctx.connected = 0;
      ctx.last_player_left_ms = now_ms();
      if(ctx.c2s_fd >= 0){
        close(ctx.c2s_fd);
        ctx.c2s_fd = -1;
      }
      if(ctx.s2c_fd >= 0){
        close(ctx.s2c_fd);
        ctx.s2c_fd = -1;
      }
      pthread_mutex_unlock(&ctx.lock);

      continue;
    }
    //game loop
    while (1) {
      pthread_mutex_lock(&ctx.lock);
      int should_exit3 = ctx.shutdown;
      pthread_mutex_unlock(&ctx.lock);
      if(should_exit3){
        if(s2c >= 0) ipc_write_line(s2c, MSG_BYE);
        break;
      }
      struct pollfd pfd;
      pfd.fd = c2s;
      pfd.events = POLLIN;
      pfd.revents = 0;

      int pr = poll(&pfd, 1, 100); //100ms
      if(pr < 0){
        if(errno == EINTR) continue;
        perror("poll");
        break;
      }
      if(pr == 0){
        continue;
      }

      if(pfd.revents & (POLLHUP | POLLERR)){
          //end of client alebo EOF
          pthread_mutex_lock(&ctx.lock);
          ctx.connected = 0;
          ctx.last_player_left_ms = now_ms();
          if(ctx.c2s_fd >= 0){
            close(ctx.c2s_fd);
            ctx.c2s_fd = -1;
          }
          if(ctx.s2c_fd >= 0){
            close(ctx.s2c_fd);
            ctx.s2c_fd = -1;
          }
          pthread_mutex_unlock(&ctx.lock);
          break;
      }

      if(pfd.revents & POLLIN){
        ssize_t n = ipc_read_line(c2s, line, sizeof(line));
        if(n <= 0){
          //end of client alebo EOF
          pthread_mutex_lock(&ctx.lock);
          ctx.connected = 0;
          ctx.last_player_left_ms = now_ms();
          if(ctx.c2s_fd >= 0){
            close(ctx.c2s_fd);
            ctx.c2s_fd = -1;
          }
          if(ctx.s2c_fd >= 0){
            close(ctx.s2c_fd);
            ctx.s2c_fd = -1;
          }
          pthread_mutex_unlock(&ctx.lock);
          break;
        }
      

      //DEBUG
      fprintf(stderr, "server rx: %s\n", line);

      if(strcmp(line, MSG_QUIT) == 0){
        //finish game and server
        pthread_mutex_lock(&ctx.lock);
        ctx.shutdown = 1;
        pthread_mutex_unlock(&ctx.lock);
        ipc_write_line(s2c, MSG_BYE);
        break;
      }
      if(strcmp(line, MSG_DISCONNECT) == 0){
        //client leaves but server is running
        pthread_mutex_lock(&ctx.lock);
        ctx.connected = 0;
        ctx.paused = 1; 
        ctx.last_player_left_ms = now_ms();
        if(ctx.c2s_fd >= 0){
          close(ctx.c2s_fd);
          ctx.c2s_fd = -1;
        }
        if(ctx.s2c_fd >= 0){
          close(ctx.s2c_fd);
          ctx.s2c_fd = -1;
        }
        pthread_mutex_unlock(&ctx.lock);
        break;
      }
      if(strcmp(line, MSG_PAUSE) == 0){
        pthread_mutex_lock(&ctx.lock);
        if(!ctx.paused){
          ctx.paused = 1;
        }
        else {
          ctx.paused = 0;
          ctx.resume_delay_active = 1;
          ctx.resume_at_ms = now_ms() + 3000;
        }
        pthread_mutex_unlock(&ctx.lock);
        continue;
      }
      direction_t d;
      if(parse_direction_from_input(line, &d)){
        //DEBUG
        //fprintf(stderr, "SERVER DIR set: %d\n",(int)d);
        //processing of inputs, parsing and calling game_set_direction
        pthread_mutex_lock(&ctx.lock);
        game_set_direction(&ctx.game, d);
        pthread_mutex_unlock(&ctx.lock);
        }
      }
      }
    }
    //threads waiting
    pthread_join(t_thread, NULL);

    ipc_cleanup_fifos();
    printf("SERVER end\n");
    return 0;
}
