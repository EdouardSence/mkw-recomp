// The start screen's background (ui/launcher/launcher_backdrop.cpp).
#pragma once
#include <SDL3/SDL.h>

struct ImDrawList;
void backdrop_load(SDL_Renderer *ren, const char *exe_dir);   // splash.png next to the program, if there is one
void backdrop_draw(ImDrawList *dl, float w, float h);         // the splash scaled to cover, else the road
void backdrop_free(void);
