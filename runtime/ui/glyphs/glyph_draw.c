// Drawing the button icons (glyph_draw.h): coverage canvases filled with signed-distance shapes and text, composed
// into an RGBA cell in MKW's own look (white button, dark outline, grey symbol). The shapes and the text placement are
// SavageWindRecomp's runtime/nvn/glyphs.c (https://github.com/Revivedx/SavageWindRecomp, branch exp/recorder, MIT,
// Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors): sd(), the shape builders, text() with
// stb_truetype, and the Xbox, PlayStation, keycap and mouse icons; adapted to a variable cell and to RGBA.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "glyph_draw.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "third_party/imgui/imstb_truetype.h"
#pragma clang diagnostic pop

static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- shapes
typedef enum { SH_CIRCLE, SH_BOX, SH_SEG, SH_TRI } ShapeType;
typedef struct { ShapeType t; float p[6], r, ring; } Shape;   // ring > 0: only a band that thick on its edge

static float sd_seg(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, h = clampf(((px - ax) * dx + (py - ay) * dy) / maxf(dx * dx + dy * dy, 1e-6f), 0, 1);
    return hypotf(px - ax - dx * h, py - ay - dy * h);
}

static float sd(const Shape *s, float x, float y) {
    float d = 0;
    switch (s->t) {
    case SH_CIRCLE: d = hypotf(x - s->p[0], y - s->p[1]) - s->r; break;
    case SH_BOX: {   // centre, half sizes, corner radius r
        float qx = fabsf(x - s->p[0]) - s->p[2] + s->r, qy = fabsf(y - s->p[1]) - s->p[3] + s->r;
        d = hypotf(maxf(qx, 0), maxf(qy, 0)) + minf(maxf(qx, qy), 0) - s->r;
        break;
    }
    case SH_SEG: d = sd_seg(x, y, s->p[0], s->p[1], s->p[2], s->p[3]) - s->r; break;
    case SH_TRI: {
        float e = minf(minf(sd_seg(x, y, s->p[0], s->p[1], s->p[2], s->p[3]), sd_seg(x, y, s->p[2], s->p[3], s->p[4], s->p[5])),
                       sd_seg(x, y, s->p[4], s->p[5], s->p[0], s->p[1]));
        float c1 = (s->p[2] - s->p[0]) * (y - s->p[1]) - (s->p[3] - s->p[1]) * (x - s->p[0]);
        float c2 = (s->p[4] - s->p[2]) * (y - s->p[3]) - (s->p[5] - s->p[3]) * (x - s->p[2]);
        float c3 = (s->p[0] - s->p[4]) * (y - s->p[5]) - (s->p[1] - s->p[5]) * (x - s->p[4]);
        int in = (c1 >= 0 && c2 >= 0 && c3 >= 0) || (c1 <= 0 && c2 <= 0 && c3 <= 0);
        d = (in ? -e : e) - s->r;
        break;
    }
    }
    return s->ring > 0 ? fabsf(d) - s->ring : d;
}

static float cover(float d) { return clampf(0.5f - d, 0, 1); }

static void fill(GlyphCanvas *c, float *a, Shape s) {
    for (int y = 0; y < c->size; y++)
        for (int x = 0; x < c->size; x++) a[y * c->size + x] = maxf(a[y * c->size + x], cover(sd(&s, x + 0.5f, y + 0.5f)));
}
static Shape circle(float x, float y, float r) { return (Shape){SH_CIRCLE, {x, y}, r, 0}; }
static Shape box(float x, float y, float hw, float hh, float r) { return (Shape){SH_BOX, {x, y, hw, hh}, r, 0}; }
static Shape seg(float ax, float ay, float bx, float by, float r) { return (Shape){SH_SEG, {ax, ay, bx, by}, r, 0}; }
static Shape tri(float ax, float ay, float bx, float by, float cx, float cy) { return (Shape){SH_TRI, {ax, ay, bx, by, cx, cy}, 0, 0}; }
static Shape ring(Shape s, float t) { s.ring = t; return s; }

// ---- text: Roboto (ui/menu/menu_look.cpp embeds it; glyph_font_set hands it over)
static stbtt_fontinfo g_font;
static int g_font_ok;

void glyph_font_set(const unsigned char *ttf) { g_font_ok = stbtt_InitFont(&g_font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0)); }

// The text's capitals cap_h tall (less if wider than max_w), centred on (cx, cy): coverage into a (the label layer)
static void text(GlyphCanvas *c, float *a, const char *s, float cx, float cy, float cap_h, float max_w) {
    if (!g_font_ok || !*s) return;
    int x0, y0, x1, cap_y1;
    stbtt_GetCodepointBox(&g_font, 'H', &x0, &y0, &x1, &cap_y1);
    float scale = cap_h / (float)(cap_y1 > 0 ? cap_y1 : 1400), width = 0;
    for (const char *p = s; *p; p++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (unsigned char)*p, &adv, &lsb);
        width += adv * scale;
        if (p[1]) width += stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)*p, (unsigned char)p[1]) * scale;
    }
    if (width > max_w) scale *= max_w / width, width = max_w;
    float pen = cx - width / 2, base = cy + cap_y1 * scale / 2;
    static unsigned char bm[GLYPH_CELL_MAX * GLYPH_CELL_MAX];
    for (const char *p = s; *p; p++) {
        int cp = (unsigned char)*p, bx0, by0, bx1, by1, adv, lsb;
        float sx = pen - floorf(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&g_font, cp, scale, scale, sx, 0, &bx0, &by0, &bx1, &by1);
        int bw = bx1 - bx0, bh = by1 - by0;
        if (bw > 0 && bh > 0 && bw * bh <= (int)sizeof bm) {
            stbtt_MakeCodepointBitmapSubpixel(&g_font, bm, bw, bh, bw, scale, scale, sx, 0, cp);
            int ox = (int)floorf(pen) + bx0, oy = (int)lroundf(base) + by0;
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    int X = ox + x, Y = oy + y;
                    if (X >= 0 && X < c->size && Y >= 0 && Y < c->size)
                        a[Y * c->size + X] = maxf(a[Y * c->size + X], bm[y * bw + x] / 255.0f);
                }
        }
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        pen += adv * scale;
        if (p[1]) pen += stbtt_GetCodepointKernAdvance(&g_font, cp, (unsigned char)p[1]) * scale;
    }
}

// ---- icons: the body goes to c->body, symbols to c->mark (grey on the body)
// MKW's labels are heavy and round: Roboto Medium is set, then grown by a disk of cap / 14 (a bold look without a second
// font; cap / 30 for words)
static void label(GlyphCanvas *c, const char *s, float cx, float cy, float cap, float max_w) {
    static float t[GLYPH_CELL_MAX * GLYPH_CELL_MAX];
    int n = c->size, r = (int)lroundf(cap / (strlen(s) > 2 ? 30 : 14));   // a word gets less: its counters close up
    memset(t, 0, sizeof(float) * (size_t)n * n);
    text(c, t, s, cx, cy, cap, max_w - 2 * r);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float v = 0;
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    int X = x + dx, Y = y + dy;
                    if (X >= 0 && Y >= 0 && X < n && Y < n && dx * dx + dy * dy <= r * r) v = maxf(v, t[Y * n + X]);
                }
            c->mark[y * n + x] = maxf(c->mark[y * n + x], v);
        }
}

void glyph_circle(GlyphCanvas *c, float cx, float cy, float r, float ring_w) {
    fill(c, c->body, ring_w > 0 ? ring(circle(cx, cy, r - ring_w), ring_w) : circle(cx, cy, r));
}

void glyph_round(GlyphCanvas *c, GlyphBox b, const char *s) {
    fill(c, c->body, circle(b.cx, b.cy, b.s / 2));
    label(c, s, b.cx, b.cy, b.s * (strlen(s) > 1 ? 0.38f : 0.52f), b.s * 0.76f);
}

void glyph_tab(GlyphCanvas *c, GlyphBox b, const char *s) {   // fills the game's shoulder box, as its L / R do
    float hw = b.w / 2, hh = b.s / 2;
    fill(c, c->body, box(b.cx, b.cy, hw, hh, b.s * 0.2f));
    label(c, s, b.cx, b.cy, b.s * 0.42f, hw * 2 - b.s * 0.2f);
}

void glyph_ps_face(GlyphCanvas *c, GlyphBox b, int face) {
    float r = b.s / 2, t = b.s * 0.075f, k = b.s * 0.2f, x = b.cx, y = b.cy;   // strokes as heavy as the letters
    fill(c, c->body, circle(x, y, r));
    switch (face) {
    case GLYPH_PS_CROSS: fill(c, c->mark, seg(x - k, y - k, x + k, y + k, t)); fill(c, c->mark, seg(x - k, y + k, x + k, y - k, t)); break;
    case GLYPH_PS_CIRCLE: fill(c, c->mark, ring(circle(x, y, k * 1.05f), t)); break;
    case GLYPH_PS_SQUARE: fill(c, c->mark, ring(box(x, y, k * 0.95f, k * 0.95f, 0), t)); break;
    default: fill(c, c->mark, ring(tri(x, y - k * 1.15f, x - k * 1.1f, y + k * 0.8f, x + k * 1.1f, y + k * 0.8f), t)); break;
    }
}

void glyph_menu_button(GlyphCanvas *c, GlyphBox b, int start, int ps) {
    float x = b.cx, y = b.cy, s = b.s, t = s * 0.045f;
    if (ps) fill(c, c->body, box(x, y, minf(b.w / 2, s * 0.5f), s * 0.3f, s * 0.3f));
    else fill(c, c->body, circle(x, y, s / 2));
    if (start) {
        float w = s * (ps ? 0.2f : 0.19f), dy = s * (ps ? 0.11f : 0.14f);
        for (int i = -1; i <= 1; i++) fill(c, c->mark, seg(x - w, y + i * dy, x + w, y + i * dy, t));
    } else if (ps) {
        fill(c, c->mark, ring(box(x, y, s * 0.1f, s * 0.1f, s * 0.02f), t));
    } else {
        fill(c, c->mark, ring(box(x - s * 0.05f, y - s * 0.05f, s * 0.12f, s * 0.1f, 0), t));
        fill(c, c->mark, ring(box(x + s * 0.06f, y + s * 0.06f, s * 0.12f, s * 0.1f, 0), t));
    }
}

static Shape arrow(float x, float y, float k, int dir) {
    switch (dir) {
    case GLYPH_UP: return tri(x, y - k, x - k, y + k * 0.7f, x + k, y + k * 0.7f);
    case GLYPH_DOWN: return tri(x, y + k, x - k, y - k * 0.7f, x + k, y - k * 0.7f);
    case GLYPH_LEFT: return tri(x - k, y, x + k * 0.7f, y - k, x + k * 0.7f, y + k);
    default: return tri(x + k, y, x - k * 0.7f, y - k, x - k * 0.7f, y + k);
    }
}

void glyph_dpad(GlyphCanvas *c, GlyphBox b, int dirs) {
    float x = b.cx, y = b.cy, L = b.s * 0.48f, w = b.s * 0.17f, r = b.s * 0.05f, k = b.s * 0.09f;
    fill(c, c->body, box(x, y, L, w, r));
    fill(c, c->body, box(x, y, w, L, r));
    if (dirs & GLYPH_UP) fill(c, c->mark, arrow(x, y - L * 0.62f, k, GLYPH_UP));
    if (dirs & GLYPH_DOWN) fill(c, c->mark, arrow(x, y + L * 0.62f, k, GLYPH_DOWN));
    if (dirs & GLYPH_LEFT) fill(c, c->mark, arrow(x - L * 0.62f, y, k, GLYPH_LEFT));
    if (dirs & GLYPH_RIGHT) fill(c, c->mark, arrow(x + L * 0.62f, y, k, GLYPH_RIGHT));
}

void glyph_keycap(GlyphCanvas *c, GlyphBox b, const char *s, int dirs) {
    float hh = b.s * 0.42f, hw = (s && strlen(s) > 1) ? minf(b.w / 2, b.s * 0.62f) : hh;
    if (!s && dirs && (dirs & (dirs - 1))) hw = minf(b.w / 2, b.s * 0.6f);
    fill(c, c->body, box(b.cx, b.cy, hw, hh, b.s * 0.12f));
    if (s) {
        label(c, s, b.cx, b.cy, b.s * (strlen(s) > 1 ? 0.32f : 0.46f), hw * 2 - b.s * 0.16f);
        return;
    }
    int n = 0, d[4];
    for (int i = 0; i < 4; i++) if (dirs & (1 << i)) d[n++] = 1 << i;
    float k = b.s * (n == 1 ? 0.17f : 0.12f);
    if (n == 4) {
        fill(c, c->mark, arrow(b.cx, b.cy - k * 1.6f, k * 0.8f, GLYPH_UP));
        fill(c, c->mark, arrow(b.cx, b.cy + k * 1.6f, k * 0.8f, GLYPH_DOWN));
        fill(c, c->mark, arrow(b.cx - k * 1.9f, b.cy, k * 0.8f, GLYPH_LEFT));
        fill(c, c->mark, arrow(b.cx + k * 1.9f, b.cy, k * 0.8f, GLYPH_RIGHT));
    } else {
        for (int i = 0; i < n; i++) fill(c, c->mark, arrow(b.cx + (n > 1 ? (i ? 1 : -1) * k * 1.5f : 0), b.cy, k, d[i]));
    }
}

void glyph_mouse(GlyphCanvas *c, GlyphBox b, int button) {
    float x = b.cx, y = b.cy, hw = b.s * 0.29f, hh = b.s * 0.45f, t = b.s * 0.05f, split = y - hh * 0.15f;
    Shape body = box(x, y, hw, hh, hw);
    fill(c, c->body, body);
    fill(c, c->mark, seg(x - hw, split, x + hw, split, t * 0.8f));
    fill(c, c->mark, seg(x, y - hh, x, split, t * 0.8f));
    for (int py = 0; py < c->size; py++)
        for (int px = 0; px < c->size; px++) {
            float X = px + 0.5f, Y = py + 0.5f, d = sd(&body, X, Y);
            if (button == 1) d = maxf(d, maxf(X - x, Y - split));
            else if (button == 3) d = maxf(d, maxf(x - X, Y - split));
            else if (button == 4 || button == 5)
                d = maxf(d, maxf(X - (x - hw * 0.3f), fabsf(Y - (y + hh * (button == 4 ? 0.2f : 0.55f))) - hh * 0.14f));
            else continue;
            c->mark[py * c->size + px] = maxf(c->mark[py * c->size + px], cover(d));
        }
    if (button == 2) fill(c, c->mark, seg(x, y - hh * 0.62f, x, y - hh * 0.32f, hw * 0.32f));
}

// ---- composing: the outline (the body grown by o pixels, translucent), the body, the symbol on it; MKW's own glyphs
// are a white body, a grey symbol and a 4-pixel black outline at 30 % (PicFont, measured 06/11/2026)
void glyph_compose(const GlyphCanvas *c, uint8_t *rgba, size_t row, const GlyphLook *look) {
    int n = c->size, o = look->outline;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float body = c->body[y * n + x], edge = body;
            for (int dy = -o; dy <= o && edge < 1; dy++)
                for (int dx = -o; dx <= o; dx++) {
                    int X = x + dx, Y = y + dy;
                    if (X < 0 || Y < 0 || X >= n || Y >= n || dx * dx + dy * dy > o * o) continue;
                    edge = maxf(edge, c->body[Y * n + X]);
                }
            float mark = minf(c->mark[y * n + x], body), ring = (edge - body) * look->outline_alpha;
            float a = body + ring;   // body over the outline
            uint8_t *p = rgba + (size_t)y * row + (size_t)x * 4;
            for (int k = 0; k < 3; k++) {
                float v = look->outline_col[k] * ring + (look->body[k] * (1 - mark) + look->mark[k] * mark) * body;
                p[k] = (uint8_t)lroundf(clampf(a > 0 ? v / a : 0, 0, 255));
            }
            p[3] = (uint8_t)lroundf(clampf(a, 0, 1) * 255);
        }
}
