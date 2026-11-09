// Catalog of the text the host shows the player (ui_text.h). One array per catalog language, indexed by UiTextId: a new
// string gets an id in ui_text.h and an entry in every language that has one (a missing entry falls back to English).
// The menu languages' arrays are in ui_text_en.c, ui_text_ru.c and ui_text_es.c.
// The software keyboard has every language; the menu has English and Russian, the start screen also Spanish.
#include <stdatomic.h>
#include <string.h>

#include "cfg.h"
#include "ov.h"
#include "platform/platform.h"
#include "ui_text.h"

#include "ui_text_cat.h"

typedef struct { const char *lang; const char *const *text; } Row;

#define SWKBD_ONLY(name, ...) static const char *const name[UI_TEXT_COUNT] = {SWKBD(__VA_ARGS__)}
SWKBD_ONLY(text_ja, "プレイヤー");
SWKBD_ONLY(text_fr, "Joueur");
SWKBD_ONLY(text_de, "Spieler");
SWKBD_ONLY(text_it, "Giocatore");
SWKBD_ONLY(text_nl, "Speler");
SWKBD_ONLY(text_pt, "Jogador");
SWKBD_ONLY(text_zh_hans, "玩家");
SWKBD_ONLY(text_zh_hant, "玩家");
SWKBD_ONLY(text_ko, "플레이어");

static const Row rows[] = {
    {"en", text_en}, {"ja", text_ja}, {"fr", text_fr}, {"de", text_de}, {"it", text_it}, {"es", text_es},
    {"nl", text_nl}, {"pt", text_pt}, {"ru", text_ru}, {"zh-Hans", text_zh_hans}, {"zh-Hant", text_zh_hant},
    {"ko", text_ko},
};

const char *ui_text_lang(const char *code) {
    static const struct { const char *code, *lang; } map[] = {
        {"en-US", "en"}, {"en-GB", "en"}, {"fr-CA", "fr"}, {"es-419", "es"}, {"pt-BR", "pt"},
        {"zh-CN", "zh-Hans"}, {"zh-TW", "zh-Hant"},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(code, map[i].code)) return map[i].lang;
    return code;
}

static const Row *find_row(const char *lang) {
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++)
        if (!strcmp(rows[i].lang, lang)) return &rows[i];
    return NULL;
}

int ui_text_defined(const char *lang, UiTextId id) {
    const Row *r = find_row(lang);
    return (unsigned)id < UI_TEXT_COUNT && r && r->text[id] && *r->text[id];
}

const char *ui_text_in(const char *lang, UiTextId id) {
    if ((unsigned)id >= UI_TEXT_COUNT) return "";
    return ui_text_defined(lang, id) ? find_row(lang)->text[id] : rows[0].text[id];
}

const char *ui_text(UiTextId id) { return ui_text_in(ui_text_lang(ov_language()), id); }

// The menu language, re-read when cfg_version changes. MKW_MENU_LANG "en", "ru" or "es"; anything else (unset included)
// follows the system UI language: Russian for "ru", "ru-RU", ..., Spanish for "es", "es-ES", ..., else English.
static const char *resolve_menu_lang(void) {
    static const char *const langs[] = {"en", "ru", "es"};
    const char *v = cfg_value("MKW_MENU_LANG");
    for (size_t i = 0; v && i < sizeof langs / sizeof *langs; i++)
        if (!strcmp(v, langs[i])) return langs[i];
    const char *sys = plat_ui_language();
    for (size_t i = 0; sys && i < sizeof langs / sizeof *langs; i++)
        if (!strncmp(sys, langs[i], 2) && (!sys[2] || sys[2] == '-' || sys[2] == '_')) return langs[i];
    return "en";
}

const char *menu_text(UiTextId id) {
    static _Atomic(const char *) lang = "en";
    static _Atomic unsigned seen = ~0u;
    unsigned v = cfg_version();
    if (v != atomic_load_explicit(&seen, memory_order_relaxed)) {
        atomic_store_explicit(&lang, resolve_menu_lang(), memory_order_relaxed);
        atomic_store_explicit(&seen, v, memory_order_relaxed);
    }
    return ui_text_in(atomic_load_explicit(&lang, memory_order_relaxed), id);
}
