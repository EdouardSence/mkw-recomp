// Controller button icons for our own pages (pad_icons.h): the shapes of ui/glyphs/glyph_draw.c (the game's prompts), drawn
// with ImGui's primitives so the in-game menu (Vulkan) and the start screen (SDL_Renderer) need no texture of them.
#include <math.h>
#include <string.h>

#include "imgui.h"
#include "ui/glyphs/pad_icons.h"

extern "C" {
#include "input_map.h"
#include "ui/glyphs/glyphs.h"
}

static const ImU32 kBody = IM_COL32(245, 245, 245, 255), kMark = IM_COL32(110, 110, 110, 255),
                   kEdge = IM_COL32(0, 0, 0, 90);

static void centred_text(ImDrawList *dl, ImVec2 c, float size, const char *t) {
    ImFont *f = ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, t);
    dl->AddText(f, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), kMark, t);
}

static void round_button(ImDrawList *dl, ImVec2 c, float s, const char *t) {
    dl->AddCircleFilled(c, s * 0.5f + s * 0.06f, kEdge);
    dl->AddCircleFilled(c, s * 0.5f, kBody);
    centred_text(dl, c, s * (strlen(t) > 1 ? 0.5f : 0.68f), t);
}

static void tab_button(ImDrawList *dl, ImVec2 c, float s, const char *t) {
    float hw = s * 0.68f, hh = s * 0.42f, e = s * 0.06f;
    dl->AddRectFilled(ImVec2(c.x - hw - e, c.y - hh - e), ImVec2(c.x + hw + e, c.y + hh + e), kEdge, s * 0.22f);
    dl->AddRectFilled(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), kBody, s * 0.18f);
    centred_text(dl, c, s * 0.52f, t);
}

static void ps_face(ImDrawList *dl, ImVec2 c, float s, int face) {
    float k = s * 0.2f, t = s * 0.09f;
    dl->AddCircleFilled(c, s * 0.5f + s * 0.06f, kEdge);
    dl->AddCircleFilled(c, s * 0.5f, kBody);
    switch (face) {
    case 0: dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), kMark, t);
            dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), kMark, t); break;
    case 1: dl->AddCircle(c, k * 1.05f, kMark, 0, t); break;
    case 2: dl->AddRect(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), kMark, 0, 0, t); break;
    default: dl->AddTriangle(ImVec2(c.x, c.y - k * 1.15f), ImVec2(c.x - k * 1.1f, c.y + k * 0.8f),
                             ImVec2(c.x + k * 1.1f, c.y + k * 0.8f), kMark, t); break;
    }
}

static void dpad(ImDrawList *dl, ImVec2 c, float s, int dir) {
    float L = s * 0.5f, w = s * 0.2f, k = s * 0.16f;
    dl->AddRectFilled(ImVec2(c.x - L, c.y - w), ImVec2(c.x + L, c.y + w), kBody, s * 0.05f);
    dl->AddRectFilled(ImVec2(c.x - w, c.y - L), ImVec2(c.x + w, c.y + L), kBody, s * 0.05f);
    float d = L * 0.62f;
    ImVec2 p = dir == PAD_DPAD_UP ? ImVec2(c.x, c.y - d) : dir == PAD_DPAD_DOWN ? ImVec2(c.x, c.y + d)
             : dir == PAD_DPAD_LEFT ? ImVec2(c.x - d, c.y) : ImVec2(c.x + d, c.y);
    float sx = dir == PAD_DPAD_LEFT ? -1 : dir == PAD_DPAD_RIGHT ? 1 : 0, sy = dir == PAD_DPAD_UP ? -1 : dir == PAD_DPAD_DOWN ? 1 : 0;
    dl->AddTriangleFilled(ImVec2(p.x + sx * k, p.y + sy * k), ImVec2(p.x - sx * k * 0.7f - sy * k, p.y - sy * k * 0.7f - sx * k),
                          ImVec2(p.x - sx * k * 0.7f + sy * k, p.y - sy * k * 0.7f + sx * k), kMark);
}

bool pad_icon_draw(ImDrawList *dl, ImVec2 c, float s, int pad, int kind) {
    if (kind != GLYPHS_XBOX && kind != GLYPHS_PLAYSTATION) return false;
    bool ps = kind == GLYPHS_PLAYSTATION;
    static const char *const xbox[] = {"A", "B", "X", "Y"};
    switch (pad) {
    case PAD_SOUTH: case PAD_EAST: case PAD_WEST: case PAD_NORTH:
        if (ps) ps_face(dl, c, s, pad - PAD_SOUTH);
        else round_button(dl, c, s, xbox[pad - PAD_SOUTH]);
        return true;
    case PAD_L: tab_button(dl, c, s, ps ? "L1" : "LB"); return true;
    case PAD_R: tab_button(dl, c, s, ps ? "R1" : "RB"); return true;
    case PAD_ZL: tab_button(dl, c, s, ps ? "L2" : "LT"); return true;
    case PAD_ZR: tab_button(dl, c, s, ps ? "R2" : "RT"); return true;
    case PAD_LSTICK: round_button(dl, c, s, ps ? "L3" : "LS"); return true;
    case PAD_RSTICK: round_button(dl, c, s, ps ? "R3" : "RS"); return true;
    case PAD_START: tab_button(dl, c, s, ps ? "OPT" : "MENU"); return true;
    case PAD_BACK: tab_button(dl, c, s, ps ? "SHR" : "VIEW"); return true;
    case PAD_DPAD_UP: case PAD_DPAD_DOWN: case PAD_DPAD_LEFT: case PAD_DPAD_RIGHT: dpad(dl, c, s, pad); return true;
    default: return false;
    }
}
