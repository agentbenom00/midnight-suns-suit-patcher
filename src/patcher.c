// Analyse a suit mod and patch it into a standalone suit that the SuitRegistry DLL registers.
//
// Two kinds of suit mods:
//  - replacers (most mods): they override the game's own packages of a suit (mesh, textures, ...). The patcher finds
//    the game outfit that uses those packages and clones everything between that outfit and the mod's files under
//    new names (<name>_<Tag>): the outfit, its palettes, the mod's packages and any game package in between (e.g. a
//    material pointing at a modded texture). The original suit is then untouched, and the copy is a new suit.
//  - standalone suits: they bring a new outfit (and usually their own AssetRegistry.bin, which would hide every other
//    suit mod). The registry is dropped and the outfit and its palettes are registered from similar game entries.
// In both cases the outfit gets the chosen name and rarity, a new EntitlementID, is unlocked from the start and free,
// and its palettes get names, new IDs and are free. CodaGame/SuitMods/<id>.json tells the SuitRegistry DLL which game
// registry entries to clone for the new packages.
// Add-on paks (hair, weapon, ... made for the same suit) are merged in as if they were part of the mod: the files of
// theirs that the suit uses are cloned with it, and everything goes into the one patched pak.
#include "patcher.h"
#include <ctype.h>
#include <math.h>
#include <tlhelp32.h>
#include <wchar.h>

struct mod_file {
    char *path;                                                 // as in the mod pak
    pak_entry e;
    int src;                                                    // 0 = the mod, k + 1 = add-on k
    int pkg;                                                    // mod package index, -1 = not part of a package
    char ext[16];
    int drop;                                                   // left out of the patched pak
};

struct mod_pkg {
    char *game;                                                 // /Game path
    char *prefix;                                               // pak path up to and with "Content/"
    int override;                                               // the game has this package too
    int fu, fe;                                                 // .uasset / .uexp file
    int src;                                                    // where its .uasset comes from (mod_file.src)
    char *cls;                                                  // class of its main export
    int in_clone;                                               // moved to a new name
};

enum { K_OTHER, K_OUTFIT, K_PALETTE };

struct clone_item {
    char *game;                                                 // original /Game path
    int mod;                                                    // mod package, -1 = the game's package
    int kind;
    int rename;                                                 // 1 = cloned under a new name (replacer mode)
    char *donor;                                                // registry donor (standalone mode)
    int unregistered;                                           // the original has no registry entry: none for the copy
};

// ---------------------------------------------------------------- small string set / map

typedef struct { char **k; int *v; size_t cap; int n; } smap;

static uint64_t shash(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++) { uint8_t c = (uint8_t)*s; if (c >= 'A' && c <= 'Z') c += 32; h ^= c; h *= 0x100000001b3ull; }
    return h;
}

static int *smap_slot(smap *m, const char *k, int add)
{
    if (add && (size_t)(m->n + 1) * 2 > m->cap) {
        size_t oc = m->cap;
        char **ok = m->k;
        int *ov = m->v;
        m->cap = m->cap ? m->cap * 2 : 256;
        m->k = xcalloc(m->cap, sizeof(char *));
        m->v = xcalloc(m->cap, sizeof(int));
        for (size_t i = 0; i < oc; i++)
            if (ok[i]) {
                size_t j = (size_t)shash(ok[i]) & (m->cap - 1);
                while (m->k[j]) j = (j + 1) & (m->cap - 1);
                m->k[j] = ok[i];
                m->v[j] = ov[i];
            }
        free(ok); free(ov);
    }
    if (!m->cap) return NULL;
    size_t j = (size_t)shash(k) & (m->cap - 1);
    while (m->k[j] && _stricmp(m->k[j], k)) j = (j + 1) & (m->cap - 1);
    if (!m->k[j]) {
        if (!add) return NULL;
        m->k[j] = xstrdup(k);
        m->v[j] = -1;
        m->n++;
    }
    return &m->v[j];
}

static int smap_get(smap *m, const char *k) { int *v = smap_slot(m, k, 0); return v ? *v : -1; }
static void smap_put(smap *m, const char *k, int v) { *smap_slot(m, k, 1) = v; }
static void smap_free(smap *m)
{
    for (size_t i = 0; i < m->cap; i++) free(m->k[i]);
    free(m->k); free(m->v);
    memset(m, 0, sizeof *m);
}

// ---------------------------------------------------------------- reading packages (mod first, then the game)

typedef struct {
    analysis *a;
    smap mods;                                                  // /Game path -> mod package
} ctx;

static int ends_with(const char *s, const char *e)
{
    size_t a = strlen(s), b = strlen(e);
    return a >= b && !_stricmp(s + a - b, e);
}

static uint8_t *mod_read(analysis *a, int file, size_t *n)
{
    mod_file *f = &a->files[file];
    uint8_t *d = pak_read(f->src ? &a->addon[f->src - 1] : &a->mod, &f->e, 1);
    *n = (size_t)f->e.usize;
    return d;
}

static int is_indexed(ctx *c, const char *game)
{
    if (smap_get(&c->mods, game) >= 0) return 1;
    char *p = game_to_pak(game, ".uasset");
    int r = game_has(c->a->game, p);
    free(p);
    return r;
}

// the package (header only, or with its .uexp); NULL if neither the mod nor the game has it
static upkg *load_pkg(ctx *c, const char *game, int full, int *from_mod)
{
    analysis *a = c->a;
    int m = smap_get(&c->mods, game);
    size_t an = 0, en = 0;
    uint8_t *ua = NULL, *ue = NULL;
    if (m >= 0 && a->pkgs[m].fu >= 0) {
        ua = mod_read(a, a->pkgs[m].fu, &an);
        if (full && a->pkgs[m].fe >= 0) ue = mod_read(a, a->pkgs[m].fe, &en);
    } else {
        m = -1;
        char *p = game_to_pak(game, ".uasset");
        ua = game_read(a->game, p, &an);
        free(p);
        if (!ua) return NULL;
        if (full) {
            p = game_to_pak(game, ".uexp");
            ue = game_read(a->game, p, &en);
            free(p);
        }
    }
    if (from_mod) *from_mod = m;
    return upkg_load_ex(ua, an, ue, en, !full);
}

// ---------------------------------------------------------------- reachability (replacer mode)

typedef struct {
    char *path;
    int state;                                                  // 0 new, 1 visiting, 2 done
    uint64_t *bits;                                             // which overridden packages it leads to
    int *kids; int nkids;
} node;

typedef struct {
    ctx *c;
    smap ids;
    node *v; int n, cap;
    int words;
    smap sidx;                                                  // overridden package -> bit
    int reads;
} graph;

static int is_item_dir(const char *g)
{
    return strstr(g, "/TacticalOutfits/") || strstr(g, "/Palettes/");
}

static int node_of(graph *gr, const char *path)
{
    int i = smap_get(&gr->ids, path);
    if (i >= 0) return i;
    if (gr->n == gr->cap) { gr->cap = gr->cap ? 2 * gr->cap : 256; gr->v = xrealloc(gr->v, sizeof(node) * gr->cap); }
    node *nd = &gr->v[gr->n];
    memset(nd, 0, sizeof *nd);
    nd->path = xstrdup(path);
    nd->bits = xcalloc(gr->words, sizeof(uint64_t));
    smap_put(&gr->ids, path, gr->n);
    return gr->n++;
}

static int bits_any(graph *gr, const uint64_t *b) { for (int i = 0; i < gr->words; i++) if (b[i]) return 1; return 0; }
static int bits_count(graph *gr, const uint64_t *b) { int n = 0; for (int i = 0; i < gr->words; i++) n += __builtin_popcountll(b[i]); return n; }
// bits below `upto` only (the mod's own files come first, the add-ons' after them)
static int bits_count_upto(const uint64_t *b, int upto)
{
    int n = 0;
    for (int i = 0; i < upto; i++) n += (int)(b[i / 64] >> (i % 64)) & 1;
    return n;
}

static void reach(graph *gr, int id, int root)
{
    if (gr->v[id].state) return;
    gr->v[id].state = 1;
    char *path = xstrdup(gr->v[id].path);
    int s = smap_get(&gr->sidx, path);
    if (s >= 0) gr->v[id].bits[s / 64] |= 1ull << (s % 64);
    upkg *p = NULL;
    jmp_buf j, *outer = sr_jmp;
    sr_jmp = &j;
    if (!setjmp(j)) p = load_pkg(gr->c, path, 0, NULL);
    else log_msg("note: %s could not be read (%s)", path, sr_error);
    sr_jmp = outer;
    gr->reads++;
    if (p) {
        int nr;
        char **refs = upkg_refs(p, &nr);
        upkg_free(p);
        for (int i = 0; i < nr; i++) {
            // don't wander from one item into other items (palettes list their outfits, outfits their palettes), nor
            // into the hero's character template (it leads to everything the hero uses)
            if (_stricmp(refs[i], path) && (!is_item_dir(refs[i]) || root) && !strstr(refs[i], "/Templates/Characters/") &&
                is_indexed(gr->c, refs[i]) &&
                !(is_item_dir(refs[i]) && strstr(refs[i], "/TacticalOutfits/") && strstr(path, "/Palettes/"))) {
                int k = node_of(gr, refs[i]);
                node *nd = &gr->v[id];
                nd->kids = xrealloc(nd->kids, sizeof(int) * (nd->nkids + 1));
                nd->kids[nd->nkids++] = k;
                reach(gr, k, 0);
                for (int w = 0; w < gr->words; w++) gr->v[id].bits[w] |= gr->v[k].bits[w];
            }
            free(refs[i]);
        }
        free(refs);
    }
    free(path);
    gr->v[id].state = 2;
}

// nodes under `id` that lead to an overridden package: they are cloned
static void collect(graph *gr, int id, smap *seen, char ***out, int *n)
{
    if (smap_get(seen, gr->v[id].path) >= 0 || !bits_any(gr, gr->v[id].bits)) return;
    smap_put(seen, gr->v[id].path, 1);
    *out = xrealloc(*out, sizeof(char *) * (*n + 1));
    (*out)[(*n)++] = xstrdup(gr->v[id].path);
    for (int i = 0; i < gr->v[id].nkids; i++) collect(gr, gr->v[id].kids[i], seen, out, n);
}

// ---------------------------------------------------------------- helpers

static const char *ref_hero(const char *outfit)
{
    static char h[16];
    const char *s = strstr(upkg_short(outfit), "_HR_");
    h[0] = 0;
    if (s) {
        s += 4;
        size_t n = strcspn(s, "_");
        if (n < sizeof h) { memcpy(h, s, n); h[n] = 0; }
    }
    return h;
}

static char *obj_path(const char *pkg)
{
    const char *s = upkg_short(pkg);
    char *o = xmalloc(strlen(pkg) + strlen(s) + 2);
    sprintf(o, "%s.%s", pkg, s);
    return o;
}

// what a suit can bring of its own instead of the hero's defaults (outfit property, analysis.look index)
static const char *LOOK_PROP[NLOOKS] = {"HeadMesh", "HairMesh"};
static const char *LOOK_WHAT[NLOOKS] = {"head", "hair"};

static void add_item(analysis *a, const char *game, int mod, int kind, int rename)
{
    for (int i = 0; i < a->nitems; i++)
        if (!_stricmp(a->items[i].game, game)) { if (kind) a->items[i].kind = kind; return; }
    a->items = xrealloc(a->items, sizeof(clone_item) * (a->nitems + 1));
    clone_item *it = &a->items[a->nitems++];
    memset(it, 0, sizeof *it);
    it->game = xstrdup(game);
    it->mod = mod;
    it->kind = kind;
    it->rename = rename;
}

// palettes in name order, the default one first
static int cmp_str(const void *x, const void *y)
{
    const char *a = *(char *const *)x, *b = *(char *const *)y;
    int da = ends_with(a, "_Default"), db = ends_with(b, "_Default");
    return da != db ? db - da : _stricmp(a, b);
}

// colours and automatic names of the palettes (names unique within the suit)
static void describe_palettes(ctx *c, char **pals, int npal)
{
    analysis *a = c->a;
    a->npal = npal < MAX_PALETTES ? npal : MAX_PALETTES;
    for (int i = 0; i < a->npal; i++) {
        pal_info *pi = &a->pal[i];
        pi->pkg = xstrdup(pals[i]);
        upkg *p = load_pkg(c, pals[i], 1, NULL);
        int e = upkg_main_export(p);
        pi->old_name = prop_get_text(p, e, "ItemName");
        // the swatch shows 4 colours (top/right/left/bottom = the Red/Green/Blue/Alpha tints, or the custom icon
        // colours); the main colour is the one most of them share, preferring a real colour over black/grey/white
        float col[4][4];
        int n = prop_get_colors(p, e, "CustomInventoryIconColors", col, 4);
        if (n < 4) {
            float cm[5][4];
            int have = palette_colormap(p, cm);
            n = 0;
            for (int k = 0; k < 4; k++) if (have & (1 << k)) memcpy(col[n++], cm[k], 16);
        }
        upkg_free(p);
        {
            char t[512];
            int o = snprintf(t, sizeof t, "  colours of %s:", upkg_short(pals[i]));
            for (int k = 0; k < n && o < (int)sizeof t - 40; k++) {
                uint8_t s8[3];
                linear_to_srgb8(col[k], s8);
                o += snprintf(t + o, sizeof t - o, " %02x%02x%02x", s8[0], s8[1], s8[2]);
            }
            log_msg("%s", t);
        }
        // a tint near mid grey leaves the texture as it is; the palette's look is its most saturated tint, or for
        // a black/grey/white palette the tint furthest from mid grey
        float best_rgb[3] = {0.5f, 0.5f, 0.5f}, best_score = -1;
        int chromatic = 0;
        for (int k = 0; k < n; k++) {
            uint8_t s8[3];
            linear_to_srgb8(col[k], s8);
            float t[3] = {s8[0] / 255.0f, s8[1] / 255.0f, s8[2] / 255.0f};
            float mx = fmaxf(t[0], fmaxf(t[1], t[2])), mn = fminf(t[0], fminf(t[1], t[2]));
            float chroma = mx - mn, light = (mx + mn) / 2;
            int is_col = chroma > 0.12f;
            float score = is_col ? 10 + chroma : fabsf(light - 0.4f);
            if (is_col > chromatic || score > best_score) {
                if (is_col < chromatic) continue;
                chromatic = is_col; best_score = score;
                memcpy(best_rgb, t, sizeof t);
            }
        }
        for (int k = 0; k < 3; k++) pi->rgb[k] = (uint8_t)(best_rgb[k] * 255.0f + 0.5f);
        pi->ncand = n ? color_names(best_rgb, pi->cand, 3) : 0;
        if (!n) { pi->cand[0] = "Original"; pi->ncand = 1; }   // no tints: the suit's own colours
    }
    // automatic names: nearest colour name not used by an earlier palette
    for (int i = 0; i < a->npal; i++) {
        const char *pick = NULL;
        for (int k = 0; k < a->pal[i].ncand && !pick; k++) {
            int used = 0;
            for (int q = 0; q < i; q++) used |= !strcmp(a->pal[q].auto_name, a->pal[i].cand[k]);
            if (!used) pick = a->pal[i].cand[k];
        }
        snprintf(a->pal[i].auto_name, sizeof a->pal[i].auto_name, "%s", pick ? pick : a->pal[i].cand[0]);
        for (int k = 2; !pick; k++) {                          // all near names taken: "Red 2"
            snprintf(a->pal[i].auto_name, sizeof a->pal[i].auto_name, "%s %d", a->pal[i].cand[0], k);
            int used = 0;
            for (int q = 0; q < i; q++) used |= !strcmp(a->pal[q].auto_name, a->pal[i].auto_name);
            if (!used) break;
        }
    }
}

// ---------------------------------------------------------------- the game's registries

typedef struct { char **v; int n; } path_list;

static void list_registries(const char *path, void *p)
{
    path_list *l = p;
    if (_strnicmp(path, "CodaGame/AssetRegistry", 22) || !ends_with(path, ".bin")) return;
    l->v = xrealloc(l->v, sizeof(char *) * (l->n + 1));
    l->v[l->n++] = xstrdup(path);
}

static char *keep_name(const char *s, void *ctx) { (void)s; (void)ctx; return NULL; }

// ok[i] = 1 if /Game package paths[i] has an entry in every one of the game's registries (the SuitRegistry DLL
// clones each source from each registry, and stops if one is missing). With try_clone, an entry the SuitRegistry
// can't clone (e.g. a blueprint's, with export path tags) counts as missing: the copy goes without one.
static void check_registered(analysis *a, char **paths, int n, int *ok, int try_clone)
{
    for (int i = 0; i < n; i++) ok[i] = 1;
    path_list l = {0};
    game_each(a->game, list_registries, &l);
    if (!l.n) sr_fail("the game's AssetRegistry.bin was not found");
    for (int k = 0; k < l.n; k++) {
        size_t sz;
        uint8_t *d = game_read(a->game, l.v[k], &sz);
        reg *r = reg_parse(d, sz);
        for (int i = 0; i < n; i++) {
            if (!ok[i]) continue;
            char *op = obj_path(paths[i]);
            ok[i] = reg_has(r, op);
            if (ok[i] && try_clone) {
                jmp_buf j, *outer = sr_jmp;
                sr_jmp = &j;
                if (setjmp(j)) {
                    ok[i] = 0;
                    log_msg("note: the SuitRegistry can't copy the registry entry of %s (%s); its copy goes without "
                            "one (the game loads it by its path)", upkg_short(paths[i]), sr_error);
                } else reg_clone(r, op, keep_name, NULL);
                sr_jmp = outer;
            }
            free(op);
        }
        reg_free(r);
        free(l.v[k]);
    }
    free(l.v);
}

// ---------------------------------------------------------------- analysis

static int mod_dir_filter(const char *dir, const char *name, void *c) { (void)dir; (void)name; (void)c; return 1; }

typedef struct { char **v; int n; const char *dir, *prefix; } list_ctx;

static void list_items(const char *path, void *p)
{
    list_ctx *l = p;
    if (!ends_with(path, ".uasset") || !strstr(path, l->dir)) return;
    const char *s = strrchr(path, '/');
    if (!s || _strnicmp(s + 1, l->prefix, strlen(l->prefix))) return;
    l->v = xrealloc(l->v, sizeof(char *) * (l->n + 1));
    l->v[l->n++] = pak_to_game(path);
}

static size_t common_prefix(const char *a, const char *b)
{
    size_t n = 0;
    while (a[n] && b[n] && tolower((uint8_t)a[n]) == tolower((uint8_t)b[n])) n++;
    return n;
}

// closest game item to `path` (same kind of item, longest shared path), for a registry donor
static char *nearest(list_ctx *l, const char *path)
{
    char *best = NULL;
    size_t bl = 0;
    for (int i = 0; i < l->n; i++) {
        size_t c = common_prefix(l->v[i], path);
        if (!best || c > bl) { best = l->v[i]; bl = c; }
    }
    return best ? xstrdup(best) : NULL;
}

// name of the patched pak: <name>_patched_P.pak for <name>_P.pak. UE4 mounts paks ending in _P (after an optional
// chunk version, <name>_<N>_P) with a higher priority, so that ending stays as it is, and so does the load order.
static wchar_t *patched_path(const wchar_t *orig)
{
    const wchar_t *fn = wcsrchr(orig, L'\\');
    size_t fs = fn ? (size_t)(fn + 1 - orig) : 0;
    const wchar_t *dot = wcsrchr(orig + fs, L'.');
    size_t end = dot ? (size_t)(dot - orig) : wcslen(orig);
    size_t at = end;
    if (end - fs >= 2 && orig[end - 2] == L'_' && towupper(orig[end - 1]) == L'P') {
        at = end - 2;
        size_t d = at;
        while (d > fs && iswdigit(orig[d - 1])) d--;
        if (d < at && d > fs + 1 && orig[d - 1] == L'_') at = d - 1;
    }
    size_t n = wcslen(orig);
    wchar_t *r = xmalloc((n + 9) * sizeof(wchar_t));
    wmemcpy(r, orig, at);
    wmemcpy(r + at, L"_patched", 8);
    wcscpy(r + at + 8, orig + at);
    return r;
}

static int is_bak(const wchar_t *p)
{
    size_t n = wcslen(p);
    return n > 4 && !_wcsicmp(p + n - 4, L".bak");
}

// <p> without a trailing ".bak"
static wchar_t *strip_bak(const wchar_t *p)
{
    wchar_t *r = _wcsdup(p);
    if (is_bak(r)) r[wcslen(r) - 4] = 0;
    return r;
}

static const wchar_t *file_part(const wchar_t *p)
{
    const wchar_t *s = wcsrchr(p, L'\\');
    return s ? s + 1 : p;
}

// <folder of beside>\<name><ext>
static wchar_t *beside(const wchar_t *path, const wchar_t *name, const wchar_t *ext)
{
    size_t dl = (size_t)(file_part(path) - path), n = dl + wcslen(name) + wcslen(ext) + 1;
    wchar_t *r = xmalloc(n * sizeof(wchar_t));
    wmemcpy(r, path, dl);
    swprintf(r + dl, n - dl, L"%ls%ls", name, ext);
    return r;
}

static int file_exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

// a file name from a manifest, as a wide string (NULL if it isn't a plain file name)
static wchar_t *manifest_name(json *j)
{
    return j && j->t == J_STR && j->s[0] && !strpbrk(j->s, "\\/:") ? utf8_to_w(j->s) : NULL;
}

analysis *analyze_pak(const wchar_t *pak_path, const wchar_t *const *addons, int naddons, const wchar_t *game_paks)
{
    analysis *a = xcalloc(1, sizeof *a);
    patcher_need_game = 0;
    a->pak_path = _wcsdup(pak_path);
    log_msg("reading %ls", pak_path);
    int r = pak_open(&a->mod, pak_path, 1);
    if (r) sr_fail("this file is not a pak file Midnight Suns can read");
    int nf;
    pak_entry *e = pak_list(&a->mod, mod_dir_filter, NULL, &nf);
    // already patched (our manifest)? then start again from the backup of the original (and of its add-ons)
    for (int i = 0; i < nf; i++) {
        if (_strnicmp(e[i].path, "CodaGame/SuitMods/", 18) || !ends_with(e[i].path, ".json")) continue;
        uint8_t *d = pak_read(&a->mod, &e[i], 1);
        json *j = json_parse((char *)d, (size_t)e[i].usize);
        json *pj = j ? json_get(j, "patcher") : NULL;
        int ours = pj != NULL;
        // the original's file name, so its backup is found when this pak was renamed to <name>_patched
        wchar_t *orig_name = manifest_name(pj ? json_get(pj, "original") : NULL);
        json *aj = pj ? json_get(pj, "addons") : NULL;
        wchar_t *prev[MAX_ADDONS];
        int nprev = 0;
        for (int k = 0; aj && aj->t == J_ARR && k < aj->n && nprev < MAX_ADDONS; k++)
            if ((prev[nprev] = manifest_name(aj->items[k]))) nprev++;
        json_free(j);
        free(d);
        if (ours && !is_bak(pak_path)) {
            wchar_t *bak = orig_name ? beside(pak_path, orig_name, L".bak") : beside(pak_path, file_part(pak_path), L".bak");
            free(orig_name);
            pak_entries_free(e, nf);
            pak_close(&a->mod);
            if (!file_exists(bak))
                sr_fail("this pak was already patched by SuitPatcher, and its backup (%ls) is gone. Patch the "
                        "original download instead.", bak);
            log_msg("this pak was already patched: starting again from its backup");
            // its add-ons too, unless others were chosen
            const wchar_t *use[MAX_ADDONS];
            wchar_t *found[MAX_ADDONS] = {0};
            int nuse = 0;
            if (naddons) for (int k = 0; k < naddons && k < MAX_ADDONS; k++) use[nuse++] = addons[k];
            else
                for (int k = 0; k < nprev; k++) {
                    wchar_t *ab = beside(pak_path, prev[k], L".bak"), *al = beside(pak_path, prev[k], L"");
                    if (file_exists(ab)) { found[k] = ab; free(al); }
                    else if (file_exists(al)) { found[k] = al; free(ab); }
                    else { log_msg("note: the add-on %ls (and its backup) is gone; patching without it", prev[k]); free(ab); free(al); }
                    if (found[k]) use[nuse++] = found[k];
                }
            for (int k = 0; k < nprev; k++) free(prev[k]);
            analysis *b = analyze_pak(bak, use, nuse, game_paks);
            for (int k = 0; k < MAX_ADDONS; k++) free(found[k]);
            // a pak patched under another name (or in place, by version 1.0.0) goes once the new one is written
            if (_wcsicmp(pak_path, b->out_path)) b->stale_path = _wcsdup(pak_path);
            free(bak);
            analysis_free(a);
            return b;
        }
        free(orig_name);
        for (int k = 0; k < nprev; k++) free(prev[k]);
        if (!ours) {
            pak_entries_free(e, nf);
            sr_fail("this pak already has a SuitRegistry manifest (%s), so it works with the SuitRegistry as it is",
                    e[i].path);
        }
    }
    a->orig_path = strip_bak(pak_path);
    a->out_path = patched_path(a->orig_path);
    a->source_name = w_to_utf8(file_part(a->orig_path));

    // the mod's files, then the add-ons' (an add-on's file replaces the mod's file at the same path)
    ctx c = {a, {0}};
    smap at = {0};                                              // pak path -> a->files index
    a->files = xcalloc(nf + 1, sizeof(mod_file));
    for (int i = 0; i < nf; i++) {
        a->files[i].path = e[i].path;
        a->files[i].e = e[i];
        smap_put(&at, e[i].path, i);
    }
    a->nfiles = nf;
    free(e);                                                    // (entries now owned by a->files)
    if (naddons > MAX_ADDONS) naddons = MAX_ADDONS;
    for (int k = 0; k < naddons; k++) {
        wchar_t *orig = strip_bak(addons[k]);
        if (!_wcsicmp(orig, a->orig_path)) sr_fail("add-on %d is the suit mod itself", k + 1);
        for (int q = 0; q < k; q++)
            if (!_wcsicmp(orig, a->addon_orig[q])) sr_fail("add-on %d is the same pak as add-on %d", k + 1, q + 1);
        a->addon_path[k] = _wcsdup(addons[k]);
        a->addon_orig[k] = orig;
        a->addon_name[k] = w_to_utf8(file_part(orig));
        a->naddons = k + 1;
        log_msg("reading add-on %d: %ls", k + 1, addons[k]);
        if (pak_open(&a->addon[k], addons[k], 1)) sr_fail("add-on %d is not a pak file Midnight Suns can read", k + 1);
        int na, replaced = 0;
        pak_entry *ae = pak_list(&a->addon[k], mod_dir_filter, NULL, &na);
        for (int i = 0; i < na; i++)
            if (!_strnicmp(ae[i].path, "CodaGame/SuitMods/", 18)) {
                pak_entries_free(ae, na);
                sr_fail("add-on %d (%s) has a SuitRegistry manifest: it is a suit of its own, not an add-on", k + 1,
                        a->addon_name[k]);
            }
        a->files = xrealloc(a->files, sizeof(mod_file) * (a->nfiles + na + 1));
        for (int i = 0; i < na; i++) {
            int old = smap_get(&at, ae[i].path);
            mod_file *f;
            if (old >= 0) {
                f = &a->files[old];
                free(f->path); free(f->e.blocks);
                replaced++;
            } else {
                f = &a->files[a->nfiles];
                smap_put(&at, ae[i].path, a->nfiles++);
            }
            memset(f, 0, sizeof *f);
            f->path = ae[i].path;
            f->e = ae[i];
            f->src = k + 1;
        }
        free(ae);
        if (replaced) log_msg("  add-on %d replaces %d of the mod's files", k + 1, replaced);
    }
    smap_free(&at);

    // packages
    char **dirs = NULL;
    int ndirs = 0, nregistry = 0;
    for (int i = 0; i < a->nfiles; i++) {
        mod_file *f = &a->files[i];
        f->pkg = -1;
        const char *dot = strrchr(f->path, '.');
        if (dot && !strchr(dot, '/')) snprintf(f->ext, sizeof f->ext, "%s", dot);
        const char *slash = strrchr(f->path, '/');
        if (!_strnicmp(f->path, "CodaGame/", 9) && slash && slash - f->path == 8 && !_strnicmp(slash + 1, "AssetRegistry", 13)) {
            f->drop = 1;                                        // a registry of its own would hide all other suits
            nregistry++;
            continue;
        }
        char *g = pak_to_game(f->path);
        if (!g || (!ends_with(f->path, ".uasset") && !ends_with(f->path, ".umap") && !ends_with(f->path, ".uexp") &&
                   !ends_with(f->path, ".ubulk") && !ends_with(f->path, ".uptnl"))) { free(g); continue; }
        int m = smap_get(&c.mods, g);
        if (m < 0) {
            a->pkgs = xrealloc(a->pkgs, sizeof(mod_pkg) * (a->npkgs + 1));
            mod_pkg *mp = &a->pkgs[a->npkgs];
            memset(mp, 0, sizeof *mp);
            mp->game = g;
            const char *ct = strstr(f->path, "/Content/");
            mp->prefix = xmalloc(ct - f->path + 10);
            memcpy(mp->prefix, f->path, ct - f->path + 9);
            mp->prefix[ct - f->path + 9] = 0;
            mp->fu = mp->fe = -1;
            mp->src = f->src;
            m = a->npkgs++;
            smap_put(&c.mods, g, m);
            // its folder, in the game's spelling, for the index
            char *gp = game_to_pak(g, "");
            char *s = strrchr(gp, '/');
            s[1] = 0;
            int have = 0;
            for (int k = 0; k < ndirs; k++) have |= !_stricmp(dirs[k], gp);
            if (!have) { dirs = xrealloc(dirs, sizeof(char *) * (ndirs + 1)); dirs[ndirs++] = gp; }
            else free(gp);
        } else free(g);
        f->pkg = m;
        if (ends_with(f->path, ".uasset") || ends_with(f->path, ".umap")) { a->pkgs[m].fu = i; a->pkgs[m].src = f->src; }
        else if (ends_with(f->path, ".uexp")) a->pkgs[m].fe = i;
    }
    if (nregistry) log_msg("the mod ships its own AssetRegistry (%d files): left out, the SuitRegistry replaces it", nregistry);
    if (!a->npkgs) sr_fail("this pak has no game packages (.uasset) in it");

    // the game
    a->paks_dir = game_paks ? _wcsdup(game_paks) : game_find_paks(a->out_path);
    if (!a->paks_dir || !game_paks_valid(a->paks_dir)) {
        patcher_need_game = 1;
        sr_fail("the Midnight Suns game folder was not found");
    }
    log_msg("game: %ls", a->paks_dir);
    if (!oodle_load(1)) sr_fail("the Oodle library (needed to read the game's files) is not available");
    a->game = game_open(a->paks_dir, dirs, ndirs);
    int noverride = 0;                                          // the mod's own (add-ons not counted)
    for (int m = 0; m < a->npkgs; m++) {
        mod_pkg *mp = &a->pkgs[m];
        if (mp->fu < 0) continue;
        char *p = game_to_pak(mp->game, ".uasset");
        mp->override = game_has(a->game, p);
        free(p);
        noverride += mp->override && !mp->src;
        upkg *u = load_pkg(&c, mp->game, 0, NULL);
        int me = upkg_main_export(u);
        mp->cls = xstrdup(me >= 0 ? upkg_class_of(u, me) : "");
        upkg_free(u);
    }

    list_ctx outfits = {NULL, 0, "/TacticalOutfits/", "TacticalOutfit_"}, palettes = {NULL, 0, "/Palettes/", "HeroSkinPalette_"};
    game_each(a->game, list_items, &outfits);
    game_each(a->game, list_items, &palettes);
    // only items the game really has (registered); others are leftovers the game never shows
    {
        int n = outfits.n + palettes.n, *ok = xcalloc(n + 1, sizeof(int));
        char **all = xmalloc(sizeof(char *) * (n + 1));
        memcpy(all, outfits.v, sizeof(char *) * outfits.n);
        memcpy(all + outfits.n, palettes.v, sizeof(char *) * palettes.n);
        log_msg("reading the game's registries...");
        check_registered(a, all, n, ok, 0);
        int k = 0, dropped = 0;
        for (int i = 0; i < outfits.n; i++) if (ok[i]) outfits.v[k++] = outfits.v[i]; else { free(outfits.v[i]); dropped++; }
        outfits.n = k;
        k = 0;
        for (int i = 0; i < palettes.n; i++) if (ok[outfits.n + dropped + i]) palettes.v[k++] = palettes.v[i]; else free(palettes.v[i]);
        palettes.n = k;
        free(all); free(ok);
    }

    // a new outfit in the mod: standalone mode
    int new_outfit = -1;
    for (int m = 0; m < a->npkgs; m++)
        if (!a->pkgs[m].override && !a->pkgs[m].src && !strcmp(a->pkgs[m].cls, "CodaTacticalOutfitPieceTemplate")) {
            if (new_outfit >= 0) { log_msg("note: the pak has more than one new suit; only %s is patched", a->pkgs[new_outfit].game); break; }
            new_outfit = m;
        }
    char **pals = NULL;
    int npal = 0;
    if (new_outfit >= 0) {
        a->mode = MODE_STANDALONE;
        a->outfit = xstrdup(a->pkgs[new_outfit].game);
        char *op = obj_path(a->outfit);
        add_item(a, a->outfit, new_outfit, K_OUTFIT, 0);
        for (int m = 0; m < a->npkgs; m++) {
            if (a->pkgs[m].override || strcmp(a->pkgs[m].cls, "CodaHeroSkinPaletteTemplate")) continue;
            upkg *u = load_pkg(&c, a->pkgs[m].game, 0, NULL);
            int uses = upkg_find_name(u, op) >= 0;
            upkg_free(u);
            if (!uses) continue;
            pals = xrealloc(pals, sizeof(char *) * (npal + 1));
            pals[npal++] = xstrdup(a->pkgs[m].game);
        }
        free(op);
        qsort(pals, npal, sizeof *pals, cmp_str);
        for (int i = 0; i < npal; i++) add_item(a, pals[i], smap_get(&c.mods, pals[i]), K_PALETTE, 0);
        // registry donors: the most similar game outfit / palettes
        for (int i = 0; i < a->nitems; i++) {
            clone_item *it = &a->items[i];
            char *d = nearest(it->kind == K_OUTFIT ? &outfits : &palettes, it->game);
            if (!d) sr_fail("no game %s found to register %s with", it->kind == K_OUTFIT ? "outfit" : "palette", it->game);
            it->donor = d;
        }
    } else {
        if (!noverride) sr_fail("this pak doesn't change any of the game's files and has no new suit in it");
        a->mode = MODE_REPLACER;
        graph gr = {&c, {0}, NULL, 0, 0, 0, {0}, 0};
        // the mod's changed files pick the suit; the add-ons' come along where the suit uses them
        int ns = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int m = 0; m < a->npkgs; m++)
                if (a->pkgs[m].override && !a->pkgs[m].src == !pass) smap_put(&gr.sidx, a->pkgs[m].game, ns++);
        gr.words = (ns + 63) / 64;
        log_msg("looking for the suit that uses the %d changed game files...", noverride);
        int best = -1, best_n = 0;
        for (int i = 0; i < outfits.n; i++) {
            int id = node_of(&gr, outfits.v[i]);
            reach(&gr, id, 1);
            int cnt = bits_count_upto(gr.v[id].bits, noverride);
            if (!cnt) continue;
            // more changed files wins; then the base game over DLC, then the shorter name
            int better = cnt > best_n;
            if (cnt == best_n && best >= 0) {
                const char *x = outfits.v[i], *y = outfits.v[best];
                int bx = !strncmp(x, "/Game/Coda/", 11), by = !strncmp(y, "/Game/Coda/", 11);
                better = bx != by ? bx > by : strlen(x) < strlen(y);
            }
            if (better) { best = i; best_n = cnt; }
        }
        log_msg("(%d packages looked at)", gr.reads);
        if (best < 0) sr_fail("none of the game's suits uses the files this mod changes, so there is no suit to copy");
        for (int i = 0; i < outfits.n; i++) {
            int id = smap_get(&gr.ids, outfits.v[i]);
            if (i != best && id >= 0 && bits_count_upto(gr.v[id].bits, noverride) == best_n)
                log_msg("note: %s uses the same files; using %s", upkg_short(outfits.v[i]), upkg_short(outfits.v[best]));
        }
        a->outfit = xstrdup(outfits.v[best]);
        // its palettes: the game palettes listing this outfit as compatible
        char *op = obj_path(a->outfit);
        for (int i = 0; i < palettes.n; i++) {
            upkg *u = NULL;
            jmp_buf j, *outer = sr_jmp;
            sr_jmp = &j;
            if (!setjmp(j)) u = load_pkg(&c, palettes.v[i], 0, NULL);
            sr_jmp = outer;
            if (!u) continue;
            int uses = upkg_find_name(u, op) >= 0;
            upkg_free(u);
            if (uses) { pals = xrealloc(pals, sizeof(char *) * (npal + 1)); pals[npal++] = xstrdup(palettes.v[i]); }
        }
        free(op);
        qsort(pals, npal, sizeof *pals, cmp_str);
        // what gets cloned: the outfit, its palettes, and everything from them that leads to a changed file
        smap seen = {0};
        char **cl = NULL;
        int ncl = 0;
        int oid = smap_get(&gr.ids, a->outfit);
        collect(&gr, oid, &seen, &cl, &ncl);
        for (int i = 0; i < npal; i++) {
            int id = node_of(&gr, pals[i]);
            reach(&gr, id, 1);
            collect(&gr, id, &seen, &cl, &ncl);
        }
        // a suit without a head or hair of its own wears the hero's default ones (HeadMesh / HairMesh of the battle
        // look, TacticalPawnConfig: on the character template, or on its blueprint class when the template keeps the
        // class default). When the mod or an add-on changes one of them, the new suit gets a copy as its own.
        {
            upkg *u = load_pkg(&c, a->outfit, 1, NULL);
            int ue = upkg_main_export(u);
            ptag t;
            int own[NLOOKS];
            for (int s = 0; s < NLOOKS; s++) own[s] = prop_find(u, ue, LOOK_PROP[s], &t);
            int nr;
            char **refs = upkg_refs(u, &nr), *chr = NULL;
            upkg_free(u);
            for (int i = 0; i < nr; i++) {
                if (!chr && strstr(refs[i], "/Templates/Characters/Heroes/")) chr = refs[i];
                else free(refs[i]);
            }
            free(refs);
            upkg *cu = NULL, *bu = NULL;
            jmp_buf j, *outer = sr_jmp;
            sr_jmp = &j;
            if (!setjmp(j) && chr) {
                cu = load_pkg(&c, chr, 1, NULL);
                // the template's class: <package>.<Name>_C among its references
                const char *cls = cu ? upkg_class_of(cu, upkg_main_export(cu)) : "";
                int cr;
                char **crefs = cu ? upkg_refs(cu, &cr) : NULL;
                for (int i = 0; crefs && i < cr; i++) {
                    size_t sl = strlen(upkg_short(crefs[i]));
                    if (!bu && !_strnicmp(cls, upkg_short(crefs[i]), sl) && !_stricmp(cls + sl, "_C"))
                        bu = load_pkg(&c, crefs[i], 1, NULL);
                    free(crefs[i]);
                }
                free(crefs);
            }
            sr_jmp = outer;
            for (int s = 0; s < NLOOKS && cu; s++) {
                if (own[s]) continue;
                const char *hp = prop_find_softpath_deep(cu, LOOK_PROP[s], "TacticalPawnConfig");
                if (!hp && bu) hp = prop_find_softpath_deep(bu, LOOK_PROP[s], "TacticalPawnConfig");
                if (!hp) continue;
                char *look = xstrdup(hp);
                look[strcspn(look, ".")] = 0;
                int id = node_of(&gr, look);
                reach(&gr, id, 0);
                if (bits_any(&gr, gr.v[id].bits)) {
                    log_msg("the suit uses %s's default %s, which this mod changes: the new suit gets its own copy",
                            upkg_short(chr), LOOK_WHAT[s]);
                    collect(&gr, id, &seen, &cl, &ncl);
                    a->look[s] = look;
                } else free(look);
            }
            upkg_free(cu);
            upkg_free(bu);
            free(chr);
        }
        smap_free(&seen);
        add_item(a, a->outfit, smap_get(&c.mods, a->outfit), K_OUTFIT, 1);
        for (int i = 0; i < npal; i++) add_item(a, pals[i], smap_get(&c.mods, pals[i]), K_PALETTE, 1);
        for (int i = 0; i < ncl; i++) {
            char *gp = game_to_pak(cl[i], ".uasset");
            int in_game = game_has(a->game, gp);
            free(gp);
            if (in_game) add_item(a, cl[i], smap_get(&c.mods, cl[i]), K_OTHER, 1);
            free(cl[i]);
        }
        free(cl);
        {
            char **paths = xmalloc(sizeof(char *) * (a->nitems + 1));
            int *ok = xcalloc(a->nitems + 1, sizeof(int));
            for (int i = 0; i < a->nitems; i++) paths[i] = a->items[i].game;
            check_registered(a, paths, a->nitems, ok, 1);
            for (int i = 0; i < a->nitems; i++) a->items[i].unregistered = !ok[i];
            free(paths); free(ok);
        }
        for (int i = 0; i < a->nitems; i++) if (a->items[i].mod >= 0) a->pkgs[a->items[i].mod].in_clone = 1;
        int left = 0;
        for (int m = 0; m < a->npkgs; m++)
            if (a->pkgs[m].override && !a->pkgs[m].in_clone) {
                if (!left++) log_msg("these changed files are not part of the suit and stay as they are (they still change the game's own files):");
                log_msg("  %s", a->pkgs[m].game);
            }
        for (int k = 0; k < a->naddons; k++) {
            int used = 0, global = 0;
            for (int m = 0; m < a->npkgs; m++)
                if (a->pkgs[m].src == k + 1 && a->pkgs[m].override) { if (a->pkgs[m].in_clone) used++; else global++; }
            log_msg("add-on %d (%s): %d changed file%s now belong%s to the new suit only%s", k + 1, a->addon_name[k], used,
                    used == 1 ? "" : "s", used == 1 ? "s" : "", global ? "; the others still change the game's own files (listed above)" : "");
        }
        for (int i = 0; i < gr.n; i++) { free(gr.v[i].path); free(gr.v[i].bits); free(gr.v[i].kids); }
        free(gr.v);
        smap_free(&gr.ids);
        smap_free(&gr.sidx);
    }
    if (a->mode == MODE_STANDALONE)
        for (int k = 0; k < a->naddons; k++)
            log_msg("add-on %d (%s): included as it is (with a new suit, add-ons still change the game's own files)", k + 1,
                    a->addon_name[k]);
    for (int i = 0; i < outfits.n; i++) free(outfits.v[i]);
    for (int i = 0; i < palettes.n; i++) free(palettes.v[i]);
    free(outfits.v); free(palettes.v);

    snprintf(a->hero, sizeof a->hero, "%s", ref_hero(a->outfit));
    {
        upkg *u = load_pkg(&c, a->outfit, 1, NULL);
        a->outfit_name = prop_get_text(u, upkg_main_export(u), "ItemName");
        upkg_free(u);
    }
    char hero_name[64] = "";
    for (int i = 0; i < npal && !hero_name[0]; i++) {
        const char *q = strstr(pals[i], "/Palettes/");
        if (!q) continue;
        q += 10;
        size_t n = strcspn(q, "/");
        if (n && n < sizeof hero_name && q[n] == '/') { memcpy(hero_name, q, n); hero_name[n] = 0; }
    }
    if (!hero_name[0]) snprintf(hero_name, sizeof hero_name, "%s", a->hero[0] ? a->hero : "a hero");
    describe_palettes(&c, pals, npal);
    if (npal > MAX_PALETTES) log_msg("note: %d palettes; the last %d are named automatically", npal, npal - MAX_PALETTES);
    for (int i = 0; i < npal; i++) free(pals[i]);
    free(pals);

    if (a->mode == MODE_REPLACER)
        snprintf(a->summary, sizeof a->summary,
                 "This mod changes %s's \"%s\" suit (%s). Patching makes it a new suit of its own with %d palette%s; "
                 "the original suit goes back to normal. %d file%s will be copied under new names.",
                 hero_name, a->outfit_name ? a->outfit_name : "?", upkg_short(a->outfit), npal,
                 npal == 1 ? "" : "s", a->nitems, a->nitems == 1 ? "" : "s");
    else
        snprintf(a->summary, sizeof a->summary,
                 "This mod adds a new suit (%s, now called \"%s\") with %d palette%s. Patching registers it with the "
                 "SuitRegistry%s.", upkg_short(a->outfit), a->outfit_name ? a->outfit_name : "?", npal,
                 npal == 1 ? "" : "s", nregistry ? " and removes the mod's own AssetRegistry files" : "");
    if (a->naddons) {
        size_t sl = strlen(a->summary);
        snprintf(a->summary + sl, sizeof a->summary - sl, " %d add-on%s merged in.", a->naddons,
                 a->naddons == 1 ? "" : "s");
    }
    log_msg("%s", a->summary);
    for (int i = 0; i < a->nitems; i++)
        log_msg("  %-8s %s%s", a->items[i].kind == K_OUTFIT ? "outfit" : a->items[i].kind == K_PALETTE ? "palette" : "file",
                a->items[i].game, a->items[i].mod >= 0 ? " (from the mod)" : "");
    smap_free(&c.mods);
    return a;
}

void analysis_free(analysis *a)
{
    if (!a) return;
    for (int i = 0; i < a->nfiles; i++) { free(a->files[i].path); free(a->files[i].e.blocks); }
    free(a->files);
    for (int i = 0; i < a->npkgs; i++) { free(a->pkgs[i].game); free(a->pkgs[i].prefix); free(a->pkgs[i].cls); }
    free(a->pkgs);
    for (int i = 0; i < a->nitems; i++) { free(a->items[i].game); free(a->items[i].donor); }
    free(a->items);
    for (int i = 0; i < a->npal; i++) { free(a->pal[i].pkg); free(a->pal[i].old_name); }
    if (a->mod.h) pak_close(&a->mod);
    for (int k = 0; k < a->naddons; k++) {
        if (a->addon[k].h) pak_close(&a->addon[k]);
        free(a->addon_path[k]); free(a->addon_orig[k]); free(a->addon_name[k]);
    }
    game_close(a->game);
    free(a->pak_path); free(a->orig_path); free(a->out_path); free(a->stale_path); free(a->paks_dir);
    free(a->outfit); free(a->outfit_name); free(a->source_name);
    for (int s = 0; s < NLOOKS; s++) free(a->look[s]);
    free(a);
}

// ---------------------------------------------------------------- patching

static void hex_digest(const char *a, const char *b, const char *c, uint8_t out[20])
{
    sha1_ctx *h = sha1_begin();
    sha1_add(h, a, strlen(a)); sha1_add(h, "|", 1);
    sha1_add(h, b, strlen(b)); sha1_add(h, "|", 1);
    sha1_add(h, c, strlen(c));
    sha1_end(h, out);
}

static void text_key(const char *tag, const char *pkg, const char *text, char key[33])
{
    uint8_t d[20];
    hex_digest(tag, pkg, text, d);
    for (int i = 0; i < 16; i++) sprintf(key + 2 * i, "%02X", d[i]);
}

static void make_tag(const char *name, char *tag, size_t n)
{
    size_t k = 0;
    int up = 1;
    for (const char *s = name; *s && k + 1 < n && k < 32; s++) {
        char ch = *s;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            if (up && ch >= 'a' && ch <= 'z') ch -= 32;
            if (!k && ch >= '0' && ch <= '9') tag[k++] = 'S';
            tag[k++] = ch;
            up = 0;
        } else up = 1;
    }
    tag[k] = 0;
    if (!k) {
        uint8_t d[20];
        hex_digest("tag", name, "", d);
        snprintf(tag, n, "Suit%02X%02X%02X", d[0], d[1], d[2]);
    }
}

static void json_str(buf *b, const char *s)
{
    buf_u8(b, '"');
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        if (c == '"' || c == '\\') { buf_u8(b, '\\'); buf_u8(b, c); }
        else if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); buf_put(b, t, 6); }
        else buf_u8(b, c);
    }
    buf_u8(b, '"');
}

static void json_rule(buf *b, const char *from, const char *to, int last)
{
    buf_put(b, "[", 1); json_str(b, from); buf_put(b, ", ", 2); json_str(b, to); buf_put(b, last ? "]" : "], ", last ? 1 : 3);
}

// one registry source: clone `donor` (a /Game package) as `target`. The rules only touch this entry; placeholders
// (\1-\3) keep one replacement from matching inside another's result.
static void json_source(buf *b, const char *donor, const char *target)
{
    char *dop = obj_path(donor);
    char *df = xstrdup(donor), *tf = xstrdup(target);
    *strrchr(df, '/') = 0;
    *strrchr(tf, '/') = 0;
    buf_put(b, "  {\"source\": ", 13);
    json_str(b, dop);
    buf_put(b, ", \"rules\": [", 12);
    json_rule(b, donor, "\x01", 0);
    json_rule(b, df, "\x03", 0);
    json_rule(b, upkg_short(donor), "\x02", 0);
    json_rule(b, "\x01", target, 0);
    json_rule(b, "\x03", tf, 0);
    json_rule(b, "\x02", upkg_short(target), 1);
    buf_put(b, "]}", 2);
    free(dop); free(df); free(tf);
}

static int game_running(void)
{
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {sizeof pe};
    int found = 0;
    if (Process32FirstW(s, &pe))
        do found |= !_wcsicmp(pe.szExeFile, L"MidnightSuns-Win64-Shipping.exe") || !_wcsicmp(pe.szExeFile, L"MidnightSuns.exe");
        while (!found && Process32NextW(s, &pe));
    CloseHandle(s);
    return found;
}

static char *with_ext(const char *pak_base, const char *ext)
{
    char *s = xmalloc(strlen(pak_base) + strlen(ext) + 1);
    sprintf(s, "%s%s", pak_base, ext);
    return s;
}

// pak path (without extension) of a /Game path, with the mod's own "<Project>/Content/" prefix
static char *pak_base(const char *prefix, const char *game)
{
    char *s = xmalloc(strlen(prefix) + strlen(game) + 1);
    sprintf(s, "%s%s", prefix, game + 6);
    return s;
}

static void write_pkg(pak_writer *w, upkg *u, const char *base)
{
    uint8_t *ua, *ue;
    size_t an, en;
    upkg_save(u, &ua, &an, &ue, &en);
    char *p = with_ext(base, ".uasset");
    pak_write_file(w, p, ua, an, 1);
    free(p);
    if (ue) { p = with_ext(base, ".uexp"); pak_write_file(w, p, ue, en, 1); free(p); }
    free(ua); free(ue);
}

static void apply_item_edits(upkg *u, clone_item *it, const char *tag, const char *display, int rarity,
                             const char *new_outfit_obj, const char *new_path)
{
    int e = upkg_main_export(u);
    if (e < 0) sr_fail("%s has no asset in it", it->game);
    char key[33];
    text_key(tag, new_path, display, key);
    prop_set_text(u, e, "ItemName", key, display);
    uint8_t d[20];
    hex_digest(tag, new_path, "EntitlementID", d);
    prop_set_guid(u, e, "EntitlementID", d);
    if (it->kind == K_OUTFIT) {
        char r[96];
        snprintf(r, sizeof r, "/Game/Coda/Templates/Items/Rarities/Rarity%d.Rarity%d", rarity, rarity);
        prop_set_softpath(u, e, "Rarity", r);
        prop_set_bool(u, e, "bUnlockedAtGameStart", 1);
        prop_set_bool(u, e, "bRequirePurchase", 0);
    } else {
        // mods can't sell or grant items: palettes must be free and owned from the start
        prop_set_bool(u, e, "bRequirePurchase", 0);
        prop_set_bool(u, e, "bUnlockedAtGameStart", 1);
        int gone = prop_filter_softarray(u, e, "CompatibleItems", new_outfit_obj);
        if (gone) log_msg("  %s: no longer listed for %d other suit%s", upkg_short(new_path), gone, gone == 1 ? "" : "s");
    }
}

patch_result run_patch(analysis *a, const patch_options *o)
{
    patch_result res = {0};
    if (!o->name || !*o->name) sr_fail("the suit needs a name");
    if (o->rarity < 0 || o->rarity > 3) sr_fail("bad rarity");
    if (game_running()) sr_fail("Midnight Suns is running - close the game first");
    ctx c = {a, {0}};
    for (int m = 0; m < a->npkgs; m++) smap_put(&c.mods, a->pkgs[m].game, m);

    // the tag in the new package names, unique in the game and the mod
    char base_tag[40], tag[48];
    make_tag(o->name, base_tag, sizeof base_tag);
    for (int k = 1;; k++) {
        if (k == 1) snprintf(tag, sizeof tag, "%s", base_tag);
        else snprintf(tag, sizeof tag, "%s%d", base_tag, k);
        int clash = 0;
        for (int i = 0; i < a->nitems && !clash && a->mode == MODE_REPLACER; i++) {
            char *np = xmalloc(strlen(a->items[i].game) + strlen(tag) + 2);
            sprintf(np, "%s_%s", a->items[i].game, tag);
            char *gp = game_to_pak(np, ".uasset");
            clash = game_has(a->game, gp) || smap_get(&c.mods, np) >= 0;
            free(gp); free(np);
        }
        if (!clash || k > 99) break;
    }
    rename_map m = {0};
    if (a->mode == MODE_REPLACER)
        for (int i = 0; i < a->nitems; i++) {
            char *np = xmalloc(strlen(a->items[i].game) + strlen(tag) + 2);
            sprintf(np, "%s_%s", a->items[i].game, tag);
            rmap_add(&m, a->items[i].game, np);
            free(np);
        }
    const char *new_outfit = a->mode == MODE_REPLACER ? rmap_get(&m, a->outfit) : a->outfit;
    char *new_outfit_obj = obj_path(new_outfit);
    snprintf(res.id, sizeof res.id, "%s%s%s", a->hero, a->hero[0] ? "_" : "", tag);
    log_msg("patching: \"%s\", rarity %d, id %s", o->name, o->rarity, res.id);

    // palette names: typed, or automatic (not clashing with the typed ones)
    int npal_items = 0;
    for (int i = 0; i < a->nitems; i++) npal_items += a->items[i].kind == K_PALETTE;
    char **pal_name = xcalloc(npal_items + 1, sizeof(char *));
    char **pal_pkg = xcalloc(npal_items + 1, sizeof(char *));
    {
        int k = 0;
        for (int i = 0; i < a->nitems; i++) if (a->items[i].kind == K_PALETTE) pal_pkg[k++] = a->items[i].game;
        for (int i = 0; i < npal_items; i++) {
            const char *typed = i < MAX_PALETTES ? o->pal_names[i] : NULL;
            if (typed && *typed) { pal_name[i] = xstrdup(typed); continue; }
        }
        for (int i = 0; i < npal_items; i++) {
            if (pal_name[i]) continue;
            const char *pick = NULL;
            if (i < a->npal) {
                for (int q = 0; q < a->pal[i].ncand + 1 && !pick; q++) {
                    const char *cand = q == 0 ? a->pal[i].auto_name : a->pal[i].cand[q - 1];
                    int used = 0;
                    for (int z = 0; z < npal_items; z++) used |= pal_name[z] && !_stricmp(pal_name[z], cand);
                    if (!used) pick = cand;
                }
            }
            char t[80];
            if (!pick) { snprintf(t, sizeof t, "Palette %d", i + 1); pick = t; }
            pal_name[i] = xstrdup(pick);
        }
    }

    // write the patched pak next to the output
    size_t ol = wcslen(a->out_path);
    wchar_t *tmp = xmalloc((ol + 16) * sizeof(wchar_t));
    swprintf(tmp, ol + 16, L"%ls.tmp", a->out_path);
    pak_writer *w = pak_write_begin(tmp);
    if (!w) sr_fail("could not create %ls (error %lu)", tmp, GetLastError());
    jmp_buf j, *outer = sr_jmp;
    sr_jmp = &j;
    if (setjmp(j)) { sr_jmp = outer; DeleteFileW(tmp); free(tmp); longjmp(*outer, 1); }
    int nout = 0;
    // 1. the mod's own files (except the cloned packages): new packages get their references updated
    for (int i = 0; i < a->nfiles; i++) {
        mod_file *f = &a->files[i];
        if (f->drop) continue;
        if (!_strnicmp(f->path, "CodaGame/SuitMods/", 18)) continue;
        mod_pkg *mp = f->pkg >= 0 ? &a->pkgs[f->pkg] : NULL;
        int item = -1;
        if (mp) for (int k = 0; k < a->nitems; k++) if (!_stricmp(a->items[k].game, mp->game)) item = k;
        if (mp && (mp->in_clone || (item >= 0 && a->items[item].rename))) continue;   // written as a clone below
        int is_header = mp && i == mp->fu;
        if (mp && (i == mp->fe) && mp->fu >= 0) continue;       // written with its .uasset
        if (is_header && (m.n || item >= 0) && !mp->override) {
            upkg *u = load_pkg(&c, mp->game, 1, NULL);
            upkg_rename(u, &m, NULL, NULL);
            if (item >= 0) {
                const char *display = a->items[item].kind == K_OUTFIT ? o->name : NULL;
                if (a->items[item].kind == K_PALETTE)
                    for (int q = 0; q < npal_items; q++) if (!_stricmp(pal_pkg[q], mp->game)) display = pal_name[q];
                apply_item_edits(u, &a->items[item], tag, display ? display : o->name, o->rarity, new_outfit_obj, mp->game);
            }
            char *base = pak_base(mp->prefix, mp->game);
            write_pkg(w, u, base);
            free(base);
            upkg_free(u);
            nout += 2;
            continue;
        }
        size_t n;
        uint8_t *d = mod_read(a, i, &n);
        pak_write_file(w, f->path, d, n, f->e.method != 0 && !ends_with(f->path, ".ubulk"));
        free(d);
        nout++;
        if (is_header && mp->fe >= 0) {
            d = mod_read(a, mp->fe, &n);
            pak_write_file(w, a->files[mp->fe].path, d, n, a->files[mp->fe].e.method != 0);
            free(d);
            nout++;
        }
    }
    // 2. clones (replacer mode)
    for (int i = 0; i < a->nitems; i++) {
        clone_item *it = &a->items[i];
        if (!it->rename) continue;
        const char *np = rmap_get(&m, it->game);
        int from_mod;
        upkg *u = load_pkg(&c, it->game, 1, &from_mod);
        if (!u) sr_fail("%s could not be read", it->game);
        upkg_rename(u, &m, it->game, np);
        if (it->kind != K_OTHER) {
            const char *display = o->name;
            for (int q = 0; q < npal_items; q++) if (!_stricmp(pal_pkg[q], it->game)) display = pal_name[q];
            apply_item_edits(u, it, tag, display, o->rarity, new_outfit_obj, np);
            for (int s = 0; s < NLOOKS && it->kind == K_OUTFIT; s++) {
                if (!a->look[s]) continue;
                char *lop = obj_path(rmap_get(&m, a->look[s]));
                prop_set_softpath(u, upkg_main_export(u), LOOK_PROP[s], lop);
                free(lop);
            }
        } else {
            // other items the suit brings (e.g. the weapon it equips): their own ID, free like the suit
            int e = upkg_main_export(u);
            ptag t;
            if (e >= 0 && prop_find(u, e, "EntitlementID", &t)) {
                uint8_t d[20];
                hex_digest(tag, np, "EntitlementID", d);
                prop_set_guid(u, e, "EntitlementID", d);
                prop_set_bool(u, e, "bRequirePurchase", 0);
            }
        }
        const char *prefix = from_mod >= 0 ? a->pkgs[from_mod].prefix : "CodaGame/Content/";
        char *base = pak_base(prefix, np);
        write_pkg(w, u, base);
        nout += 2;
        upkg_free(u);
        // bulk data files go along under the new name
        static const char *bulk_ext[] = {".ubulk", ".uptnl"};
        for (int k = 0; k < 2; k++) {
            size_t n = 0;
            uint8_t *d = NULL;
            if (from_mod >= 0) {
                for (int f = 0; f < a->nfiles && !d; f++)
                    if (a->files[f].pkg == from_mod && ends_with(a->files[f].path, bulk_ext[k])) d = mod_read(a, f, &n);
            } else {
                char *gp = game_to_pak(it->game, bulk_ext[k]);
                d = game_read(a->game, gp, &n);
                free(gp);
            }
            if (!d) continue;
            char *p = with_ext(base, bulk_ext[k]);
            pak_write_file(w, p, d, n, 0);
            free(p); free(d);
            nout++;
        }
        free(base);
    }
    // 3. the SuitRegistry manifest
    buf mj = {0};
    const char *head = "{\n \"format\": 1,\n \"id\": ";
    buf_put(&mj, head, strlen(head));
    json_str(&mj, res.id);
    buf_put(&mj, ",\n \"title\": ", 12);
    char title[256];
    snprintf(title, sizeof title, "%s%s%s%s", o->name, a->hero[0] ? " (" : "", a->hero, a->hero[0] ? ")" : "");
    json_str(&mj, title);
    buf_put(&mj, ",\n \"kind\": \"suit\",\n \"sources\": [\n", 32);
    int first = 1;
    for (int i = 0; i < a->nitems; i++) {
        clone_item *it = &a->items[i];
        const char *donor = it->rename ? it->game : it->donor;
        const char *target = it->rename ? rmap_get(&m, it->game) : it->game;
        if (!donor || it->unregistered) continue;
        if (!first) buf_put(&mj, ",\n", 2);
        first = 0;
        json_source(&mj, donor, target);
    }
    buf_put(&mj, "\n ],\n \"rules\": [],\n \"keep\": [],\n \"patcher\": {\"tool\": \"SuitPatcher\", \"version\": ", 79);
    json_str(&mj, PATCHER_VERSION);
    buf_put(&mj, ", \"original\": ", 14);
    json_str(&mj, a->source_name);
    buf_put(&mj, ", \"base\": ", 10);
    json_str(&mj, a->mode == MODE_REPLACER ? a->outfit : "");
    if (a->naddons) {
        buf_put(&mj, ", \"addons\": [", 13);
        for (int k = 0; k < a->naddons; k++) {
            if (k) buf_put(&mj, ", ", 2);
            json_str(&mj, a->addon_name[k]);
        }
        buf_put(&mj, "]", 1);
    }
    buf_put(&mj, "},\n \"note\": ", 12);
    json_str(&mj, "Made by SuitPatcher. Read by the Midnight Suns SuitRegistry (version.dll): each source is a game "
                  "registry entry cloned under the new name of one package of this suit.");
    buf_put(&mj, "\n}\n", 3);
    char mpath[160];
    snprintf(mpath, sizeof mpath, "CodaGame/SuitMods/%s.json", res.id);
    pak_write_file(w, mpath, mj.p, mj.n, 0);
    buf_free(&mj);
    nout++;
    pak_write_end(w);
    sr_jmp = outer;

    // swap it in: the original (and each add-on) becomes <name>.pak.bak, unless we read from that backup already, and
    // the patched pak is <name>_patched.pak. Every rename is undone if a later one fails.
    pak_close(&a->mod);
    for (int k = 0; k < a->naddons; k++) pak_close(&a->addon[k]);
    const wchar_t *src[1 + MAX_ADDONS], *orig[1 + MAX_ADDONS];
    wchar_t *bak[1 + MAX_ADDONS];
    int moved[1 + MAX_ADDONS] = {0}, nsrc = 1 + a->naddons;
    src[0] = a->pak_path; orig[0] = a->orig_path;
    for (int k = 0; k < a->naddons; k++) { src[k + 1] = a->addon_path[k]; orig[k + 1] = a->addon_orig[k]; }
    for (int k = 0; k < nsrc; k++) {
        size_t gl = wcslen(orig[k]);
        bak[k] = xmalloc((gl + 8) * sizeof(wchar_t));
        swprintf(bak[k], gl + 8, L"%ls.bak", orig[k]);
    }
    for (int k = nsrc - 1; k >= 0; k--) {
        if (_wcsicmp(src[k], orig[k])) continue;                 // read from the backup already
        if (!MoveFileExW(orig[k], bak[k], MOVEFILE_REPLACE_EXISTING)) {
            DWORD err = GetLastError();
            for (int q = k + 1; q < nsrc; q++) if (moved[q]) MoveFileExW(bak[q], orig[q], 0);
            DeleteFileW(tmp);
            const wchar_t *fn = file_part(orig[k]);
            sr_fail(err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED
                        ? "%ls is in use or read-only (is the game running?)"
                        : "could not rename %ls (error %lu)", fn, err);
        }
        moved[k] = 1;
    }
    res.backup_path = _wcsdup(bak[0]);
    if (!MoveFileExW(tmp, a->out_path, MOVEFILE_REPLACE_EXISTING)) {
        DWORD err = GetLastError();
        for (int q = 0; q < nsrc; q++) if (moved[q]) MoveFileExW(bak[q], orig[q], 0);
        sr_fail("could not write %ls (error %lu); the patched pak is %ls", a->out_path, err, tmp);
    }
    // nothing else may load the mod or an add-on a second time: a pak under the original's name (when patching from
    // the backup; earlier versions patched in place) or an earlier patched pak under another name
    for (int k = 0; k < nsrc; k++) {
        if (moved[k] || !file_exists(orig[k])) continue;
        if (DeleteFileW(orig[k])) log_msg("removed %ls (the backup is the original)", orig[k]);
        else {
            DWORD err = GetLastError();
            log_msg("could not remove %ls (error %lu): delete it, or it loads twice", orig[k], err);
        }
    }
    if (a->stale_path && _wcsicmp(a->stale_path, a->orig_path)) {
        if (DeleteFileW(a->stale_path)) log_msg("removed %ls (patched before, replaced now)", a->stale_path);
        else {
            DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND)
                log_msg("could not remove %ls (error %lu): delete it, or the suit shows up twice", a->stale_path, err);
        }
    }
    free(a->stale_path);
    a->stale_path = NULL;
    // reopen the sources (now the backups) so the same analysis can patch again (with other names)
    pak_open(&a->mod, bak[0], 1);
    free(a->pak_path);
    a->pak_path = bak[0];
    for (int k = 0; k < a->naddons; k++) {
        pak_open(&a->addon[k], bak[k + 1], 1);
        free(a->addon_path[k]);
        a->addon_path[k] = bak[k + 1];
    }
    res.out_path = a->out_path;
    res.nfiles = nout;
    log_msg("done: %ls (%d files); original kept as %ls%ls", a->out_path, nout, res.backup_path,
            a->naddons ? L" (and the add-ons as .bak files too)" : L"");
    for (int i = 0; i < npal_items; i++) free(pal_name[i]);
    free(pal_name); free(pal_pkg); free(tmp); free(new_outfit_obj);
    for (int i = 0; i < m.n; i++) { free(m.from[i]); free(m.to[i]); }
    free(m.from); free(m.to);
    smap_free(&c.mods);
    return res;
}
