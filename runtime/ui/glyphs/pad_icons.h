// Controller button icons drawn with ImGui (ui/glyphs/pad_icons.cpp), for the Controls tab: the same look as the game's
// prompts (ui/glyphs/glyphs.c). ImGui's thread only.
#pragma once
#include "imgui.h"

// The icon of a controller button (input_map.h PAD_*) for the icon kind (glyphs.h GLYPHS_*), centred on c, s high.
// false when the kind has no icons of its own (Nintendo, keyboard) or the button none: the caller writes its name.
bool pad_icon_draw(ImDrawList *dl, ImVec2 c, float s, int pad, int kind);
