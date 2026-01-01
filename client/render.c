#include "render.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void render_draw(FILE *out, const game_state_t *g){
  const int W = g->width;
  const int H = g->height;

  const int row = (W+2)+1;
  const int rows = (H+2);
  const int  canvas_sz = rows * row + 128;

  char *buf = (char*)malloc(canvas_sz);
  if(!buf) return;

  //default vypln medzerami
  memset(buf, ' ', canvas_sz);

  //newline na konci riadkov
  for (int i = 0; i< rows; i++) {
    buf[i*row + (W + 2)] = '\n';
  }

  //horny a dolny ram
  for(int i = 0; i < W+2; i++){
    buf[0*row + i] = '#';
    buf[(rows-1)*row + i] = '#';
  }
  //bocne ramy
  for(int i = 1; i < rows; i++){
    buf[i*row + 0] = '#';
    buf[i*row + (W + 1)] = '#';
  }
  //jedlo
  if(g->food.x >= 0 && g->food.x < W && g->food.y >=0 && g->food.y < H){
    int ox = g->food.x + 1;
    int oy = g->food.y + 1;
    buf[oy*row + ox] = '*';
  }
  //snake
  for(size_t i = 0; i < g->snake_len; i++){
    int sx = g->snake[i].x;
    int sy = g->snake[i].y;
    if(sx < 0 || sx >= W || sy < 0 || sy >= H) continue;

    int ox = sx + 1;
    int oy = sy + 1;
    buf[oy*row + ox] = (i == 0) ? '@' : 'o';
  }
  //HUD
  int used = rows * row;
  int n = snprintf(buf + used, canvas_sz - used,
                   "score=%d  status=%d time=%d/%d ms\n",
                   g->score, g->status, g->elapsed_ms, g->time_limit_ms
                   );
  if(n < 0) n = 0;
  used += n;

  fprintf(out, "\033[H");
  fwrite(buf, 1, used, out);
  fflush(out);

  free(buf);
}
