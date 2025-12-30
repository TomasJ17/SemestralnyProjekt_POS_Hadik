#include "render.h"
#include <stdio.h>

static int is_snake_cell(const game_state_t *g, int x, int y, int *is_head){
  for(int i = 0; i < g->snake_len; i++){
    if(g->snake[i].x == x && g->snake[i].y == y){
      if(is_head) *is_head = (i == 0);
      return 1;
    }
  }
  if(is_head) *is_head = 0;
  return 0;
}

void render_draw(FILE *out, const game_state_t *g){
  fprintf(out, "\033[2J\033[H");
  
  for(int i = 0; i < g->width + 2; i++) fputc('#', out);
  fputc('\n',out);
  
  for(int y = 0; y < g->height; y++){
      fputc('#', out);
    for (int x = 0; x < g->width; x++) {
      if(g->food.x == x && g->food.y == y){
        fputc('*', out);
        continue;
      } 
      int is_head = 0;
      if(is_snake_cell(g,x,y,&is_head)){
        fputc(is_head ? '@' : 'o', out);
      } else{
        fputc(' ', out);
      }
    }
    fputc('#', out);
    fputc('\n', out);
  }

  for(int i = 0; i < g->width + 2; i++) fputc('#', out);
  fputc('\n',out);

  fprintf(out, "score = %d  status = %d  time=%d/%d ms\n",
          g->score, g->status, g->elapsed_ms, g->time_limit_ms
          );
  //okamzite vypisanie out
  fflush(out);
}
