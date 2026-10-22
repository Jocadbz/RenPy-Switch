#ifndef GAME_PICKER_H
#define GAME_PICKER_H

#include <SDL2/SDL.h>
SDL_Texture *load_selected_game_splash(SDL_Renderer *renderer);

/* Native boot-time game picker for the Ren'Py loader.

   Scans sdmc:/renpy/ for valid Ren'Py .apk / .zip archives (an .apk contains
   assets/x-game/, a .zip contains a <dir>/game/ tree), extracts each game's
   gui/window_icon.png, and shows a 256x256 icon grid with the title beneath.
   On selection it sets the env vars renpy.loader.find_switch_archive() reads:
       RENPY_SWITCH_GAME_ARCHIVE, RENPY_SWITCH_GAME_KIND, RENPY_SWITCH_GAME_ROOT
   and returns 1. Returns 0 if no games were found / nothing was picked. */
int game_picker_run(void);

#endif
