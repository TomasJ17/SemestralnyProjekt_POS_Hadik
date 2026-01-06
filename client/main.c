#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/poll.h>
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


/*
 * client/main.c
 * -------------
 * Terminal client for the Snake game using POSIX named FIFOs (see ipc/)
 *
 * Responsibilities:
 *  - Optionally start/stop the server process (fork/exec)
 *  - Connect to an existing or newly started server via FIFOs 
 *  - Perform handshake (READY -> START/JOIN)
 *  - Receive game state updates from the server in a dedicated RX thread
 *  - Render the last received state in a dedicated render thread (~60 FPS)
 *  - Read user input (w/a/s/d, pause, disconnect, quit) using poll() to avoid blocking
 *
 * Concurrency model:
 *  - RX thread updates ctx->last state under a mutex
 *  - Render thread copies ctx->last under the mutex and draws without holding
 *  - Main thread sends player commands; it checks ctx->running to terminate cleanly
 * AI-assisted consultation (project-owned code):
 *  - FIFO connect/retry + join/disconnect lifecycle corner cases were debugged with AI
 *  (the final integration and logic decisions were implemented by the author)
 * */


/*
 * Shared client context between threads
 * last/has_state/running are protected by lock
 * */
typedef struct{
  int c2s_fd;             // FIFO client -> server (write)
  int s2c_fd;             // FIFO server -> client (read)

  pthread_mutex_t lock;   // protects fields below
  game_state_t last;      // last received state from server
  int has_state;          // wherher last contains a valid state
  
  int running;            // global stop flag for threads

  int walls_map_loaded;   // local helper: map file loaded (WORLD_WALLS rendering)
} client_ctx_t;

/*
 * Starts the server as a child process (fork + exec)
 * Parent continues as the client
 * */
static void start_server_process(void){
  pid_t pid = fork();
  if( pid < 0){
    perror("fork");
    exit(2);
  }
  if(pid ==  0){
    // Child replaces itself with the server executable
    execl("./server/server", "./server/server", (char*)NULL);
    perror("execl");
    exit(3);
  }
  //Parent continues as client
}

/*
 * Optional helper: checks if something is listening on FIFO_C2S
 * Opens non-blocking write end; if it succeeds, server is likely running
 * */
static int server_is_running(void){
  int fd = ipc_open_c2s_write_nb();
  if(fd >= 0){
    close(fd);
    return 1;
  }
  /*
   * Typical cases:
   *  - ENOENT: FIFO doesnt exist -> no server
   *  - ENXIO: FIFO exists but no reader -> no server
   * */
  if(errno == ENOENT) return 0;
  if(errno == ENXIO) return 0;
  return 0;
}

/*
 * Restore terminal state (show cursor)
 * Registered via atexit() so it also runs on normal terminantion paths
 * */
static void term_restore(void){
  printf("\033[?25h\n"); // ukaz kurzor
  fflush(stdout);
}

/*
 * Sleep helper in ms
 * */
static void ms_sleep(int ms){
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

/*
 * Simple parsing helpers for the textual STATE line
 * The server sends one line per tick:
 *    -STATE status=... alive=... score=... time=elapsed/limit w=.. h=.. <snake list...>
 *
 * Helper: finds substring a returns pointer on address behind it, else NULL*/
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

/*
 * Parses snake body list from the tail of the STATE message.
 * Expected format (after food=[x,y]):
 *  x,y:x,y:x,y
 *
 * We tokenize and fill g->snake[], g->snake_len 
 * */
static int parse_snake_list(const char *p, game_state_t *g){
  g->snake_len = 0;
  char buf[4096];
  strncpy(buf, p, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char *saveptr = NULL;
  // Token separators: ';', '|', spaces, and line breaks
  // I am using ',' but replacing for ' '
  char *tok = strtok_r(buf, ";| \r\n", &saveptr);

  while(tok && g->snake_len < MAX_SNAKE_LEN){
    // Strip optional brackets or parentheses
    while(*tok == '[' || *tok == '(') tok++;
    // Normalize token "x,y" -> "x y" for sscanf
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

/*
 * Parse full STATE line into the provided game_state_t
 * Only the parts needed by renderer are stored (dimensions, food, snake, status, ...)
 * */
static int parse_state_line_into(game_state_t *g, const char *line){
  //expecting new state
  //STATE status,alive,score,time,w,h,food,len,body
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
  // Dimensions are included for WORLD_WRAP mode
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

  // Snake list begins after "food=[...]" closing bracket
  const char *foodp = strstr(line, "food=[");
  if(!foodp) return 0;

  const char *after = strchr(foodp, ']');
  if(!after) return 0;
  after++; // move past ']'
  while(*after == ' ') after++;

  return parse_snake_list(after, g);
}

/*
 * Loads obstacle map from a text file int g->obstacles[y][x]
 * Map file format:
 *  - MAP_H lines
 *  - each line has MAP_W characters
 *  - '#' means obstacle, anything else is free
 * */
static int load_map_obstacles(game_state_t *g, const char *path){
  FILE *f = fopen(path, "r");
  if(!f){
    perror("cannot open file with map");
    return 0;
  }

  //Initialization, clear to 0 first (free cells)
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

/*
 * RX thread: blocks on server->client FIFO and updates ctx->last
 * Terminates when:
 *  - FIFO closes (EOF) or error
 *  - server sends BYE
 *  - a GAME_OVER state is detected
 * */
static void *rx_thread(void *arg){
  client_ctx_t *ctx = (client_ctx_t*)arg;
  char line[4096];

  while(1){
    ssize_t n = ipc_read_line(ctx->s2c_fd, line, sizeof(line));
    //fprintf(stderr, "client rx raw: %s\n", line);
    if(n <= 0){
      // EOF / error -> server is gone
      pthread_mutex_lock(&ctx->lock);
      ctx->running = 0;
      pthread_mutex_unlock(&ctx->lock);
      break;
    }

    if(strcmp(line, MSG_BYE) == 0){
      // Server requests clean termination
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
        /*
         * Determine world type:
         *  - WORLD_WALLS uses fixes MAP_W x MAP_H(defined in game_state.h) and obstacles from file
         *  - WORLD_WRAP uses custom w/h and no obstacles
         * NOTE: The server also sets world/mode, but the client infers it here based on dimensions
         * */
        int is_walls = (ctx->last.width == MAP_W && ctx->last.height == MAP_H);
        if(is_walls){
          ctx->last.world = WORLD_WALLS;
          ctx->last.mode = (ctx->last.time_limit_ms > 0) ? MODE_TIME : MODE_STANDARD;
          // Load map obstacles once to avoid re-reading the file each tick
          if(!ctx->walls_map_loaded){
            if(!load_map_obstacles(&ctx->last, "maps/map1.txt")){
              // On failure, fall back to empty obstacle
              for(int i = 0; i < MAP_H; i++){
                for(int j = 0; j < MAP_W; j++){
                  ctx->last.obstacles[i][j] = 0;
                }
              }
            }
            ctx->walls_map_loaded = 1;
          }
        }else{
          // WORLD_WRAP: ensure obstacles are cleared (renderer expects empty obstacle)
          ctx->last.world = WORLD_WRAP;
          ctx->last.mode = (ctx->last.time_limit_ms > 0) ? MODE_TIME : MODE_STANDARD;
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

/*
 * Render thread: draws the most recent state
 * Uses a local copy to minimaze time holding the mutex
 * */
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
    ms_sleep(16); // ~60 fps
  }
  return NULL;
}

/*
 * Stops any existing server by connecting and sending QUIT
 *
 * Why this exists:
 *  - The project uses fixed FIFO paths in tmp/, so only one server instance should exist
 *  - If a previous run crashed, FIFOs might exist and a server might still be alive
 *
 *  Strategy:
 *    - Open both FIFOs (non-blocking + retries) to avoid deadlocks on open()
 *    - Wait for READY, then send a quick START and QUIT
 *    - Wait a bit for server to unlink() the FIFOs
 *
 *  AI-assisted consultation:
 *    - FIFO open semantics and retry patterns were consulted during debugging
 * */
static void stop_existing_server(void){
  // If fifo doesnt exist there is nothing to kill(stop)
  if(access(FIFO_C2S, F_OK) != 0 || access(FIFO_S2C, F_OK) != 0){
    return;
  }
  int c2s = -1;
  int s2c = -1;

  // Non-blocking retry: close both sides, so server is not left on open()
  for(int i=0; i < 40; i++){
    c2s = ipc_open_c2s_write_nb();
    if(c2s >= 0) break;
    if(errno == ENOENT || errno == ENXIO){
      usleep(50 * 1000);
      continue;
    }
    // Another(unexpected) error
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
  // Switch for blocking, so read_line is working properly (waits for newline)
  ipc_set_blocking(c2s, 1);
  ipc_set_blocking(s2c, 1);

  // Handshake: server sends READY after connection and waits for START
  char line[256];
  int got_ready = 0;
  for(int i=0; i < 40; i++){
    struct pollfd pfd = {.fd = s2c, .events = POLLIN, .revents = 0};
    int pr = poll(&pfd, 1, 50);
    if(pr < 0){
      if(errno == EINTR) continue;
      break;
    }
    if(pr == 0) continue;
    ssize_t n = ipc_read_line(s2c, line, sizeof(line));
    if(n <= 0) continue;
    if(strcmp(line, MSG_READY) == 0){
      got_ready = 1;
      break;
    }
    if(starts_with(line, MSG_STATE)) continue;
  }
    // Send a minimal START to satisfy the server logic, then QUIT immediatly 
  if(got_ready){
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
  // Wait some period, so server can make unlink FIFOs
  for(int i = 0; i < 40; i++){
    if(access(FIFO_C2S, F_OK) != 0 && access(FIFO_S2C, F_OK) != 0) break;
    usleep(50 * 1000);
  }
}
/*
 * Connect to server with retry logic
 *  
 * IMPORTANT:
 *  - FIFO open() can block depending on which side opens first
 *  - We open C2S in non-blocking mode first to avoid deadlock
 *  - After both ends are open, we switch C2S to blocking mode for steady-state
 * */
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
    
    // s2c read can be blocking, server should have already opened the write end
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
    
    // Switch to blocking for stable write
    ipc_set_blocking(c2s, 1);

    *out_c2s = c2s;
    *out_s2c = s2c;
    return 0;
  }
  return -1;
}

/*
 * Main: user menu + connection lifecycle
 *
 * Input notes:
 *  - scanf() leaves '\n' in stdin; we "drain" it with getchar() loops
 *  - Later we use poll() + fgets() for interactive controls during gameplay
 * */
int main(void) {
  printf("CLIENT started\n");
  printf("CLIENT PID: %d\n", getpid());
  atexit(term_restore);
  printf("Menu:\n1. New game\n2. Exit\n");
  int choice = 0;
  scanf("%d", &choice);
  // Drain leftover newline after scanf
  int c;
  while((c = getchar()) != '\n' && c != EOF) {}
  if(choice != 1){
    //stop_existing_server();
    printf("Bye\n");
    return 0;
  }
while(1){
  printf("Server:\n1) New server (new game)\n2) Join existing game\n3) Exit\n4) Force kill server process(admin)\n");
  int serv_opt = 1;
  scanf("%d", &serv_opt);
  // Drain newline
  int ch;
  while((ch = getchar()) != '\n' && ch != EOF) {};

  int c2s = -1;
  int s2c = -1;
  if(serv_opt == 4){
    stop_existing_server();
    printf("Force kill done (if server was running)\n");
    continue;
  }
  if(serv_opt == 3){
    //stop_existing_server();
    break;  
  }
  // Default configuration values
  int world = WORLD_WRAP;
  int mode = MODE_TIME;
  int limit_ms = 30000;
  int w = 20, h = 10;

  if(serv_opt == 1){
      // WORLD slection
    int world_choice = 0;
    printf("Vyber si svet:\n");
    printf("1) WRAP (prazdna mapa, WRAP)\n");
    printf("2) WALLS (prekazky zo suboru, bez WRAP)\n");
    printf("3) EXIT\n");

    scanf("%d", &world_choice);
    // Drain newline
    int cr;
    while((cr = getchar()) != '\n' && cr != EOF) {};
    if(world_choice == 3){
      continue;
    }
    if(world_choice == 1) world = WORLD_WRAP;
    else if(world_choice == 2) world = WORLD_WALLS;
    else {
      printf("Zla volba\n");
      continue;
    }
      // MODE selection
    int mode_choice = 0;
    printf("Vyber si rezim:\n");
    printf("1) TIME (hra skonci po vyprsani casu)\n");
    printf("2) STANDARD (hra skonci po 10s idle bez hraca)\n");
    printf("3) EXIT\n");
    scanf("%d", &mode_choice);
    int dr;
    while((dr = getchar()) != '\n' && dr != EOF) {}
    if(mode_choice == 3) continue;
    if(mode_choice == 1) mode = MODE_TIME;
    else if(mode_choice == 2) mode = MODE_STANDARD;
    else{
      printf("Zla volba\n");
      continue;
    }
    if(mode == MODE_TIME){
      int sec = 30;
      printf("Zadaj casovy limit v sekundach (0 = spat): ");
      scanf("%d", &sec);
      int aa;
      while((aa = getchar()) != '\n' && aa != EOF) {}
      if(sec == 0) continue;
      if(sec < 1) sec = 30;
      limit_ms = sec * 1000;
    }else{
      limit_ms = 0; // STANDARD
    }
    if(world == WORLD_WRAP){
      printf("Zadaj sirku sveta (10-80): ");
      scanf("%d", &w);
      int bb;
      while((bb = getchar()) != '\n' && bb != EOF) {}

      printf("Zadaj vysku sveta (10-40): ");
      scanf("%d", &h);
      int cc;
      while((cc = getchar()) != '\n' && cc != EOF) {}
      if(w < 10) w = 10;
      if(w > 80) w = 80;
      if(h < 10) h = 10;
      if(h > 40) h = 40;
    }else{
      //WALLS fixed size
      w = MAP_W;
      h = MAP_H;
    }


  // Enforce "single player": stop old one, then start a fresh one
  //stop_existing_server();

  //
  if(server_is_running()){
    printf("Server bezi. Nemozes spustit dalsi server. Pouzi JOIN alebo pockaj kym server dobehne");
    continue;
  }
  start_server_process();
  ms_sleep(200); //to create FIFO in time
}

  if(connect_with_retry(&c2s, &s2c) != 0){
    if(serv_opt == 2) fprintf(stderr, "No existing game to join\n");
    else fprintf(stderr, "Cannot connect to new server\n");
    continue;
  }

  // Handshake: server sends READY first
  char line[256];

   if(ipc_read_line(s2c, line, sizeof(line)) <= 0 || strcmp(line, MSG_READY) != 0){
    fprintf(stderr, "Server neposlal READY");
    // CLosing FIFOs
    close(c2s);
    close(s2c);
    continue;
    }

    // Send JOIN or START configuration message
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

  // Initialization of local state (useful before first STATE arrives)
  client_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  if(world == WORLD_WALLS){
    ctx.last.height = MAP_H;
    ctx.last.width = MAP_W;
    ctx.last.world = WORLD_WALLS;
    ctx.last.mode = mode;
    load_map_obstacles(&ctx.last, "maps/map1.txt");
  }else{
    ctx.last.width = w;
    ctx.last.height = h;
    ctx.last.world = WORLD_WRAP;
    ctx.last.mode = mode;
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
  // Terminal init, hide cursor + clear once
  printf("\033[?25l\033[2J");
  fflush(stdout);

  // Start RX and render threads
  pthread_t t;
  if(pthread_create(&t, NULL, rx_thread, &ctx) != 0){
    perror("nepodarilo sa vytvorit vlakno");
    close(c2s);
    close(s2c);
    pthread_mutex_destroy(&ctx.lock);
    return 7;
  }
  pthread_t tr;
  if(pthread_create(&tr, NULL, render_thread, &ctx) != 0){
    perror("nepodarilo sa vytvorit vlakno");
    
    pthread_mutex_lock(&ctx.lock);
    ctx.running = 0;
    pthread_mutex_unlock(&ctx.lock);

    close(c2s);
    close(s2c);

    pthread_join(t, NULL);
    pthread_mutex_destroy(&ctx.lock);
    term_restore();
    printf("\033[2J\033[H");
    fflush(stdout);

    return 8;
  }

  printf("From server: %s\n", line); //READY

  printf("Controls: w/a/s/d, p = pause/resume ,x = disconnect,q = quit\n");

  int exit_client = 0;
  /*
   * Gameplay input loop:
   *  - Uses poll(STDIN) with a timeout to avoid blocking forever
   *  - When key is available, reads full line via fgets
   * */
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
      if(errno == EINTR) continue; // interrupted by signal, retry
      perror("poll stdin");
      break;
    }
    if(pr == 0){
      // timeout, no input -> continue main loop
      continue;
    }

  if(pfd.revents & POLLIN){
    //printf("> ");
    char cmd[32];
    // fghets() reads a full line including '\n'
    if (!fgets(cmd, sizeof(cmd), stdin)) break;
    // Remove trailing newline for easy comparisons
    cmd[strcspn(cmd, "\n")] = 0;

    if(strcmp(cmd, "x") == 0){
      ipc_write_line(c2s, MSG_DISCONNECT);
      break;
    }

    if(strcmp(cmd, "q") == 0){
      //FIX
      //ipc_write_line(c2s, MSG_QUIT);
      // q - quits client, server just disconnected/paused
      ipc_write_line(c2s, MSG_DISCONNECT);
      exit_client = 1;
      break;
    }

    if(strcmp(cmd, "p") == 0){
      ipc_write_line(c2s, MSG_PAUSE);
      continue;
    }

      // Direction input send to server as "INPUT <char>"
    if(cmd[0] == 'w' || cmd[0] == 'a' || cmd[0] == 's' || cmd[0] == 'd'){
      char msg[64];
      snprintf(msg, sizeof(msg), "%s %s", MSG_INPUT, cmd);
      ipc_write_line(c2s, msg);
    }
  }
  }
  // Stop threads and cleanup session
  pthread_mutex_lock(&ctx.lock);
  ctx.running = 0;
  pthread_mutex_unlock(&ctx.lock);

  close(c2s);
  close(s2c);
  
  pthread_join(t, NULL);
  pthread_join(tr, NULL);
  pthread_mutex_destroy(&ctx.lock);
  if(exit_client) return 0;
  term_restore();
  // Clear screen and move cursor home for clean menu redraw
  printf("\033[2J\033[H");
  fflush(stdout);
  }
  return 0;
}
