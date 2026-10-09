// Finding the game and reading its own files (from its pakchunk*-WindowsNoEditor.pak, later paks winning).
#include "patcher.h"

#define GAME_DIR L"Marvel's Midnight Suns"
#define PAKS_SUB L"MidnightSuns\\Content\\Paks"

int patcher_need_game;

static int is_game_pak(const wchar_t *n)
{
    if (_wcsnicmp(n, L"pakchunk", 8)) return 0;
    const wchar_t *p = n + 8;
    if (!iswdigit(*p)) return 0;
    while (iswdigit(*p)) p++;
    return !_wcsicmp(p, L"-WindowsNoEditor.pak");
}

int game_paks_valid(const wchar_t *paks)
{
    wchar_t *p = wpath(paks, L"pakchunk0-WindowsNoEditor.pak");
    int ok = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    free(p);
    return ok;
}

static wchar_t *settings_path(void) { return wpath(data_dir(), L"SuitPatcher.ini"); }

void game_remember_paks(const wchar_t *paks)
{
    wchar_t *s = settings_path();
    WritePrivateProfileStringW(L"Game", L"Paks", paks, s);
    free(s);
}

// a game folder or a Steam library: return its Paks folder if the game is there
static wchar_t *try_dir(const wchar_t *dir, const wchar_t *sub)
{
    wchar_t *a = sub ? wpath(dir, sub) : _wcsdup(dir);
    wchar_t *p = wpath(a, PAKS_SUB);
    free(a);
    if (game_paks_valid(p)) return p;
    free(p);
    return NULL;
}

// a Unix path (Linux Steam, seen from Wine/Proton) -> Z:\...
static wchar_t *unix_to_wine(const char *u)
{
    size_t n = strlen(u);
    char *s = xmalloc(n + 3);
    s[0] = 'Z'; s[1] = ':';
    memcpy(s + 2, u, n + 1);
    for (char *c = s; *c; c++) if (*c == '/') *c = '\\';
    wchar_t *w = utf8_to_w(s);
    free(s);
    return w;
}

// "path" values of a Steam libraryfolders.vdf
static wchar_t *from_vdf(const wchar_t *steam, int unix_paths)
{
    wchar_t *v = wpath(steam, L"steamapps\\libraryfolders.vdf");
    size_t n;
    char *d = (char *)file_read_all(v, &n);
    free(v);
    wchar_t *found = try_dir(steam, L"steamapps\\common\\" GAME_DIR);
    if (found || !d) { free(d); return found; }
    for (char *q = strstr(d, "\"path\""); q && !found; q = strstr(q + 6, "\"path\"")) {
        char *a = strchr(q + 6, '"');
        if (!a) break;
        char *b = a + 1;
        buf s = {0};
        while (*b && *b != '"') {
            if (*b == '\\' && b[1]) b++;                        // vdf escapes "\\"
            buf_u8(&s, (uint8_t)*b++);
        }
        buf_u8(&s, 0);
        wchar_t *lib = unix_paths && s.p[0] == '/' ? unix_to_wine((char *)s.p) : utf8_to_w((char *)s.p);
        found = try_dir(lib, L"steamapps\\common\\" GAME_DIR);
        free(lib);
        buf_free(&s);
    }
    free(d);
    return found;
}

static wchar_t *reg_string(HKEY root, const wchar_t *key, const wchar_t *val)
{
    wchar_t buf_[1024];
    DWORD n = sizeof buf_ - sizeof(wchar_t), type;
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WOW64_32KEY, &k)) return NULL;
    LONG r = RegQueryValueExW(k, val, NULL, &type, (BYTE *)buf_, &n);
    RegCloseKey(k);
    if (r || (type != REG_SZ && type != REG_EXPAND_SZ)) return NULL;
    buf_[n / sizeof(wchar_t)] = 0;
    for (wchar_t *c = buf_; *c; c++) if (*c == L'/') *c = L'\\';
    return _wcsdup(buf_);
}

// Epic: the launcher's install manifests (Windows); Heroic's legendary list (Linux, via Wine's Z: drive)
static wchar_t *from_epic_json(const char *data, size_t n, const char *loc_key, int unix_paths)
{
    wchar_t *found = NULL;
    for (const char *q = strstr(data, loc_key); q && !found; q = strstr(q + 1, loc_key)) {
        const char *a = strchr(q + strlen(loc_key), '"');
        if (!a) break;
        buf s = {0};
        for (const char *b = a + 1; *b && *b != '"'; b++) {
            if (*b == '\\' && b[1]) b++;
            buf_u8(&s, (uint8_t)*b);
        }
        buf_u8(&s, 0);
        if (strstr((char *)s.p, "Midnight") || strstr((char *)s.p, "Wombat")) {
            wchar_t *dir = unix_paths && s.p[0] == '/' ? unix_to_wine((char *)s.p) : utf8_to_w((char *)s.p);
            found = try_dir(dir, NULL);
            free(dir);
        }
        buf_free(&s);
    }
    (void)n;
    return found;
}

wchar_t *game_find_paks(const wchar_t *beside)
{
    // 1. the mod pak sits in the game's Paks folder
    if (beside) {
        wchar_t *d = _wcsdup(beside);
        wchar_t *s = wcsrchr(d, L'\\');
        if (s) { *s = 0; if (game_paks_valid(d)) return d; }
        free(d);
    }
    // 2. the folder chosen last time
    wchar_t *ini = settings_path(), saved[1024];
    GetPrivateProfileStringW(L"Game", L"Paks", L"", saved, 1024, ini);
    free(ini);
    if (saved[0] && game_paks_valid(saved)) return _wcsdup(saved);
    wchar_t *found = NULL;
    // 3. Steam on Windows
    static const struct { HKEY root; const wchar_t *key, *val; } R[] = {
        {HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 368260", L"InstallLocation"},
    };
    for (int i = 0; i < 4 && !found; i++) {
        wchar_t *s = reg_string(R[i].root, R[i].key, R[i].val);
        if (!s) continue;
        found = i == 3 ? try_dir(s, NULL) : from_vdf(s, 0);
        free(s);
    }
    static const wchar_t *defaults[] = {L"C:\\Program Files (x86)\\Steam", L"C:\\Program Files\\Steam"};
    for (int i = 0; i < 2 && !found; i++) found = from_vdf(defaults[i], 0);
    // 4. Epic on Windows
    if (!found) {
        WIN32_FIND_DATAW fd;
        const wchar_t *dir = L"C:\\ProgramData\\Epic\\EpicGamesLauncher\\Data\\Manifests\\";
        wchar_t *pat = wpath(dir, L"*.item");
        HANDLE f = FindFirstFileW(pat, &fd);
        free(pat);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                wchar_t *p = wpath(dir, fd.cFileName);
                size_t n;
                char *d = (char *)file_read_all(p, &n);
                free(p);
                if (d && strstr(d, "Midnight Suns")) found = from_epic_json(d, n, "\"InstallLocation\"", 0);
                free(d);
            } while (!found && FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    // 5. Linux (Wine / Proton): Steam and Heroic in the Unix home folders, seen through the Z: drive. Wine doesn't
    // pass $HOME on, so every Z:\home\* is tried (and $HOME / WINEHOMEDIR when they are there).
    if (!found) {
        char **homes = NULL;
        int nh = 0;
        const char *env[2] = {getenv("HOME"), getenv("WINEHOMEDIR")};
        for (int i = 0; i < 2; i++) {
            const char *h = env[i];
            if (!h) continue;
            if (!strncmp(h, "\\??\\", 4)) h += 4;
            if (h[0] == 'Z' && h[1] == ':') h += 2;
            if (h[0] != '/' && h[0] != '\\') continue;
            homes = xrealloc(homes, sizeof(char *) * (nh + 1));
            homes[nh] = xstrdup(h);
            for (char *c = homes[nh]; *c; c++) if (*c == '\\') *c = '/';
            nh++;
        }
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(L"Z:\\home\\*", &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                char *u = w_to_utf8(fd.cFileName);
                homes = xrealloc(homes, sizeof(char *) * (nh + 1));
                homes[nh] = xmalloc(strlen(u) + 8);
                sprintf(homes[nh++], "/home/%s", u);
                free(u);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        static const char *steams[] = {"/.steam/steam", "/.local/share/Steam", "/.steam/debian-installation",
                                       "/.var/app/com.valvesoftware.Steam/.local/share/Steam", "/snap/steam/common/.local/share/Steam"};
        static const char *heroic[] = {"/.config/heroic/legendaryConfig/legendary/installed.json",
                                       "/.var/app/com.heroicgameslauncher.hgl/config/heroic/legendaryConfig/legendary/installed.json"};
        for (int h = 0; h < nh && !found; h++) {
            for (int i = 0; i < 5 && !found; i++) {
                char *u = xmalloc(strlen(homes[h]) + 64);
                sprintf(u, "%s%s", homes[h], steams[i]);
                wchar_t *w = unix_to_wine(u);
                found = from_vdf(w, 1);
                free(w); free(u);
            }
            for (int i = 0; i < 2 && !found; i++) {
                char *u = xmalloc(strlen(homes[h]) + 128);
                sprintf(u, "%s%s", homes[h], heroic[i]);
                wchar_t *w = unix_to_wine(u);
                size_t n;
                char *d = (char *)file_read_all(w, &n);
                if (d) found = from_epic_json(d, n, "\"install_path\"", 1);
                free(d); free(w); free(u);
            }
        }
        for (int h = 0; h < nh; h++) free(homes[h]);
        free(homes);
    }
    return found;
}

// ---------------------------------------------------------------- index

typedef struct { char *path; int pak; pak_entry e; } gfile;

struct gindex {
    pak *paks; int npaks;
    gfile *files; int n, cap;
    int *slots; size_t nslots;                                  // open addressing: index + 1, 0 = empty
    char **extra; int nextra;
};

static uint64_t path_hash(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++) { uint8_t c = (uint8_t)*s; if (c >= 'A' && c <= 'Z') c += 32; h ^= c; h *= 0x100000001b3ull; }
    return h;
}

static int *slot_of(gindex *g, const char *path)
{
    size_t m = g->nslots - 1, i = (size_t)path_hash(path) & m;
    while (g->slots[i] && _stricmp(g->files[g->slots[i] - 1].path, path)) i = (i + 1) & m;
    return &g->slots[i];
}

static void rehash(gindex *g)
{
    free(g->slots);
    g->nslots = 1024;
    while (g->nslots < (size_t)g->n * 3) g->nslots *= 2;
    g->slots = xcalloc(g->nslots, sizeof(int));
    for (int i = 0; i < g->n; i++) *slot_of(g, g->files[i].path) = i + 1;
}

static int contains_ci(const char *s, const char *sub)
{
    size_t n = strlen(sub);
    for (; *s; s++) if (!_strnicmp(s, sub, n)) return 1;
    return 0;
}

int game_index_everything;                                      // test tool: index every folder

static int index_filter(const char *dir, const char *name, void *ctx)
{
    gindex *g = ctx;
    // suits, palettes, the heroes' models and character templates (default hair), weapon items and weapons
    int want = game_index_everything || contains_ci(dir, "/Character/") || contains_ci(dir, "/TacticalOutfits/") || contains_ci(dir, "/Palettes/") ||
               contains_ci(dir, "/Rarities/") || contains_ci(dir, "/Templates/Characters/Heroes/") ||
               contains_ci(dir, "/Templates/Items/Weapons/") || contains_ci(dir, "/Weapons/");
    for (int i = 0; i < g->nextra && !want; i++) want = !_stricmp(dir, g->extra[i]);
    if (!_stricmp(dir, "CodaGame/")) return !name || !_strnicmp(name, "AssetRegistry", 13);   // the registries
    if (!want || _strnicmp(dir, "CodaGame/Content/", 17)) return 0;
    (void)name;
    return 1;
}

gindex *game_open(const wchar_t *paks, char **extra_dirs, int nextra)
{
    gindex *g = xcalloc(1, sizeof *g);
    g->extra = extra_dirs; g->nextra = nextra;
    wchar_t *pat = wpath(paks, L"pakchunk*.pak");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    free(pat);
    wchar_t **names = NULL;
    int nn = 0;
    if (f != INVALID_HANDLE_VALUE) {
        do if (is_game_pak(fd.cFileName)) { names = xrealloc(names, sizeof(wchar_t *) * (nn + 1)); names[nn++] = _wcsdup(fd.cFileName); }
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (!nn) sr_fail("no game paks (pakchunk*-WindowsNoEditor.pak) in %ls", paks);
    // later paks override earlier ones: pakchunk0, pakchunk1, ... pakchunk11 (by number)
    for (int a = 1; a < nn; a++)
        for (int b = a; b > 0 && _wtoi(names[b - 1] + 8) > _wtoi(names[b] + 8); b--) { wchar_t *t = names[b]; names[b] = names[b - 1]; names[b - 1] = t; }
    g->paks = xcalloc(nn, sizeof(pak));
    g->npaks = nn;
    g->nslots = 1024;
    g->slots = xcalloc(g->nslots, sizeof(int));
    for (int i = 0; i < nn; i++) {
        wchar_t *p = wpath(paks, names[i]);
        int r = pak_open(&g->paks[i], p, 1);
        free(p);
        if (r) sr_fail("the game's %ls could not be read", names[i]);
        int ne;
        pak_entry *e = pak_list(&g->paks[i], index_filter, g, &ne);
        for (int k = 0; k < ne; k++) {
            if (g->n * 2 >= (int)g->nslots) { rehash(g); }
            int *s = slot_of(g, e[k].path);
            if (*s) {                                           // overridden by a later pak
                gfile *old = &g->files[*s - 1];
                free(old->e.path); free(old->e.blocks);
                old->e = e[k];
                old->path = e[k].path;
                old->pak = i;
                continue;
            }
            if (g->n == g->cap) { g->cap = g->cap ? 2 * g->cap : 4096; g->files = xrealloc(g->files, sizeof(gfile) * g->cap); }
            g->files[g->n] = (gfile){e[k].path, i, e[k]};
            g->n++;
            *slot_of(g, e[k].path) = g->n;
        }
        free(e);
        free(names[i]);
    }
    free(names);
    log_msg("game files indexed: %d", g->n);
    return g;
}

void game_close(gindex *g)
{
    if (!g) return;
    for (int i = 0; i < g->n; i++) { free(g->files[i].e.path); free(g->files[i].e.blocks); }
    for (int i = 0; i < g->npaks; i++) pak_close(&g->paks[i]);
    free(g->paks); free(g->files); free(g->slots);
    free(g);
}

int game_has(gindex *g, const char *path) { return *slot_of(g, path) != 0; }

uint8_t *game_read(gindex *g, const char *path, size_t *n)
{
    int s = *slot_of(g, path);
    if (!s) return NULL;
    gfile *f = &g->files[s - 1];
    uint8_t *d = pak_read(&g->paks[f->pak], &f->e, 1);
    *n = (size_t)f->e.usize;
    return d;
}

void game_each(gindex *g, game_each_fn f, void *ctx)
{
    for (int i = 0; i < g->n; i++) f(g->files[i].path, ctx);
}

char *game_to_pak(const char *game_path, const char *ext)
{
    if (strncmp(game_path, "/Game/", 6)) sr_fail("not a game path: %s", game_path);
    size_t n = strlen(game_path), e = strlen(ext);
    char *s = xmalloc(n + e + 32);
    sprintf(s, "CodaGame/Content/%s%s", game_path + 6, ext);
    return s;
}

char *pak_to_game(const char *p)
{
    const char *c = strstr(p, "/Content/");
    if (!c) return NULL;
    // only <Project>/Content/ (CodaGame, or the MidnightSuns folder some mods use)
    if (memchr(p, '/', c - p)) return NULL;
    const char *rest = c + 9;
    size_t n = strlen(rest);
    const char *dot = strrchr(rest, '.');
    if (dot && !strchr(dot, '/')) n = dot - rest;
    char *s = xmalloc(n + 8);
    memcpy(s, "/Game/", 6);
    memcpy(s + 6, rest, n);
    s[6 + n] = 0;
    return s;
}
