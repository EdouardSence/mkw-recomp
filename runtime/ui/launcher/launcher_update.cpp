// The start screen's update (launcher_update.h). The network side is tools/prepare/update.py in the package's Python
// (urllib and OpenSSL; the runtime has no HTTP), run in a child process whose event lines a thread of ours reads, as
// launcher_setup.cpp does for prepare.py. The swap is done here, once that Python has exited, because Windows cannot
// rename a folder whose program is still running:
//   Windows  each top entry of the new package (mkw.exe, its DLLs, setup/, licenses/) replaces the one next to the
//            program, which is renamed to <name>.old first (a running exe and its loaded DLLs can be renamed, not
//            removed); a failed rename puts back the ones already done.
//   Linux    the new AppImage replaces $APPIMAGE (a rename in the same folder: the running one keeps its inode), and
//            this process becomes it (execv): its AppRun copies the new mkw over this one, which a running mkw would
//            refuse (text file busy).
// The mark update-done next to the program tells the next start to remove the *.old files (update.py clean) and set
// up again what changed. The offer itself follows hookshot's host/hos_update.c (GPL-3.0, idea only).
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "ui/launcher/launcher_update.h"

extern "C" {
#include "cfg.h"
#include "platform/platform.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern const char rt_port_version[];
}

static SDL_Mutex *g_lock;
static UpdateState g_state;
static SDL_Process *g_proc;
static char g_dest[1200];   // where the download goes

static void set_state(void (*fn)(UpdateState &, const char *), const char *arg) {
    SDL_LockMutex(g_lock);
    fn(g_state, arg);
    SDL_UnlockMutex(g_lock);
}

static void fail(UpdateState &s, const char *why) {
    s.phase = UPDATE_FAILED;
    snprintf(s.error, sizeof s.error, "%s", why);
}

// The package's Python and update.py; a source checkout has no update (it is updated with git).
static bool find_tools(const char *exe_dir, char *python, char *script, size_t cap) {
#ifdef _WIN32
    snprintf(python, cap, "%s/setup/python/python.exe", exe_dir);
#else
    snprintf(python, cap, "%s/setup/python/bin/python3", exe_dir);
#endif
    snprintf(script, cap, "%s/setup/src/tools/prepare/update.py", exe_dir);
    return plat_file_exists(python) && plat_file_exists(script);
}

// Packaged and replaceable: on Linux only the AppImage (APPIMAGE, set by its runtime), whose folder takes the download.
static bool installable(const char *exe_dir, char *dest, size_t cap) {
    char python[1200], script[1200];
    if (!find_tools(exe_dir, python, script, sizeof python)) return false;
#ifdef _WIN32
    snprintf(dest, cap, "%s/update", exe_dir);
    return true;
#else
    const char *app = getenv("APPIMAGE");
    if (!app || !*app || strlen(app) >= cap) return false;
    snprintf(dest, cap, "%s", app);
    char *cut = strrchr(dest, '/');
    if (!cut) return false;
    snprintf(cut, cap - (size_t)(cut - dest), "/.mkw-recomp-update");
    return true;
#endif
}

// One event line of update.py (its header lists them).
static void on_line(char *line) {
    rt_log("update: %s", line);
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0;
    SDL_LockMutex(g_lock);
    UpdateState &s = g_state;
    if (!strcmp(line, "update") && arg) {
        long long b = 0;
        if (sscanf(arg, "%95s %lld", s.tag, &b) == 2) s.bytes = b, s.phase = UPDATE_OFFER;
    } else if (!strcmp(line, "latest")) {
        s.phase = UPDATE_IDLE;
    } else if (!strcmp(line, "progress") && arg) {
        sscanf(arg, "%lld %lld", &s.done, &s.total);
    } else if (!strcmp(line, "ready") && arg) {
        snprintf(s.path, sizeof s.path, "%s", arg);
        s.phase = UPDATE_READY;
    } else if (!strcmp(line, "error") && arg) {
        char *why = strchr(arg, ' ');
        fail(s, why ? why + 1 : arg);
    }
    SDL_UnlockMutex(g_lock);
}

static int SDLCALL reader(void *) {
    SDL_IOStream *out = SDL_GetProcessOutput(g_proc);
    char line[2048];
    size_t n = 0;
    char c;
    for (;;) {
        if (SDL_ReadIO(out, &c, 1) != 1) {   // the pipe does not block: no data yet is not the end
            if (SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) break;
            SDL_Delay(20);
            continue;
        }
        if (c == '\r') continue;
        if (c != '\n') {
            if (n + 1 < sizeof line) line[n++] = c;
            continue;
        }
        line[n] = 0;
        n = 0;
        on_line(line);
    }
    int code = -1;
    SDL_WaitProcess(g_proc, true, &code);
    SDL_DestroyProcess(g_proc);
    g_proc = nullptr;
    SDL_LockMutex(g_lock);
    // A check that fails (offline, GitHub down) offers nothing and says nothing: only a download's failure is shown.
    if (g_state.phase == UPDATE_CHECKING) g_state.phase = UPDATE_IDLE;
    if (g_state.phase == UPDATE_FETCHING) fail(g_state, code ? "the download stopped" : "no package came");
    if (g_state.phase == UPDATE_FAILED && !g_state.tag[0]) g_state.phase = UPDATE_IDLE;
    SDL_UnlockMutex(g_lock);
    rt_log("update: ended with code %d", code);
    return 0;
}

static bool spawn(const char *const *args) {
    g_proc = SDL_CreateProcess(args, true);
    if (!g_proc) {
        rt_log("update: cannot start %s: %s", args[0], SDL_GetError());
        return false;
    }
    SDL_DetachThread(SDL_CreateThread(reader, "update", nullptr));
    return true;
}

void update_check_start(const char *exe_dir) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    char python[1200], script[1200];
    if (!cfg_bool("MKW_UPDATE_CHECK") || !installable(exe_dir, g_dest, sizeof g_dest) ||
        !find_tools(exe_dir, python, script, sizeof python))
        return;
    g_state = UpdateState{};
    g_state.phase = UPDATE_CHECKING;
    const char *args[] = {python, "-I", script, "check", "--version", rt_port_version, nullptr};
    if (!spawn(args)) g_state.phase = UPDATE_IDLE;
}

void update_fetch_start(const char *exe_dir) {
    UpdateState s = update_state();
    char python[1200], script[1200];
    if (s.phase != UPDATE_OFFER || g_proc || !find_tools(exe_dir, python, script, sizeof python)) return;
    SDL_LockMutex(g_lock);
    g_state.phase = UPDATE_FETCHING;
    g_state.done = 0, g_state.total = s.bytes;
    SDL_UnlockMutex(g_lock);
    const char *args[] = {python, "-I", script, "fetch", "--tag", s.tag, "--dest", g_dest, nullptr};
    if (!spawn(args)) set_state(fail, SDL_GetError());
}

UpdateState update_state(void) {
    if (!g_lock) return g_state;
    SDL_LockMutex(g_lock);
    UpdateState s = g_state;
    SDL_UnlockMutex(g_lock);
    return s;
}

static bool write_mark(const char *exe_dir, const char *tag) {
    char p[1200];
    snprintf(p, sizeof p, "%s/update-done", exe_dir);
    FILE *f = fopen(p, "wb");
    if (!f) return false;
    fprintf(f, "%s\n", tag);
    return fclose(f) == 0;
}

#ifdef _WIN32
struct Swap {
    const char *exe_dir, *from;
    char names[64][256];
    int n, moved;
    bool failed;
};

static SDL_EnumerationResult SDLCALL list_entry(void *ud, const char *, const char *name) {
    Swap *w = (Swap *)ud;
    if (w->n == 64 || strlen(name) >= sizeof w->names[0]) return w->failed = true, SDL_ENUM_FAILURE;
    snprintf(w->names[w->n++], sizeof w->names[0], "%s", name);
    return SDL_ENUM_CONTINUE;
}

static void path_of(char *out, size_t cap, const char *dir, const char *name, const char *suffix) {
    snprintf(out, cap, "%s/%s%s", dir, name, suffix);
}

// Entry i back: the new one to the package's folder, the old one to its name.
static void undo(Swap &w, int i) {
    char cur[1500], old[1500], back[1500];
    path_of(cur, sizeof cur, w.exe_dir, w.names[i], "");
    path_of(old, sizeof old, w.exe_dir, w.names[i], ".old");
    path_of(back, sizeof back, w.from, w.names[i], "");
    SDL_RenamePath(cur, back);
    if (SDL_GetPathInfo(old, nullptr)) SDL_RenamePath(old, cur);
}
#endif

int update_apply(const char *exe_dir) {
    UpdateState s = update_state();
    if (s.phase != UPDATE_READY) return 0;
#ifdef _WIN32
    Swap w = {exe_dir, s.path, {}, 0, 0, false};
    if (!SDL_EnumerateDirectory(s.path, list_entry, &w) || w.failed || !w.n) {
        set_state(fail, "the new package cannot be read");
        return 0;
    }
    char cur[1500], old[1500], src[1500];
    for (; w.moved < w.n; w.moved++) {
        const char *name = w.names[w.moved];
        path_of(cur, sizeof cur, exe_dir, name, "");
        path_of(old, sizeof old, exe_dir, name, ".old");
        path_of(src, sizeof src, s.path, name, "");
        if (SDL_GetPathInfo(old, nullptr)) {   // left by an update whose clean-up did not run
            char why[1700];
            snprintf(why, sizeof why, "remove %s first", old);
            set_state(fail, why);
            break;
        }
        if (SDL_GetPathInfo(cur, nullptr) && !SDL_RenamePath(cur, old)) {
            set_state(fail, SDL_GetError());
            break;
        }
        if (!SDL_RenamePath(src, cur)) {
            set_state(fail, SDL_GetError());
            if (SDL_GetPathInfo(old, nullptr)) SDL_RenamePath(old, cur);
            break;
        }
    }
    if (w.moved < w.n) {
        while (w.moved-- > 0) undo(w, w.moved);
        rt_log("update: install failed, the old files are back: %s", update_state().error);
        return 0;
    }
#else
    const char *app = getenv("APPIMAGE");
    if (!app || !SDL_RenamePath(s.path, app)) {
        set_state(fail, app ? SDL_GetError() : "not started from the AppImage");
        return 0;
    }
    SDL_RemovePath(g_dest);
#endif
    write_mark(exe_dir, s.tag);
    rt_log("update: %s installed, starting it", s.tag);
    return 1;
}

void update_relaunch(const char *exe_dir) {
#ifdef _WIN32
    char exe[1200];
    snprintf(exe, sizeof exe, "%s/mkw.exe", exe_dir);
    const char *args[] = {exe, nullptr};
    SDL_Process *p = SDL_CreateProcess(args, false);
    if (!p) rt_log("update: cannot start %s: %s", exe, SDL_GetError());
    else SDL_DestroyProcess(p);   // it keeps running: only our handle goes
#else
    (void)exe_dir;
    const char *app = getenv("APPIMAGE");
    if (!app) return;
    char *const args[] = {(char *)app, nullptr};
    execv(app, args);
    rt_log("update: cannot start %s", app);
#endif
}

int update_just_installed(const char *exe_dir, char *tag, unsigned cap) {
    char p[1200];
    snprintf(p, sizeof p, "%s/update-done", exe_dir);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    tag[0] = 0;
    if (!fgets(tag, (int)cap, f)) tag[0] = 0;
    fclose(f);
    tag[strcspn(tag, "\r\n")] = 0;
    remove(p);
    char python[1200], script[1200];
    if (find_tools(exe_dir, python, script, sizeof python)) {   // the *.old files: nobody waits for it
        const char *args[] = {python, "-I", script, "clean", "--game", exe_dir, nullptr};
        SDL_Process *c = SDL_CreateProcess(args, false);
        if (c) SDL_DestroyProcess(c);
    }
    rt_log("update: %s installed by the previous run", tag);
    return 1;
}
