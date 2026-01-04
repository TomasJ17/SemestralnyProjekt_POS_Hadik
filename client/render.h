#ifndef RENDER_H
#define RENDER_H

#include "../common/game_state.h"
#include <stdio.h>

/*
 * Renders the current game state into an ASCII "frame" and prints it to the terminal.
 * The function is designed to be called repeatedly (e.g ~60 FPS) and uses ANSI
 * escape codes to redraw in place
 * */
void render_draw(FILE *out, const game_state_t *g);

#endif
