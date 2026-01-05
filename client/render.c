#include "render.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * client/render.c
 * ---------------
 * ASCII renderer for the Snake client
 *
 * Responsibilities:
 *  - Convert game_state_t into an ASCII frame:
 *      - border '#'
 *      - food '*'
 *      - snake head '@', body 'o'
 *      - obstacles '#' when WORLD_WALLS
 *  - Print the frame efficiently without flicker (off-screen canvas)
 *  - Print a HUD line (score, status, and time in MODE_TIME)
 *
 *  - AI-assisted consultation:
 *    - Obstacle rendering mapping [y][x] and avoiding double borders were debugged       (final code integrated  by the author)
 * */
void render_draw(FILE *out, const game_state_t *g){
  const int W = g->width;
  const int H = g->height;

  /*
   *  Rendered coordinate system:
   *    - game coords are [0..W-1][0..H-1]
   *    - we render with a 1-cell border around, so:
   *        - rendered_x = x + 1;
   *        - rendered_y = y + 1;
   *
   *  Each rendered row has:
   *    - (W+2) characters: left border + W cells + right border
   *    - 1 newline cahracter
   *    - 'row' is the stride (bytes) between starts of consecutive rows
   * */
  const int row = (W+2)+1;
  const int rows = (H+2);
  /*
   *  Canvas holds the whole frame + extra space for HUD text at the end
   *  The +128 is a simple safety margin for the HUD line(s)
   * */
  const int  canvas_sz = rows * row + 128;

  char *buf = (char*)malloc(canvas_sz);
  if(!buf) return;

  // Default fill everything with spaces
  memset(buf, ' ', canvas_sz);

  // Newline at the end of rows so its printed properly
  for (int i = 0; i< rows; i++) {
    buf[i*row + (W + 2)] = '\n';
  }

  // Top and bottom border
  for(int i = 0; i < W+2; i++){
    buf[0*row + i] = '#';
    buf[(rows-1)*row + i] = '#';
  }
  // Left and right border
  for(int i = 1; i < rows; i++){
    buf[i*row + 0] = '#';
    buf[i*row + (W + 1)] = '#';
  }
  /*obstacles drawing for WORLD WALLS
   * obstacles stored as g->obstacles[y][x]
   * we skip border cells because it has been already drawn
   * AI help: had problem with double border and had swapped mapping of obstacles [x][y]*/
  if(g->world == WORLD_WALLS){
    for(int y = 0; y < H; y++){
      for(int x = 0; x < W; x++){
        if(y < MAP_H && x < MAP_W && g->obstacles[y][x]){
          //skipe the border cells already draw above
          if(x == 0 || y == 0 || x == W-1 || y == H-1) continue;
          int ox = x + 1;
          int oy = y + 1;
          buf[oy*row + ox] = '#';
        }
      }
    }
  }
  // Food only inside the bounds
  if(g->food.x >= 0 && g->food.x < W && g->food.y >=0 && g->food.y < H){
    int ox = g->food.x + 1;
    int oy = g->food.y + 1;
    buf[oy*row + ox] = '*';
  }
  // Drawing of snake segments first is head '@' rest is body 'o'
  for(size_t i = 0; i < g->snake_len; i++){
    int sx = g->snake[i].x;
    int sy = g->snake[i].y;
    if(sx < 0 || sx >= W || sy < 0 || sy >= H) continue;

    int ox = sx + 1;
    int oy = sy + 1;
    buf[oy*row + ox] = (i == 0) ? '@' : 'o';
  }
  /*
 * AI-ASSISTED: Rendering approach.
 * - Draw into an off-screen buffer (avoids flicker)
 * - Append HUD safely using snprintf(buf+used, remaining)
 * - Use ANSI cursor-home to redraw the frame in-place (no scrolling)
 */
  /*
   *  HUD text printed after the frame
   *  In MODE_STANDARD (walls), time is irrelevant so it is not shown
   * */
  int used = rows * row;
  int n;
  if(g->mode == MODE_TIME){  
    n = snprintf(buf + used, canvas_sz - used,
                   "score=%d  status=%d time=%d/%d ms\n",
                   g->score, g->status, g->elapsed_ms, g->time_limit_ms
                   );
  }else{
    n = snprintf(buf + used, canvas_sz - used,
                 "score=%d  status=%d\n",
                  g->score, g->status
                 );
  }
  /*
   *  snprintf(buf, max, "%s\n"%s) returns:
   *    - number of chars that *would* be written (excluding '\0')
   *    - negative value on encoding error
   *
   *  If n < 0, we just skip then HUD (used stays minimal)
   * */
  if(n < 0) n = 0;
  used += n;
  /*
   *  ANSI: move cursor to home position (top-left) and redraw the entire frame
   *    -This makes animation stable without scrolling the terminal
   * */
  fprintf(out, "\033[H");
  fwrite(buf, 1, used, out);
  fflush(out);

  free(buf);
}
