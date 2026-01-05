/*
 * client/render.h
 * ---------------
 * Rendering interface for the terminal Snake client
 *
 * Responsibilities:
 *  - Declare render_draw(), a single function that draws one "frame" of the game 
 *    (ASCII + HUD) using ANSI escape codes
 *
 * Notes:
 *  - Rendering is kept separate from IPC and game logic
 *  - The client render thread calls render_draw() repeatedly (~60 FPS)
 * */

#ifndef RENDER_H
#define RENDER_H

#include "../common/game_state.h"
#include <stdio.h>

/*
 * Renders the current game state into an ASCII "frame" and prints it to the terminal
 *
 * Implementation details:
 *  - Uses an off-screen buffer (canvas) and prints it in one fwrite() call to reduce flicker
 *  - Uses ANSI cursor-home (\033[H) so the terminal does not scroll
 * */
void render_draw(FILE *out, const game_state_t *g);

#endif
