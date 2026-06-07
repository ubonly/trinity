/* ╔══════════════════════════════════════════════════════╗
 * ║            Trinity Window Manager                    ║
 * ║  Lightweight dynamic tiling X11 WM written in C     ║
 * ║  Config: ~/.config/trinitywm/trinitywm.conf         ║
 * ╚══════════════════════════════════════════════════════╝ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/inotify.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>


/* ─────────────────────── Constants ─────────────────────── */
#define MAX_WORKSPACES  9
#define MAX_PATH        512
#define MAX_LINE        1024
#define MAX_CMD         512
#define BUTTONMASK      (ButtonPressMask | ButtonReleaseMask)
#define MOUSEMASK       (BUTTONMASK | PointerMotionMask)
/* Strip CapsLock, NumLock, ScrollLock from modifier state */
#define CLEANMASK(mask) ((mask) & ~(LockMask | Mod2Mask | Mod5Mask))

/* ─────────────────────── Types ─────────────────────── */
typedef struct Client Client;
struct Client {
    Window       win;
    int          x, y, w, h;
    unsigned int tags;
    int          isfloating;
    Client      *next;
    Client      *snext; /* focus stack */
};

typedef struct Keybind Keybind;
struct Keybind {
    unsigned int mod;
    KeySym       key;
    char         action[MAX_CMD];
    Keybind     *next;
};

typedef struct {
    int   border_width;
    char  color_active[32];
    char  color_inactive[32];
    float mfact;
    int   nmaster;
    char  terminal[MAX_CMD];
    char  menu[MAX_CMD];
} Config;

/* ─────────────────────── Globals ─────────────────────── */
static Display      *dpy;
static Window        root;
static int           screen, sw, sh;
static Client       *clients  = NULL;
static Client       *sel      = NULL;
static Client       *stack    = NULL;
static unsigned int  tagset   = 1;
static int           running  = 1;
static unsigned long col_active, col_inactive;
static int         (*xerrorxlib)(Display *, XErrorEvent *);
static Keybind      *keybinds = NULL;
static int           inotify_fd = -1;
static int           inotify_wd = -1;
static char          config_path[MAX_PATH*2] = "";
static char          config_dir[MAX_PATH*2]  = "";

/* Default config — overridden by .conf file */
static Config cfg = {
    .border_width  = 2,
    .color_active  = "#3b82f6",
    .color_inactive = "#1e293b",
    .mfact         = 0.55f,
    .nmaster       = 1,
    .terminal      = "kitty",
    .menu          = "dmenu_run",
};

/* ─────────────────────── Prototypes ─────────────────────── */
static void         sigchld(int);
static void         setup(void);
static void         setup_ewmh(void);
static void         run(void);

static void         cleanup(void);
static void         load_config(void);
static void         reload_config(void);
static void         setup_inotify(void);
static void         grabkeys(void);
static void         grabbuttons(Client *, int);
static void         focus(Client *);
static void         tile(void);
static void         resize(Client *, int, int, int, int);
static void         configure(Client *);
static void         manage(Window, XWindowAttributes *);
static void         unmanage(Client *, int);
static void         detach(Client *);
static void         attach(Client *);
static void         detachstack(Client *);
static void         attachstack(Client *);
static Client      *wintoclient(Window);
static unsigned long getcolor(const char *);
static void         execute_action(const char *);
static void         movemouse(void);
static void         resizemouse(void);
static int          xerror(Display *, XErrorEvent *);
static int          xerrordummy(Display *, XErrorEvent *);
static void         ev_buttonpress(XEvent *);
static void         ev_configurerequest(XEvent *);
static void         ev_destroynotify(XEvent *);
static void         ev_keypress(XEvent *);
static void         ev_maprequest(XEvent *);
static void         ev_enternotify(XEvent *);
static void         ev_unmapnotify(XEvent *);

/* ═══════════════════════════════════════════════════════════════
 *  CONFIG PARSING
 * ═══════════════════════════════════════════════════════════════ */

/* Strip leading/trailing whitespace in-place */
static void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    memmove(s, p, strlen(p) + 1);
    p = s + strlen(s) - 1;
    while (p >= s && isspace((unsigned char)*p)) p--;
    *(p + 1) = '\0';
}

/* Parse "Super+Shift+Return" → mod mask + keysym string */
static unsigned int parse_modmask(const char *combo, char *ks_out, size_t ks_sz) {
    unsigned int mod = 0;
    char buf[MAX_LINE];
    strncpy(buf, combo, sizeof(buf) - 1);
    buf[sizeof(buf)-1] = '\0';

    char *toks[16];
    int   n = 0;
    char *t = strtok(buf, "+");
    while (t && n < 15) { toks[n++] = t; t = strtok(NULL, "+"); }

    if (n > 0) { strncpy(ks_out, toks[n-1], ks_sz-1); ks_out[ks_sz-1] = '\0'; }

    for (int i = 0; i < n - 1; i++) {
        if      (strcasecmp(toks[i], "super")   == 0 || strcasecmp(toks[i], "mod4") == 0) mod |= Mod4Mask;
        else if (strcasecmp(toks[i], "shift")   == 0)                                      mod |= ShiftMask;
        else if (strcasecmp(toks[i], "ctrl")    == 0 || strcasecmp(toks[i], "control") == 0) mod |= ControlMask;
        else if (strcasecmp(toks[i], "alt")     == 0 || strcasecmp(toks[i], "mod1") == 0)  mod |= Mod1Mask;
    }
    return mod;
}

static void add_keybind(const char *combo, const char *action) {
    char ks_str[64] = "";
    unsigned int mod = parse_modmask(combo, ks_str, sizeof(ks_str));
    KeySym key = XStringToKeysym(ks_str);
    if (key == NoSymbol) {
        fprintf(stderr, "trinitywm: unknown key '%s' in binding '%s'\n", ks_str, combo);
        return;
    }
    Keybind *kb = calloc(1, sizeof(Keybind));
    if (!kb) return;
    kb->mod = mod;
    kb->key = key;
    strncpy(kb->action, action, MAX_CMD - 1);
    kb->next  = keybinds;
    keybinds  = kb;
}

static void free_keybinds(void) {
    Keybind *kb = keybinds;
    while (kb) { Keybind *nx = kb->next; free(kb); kb = nx; }
    keybinds = NULL;
}

static void write_default_config(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror("trinitywm: write default config"); return; }

    fprintf(f,
"# ╔══════════════════════════════════════════════════════╗\n"
"# ║           Trinity Window Manager Config              ║\n"
"# ╚══════════════════════════════════════════════════════╝\n"
"#\n"
"# Modifiers: Super, Shift, Ctrl, Alt\n"
"# Actions:\n"
"#   exec <terminal|menu|command>  — run a program\n"
"#   killclient                    — close focused window\n"
"#   focusnext / focusprev         — cycle focus\n"
"#   mfact <+/-float>              — resize master area\n"
"#   togglefloat                   — float/unfloat window\n"
"#   workspace <1-9>               — switch workspace\n"
"#   moveto <1-9>                  — move window to workspace\n"
"#   quit                          — exit the WM\n"
"\n"
"[appearance]\n"
"border_width   = 2\n"
"color_active   = #3b82f6\n"
"color_inactive = #1e293b\n"
"\n"
"[behavior]\n"
"# Master area fraction (0.05 – 0.95)\n"
"mfact   = 0.55\n"
"# Number of windows in master area\n"
"nmaster = 1\n"
"\n"
"[programs]\n"
"terminal = kitty\n"
"menu     = dmenu_run\n"
"\n"
"[keybindings]\n"
"# ── Launch ──────────────────────────────\n"
"Super+Return  = exec terminal\n"
"Super+d       = exec menu\n"
"\n"
"# ── Window control ──────────────────────\n"
"Super+Shift+c = killclient\n"
"Super+j       = focusnext\n"
"Super+k       = focusprev\n"
"Super+space   = togglefloat\n"
"\n"
"# ── Master area ─────────────────────────\n"
"Super+h       = mfact -0.05\n"
"Super+l       = mfact +0.05\n"
"\n"
"# ── Exit ────────────────────────────────\n"
"Super+Shift+q = quit\n"
"\n"
"# ── Workspaces ──────────────────────────\n"
"Super+1       = workspace 1\n"
"Super+2       = workspace 2\n"
"Super+3       = workspace 3\n"
"Super+4       = workspace 4\n"
"Super+5       = workspace 5\n"
"Super+6       = workspace 6\n"
"Super+7       = workspace 7\n"
"Super+8       = workspace 8\n"
"Super+9       = workspace 9\n"
"\n"
"# ── Move window to workspace ────────────\n"
"Super+Shift+1 = moveto 1\n"
"Super+Shift+2 = moveto 2\n"
"Super+Shift+3 = moveto 3\n"
"Super+Shift+4 = moveto 4\n"
"Super+Shift+5 = moveto 5\n"
"Super+Shift+6 = moveto 6\n"
"Super+Shift+7 = moveto 7\n"
"Super+Shift+8 = moveto 8\n"
"Super+Shift+9 = moveto 9\n"
    );
    fclose(f);
    fprintf(stderr, "trinitywm: created default config → %s\n", path);
}

static void parse_config_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char  line[MAX_LINE];
    char  section[64] = "";

    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        /* [section] header */
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) { *end = '\0'; strncpy(section, line+1, sizeof(section)-1); }
            continue;
        }

        /* key = value  (strip inline comment) */
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *value = eq + 1;
        char *hash = strchr(value, '#');
        if (hash) *hash = '\0';
        trim(key); trim(value);
        if (!*key || !*value) continue;

        if (strcmp(section, "appearance") == 0) {
            if      (!strcmp(key, "border_width"))   cfg.border_width = atoi(value);
            else if (!strcmp(key, "color_active"))   strncpy(cfg.color_active,   value, sizeof(cfg.color_active)-1);
            else if (!strcmp(key, "color_inactive")) strncpy(cfg.color_inactive, value, sizeof(cfg.color_inactive)-1);

        } else if (strcmp(section, "behavior") == 0) {
            if      (!strcmp(key, "mfact"))   cfg.mfact   = (float)atof(value);
            else if (!strcmp(key, "nmaster")) cfg.nmaster = atoi(value);

        } else if (strcmp(section, "programs") == 0) {
            if      (!strcmp(key, "terminal")) strncpy(cfg.terminal, value, sizeof(cfg.terminal)-1);
            else if (!strcmp(key, "menu"))     strncpy(cfg.menu,     value, sizeof(cfg.menu)-1);

        } else if (strcmp(section, "keybindings") == 0) {
            add_keybind(key, value);
        }
    }
    fclose(f);
}

static void load_config(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/root";
    snprintf(config_dir,  sizeof(config_dir),  "%s/.config/trinitywm", home);
    snprintf(config_path, sizeof(config_path), "%s/trinitywm.conf",    config_dir);

    mkdir(config_dir, 0755);

    FILE *f = fopen(config_path, "r");
    if (!f) { write_default_config(config_path); }
    else    { fclose(f); }

    parse_config_file(config_path);
}

static void reload_config(void) {
    fprintf(stderr, "trinitywm: reloading config...\n");

    /* Reset to defaults */
    cfg.border_width = 2;
    strncpy(cfg.color_active,   "#3b82f6", sizeof(cfg.color_active)-1);
    strncpy(cfg.color_inactive, "#1e293b", sizeof(cfg.color_inactive)-1);
    cfg.mfact   = 0.55f;
    cfg.nmaster = 1;
    strncpy(cfg.terminal, "kitty",     sizeof(cfg.terminal)-1);
    strncpy(cfg.menu,     "dmenu_run", sizeof(cfg.menu)-1);

    /* Clear old keybindings */
    free_keybinds();

    /* Re-parse config */
    parse_config_file(config_path);

    /* Apply new colors */
    col_active   = getcolor(cfg.color_active);
    col_inactive = getcolor(cfg.color_inactive);

    /* Update border colors on all existing clients */
    for (Client *c = clients; c; c = c->next) {
        XSetWindowBorderWidth(dpy, c->win, cfg.border_width);
        XSetWindowBorder(dpy, c->win, (c == sel) ? col_active : col_inactive);
    }

    /* Re-grab keys with new bindings */
    grabkeys();

    /* Re-tile with possibly new mfact/nmaster */
    tile();

    fprintf(stderr, "trinitywm: config reloaded\n");
}

static void setup_inotify(void) {
    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        perror("trinitywm: inotify_init1");
        return;
    }
    /* Watch the directory (not the file) because editors like vim
     * delete and recreate the file, which kills the watch. */
    inotify_wd = inotify_add_watch(inotify_fd, config_dir,
                                   IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (inotify_wd < 0)
        perror("trinitywm: inotify_add_watch");
}

/* ═══════════════════════════════════════════════════════════════
 *  ACTION DISPATCH
 * ═══════════════════════════════════════════════════════════════ */

static void execute_action(const char *action_str) {
    char act[MAX_CMD] = "", arg[MAX_CMD] = "", buf[MAX_CMD];
    strncpy(buf, action_str, sizeof(buf)-1);
    trim(buf);
    char *sp = strchr(buf, ' ');
    if (sp) { *sp = '\0'; strncpy(arg, sp+1, MAX_CMD-1); trim(arg); }
    strncpy(act, buf, MAX_CMD-1);

    /* ── exec <terminal|menu|command> ── */
    if (!strcmp(act, "exec")) {
        const char *cmd = !strcmp(arg, "terminal") ? cfg.terminal
                        : !strcmp(arg, "menu")     ? cfg.menu
                        : arg;
        if (fork() == 0) {
            if (dpy) close(ConnectionNumber(dpy));
            setsid();
            execl("/bin/sh", "sh", "-c", cmd, NULL);
            exit(EXIT_SUCCESS);
        }

    /* ── killclient ── */
    } else if (!strcmp(act, "killclient")) {
        if (!sel) return;
        XGrabServer(dpy);
        XSetErrorHandler(xerrordummy);
        XKillClient(dpy, sel->win);
        XSync(dpy, False);
        XSetErrorHandler(xerror);
        XUngrabServer(dpy);

    /* ── focusnext ── */
    } else if (!strcmp(act, "focusnext")) {
        if (!sel) return;
        Client *c;
        for (c = sel->next; c && !(c->tags & tagset); c = c->next);
        if (!c) for (c = clients; c && !(c->tags & tagset); c = c->next);
        if (c) focus(c);

    /* ── focusprev ── */
    } else if (!strcmp(act, "focusprev")) {
        if (!sel) return;
        Client *c = NULL, *i;
        for (i = clients; i != sel; i = i->next)
            if (i->tags & tagset) c = i;
        if (!c) for (; i; i = i->next) if (i->tags & tagset) c = i;
        if (c) focus(c);

    /* ── mfact <delta> ── */
    } else if (!strcmp(act, "mfact")) {
        float f = cfg.mfact + (float)atof(arg);
        if (f >= 0.05f && f <= 0.95f) { cfg.mfact = f; tile(); }

    /* ── togglefloat ── */
    } else if (!strcmp(act, "togglefloat")) {
        if (sel) { sel->isfloating = !sel->isfloating; tile(); }

    /* ── quit ── */
    } else if (!strcmp(act, "quit")) {
        running = 0;

    /* ── workspace <n> ── */
    } else if (!strcmp(act, "workspace")) {
        int n = atoi(arg);
        if (n < 1 || n > MAX_WORKSPACES) return;
        unsigned int newtag = 1u << (n - 1);
        if (newtag == tagset) return;
        tagset = newtag;
        for (Client *c = clients; c; c = c->next) {
            if (c->tags & tagset) XMoveWindow(dpy, c->win, c->x, c->y);
            else                  XMoveWindow(dpy, c->win, c->x + 2*sw, c->y);
        }
        focus(NULL);
        tile();

    /* ── moveto <n> ── */
    } else if (!strcmp(act, "moveto")) {
        if (!sel) return;
        int n = atoi(arg);
        if (n < 1 || n > MAX_WORKSPACES) return;
        sel->tags = 1u << (n - 1);
        char ws[MAX_CMD];
        snprintf(ws, sizeof(ws), "workspace %d", n);
        execute_action(ws);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  CLIENT LIST HELPERS
 * ═══════════════════════════════════════════════════════════════ */

static void detach(Client *c) {
    Client **tc;
    for (tc = &clients; *tc && *tc != c; tc = &(*tc)->next);
    *tc = c->next;
}
static void attach(Client *c)      { c->next  = clients; clients = c; }
static void detachstack(Client *c) {
    Client **tc;
    for (tc = &stack; *tc && *tc != c; tc = &(*tc)->snext);
    *tc = c->snext;
}
static void attachstack(Client *c) { c->snext = stack;   stack   = c; }
static Client *wintoclient(Window w) {
    for (Client *c = clients; c; c = c->next) if (c->win == w) return c;
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 *  X11 HELPERS
 * ═══════════════════════════════════════════════════════════════ */

static unsigned long getcolor(const char *colstr) {
    Colormap cmap = DefaultColormap(dpy, screen);
    XColor color;
    if (!XAllocNamedColor(dpy, cmap, colstr, &color, &color)) {
        fprintf(stderr, "trinitywm: bad color '%s'\n", colstr);
        return 0;
    }
    return color.pixel;
}

static void configure(Client *c) {
    XConfigureEvent ce = {
        .type              = ConfigureNotify,
        .display           = dpy,
        .event             = c->win,
        .window            = c->win,
        .x                 = c->x,
        .y                 = c->y,
        .width             = c->w - 2 * cfg.border_width,
        .height            = c->h - 2 * cfg.border_width,
        .border_width      = cfg.border_width,
        .above             = None,
        .override_redirect = False,
    };
    XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

static void resize(Client *c, int x, int y, int w, int h) {
    c->x = x; c->y = y; c->w = w; c->h = h;
    int rw = w - 2 * cfg.border_width;
    int rh = h - 2 * cfg.border_width;
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    XWindowChanges wc = {
        .x = x, .y = y, .width = rw, .height = rh,
        .border_width = cfg.border_width,
    };
    XConfigureWindow(dpy, c->win, CWX|CWY|CWWidth|CWHeight|CWBorderWidth, &wc);
    configure(c);
    XSync(dpy, False);
}

static void focus(Client *c) {
    if (!c || !(c->tags & tagset))
        for (c = stack; c && !(c->tags & tagset); c = c->snext);
    if (sel && sel != c)
        XSetWindowBorder(dpy, sel->win, col_inactive);
    if (c) {
        detachstack(c); attachstack(c);
        XSetWindowBorder(dpy, c->win, col_active);
        XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
        XRaiseWindow(dpy, c->win);
    } else {
        XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
    }
    sel = c;
}

/* ═══════════════════════════════════════════════════════════════
 *  TILING LAYOUT  (Master/Stack)
 * ═══════════════════════════════════════════════════════════════ */

static void tile(void) {
    int n = 0, i = 0, mw, my, sy;
    Client *c;

    for (c = clients; c; c = c->next)
        if ((c->tags & tagset) && !c->isfloating) n++;
    if (n == 0) return;

    mw = (n > cfg.nmaster) ? (int)(sw * cfg.mfact) : sw;
    my = sy = 0;

    for (c = clients; c; c = c->next) {
        if (!(c->tags & tagset) || c->isfloating) continue;
        if (i < cfg.nmaster) {
            int h = (sh - my) / (cfg.nmaster - i);
            resize(c, 0, my, mw, h);
            my += h;
        } else {
            int h = (sh - sy) / (n - i);
            resize(c, mw, sy, sw - mw, h);
            sy += h;
        }
        i++;
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  MOUSE DRAG  (Super+LMB move, Super+RMB resize)
 * ═══════════════════════════════════════════════════════════════ */

static void movemouse(void) {
    if (!sel) return;
    int ocx = sel->x, ocy = sel->y, rx, ry, di;
    unsigned int dui;
    Window dummy;
    XEvent ev;

    if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
                     None, None, CurrentTime) != GrabSuccess) return;
    XQueryPointer(dpy, root, &dummy, &dummy, &rx, &ry, &di, &di, &dui);
    if (!sel->isfloating) { sel->isfloating = 1; tile(); }

    for (;;) {
        XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
        if      (ev.type == MotionNotify)
            resize(sel, ocx+(ev.xmotion.x-rx), ocy+(ev.xmotion.y-ry), sel->w, sel->h);
        else if (ev.type == ButtonRelease) break;
    }
    XUngrabPointer(dpy, CurrentTime);
}

static void resizemouse(void) {
    if (!sel) return;
    int ocx = sel->x, ocy = sel->y;
    XEvent ev;

    if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
                     None, None, CurrentTime) != GrabSuccess) return;
    if (!sel->isfloating) { sel->isfloating = 1; tile(); }

    for (;;) {
        XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
        if (ev.type == MotionNotify) {
            int nw = ev.xmotion.x - ocx;
            int nh = ev.xmotion.y - ocy;
            resize(sel, sel->x, sel->y, nw < 20 ? 20 : nw, nh < 20 ? 20 : nh);
        } else if (ev.type == ButtonRelease) break;
    }
    XUngrabPointer(dpy, CurrentTime);
}

/* ═══════════════════════════════════════════════════════════════
 *  GRAB KEYS / BUTTONS
 * ═══════════════════════════════════════════════════════════════ */

static void grabkeys(void) {
    /* Lock key variants to grab: none, CapsLock, NumLock, both */
    static const unsigned int locks[] = { 0, LockMask, Mod2Mask, LockMask|Mod2Mask };
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    for (Keybind *kb = keybinds; kb; kb = kb->next) {
        KeyCode code = XKeysymToKeycode(dpy, kb->key);
        if (code)
            for (unsigned int i = 0; i < sizeof(locks)/sizeof(locks[0]); i++)
                XGrabKey(dpy, code, kb->mod | locks[i], root, True, GrabModeAsync, GrabModeAsync);
    }
}

static void grabbuttons(Client *c, int focused) {
    XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
    if (focused) {
        XGrabButton(dpy, Button1, Mod4Mask, c->win, False, BUTTONMASK, GrabModeAsync, GrabModeSync, None, None);
        XGrabButton(dpy, Button3, Mod4Mask, c->win, False, BUTTONMASK, GrabModeAsync, GrabModeSync, None, None);
    } else {
        XGrabButton(dpy, AnyButton, AnyModifier, c->win, False, BUTTONMASK, GrabModeAsync, GrabModeSync, None, None);
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  MANAGE / UNMANAGE WINDOWS
 * ═══════════════════════════════════════════════════════════════ */

static void manage(Window w, XWindowAttributes *wa) {
    Client *c = calloc(1, sizeof(Client));
    if (!c) { fprintf(stderr, "trinitywm: out of memory\n"); exit(1); }
    c->win = w;
    c->x = wa->x; c->y = wa->y;
    c->w = wa->width; c->h = wa->height;
    c->tags = tagset;

    XSelectInput(dpy, c->win, EnterWindowMask|FocusChangeMask|PropertyChangeMask|StructureNotifyMask);
    grabbuttons(c, 0);
    XSetWindowBorderWidth(dpy, c->win, cfg.border_width);
    XSetWindowBorder(dpy, c->win, col_inactive);
    XMapWindow(dpy, c->win);
    attach(c); attachstack(c);
    focus(c);
    tile();
}

static void unmanage(Client *c, int destroyed) {
    (void)destroyed;
    detach(c); detachstack(c);
    if (sel == c) focus(NULL);
    free(c);
    tile();
}

/* ═══════════════════════════════════════════════════════════════
 *  EVENT HANDLERS
 * ═══════════════════════════════════════════════════════════════ */

static void ev_buttonpress(XEvent *e) {
    XButtonPressedEvent *ev = &e->xbutton;
    Client *c = wintoclient(ev->window);
    if (!c) return;
    focus(c);
    if (ev->state == Mod4Mask) {
        if      (ev->button == Button1) movemouse();
        else if (ev->button == Button3) resizemouse();
    }
}

static void ev_configurerequest(XEvent *e) {
    XConfigureRequestEvent *ev = &e->xconfigurerequest;
    Client *c = wintoclient(ev->window);
    if (c) {
        if (ev->value_mask & CWX)      c->x = ev->x;
        if (ev->value_mask & CWY)      c->y = ev->y;
        if (ev->value_mask & CWWidth)  c->w = ev->width;
        if (ev->value_mask & CWHeight) c->h = ev->height;
        resize(c, c->x, c->y, c->w, c->h);
    } else {
        XWindowChanges wc = {
            .x = ev->x, .y = ev->y, .width = ev->width, .height = ev->height,
            .border_width = ev->border_width, .sibling = ev->above, .stack_mode = ev->detail,
        };
        XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
    }
    XSync(dpy, False);
}

static void ev_destroynotify(XEvent *e) {
    Client *c = wintoclient(e->xdestroywindow.window);
    if (c) unmanage(c, 1);
}

static void ev_keypress(XEvent *e) {
    XKeyEvent *ev = &e->xkey;
    KeySym ks = XLookupKeysym(ev, 0);
    unsigned int state = CLEANMASK(ev->state);
    for (Keybind *kb = keybinds; kb; kb = kb->next)
        if (ks == kb->key && state == CLEANMASK(kb->mod))
            execute_action(kb->action);
}

static void ev_maprequest(XEvent *e) {
    XWindowAttributes wa;
    Window w = e->xmaprequest.window;
    if (!XGetWindowAttributes(dpy, w, &wa) || wa.override_redirect) return;
    if (!wintoclient(w)) manage(w, &wa);
}

static void ev_enternotify(XEvent *e) {
    XCrossingEvent *ev = &e->xcrossing;
    /* ignore inferior/grab events to avoid spurious focus changes */
    if (ev->mode != NotifyNormal || ev->detail == NotifyInferior) return;
    Client *c = wintoclient(ev->window);
    if (c) focus(c);
}

static void ev_unmapnotify(XEvent *e) {
    Client *c = wintoclient(e->xunmap.window);
    if (c) unmanage(c, 0);
}

/* ═══════════════════════════════════════════════════════════════
 *  ERROR HANDLING
 * ═══════════════════════════════════════════════════════════════ */

static int xerrordummy(Display *d, XErrorEvent *ee) { (void)d; (void)ee; return 0; }

static int xerror(Display *d, XErrorEvent *ee) {
    if (ee->error_code == BadWindow ||
        (ee->request_code == X_SetInputFocus  && ee->error_code == BadMatch)  ||
        (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch)  ||
        (ee->request_code == X_KillClient      && ee->error_code == BadValue))
        return 0;
    fprintf(stderr, "trinitywm: X error: request=%d error=%d\n",
            ee->request_code, ee->error_code);
    return xerrorxlib(d, ee);
}

/* ═══════════════════════════════════════════════════════════════
 *  SETUP  /  MAIN LOOP  /  CLEANUP
 * ═══════════════════════════════════════════════════════════════ */

static void sigchld(int s) {
    (void)s;
    if (signal(SIGCHLD, sigchld) == SIG_ERR) { perror("signal"); exit(1); }
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

static void setup(void) {
    sigchld(0);
    load_config();

    screen = DefaultScreen(dpy);
    sw     = DisplayWidth(dpy,  screen);
    sh     = DisplayHeight(dpy, screen);
    root   = RootWindow(dpy,    screen);

    setup_ewmh();

    col_active   = getcolor(cfg.color_active);

    col_inactive = getcolor(cfg.color_inactive);

    XSetWindowAttributes wa;
    wa.cursor    = XCreateFontCursor(dpy, XC_left_ptr);
    wa.event_mask = SubstructureRedirectMask | SubstructureNotifyMask
                  | ButtonPressMask          | KeyPressMask;
    XChangeWindowAttributes(dpy, root, CWEventMask|CWCursor, &wa);
    grabkeys();
    setup_inotify();
}

static void setup_ewmh(void) {
    Atom net_supporting_wm_check = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom net_supported = XInternAtom(dpy, "_NET_SUPPORTED", False);
    Atom utf8_string = XInternAtom(dpy, "UTF8_STRING", False);

    /* Create an invisible window to act as the supporting WM window */
    Window wm_check_win = XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);

    /* Set properties on root window */
    XChangeProperty(dpy, root, net_supporting_wm_check, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&wm_check_win, 1);

    /* Set properties on supporting window */
    XChangeProperty(dpy, wm_check_win, net_supporting_wm_check, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&wm_check_win, 1);
    XChangeProperty(dpy, wm_check_win, net_wm_name, utf8_string, 8,
                    PropModeReplace, (unsigned char *)"Trinity WM", 10);

    /* Advertise support */
    Atom supported_atoms[] = { net_supporting_wm_check, net_wm_name };
    XChangeProperty(dpy, root, net_supported, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)supported_atoms, 2);
}


static void drain_inotify(void) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t len = read(inotify_fd, buf, sizeof(buf));
        if (len <= 0) break;
        const struct inotify_event *ev;
        for (char *ptr = buf; ptr < buf + len;
             ptr += sizeof(struct inotify_event) + ev->len) {
            ev = (const struct inotify_event *)ptr;
            /* Check if our config file was the one changed */
            if (ev->len > 0 && strstr(ev->name, "trinitywm.conf"))
                reload_config();
        }
    }
}

static void run(void) {
    static void (*handlers[LASTEvent])(XEvent *) = {
        [ButtonPress]      = ev_buttonpress,
        [ConfigureRequest] = ev_configurerequest,
        [DestroyNotify]    = ev_destroynotify,
        [KeyPress]         = ev_keypress,
        [MapRequest]       = ev_maprequest,
        [EnterNotify]      = ev_enternotify,
        [UnmapNotify]      = ev_unmapnotify,
    };

    int xfd = ConnectionNumber(dpy);
    XSync(dpy, False);

    while (running) {
        /* Process any pending X events first */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type < LASTEvent && handlers[ev.type])
                handlers[ev.type](&ev);
        }

        /* select() on both X11 fd and inotify fd */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        int maxfd = xfd;
        if (inotify_fd >= 0) {
            FD_SET(inotify_fd, &fds);
            if (inotify_fd > maxfd) maxfd = inotify_fd;
        }

        if (select(maxfd + 1, &fds, NULL, NULL, NULL) > 0) {
            if (inotify_fd >= 0 && FD_ISSET(inotify_fd, &fds))
                drain_inotify();
            /* X events will be handled at top of loop */
        }
    }
}

static void cleanup(void) {
    while (clients) unmanage(clients, 0);
    free_keybinds();
    if (inotify_wd >= 0) inotify_rm_watch(inotify_fd, inotify_wd);
    if (inotify_fd >= 0) close(inotify_fd);
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    XSync(dpy, False);
    XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
}

int main(void) {
    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "trinitywm: cannot open display\n");
        return 1;
    }
    xerrorxlib = XSetErrorHandler(xerror);
    setup();
    run();
    cleanup();
    XCloseDisplay(dpy);
    return 0;
}
