// The start screen's background (launcher_backdrop.h): the player's splash.png, else a road drawn by code. Split from
// launcher.cpp, which keeps the pages.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>

#include "imgui.h"
#include "ui/launcher/launcher_backdrop.h"

extern "C" {
#include "platform/platform.h"
}

static SDL_Texture *g_splash;   // splash.png next to the executable, the player's own picture
static int g_splash_w, g_splash_h;

void backdrop_load(SDL_Renderer *ren, const char *exe_dir) {
    char path[1100];
    snprintf(path, sizeof path, "%s/splash.png", exe_dir);
    if (!plat_file_exists(path)) return;
    SDL_Surface *s = SDL_LoadPNG(path);
    if (!s) return;
    g_splash = SDL_CreateTextureFromSurface(ren, s);
    g_splash_w = s->w, g_splash_h = s->h;
    SDL_DestroySurface(s);
}

void backdrop_free(void) {
    if (g_splash) SDL_DestroyTexture(g_splash);
    g_splash = nullptr;
}

// ---- the background: the player's splash.png scaled to cover the window, else a road drawn in perspective (no art of
// the game ships with the port)
void backdrop_draw(ImDrawList *dl, float w, float h) {
    if (g_splash) {
        float k = fmaxf(w / g_splash_w, h / g_splash_h), sw = g_splash_w * k, sh = g_splash_h * k;
        dl->AddImage((ImTextureID)(intptr_t)g_splash, ImVec2((w - sw) / 2, (h - sh) / 2), ImVec2((w + sw) / 2, (h + sh) / 2));
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w * 0.6f, h), IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 0),
                                    IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 170));
        return;
    }
    float hy = h * 0.42f;   // the horizon
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, hy), IM_COL32(18, 40, 70, 255), IM_COL32(18, 40, 70, 255),
                                IM_COL32(236, 160, 84, 255), IM_COL32(236, 160, 84, 255));
    dl->AddRectFilledMultiColor(ImVec2(0, hy), ImVec2(w, h), IM_COL32(24, 70, 44, 255), IM_COL32(24, 70, 44, 255),
                                IM_COL32(10, 30, 20, 255), IM_COL32(10, 30, 20, 255));
    float vx = w * 0.62f;   // the vanishing point, right of the buttons
    dl->AddQuadFilled(ImVec2(vx - 4, hy), ImVec2(vx + 4, hy), ImVec2(w * 1.15f, h), ImVec2(w * 0.10f, h),
                      IM_COL32(52, 54, 60, 255));
    float t = (float)fmod(ImGui::GetTime() * 0.6, 1.0);
    for (int i = 0; i < 12; i++) {   // the centre dashes, moving towards the viewer
        float a = (i + t) / 12.0f, b = (i + t + 0.45f) / 12.0f;
        float ya = hy + (h - hy) * a * a, yb = hy + (h - hy) * b * b;
        float xa = vx + (w * 0.625f - vx) * a * a, xb = vx + (w * 0.625f - vx) * b * b;
        float wa = 1 + 10 * a * a, wb = 1 + 10 * b * b;
        dl->AddQuadFilled(ImVec2(xa - wa, ya), ImVec2(xa + wa, ya), ImVec2(xb + wb, yb), ImVec2(xb - wb, yb),
                          IM_COL32(238, 194, 84, 255));
    }
    for (int side = 0; side < 2; side++) {   // the kerbs, red and white
        float ex = side ? w * 1.15f : w * 0.10f;
        for (int i = 0; i < 16; i++) {
            float a = i / 16.0f, b = (i + 1) / 16.0f;
            float ya = hy + (h - hy) * a * a, yb = hy + (h - hy) * b * b;
            float xa = vx + (ex - vx) * a * a, xb = vx + (ex - vx) * b * b, k = 1 + 18 * b * b;
            ImU32 c = i & 1 ? IM_COL32(230, 230, 230, 255) : IM_COL32(200, 40, 40, 255);
            float d = side ? 1 : -1;
            dl->AddQuadFilled(ImVec2(xa, ya), ImVec2(xa + d * k * 0.6f, ya), ImVec2(xb + d * k, yb), ImVec2(xb, yb), c);
        }
    }
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w * 0.55f, h), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150));
}

