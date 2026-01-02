#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>

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

//funkcia na vratenie kurzora
static void term_restore(void){
  printf("\033[?25h\n"); // ukaz kurzor
  fflush(stdout);
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

static void *rx_thread(void *arg){
  client_ctx_t *ctx = (client_ctx_t*)arg;
  char line[4096];

  while(1){
    ssize_t n = ipc_read_line(ctx->s2c_fd, line, sizeof(line));
    if(n <= 0){
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
      if(parsed_ok){
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

int main(void) {
  printf("CLIENT started\n");
  printf("CLIENT PID: %d\n", getpid());

  printf("Menu:\n1. New game\n2. Exit\n");
  int choice = 0;
  scanf("%d", &choice);
  int c;
  while((c = getchar()) != '\n' && choice != EOF) {}
  if(choice != 1){
    printf("Bye\n");
    return 0;
  }

  int world = WORLD_WRAP;
  int mode = MODE_TIME;
  int limit_ms = 30000;

  printf("Vyber si rezim:\n");
  printf("1) TIME (prazdna mapa, WRAP)\n");
  printf("2) WALLS (prekazky zo suboru, bez WRAP)\n");
  int opt = 1;
  scanf("%d", &opt);

  if(opt == 1){
    world = WORLD_WRAP;
    mode = MODE_TIME;
    int sec = 30;
    printf("Zadaj casovy limit v sekundach:");
    scanf("%d", &sec);
    if(sec < 1) sec = 30;
    limit_ms = sec * 1000;
  }else{
    world = WORLD_WALLS;
    mode = MODE_STANDARD; // kym nenanbura
    limit_ms = 0;
  }

  int i;
  while((i = getchar()) != '\n' && i != EOF){}

  start_server_process();
  //pockaj 200ms kym server spravi fifo
  usleep(200 * 1000);

  // klient sa pripoji na FIFO
  int c2s = ipc_open_c2s_write(); //klient pise na server
  if(c2s < 0){
    perror("open c2s write");
    return 4;
  }

  int s2c = ipc_open_s2c_read(); //caka na server read
  if(s2c < 0){
    perror("open s2c read");
    close(c2s);
    return 5;
  }

  //handshake
  char line[256];

   if( ipc_read_line(s2c, line, sizeof(line)) <= 0 || strcmp(line, MSG_READY) != 0){
    fprintf(stderr, "Server neposlal READY");
    //zatvaram fifa
    close(c2s);
    close(s2c);
    return 6;
  }

  char start_msg[128];
  snprintf(start_msg, sizeof(start_msg),"%s mode=%d world=%d limit=%d",
            MSG_START, mode, world, limit_ms
           );
  ipc_write_line(c2s, start_msg);

  client_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  if(world == WORLD_WALLS){
    ctx.last.height = MAP_H;
    ctx.last.width = MAP_W;
    ctx.last.world = WORLD_WALLS;
    ctx.last.mode = MODE_STANDARD;
    load_map_obstacles(&ctx.last, "maps/map1.txt");
  }else{
    ctx.last.width = 20;
    ctx.last.height = 10;
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
  atexit(term_restore);

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

  printf("Controls: w/a/s/d, p = pause/resume ,q = quit\n");

  while(1){
    pthread_mutex_lock(&ctx.lock);
    int run = ctx.running;
    pthread_mutex_unlock(&ctx.lock);
    if(!run) break;

    //printf("> ");
    char cmd[32];
    if (!fgets(cmd, sizeof(cmd), stdin)) break;
    cmd[strcspn(cmd, "\n")] = 0;

    if(strcmp(cmd, "q") == 0){
      ipc_write_line(c2s, MSG_DISCONNECT);
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

  pthread_mutex_lock(&ctx.lock);
  ctx.running = 0;
  pthread_mutex_unlock(&ctx.lock);

  close(c2s);
  close(s2c);

  pthread_join(t, NULL);
  pthread_join(tr, NULL);

  return 0;
}
