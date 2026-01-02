#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "../ipc/ipc.h"
#include "../common/game_protocol.h"
#include "../common/game_state.h"
//server context thread data typ
typedef struct {
  int c2s_fd;   // client->server(read)
  int s2c_fd;   // server->client(write)
  int connected;// ci je klient pripojeny
  int shutdown; // ci sa ma skoncit server
  int game_started;
  
  pthread_mutex_t lock;
  game_state_t game;

  int tick_ms;
  int paused;               // 1 - hrac je v menu, hadik stoji
  int resume_delay_active;  // 1 - cakame 3s po navrate
  int64_t resume_at_ms;     // kedy sa moze znova hybat
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

//tick thread, hra bezi aj bez klienta
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

  //pauza/resume netickuj ale posli STATE(ak si klient)
    if(ctx->paused || (ctx->resume_delay_active && now < ctx->resume_at_ms)){
      if(ctx->connected && ctx->s2c_fd >= 0){
        game_make_state_line(&ctx->game, out, sizeof(out));
        fd_to_write = ctx->s2c_fd;
      }

      pthread_mutex_unlock(&ctx->lock);

      //odosli spravu
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
      continue;
    }
    //ak delay skoncil, ukonci flag
    if(ctx->resume_delay_active && now >= ctx->resume_at_ms){
      ctx->resume_delay_active = 0;
    }
  if(ctx->game_started){
  //update hry
  game_tick(&ctx->game, ctx->tick_ms);
  //standardny mode ak nikto nieje pripojeny 10s koniec hry
  if(ctx->game.mode == MODE_STANDARD && !ctx->connected){
    int64_t t = now_ms();
    if (ctx->last_player_left_ms != 0 && (t - ctx->last_player_left_ms) >= 10000) {
        ctx->game.status = GAME_OVER;
        ctx->game.alive = 0;
      }
    }
  //ak sa hra skoncila ukonci server
  if(ctx->game.status == GAME_OVER || !ctx->game.alive){
      ctx->shutdown = 1;
    }
  }
  // posielaj STATE len ked hra bezi
  if(ctx->game_started && ctx->connected && ctx->s2c_fd >= 0){
      game_make_state_line(&ctx->game, out, sizeof(out));
      fd_to_write = ctx->s2c_fd;      
    }
    pthread_mutex_unlock(&ctx->lock);

    //odoslanie
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

  //validacia ohranicenia mapy
 for(int i = 0; i < MAP_W; i++){
    if(!g->obstacles[0][i] || !g->obstacles[MAP_H - 1][i]){
      fprintf(stderr, "ohranicenie mapy, nemoze dat prekazku");
      return 0;
    }
  }

  for(int i = 0; i < MAP_H; i++){
    if(!g->obstacles[i][0] || !g->obstacles[i][MAP_W - 1]){
      fprintf(stderr, "ohranicenie mapy, nemoze dat prekazku");
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

    // server vytvara IPC rozhranie
    if (ipc_create_fifos() != 0) return 1;

    server_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.game.status = GAME_OVER;
    ctx.game.alive = 0;
    ctx.c2s_fd = -1;
    ctx.s2c_fd = -1;
    ctx.connected = 0;
    ctx.shutdown = 0;
    ctx.tick_ms = 150; // rychlost hry vacsie cislo pomalsia
    ctx.last_player_left_ms = 0;
    ctx.paused = 0;
    ctx.resume_delay_active = 0;
    ctx.resume_at_ms = 0;
    ctx.game_started = 0;

    pthread_mutex_init(&ctx.lock, NULL);

    //standard limit 0
    //game_init(&ctx.game, 20, 10, MODE_STANDARD, WORLD_WRAP, 0);
    //spustenie tick thread
    pthread_t t_thread;
    if(pthread_create(&t_thread, NULL, tick_thread, &ctx) != 0){
      perror("Vlakno sa nepodarilo vytvorit server main 122");
      ipc_cleanup_fifos();
      return 2;
  }
    //main thread spracovanie klientov a sprav
    while (1) {
      pthread_mutex_lock(&ctx.lock);
      int should_exit = ctx.shutdown;
      pthread_mutex_unlock(&ctx.lock);
      if(should_exit) break;
      int c2s = ipc_open_c2s_read(); //cakame na klientov write
      if(c2s < 0){
        perror("open c2s read");
        break;
      }

      int s2c = ipc_open_s2c_write(); // cakame na klient read
      if(s2c < 0){
        perror("open s2c write");
        close(c2s);
        break;
    }

    pthread_mutex_lock(&ctx.lock);
    ctx.c2s_fd = c2s;
    ctx.s2c_fd = s2c;
    ctx.connected = 1;
    //stopujeme timeout
    ctx.last_player_left_ms = 0;
    pthread_mutex_unlock(&ctx.lock);
    
    char line[256];

    //handshake
    ipc_write_line(s2c, MSG_READY);

    //input pre nastavenie hry od klienta
    char cfg[256];
    ssize_t cn = ipc_read_line(c2s, cfg, sizeof(cfg));
    if(cn <= 0 || !starts_with(cfg, MSG_START)){
      fprintf(stderr, "SERVER: expected START, got: %s\n", cfg);
      pthread_mutex_lock(&ctx.lock);
      ctx.connected = 0;
      ctx.last_player_left_ms = now_ms();
      close(ctx.c2s_fd);
      close(ctx.s2c_fd);
      ctx.c2s_fd = -1;
      ctx.s2c_fd = -1;
      pthread_mutex_unlock(&ctx.lock);
      break;
    }

    int mode = MODE_TIME;
    int world = WORLD_WRAP;
    int limit = 30000;

    parse_int_after(cfg, "mode=", &mode);
    parse_int_after(cfg, "world=", &world);
    parse_int_after(cfg, "limit=", &limit);

    pthread_mutex_lock(&ctx.lock);

    //TIME prazdna mapa, wrap, limit zo vstupu
    if(world == WORLD_WRAP){
      game_init(&ctx.game, 20, 10, (game_mode_t)mode, WORLD_WRAP, limit);
      clear_obstacles(&ctx.game);
    }

    //WALLS MAP_W x MAP_H, bez wrap, standard kym nenabura
    else{
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

    //game loop
    while (1) {
      ssize_t n = ipc_read_line(c2s, line, sizeof(line));
      if(n <= 0){
        //koniec klienta alebo EOF
        pthread_mutex_lock(&ctx.lock);
        ctx.connected = 0;
        ctx.last_player_left_ms = now_ms();
        close(ctx.c2s_fd);
        close(ctx.s2c_fd);
        ctx.s2c_fd = -1;
        ctx.c2s_fd = -1;
        pthread_mutex_unlock(&ctx.lock);
        break;
      }
      //DEBUG
      fprintf(stderr, "server rx: %s\n", line);

      if(strcmp(line, MSG_QUIT) == 0){
        //ukonci hru aj server
        pthread_mutex_lock(&ctx.lock);
        ctx.shutdown = 1;
        pthread_mutex_unlock(&ctx.lock);
        ipc_write_line(s2c, MSG_BYE);
        break;
      }
      if(strcmp(line, MSG_DISCONNECT) == 0){
        //klient odide ale server bezi
        pthread_mutex_lock(&ctx.lock);
        ctx.connected = 0;
        ctx.last_player_left_ms = now_ms();
        close(ctx.c2s_fd);
        close(ctx.s2c_fd);
        ctx.c2s_fd = -1;
        ctx.s2c_fd = -1;
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
      //spracovanie inputov, parsovanie a volanie game_set_direction
      pthread_mutex_lock(&ctx.lock);
      game_set_direction(&ctx.game, d);
      pthread_mutex_unlock(&ctx.lock);
        }
      }
    }
    //cakanie vlakien
    pthread_join(t_thread, NULL);

    ipc_cleanup_fifos();
    printf("SERVER end\n");
    return 0;
}
