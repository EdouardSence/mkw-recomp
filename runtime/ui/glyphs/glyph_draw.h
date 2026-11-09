// Drawing the button icons (ui/glyphs/glyph_draw.c), for the game's glyph sheet (ui/glyphs/glyphs.c) and our own menus.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { GLYPH_CELL_MAX = 128 };
enum { GLYPH_UP = 1, GLYPH_DOWN = 2, GLYPH_LEFT = 4, GLYPH_RIGHT = 8 };
enum { GLYPH_PS_CROSS, GLYPH_PS_CIRCLE, GLYPH_PS_SQUARE, GLYPH_PS_TRIANGLE };

// Coverage 0..1 of a size x size cell, y down: the button's body and the symbol on it.
typedef struct { int size; float body[GLYPH_CELL_MAX * GLYPH_CELL_MAX], mark[GLYPH_CELL_MAX * GLYPH_CELL_MAX]; } GlyphCanvas;
typedef struct { float cx, cy, s, w; } GlyphBox;   // where the icon goes: centre, height and width
typedef struct { float body[3], mark[3], outline_col[3], outline_alpha; int outline; } GlyphLook;   // colours 0..255

void glyph_font_set(const unsigned char *ttf);   // the font of the labels (Roboto); before the first drawing
void glyph_round(GlyphCanvas *c, GlyphBox b, const char *label);       // Xbox face buttons, stick presses
void glyph_circle(GlyphCanvas *c, float cx, float cy, float r, float ring_w);   // filled, or a ring ring_w thick
void glyph_tab(GlyphCanvas *c, GlyphBox b, const char *label);         // shoulders, triggers
void glyph_ps_face(GlyphCanvas *c, GlyphBox b, int face);              // GLYPH_PS_*
void glyph_menu_button(GlyphCanvas *c, GlyphBox b, int start, int ps); // Menu / View, Options / Create
void glyph_dpad(GlyphCanvas *c, GlyphBox b, int dirs);                 // GLYPH_UP... marked with arrows
void glyph_keycap(GlyphCanvas *c, GlyphBox b, const char *label, int dirs);   // label NULL: arrows for dirs
void glyph_mouse(GlyphCanvas *c, GlyphBox b, int button);              // 1 left, 2 middle, 3 right, 4/5 side, 0 none
// The canvas into RGBA rows `row` bytes apart: outline, body, symbol.
void glyph_compose(const GlyphCanvas *c, uint8_t *rgba, size_t row, const GlyphLook *look);

#ifdef __cplusplus
}
#endif
