#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <poll.h>

#include "../ipc/ipc.h"
#include "../common/game_protocol.h"
#include "../common/game_state.h"
#include "render.h"

typedef struct{
  int c2s_fd;
  int s2c_fd;

  pthread_mutex_t lock;
  game_state_t last;
  int has_state;
  
  int running;

  int walls_map_loaded;
} client_ctx_t;

static void start_server_process(void){
  pid_t pid = fork();
  if( pid < 0){
    perror("fork");
    exit(2);
  }
  if(pid ==  0){
    // potomok spusti server
    execl("./server/server", "./server/server", (char*)NULL);
    perror("execl");
    exit(3);
  }
  //rodic pokracuje ako klient
}

static int server_is_running(void){
  int fd = ipc_open_c2s_write_nb();
  if(fd >= 0){
    close(fd);
    return 1;
  }
  if(errno == ENOENT) return 0;
  if(errno == ENXIO) return 0;
  return 0;
}

//funkcia na vratenie kurzora
static void term_restore(void){
  printf("\033[?25h\n"); // ukaz kurzor
  fflush(stdout);
}

static void ms_sleep(int ms){
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

//helper: najde substring a vrati pointer za nim, inak NULL
static const char *find_after(const char *s, const char *needle){
  const char *p = strstr(s, needle);
  if(!p) return NULL;
  return p + strlen(needle);
}

static int parse_int_after(const char *line, const char *key, int *out){
  const char *p = find_after(line, key);
  if(!p) return 0;
  return (sscanf(p, "%d", out) == 1);
}

static int parse_food(const char *line, int *fx, int *fy){
  const char *p = find_after(line, "food=[");
  if(!p) return 0;
  return (sscanf(p, "%d,%d", fx, fy) == 2);
}

static int parse_snake_list(const char *p, game_state_t *g){
  g->snake_len = 0;
  char buf[4096];
  strncpy(buf, p, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *saveptr = NULL;
  char *tok = strtok_r(buf, ";| \r\n", &saveptr);

  while(tok && g->snake_len < MAX_SNAKE_LEN){
    while(*tok == '[' || *tok == '(') tok++;

    for(char *q = tok; *q; q++){
      if(*q == ']' || *q == ')'){
        *q = '\0';
        break;
      }
      if(*q == ',') *q = ' ';
    }
    int x = 0, y = 0;
    if(sscanf(tok, "%d %d", &x, &y) == 2){
      g->snake[g->snake_len++] = (point_t){x,y};
    }
    tok = strtok_r(NULL, ";| \r\n", &saveptr);
  }
  return (g->snake_len > 0);
}

static int parse_state_line_into(game_state_t *g, const char *line){
  //ockavame novy state
  //STAET status,alive,score,time,w,h,food,len,body
  if(!starts_with(line, MSG_STATE)) return 0;

  int status = 0,
  alive = 0,
  score = 0,
  elapsed = 0,
  limit = 0;
  
  if(!parse_int_after(line, "status=", &status)) return 0;
  if(!parse_int_after(line, "alive=", &alive)) return 0;
  if(!parse_int_after(line, "score=", &score)) return 0;

  //time=elapsed/limit
  {
    const char *p = find_after(line, "time=");
    if(!p) return 0;
    if(sscanf(p, "%d/%d", &elapsed, &limit) != 2) return 0;
  }

  int w = 0, h = 0;
  if(parse_int_after(line, "w=", &w)) g->width = w;
  if(parse_int_after(line, "h=", &h)) g->height = h;

  int fx = 0, fy = 0;
  if(!parse_food(line, &fx, &fy)) return 0;

  g->status = (game_status_t)status;
  g->alive = alive;
  g->score = score;
  g->elapsed_ms = elapsed;
  g->time_limit_ms = limit;
  g->food.x = fx;
  g->food.y = fy;

  const char *foodp = strstr(line, "food=[");
  if(!foodp) return 0;

  const char *after = strchr(foodp, ']');
  if(!after) return 0;
  after++;
  while(*after == ' ') after++;

  return parse_snake_list(after, g);
}

static int load_map_obstacles(game_state_t *g, const char *path){
  FILE *f = fopen(path, "r");
  if(!f){
    perror("cannot open file with map");
    return 0;
  }

  //inicializacia/vynulovanie prekazok
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
      char c = line[j];
      if(c == '#') g->obstacles[i][j] = 1;
      else g->obstacles[i][j] = 0;
    }
  }
  fclose(f);
  return 1;
}


static void *rx_thread(void *arg){
  client_ctx_t *ctx = (client_ctx_t*)arg;
  char line[4096];

  while(1){
    ssize_t n = ipc_read_line(ctx->s2c_fd, line, sizeof(line));
    //fprintf(stderr, "client rx raw: %s\n", line);
    if(n <= 0){
      pthread_mutex_lock(&ctx->lock);
      ctx->running = 0;
      pthread_mutex_unlock(&ctx->lock);
      break;
    }

    if(strcmp(line, MSG_BYE) == 0){
      pthread_mutex_lock(&ctx->lock);
      ctx->running = 0;
      pthread_mutex_unlock(&ctx->lock);
      break;
    }

    if(starts_with(line, MSG_STATE)){
      int parsed_ok = 0;
      int game_over = 0;
      pthread_mutex_lock(&ctx->lock);
      parsed_ok = parse_state_line_into(&ctx->last, line);
      //fprintf(stderr, "client parse state ok=%d\n", parsed_ok);
      if(parsed_ok){
        int is_walls = (ctx->last.width == MAP_W && ctx->last.height == MAP_H);
        if(is_walls){
          ctx->last.world = WORLD_WALLS;
          ctx->last.mode = MODE_STANDARD;
          if(!ctx->walls_map_loaded){
            if(!load_map_obstacles(&ctx->last, "maps/map1.txt")){
              for(int i = 0; i < MAP_H; i++){
                for(int j = 0; j < MAP_W; j++){
                  ctx->last.obstacles[i][j] = 0;
                }
              }
            }
            ctx->walls_map_loaded = 1;
          }
        }else{
          for(int i = 0; i < MAP_H; i++){
            for(int j = 0; j < MAP_W; j++){
              ctx->last.obstacles[i][j] = 0;
            }
          }
          ctx->walls_map_loaded = 0;
        }

        ctx->has_state = 1;
        game_over = (ctx->last.status == GAME_OVER || (!ctx->last.alive));
      }else{
        fprintf(stderr, "CLIENT: parse STATE failed: %s\n", line);
      }
      pthread_mutex_unlock(&ctx->lock);

      if(!parsed_ok) continue;

      if(game_over){
        pthread_mutex_lock(&ctx->lock);
        ctx->running = 0;
        pthread_mutex_unlock(&ctx->lock);
        break;
      }
    }
  }
  return NULL;
}

static void *render_thread(void *arg){
  client_ctx_t *ctx = (client_ctx_t*)arg;

  while(1){
    game_state_t local;
    int has = 0;

    pthread_mutex_lock(&ctx->lock);
    int run = ctx->running;
    if(ctx->has_state){
      local = ctx->last;
      has = 1;
    }
    pthread_mutex_unlock(&ctx->lock);
    if(!run)break;
    if(has){
      render_draw(stdout, &local);
    }
    usleep(16000); //cca 60 fps
  }
  return NULL;
}


static void stop_existing_server(void){
  //if fifo doesnt exist there is nothin to kill
  if(access(FIFO_C2S, F_OK) != 0 || access(FIFO_S2C, F_OK) != 0){
    return;
  }
  int c2s = -1;
  int s2c = -1;

  //non-blocking retry: close both sides, so server is not left on open()
  for(int i=0; i < 40; i++){
    c2s = ipc_open_c2s_write_nb();
    if(c2s >= 0) break;
    if(errno == ENOENT || errno == ENXIO){
      usleep(50 * 1000);
      continue;
    }
    //another error
    return;
  }
  for(int i = 0; i < 40; i++){
    s2c = ipc_open_s2c_read_nb();
    if(s2c >= 0) break;
    if(errno == ENOENT){
      usleep(50 * 1000);
      continue;
    }
    close(c2s);
    return;
  }
  //switch for blocking, so read_line is working
  ipc_set_blocking(c2s, 1);
  ipc_set_blocking(s2c, 1);

  //server sends READY after connection and waits for START
  char line[256];
  ssize_t n = ipc_read_line(s2c, line, sizeof(line));
  if(n > 0 && strcmp(line, MSG_READY) == 0){
    //send START, any because server is immediatly ended 
    char start_msg[128];
    snprintf(start_msg, sizeof(start_msg), 
              "%s mode=%d world=%d limit=%d",
             MSG_START, MODE_TIME, WORLD_WRAP, 1000
             );
    ipc_write_line(c2s, start_msg);
    //immediate QUIT server sets shutdown and ends
    ipc_write_line(c2s, MSG_QUIT);
  }
  close(c2s);
  close(s2c);
  //wait some period, so server can make unlink FIFO
  for(int i = 0; i < 40; i++){
    if(access(FIFO_C2S, F_OK) != 0 && access(FIFO_S2C, F_OK) != 0) break;
    usleep(50 * 1000);
  }
}

static int connect_with_retry(int *out_c2s, int *out_s2c){
  int c2s = -1;
  int s2c = -1;
  
  for(int i = 0; i < 60; i++){
    c2s = ipc_open_c2s_write_nb();
    if(c2s < 0){
      if(errno == ENOENT || errno == ENXIO){
        usleep(50 * 1000);
        continue;
      }
      return -1;
    }

    s2c = ipc_open_s2c_read();
    if(s2c < 0){
      close(c2s);
      c2s = -1;
      if(errno == ENOENT){
        usleep(50 * 1000);
        continue;
      }
      return -1;
    }
    
    ipc_set_blocking(c2s, 1);

    *out_c2s = c2s;
    *out_s2c = s2c;
    return 0;
  }
  return -1;
}

int main(void) {
  printf("CLIENT started\n");
  printf("CLIENT PID: %d\n", getpid());
  atexit(term_restore);
  printf("Menu:\n1. New game\n2. Exit\n");
  int choice = 0;
  scanf("%d", &choice);
  int c;
  while((c = getchar()) != '\n' && c != EOF) {}
  if(choice != 1){
    stop_existing_server();
    printf("Bye\n");
    return 0;
  }
while(1){
  printf("Server:\n1) New server (new game)\n2) Join existing game\n3) Exit\n");
  int serv_opt = 1;
  scanf("%d", &serv_opt);
  int ch;
  while((ch = getchar()) != '\n' && ch != EOF) {};

  int c2s = -1;
  int s2c = -1;
  if(serv_opt == 3){
    stop_existing_server();
    break;  
  }

  int world = WORLD_WRAP;
  int mode = MODE_TIME;
  int limit_ms = 30000;
  int w = 20, h = 10;

  if(serv_opt == 1){
    printf("Vyber si rezim:\n");
    printf("1) TIME (prazdna mapa, WRAP)\n");
    printf("2) WALLS (prekazky zo suboru, bez WRAP)\n");
    printf("3) EXIT\n");
    int opt = 1;
    scanf("%d", &opt);
    int cr;
    while((cr = getchar()) != '\n' && cr != EOF) {};
    if(opt == 3){
      continue;
    }

    if(opt == 1){
      world = WORLD_WRAP;
      mode = MODE_TIME;

      int sec = 30;
      printf("Zadaj casovy limit v sekundach (0 = spat):");
      scanf("%d", &sec);
      int aa;
      while((aa = getchar()) != '\n' && aa != EOF) {};

      if(sec == 0){
        continue;
      }

      if(sec < 1) sec = 30;
      limit_ms = sec * 1000;
      printf("Zadaj sirku sveta (10 - 80):");
      scanf("%d", &w);
      int bb;
      while((bb = getchar()) != '\n' && bb != EOF) {};

          
      printf("Zadaj vysku sveta (10 - 40):");
      scanf("%d", &h);
      int cc;
      while((cc = getchar()) != '\n' && cc != EOF) {};

      if(w < 10) w = 10;
      if(w > 80) w = 80;
      if(h < 10) h = 10;
      if(h > 40) h = 40;

    }else{
      world = WORLD_WALLS;
      mode = MODE_STANDARD; // kym nenanbura
      limit_ms = 0;
      w = MAP_W;
      h = MAP_H;
  }

  stop_existing_server();
  start_server_process();
  ms_sleep(200); //to create FIFO in time
}


  if(serv_opt == 2){
    if(access(FIFO_C2S, F_OK) != 0 || access(FIFO_S2C, F_OK) != 0){
      fprintf(stderr, "No existing game to join\n");
      return 1;
    }
  }

  if(connect_with_retry(&c2s, &s2c) != 0){
    if(serv_opt == 2) fprintf(stderr, "No existing game to join\n");
    else fprintf(stderr, "Cannot connect to new server\n");
    return 1;
  }

  //handshake
  char line[256];

   if(ipc_read_line(s2c, line, sizeof(line)) <= 0 || strcmp(line, MSG_READY) != 0){
    fprintf(stderr, "Server neposlal READY");
    //zatvaram fifa
    close(c2s);
    close(s2c);
    continue;
    }

    char start_msg[256];
    if(serv_opt == 2){
      ipc_write_line(c2s, MSG_JOIN);
    } else {
        if(world == WORLD_WRAP){
        snprintf(start_msg, sizeof(start_msg),"%s mode=%d world=%d limit=%d w=%d h=%d",
            MSG_START, mode, world, limit_ms, w, h);
    } else {
      snprintf(start_msg, sizeof(start_msg), "%s mode=%d world=%d limit=%d",
               MSG_START, mode, world, limit_ms);
      }
      ipc_write_line(c2s, start_msg);
    }

  client_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  if(world == WORLD_WALLS){
    ctx.last.height = MAP_H;
    ctx.last.width = MAP_W;
    ctx.last.world = WORLD_WALLS;
    ctx.last.mode = MODE_STANDARD;
    load_map_obstacles(&ctx.last, "maps/map1.txt");
  }else{
    ctx.last.width = w;
    ctx.last.height = h;
    ctx.last.world = WORLD_WRAP;
    ctx.last.mode = MODE_TIME;
    for(int i = 0; i < MAP_H; i++){
      for(int j = 0; j < MAP_W; j++){
        ctx.last.obstacles[i][j] = 0;
      }
    }
  }

  ctx.c2s_fd = c2s;
  ctx.s2c_fd = s2c;
  ctx.running = 1;
  pthread_mutex_init(&ctx.lock, NULL);
  //terminal init, hide cursor + clear once
  printf("\033[?25l\033[2J");
  fflush(stdout);

  pthread_t t;
  if(pthread_create(&t, NULL, rx_thread, &ctx) != 0){
    perror("nepodarilo sa vytvorit vlakno");
    close(c2s);
    close(s2c);
    return 7;
  }
  pthread_t tr;
  if(pthread_create(&tr, NULL, render_thread, &ctx) != 0){
    perror("nepodarilo sa vytvorit vlakno");
    return 8;
  }

  printf("From server: %s\n", line); //READY

  printf("Controls: w/a/s/d, p = pause/resume ,x = disconnect,q = quit\n");

  while(1){
    pthread_mutex_lock(&ctx.lock);
    int run = ctx.running;
    pthread_mutex_unlock(&ctx.lock);
    if(!run) break;
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int pr = poll(&pfd, 1, 100);
    if(pr < 0){
      if(errno == EINTR) continue;
      perror("poll stdin");
      break;
    }
    if(pr == 0){
      continue;
    }

  if(pfd.revents & POLLIN){
    //printf("> ");
    char cmd[32];
    if (!fgets(cmd, sizeof(cmd), stdin)) break;
    cmd[strcspn(cmd, "\n")] = 0;

    if(strcmp(cmd, "x") == 0){
      ipc_write_line(c2s, MSG_DISCONNECT);
      break;
    }

    if(strcmp(cmd, "q") == 0){
      ipc_write_line(c2s, MSG_QUIT);
      break;
    }

    if(strcmp(cmd, "p") == 0){
      ipc_write_line(c2s, MSG_PAUSE);
      continue;
    }

      //poslem spravu serveru
    if(cmd[0] == 'w' || cmd[0] == 'a' || cmd[0] == 's' || cmd[0] == 'd'){
      char msg[64];
      snprintf(msg, sizeof(msg), "%s %s", MSG_INPUT, cmd);
      ipc_write_line(c2s, msg);
    }
  }
  }
  pthread_mutex_lock(&ctx.lock);
  ctx.running = 0;
  pthread_mutex_unlock(&ctx.lock);

  close(c2s);
  close(s2c);
  
  pthread_join(t, NULL);
  pthread_join(tr, NULL);
  pthread_mutex_destroy(&ctx.lock);
  term_restore();
  printf("\033[2J\033[H");
  fflush(stdout);
  }
  return 0;
}
