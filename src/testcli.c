// Developer test tool (not shipped): exercises the package editor and the patcher from the command line.
//   names <pkg.uasset>                      name map + hash check
//   replace <in.uasset> <out.uasset> a=b..  substring replace in all names (like uassettool clone)
//   rename <in.uasset> <out.uasset> /Game/X=/Game/Y..  reference rename (first pair = this package)
//   props <pkg.uasset>                      tags of the main export
//   edit <in.uasset> <out.uasset>           set name/rarity/guid/bools on an outfit or palette
//   analyze <mod.pak> [+addon.pak...] [game Paks folder]    run the patcher's analysis
//   patch <mod.pak> [+addon.pak...] <name> <rarity 0-3> [palette names...]   analyze + patch
#include "upkg.h"
#include "patcher.h"

HINSTANCE sr_self;
static int NULL_FILTER(const char *d, const char *n, void *c) { (void)d; (void)n; (void)c; return 1; }

static uint8_t *readf(const char *path, size_t *n)
{
    wchar_t *w = utf8_to_w(path);
    uint8_t *d = file_read_all(w, n);
    free(w);
    return d;
}

static void writef(const char *path, const uint8_t *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) sr_fail("cannot write %s", path);
    fwrite(d, 1, n, f);
    fclose(f);
}

static char *uexp_of(const char *ua)
{
    char *s = xstrdup(ua);
    size_t l = strlen(s);
    if (l > 7) strcpy(s + l - 7, ".uexp");
    return s;
}

static upkg *load(const char *path)
{
    size_t an, en = 0;
    uint8_t *a = readf(path, &an);
    if (!a) sr_fail("cannot read %s", path);
    char *ep = uexp_of(path);
    uint8_t *e = readf(ep, &en);
    free(ep);
    return upkg_load(a, an, e, en);
}

static void save(upkg *p, const char *path)
{
    uint8_t *a, *e;
    size_t an, en;
    upkg_save(p, &a, &an, &e, &en);
    writef(path, a, an);
    if (e) { char *ep = uexp_of(path); writef(ep, e, en); free(ep); }
}

static int ends_with_ci(const char *s, const char *e)
{
    size_t a = strlen(s), b = strlen(e);
    return a >= b && !_stricmp(s + a - b, e);
}

typedef struct { gindex *g; char **want; int nwant; } wu;

// whouses: game packages whose name map has one of the wanted paths
static void each_uses(const char *path, void *ctx)
{
    wu *w = ctx;
    if (!ends_with_ci(path, ".uasset")) return;
    size_t an;
    uint8_t *ua = game_read(w->g, path, &an);
    if (!ua) return;
    jmp_buf jj, *outer = sr_jmp;
    sr_jmp = &jj;
    if (!setjmp(jj)) {
        upkg *p = upkg_load_ex(ua, an, NULL, 0, 1);
        for (int k = 0; k < w->nwant; k++)
            if (upkg_find_name(p, w->want[k]) >= 0) printf("%s uses %s\n", path, upkg_short(w->want[k]));
        upkg_free(p);
    }
    sr_jmp = outer;
}

static void list_match(const char *path, void *ctx)
{
    const char *t = ctx;
    size_t n = strlen(t);
    for (const char *s = path; *s; s++)
        if (!_strnicmp(s, t, n)) { if (ends_with_ci(path, ".uasset")) printf("%s\n", path); return; }
}


// ---- property dump (gdump): every tagged property of the main export, structs and struct arrays nested
static const char *fn_at(upkg *p, const uint8_t *q) { return upkg_name_at(p, (int32_t)rd32(q)); }

static const char *obj_name(upkg *p, int32_t i, char *t, size_t n)
{
    if (i < 0 && -i - 1 < p->nimp) {
        pimport *im = &p->imp[-i - 1];
        const char *outer = im->outer < 0 && -im->outer - 1 < p->nimp ? upkg_name_at(p, p->imp[-im->outer - 1].name) : "";
        snprintf(t, n, "%s:%s", outer, upkg_name_at(p, im->name));
    } else if (i > 0 && i <= p->nexp) snprintf(t, n, "export %d %s", i - 1, upkg_name_at(p, p->exp[i - 1].name));
    else snprintf(t, n, "null");
    return t;
}

static void dump_props(upkg *p, size_t at, size_t end, int depth);

static void dump_value(upkg *p, const char *type, const char *sub, size_t v, int32_t size, int depth)
{
    char t[512];
    if (!strcmp(type, "ObjectProperty")) printf(" = %s\n", obj_name(p, (int32_t)rd32(p->ue + v), t, sizeof t));
    else if (!strcmp(type, "SoftObjectProperty")) printf(" = %s\n", fn_at(p, p->ue + v));
    else if (!strcmp(type, "NameProperty")) printf(" = %s\n", fn_at(p, p->ue + v));
    else if (!strcmp(type, "IntProperty")) printf(" = %d\n", (int32_t)rd32(p->ue + v));
    else if (!strcmp(type, "FloatProperty")) { float f; memcpy(&f, p->ue + v, 4); printf(" = %g\n", f); }
    else if (!strcmp(type, "ByteProperty") || !strcmp(type, "EnumProperty"))
        printf(" = %s\n", size == 8 ? fn_at(p, p->ue + v) : "(byte)");
    else if (!strcmp(type, "StructProperty")) {
        if (sub && (!strcmp(sub, "Vector") || !strcmp(sub, "Guid") || !strcmp(sub, "LinearColor") || !strcmp(sub, "Rotator") ||
                    !strcmp(sub, "Color") || !strcmp(sub, "Vector2D") || !strcmp(sub, "Quat")))
            printf(" (%d bytes)\n", size);
        else if (sub && !strcmp(sub, "SoftObjectPath")) printf(" = %s\n", fn_at(p, p->ue + v));
        else { printf("\n"); dump_props(p, v, v + (size_t)size, depth + 1); }
    } else printf(" (%d bytes)\n", size);
}

static void dump_props(upkg *p, size_t at, size_t end, int depth)
{
    while (at + 8 <= end) {
        const char *name = fn_at(p, p->ue + at);
        if (!strcmp(name, "None")) return;
        const char *type = fn_at(p, p->ue + at + 8);
        int32_t size = (int32_t)rd32(p->ue + at + 16);
        size_t q = at + 24;
        const char *sub = NULL;
        if (!strcmp(type, "StructProperty")) { sub = fn_at(p, p->ue + q); q += 8 + 16; }
        else if (!strcmp(type, "BoolProperty")) { printf("%*s%s Bool = %d\n", depth * 2, "", name, p->ue[q]); q += 1; }
        else if (!strcmp(type, "ByteProperty") || !strcmp(type, "EnumProperty") || !strcmp(type, "ArrayProperty") ||
                 !strcmp(type, "SetProperty")) { sub = fn_at(p, p->ue + q); q += 8; }
        else if (!strcmp(type, "MapProperty")) q += 16;
        if (p->ue[q++]) q += 16;
        size_t v = q;
        if (strcmp(type, "BoolProperty")) {
            printf("%*s%s %s%s%s", depth * 2, "", name, type, sub ? "/" : "", sub ? sub : "");
            if (!strcmp(type, "ArrayProperty")) {
                int32_t n = (int32_t)rd32(p->ue + v);
                printf(" [%d]\n", n);
                size_t e = v + 4;
                if (!strcmp(sub, "StructProperty")) {
                    const char *sn = fn_at(p, p->ue + e + 32);
                    int32_t isz = (int32_t)rd32(p->ue + e + 16);
                    e += 8 + 8 + 8 + 8 + 16 + 1;
                    for (int k = 0; k < n; k++) {
                        printf("%*s[%d] %s\n", depth * 2 + 2, "", k, sn);
                        if (!strcmp(sn, "SoftObjectPath")) { printf("%*s= %s\n", depth * 2 + 4, "", fn_at(p, p->ue + e)); e += 8; int32_t l = (int32_t)rd32(p->ue + e); e += 4 + (l < 0 ? -2 * l : l); continue; }
                        size_t s0 = e;
                        dump_props(p, e, v + 4 + 49 + (size_t)isz, depth + 2);
                        // skip past this element's None
                        size_t z = s0;
                        while (strcmp(fn_at(p, p->ue + z), "None")) {
                            const char *ty = fn_at(p, p->ue + z + 8);
                            int32_t sz = (int32_t)rd32(p->ue + z + 16);
                            size_t w = z + 24;
                            if (!strcmp(ty, "StructProperty")) w += 24;
                            else if (!strcmp(ty, "BoolProperty")) w += 1;
                            else if (!strcmp(ty, "ByteProperty") || !strcmp(ty, "EnumProperty") || !strcmp(ty, "ArrayProperty") || !strcmp(ty, "SetProperty")) w += 8;
                            else if (!strcmp(ty, "MapProperty")) w += 16;
                            if (p->ue[w++]) w += 16;
                            z = w + (size_t)sz;
                        }
                        e = z + 8;
                    }
                } else if (!strcmp(sub, "ObjectProperty")) {
                    char t[512];
                    for (int k = 0; k < n; k++) printf("%*s[%d] %s\n", depth * 2 + 2, "", k, obj_name(p, (int32_t)rd32(p->ue + e + 4 * k), t, sizeof t));
                } else if (!strcmp(sub, "SoftObjectProperty")) {
                    for (int k = 0; k < n; k++) { printf("%*s[%d] %s\n", depth * 2 + 2, "", k, fn_at(p, p->ue + e)); e += 8; int32_t l = (int32_t)rd32(p->ue + e); e += 4 + (l < 0 ? -2 * l : l); }
                } else if (!strcmp(sub, "NameProperty")) {
                    for (int k = 0; k < n; k++) printf("%*s[%d] %s\n", depth * 2 + 2, "", k, fn_at(p, p->ue + e + 8 * k));
                }
            } else dump_value(p, type, sub, v, size, depth);
        }
        at = v + (size_t)size;
    }
}

static void print_line(const char *l) { printf("%s\n", l); fflush(stdout); }

int main(int argc, char **argv)
{
    jmp_buf j;
    sr_jmp = &j;
    if (setjmp(j)) { fprintf(stderr, "error: %s\n", sr_error); return 1; }
    log_hook = print_line;
    if (argc < 3) { fprintf(stderr, "usage: see testcli.c\n"); return 2; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "findgame")) {
        wchar_t *w = utf8_to_w(argv[2]);
        printf("HOME=%s\nfound: %ls\n", getenv("HOME") ? getenv("HOME") : "(none)", game_find_paks(w));
        return 0;
    }
    if (!strcmp(cmd, "reghas")) {                              // reghas <registry pak> <object path>...
        wchar_t *w = utf8_to_w(argv[2]);
        pak pk;
        if (pak_open(&pk, w, 1)) sr_fail("not a pak");
        int ne;
        pak_entry *e = pak_list(&pk, NULL_FILTER, NULL, &ne);
        for (int i = 0; i < ne; i++) {
            if (!strstr(e[i].path, "AssetRegistry")) continue;
            reg *r = reg_parse(pak_read(&pk, &e[i], 1), (size_t)e[i].usize);
            printf("%s:", e[i].path);
            for (int k = 3; k < argc; k++) printf(" %d", reg_has(r, argv[k]));
            printf("\n");
            reg_free(r);
        }
        return 0;
    }
    if (!strcmp(cmd, "names")) {
        upkg *p = load(argv[2]);
        int bad = 0;
        for (int i = 0; i < p->nnames; i++) {
            uint16_t a = p->names[i].h1, b = p->names[i].h2;
            p->names[i].changed = 1;
            uint8_t *x, *y; size_t xn, yn;
            upkg_save(p, &x, &xn, &y, &yn);                     // recomputes the hash
            free(x); free(y);
            int ok = a == p->names[i].h1 && b == p->names[i].h2;
            bad += !ok;
            printf("%d\t%s%s\n", i, p->names[i].s, ok ? "" : "\t<- HASH MISMATCH");
        }
        printf("%d names, %d hash mismatches; %d imports, %d exports, main export %d (%s)\n", p->nnames, bad, p->nimp,
               p->nexp, upkg_main_export(p), upkg_main_export(p) >= 0 ? upkg_class_of(p, upkg_main_export(p)) : "-");
        return bad != 0;
    }
    if (!strcmp(cmd, "replace") || !strcmp(cmd, "rename")) {
        upkg *p = load(argv[2]);
        rename_map m = {0};
        for (int k = 4; k < argc; k++) {
            char *s = xstrdup(argv[k]), *eq = strchr(s, '=');
            if (!eq) sr_fail("expected old=new");
            *eq = 0;
            if (cmd[1] == 'e' && cmd[2] == 'p') printf("%d names changed\n", upkg_replace_names(p, s, eq + 1));
            else rmap_add(&m, s, eq + 1);
        }
        if (m.n) upkg_rename(p, &m, m.from[0], m.to[0]);
        save(p, argv[3]);
        return 0;
    }
    if (!strcmp(cmd, "dump")) {                                // dump <pkg.uasset>: every property of every export
        upkg *p = load(argv[2]);
        for (int x = 0; x < p->nexp; x++) {
            printf("== export %d %s : %s\n", x, upkg_name_at(p, p->exp[x].name), upkg_class_of(p, x));
            size_t st = (size_t)(p->exp[x].off - p->total);
            dump_props(p, st, st + (size_t)p->exp[x].size, 1);
        }
        return 0;
    }
    if (!strcmp(cmd, "props")) {
        upkg *p = load(argv[2]);
        int e = upkg_main_export(p);
        printf("main export %d: %s\n", e, upkg_class_of(p, e));
        size_t at = (size_t)(p->exp[e].off - p->total);
        (void)at;
        static const char *want[] = {"ItemName", "Rarity", "EntitlementID", "bUnlockedAtGameStart", "bRequirePurchase",
                                     "CompatibleItems", "CustomInventoryIconColors", NULL};
        for (int i = 0; want[i]; i++) {
            ptag t;
            if (!prop_find(p, e, want[i], &t)) { printf("  %-28s (absent)\n", want[i]); continue; }
            printf("  %-28s %s%s%s size %d", want[i], t.type, t.sub ? "/" : "", t.sub ? t.sub : "", t.size);
            if (!strcmp(t.type, "TextProperty")) { char *s = prop_get_text(p, e, want[i]); printf("  \"%s\"", s ? s : "?"); free(s); }
            if (!strcmp(t.type, "BoolProperty")) printf("  = %d", p->ue[t.bool_at]);
            if (!strcmp(t.type, "SoftObjectProperty")) printf("  = %s", upkg_name_at(p, (int32_t)rd32(p->ue + t.val)));
            printf("\n");
        }
        float c[8][4];
        int n = prop_get_colors(p, e, "CustomInventoryIconColors", c, 8);
        for (int i = 0; i < n; i++) printf("  icon colour %d: %.3f %.3f %.3f %.3f\n", i, c[i][0], c[i][1], c[i][2], c[i][3]);
        float cm[5][4];
        int have = palette_colormap(p, cm);
        static const char *ch[] = {"Red", "Green", "Blue", "Alpha", "Emissive"};
        for (int i = 0; i < 5; i++)
            if (have & (1 << i)) printf("  ColorMap %-8s %.3f %.3f %.3f %.3f\n", ch[i], cm[i][0], cm[i][1], cm[i][2], cm[i][3]);
        return 0;
    }
    if (!strcmp(cmd, "glist")) {                               // glist <game Paks> <text>: game files with it in the path
        if (!oodle_load(1)) sr_fail("no Oodle");
        game_index_everything = 1;
        gindex *g = game_open(utf8_to_w(argv[2]), NULL, 0);
        game_each(g, list_match, argv[3]);
        return 0;
    }
    if (!strcmp(cmd, "gpkg") || !strcmp(cmd, "whouses") || !strcmp(cmd, "gdump")) {    // gpkg|whouses <game Paks> </Game/path>...
        if (!oodle_load(1)) sr_fail("no Oodle");
        game_index_everything = 1;
        gindex *g = game_open(utf8_to_w(argv[2]), NULL, 0);
        if (!strcmp(cmd, "gdump")) {                            // gdump <Paks> </Game/path>: properties of the main export
            char *pp = game_to_pak(argv[3], ".uasset"), *ep = game_to_pak(argv[3], ".uexp");
            size_t an, en = 0;
            uint8_t *ua = game_read(g, pp, &an), *ue = game_read(g, ep, &en);
            if (!ua) sr_fail("not in the game");
            upkg *p = upkg_load(ua, an, ue, en);
            for (int x = 0; x < p->nexp; x++) {
                printf("== export %d %s : %s\n", x, upkg_name_at(p, p->exp[x].name), upkg_class_of(p, x));
                size_t st = (size_t)(p->exp[x].off - p->total);
                dump_props(p, st, st + (size_t)p->exp[x].size, 1);
            }
            return 0;
        }
        if (!strcmp(cmd, "gpkg")) {                             // exports, references and names of game packages
            for (int k = 3; k < argc; k++) {
                char *pp = game_to_pak(argv[k], ".uasset");
                size_t an, en = 0;
                uint8_t *ua = game_read(g, pp, &an);
                if (!ua) { printf("%s: not in the index\n", argv[k]); continue; }
                char *ep = game_to_pak(argv[k], ".uexp");
                uint8_t *ue = game_read(g, ep, &en);
                upkg *p = upkg_load_ex(ua, an, ue, en, ue == NULL);
                printf("== %s\n", argv[k]);
                for (int i = 0; i < p->nexp; i++)
                    printf("  export %d %s : %s\n", i, upkg_name_at(p, p->exp[i].name), upkg_class_of(p, i));
                int nr;
                char **r = upkg_refs(p, &nr);
                for (int i = 0; i < nr; i++) printf("  ref %s\n", r[i]);
                printf("  names:");
                for (int i = 0; i < p->nnames; i++) printf(" %s", p->names[i].s);
                printf("\n");
            }
            return 0;
        }
        wu c = {g, argv + 3, argc - 3};
        game_each(g, each_uses, &c);
        return 0;
    }
    if (!strcmp(cmd, "edit")) {
        upkg *p = load(argv[2]);
        int e = upkg_main_export(p);
        prop_set_text(p, e, "ItemName", "0123456789ABCDEF0123456789ABCDEF", "Test Näme");
        prop_set_softpath(p, e, "Rarity", "/Game/Coda/Templates/Items/Rarities/Rarity1.Rarity1");
        uint8_t g[16];
        for (int i = 0; i < 16; i++) g[i] = (uint8_t)(i * 17);
        prop_set_guid(p, e, "EntitlementID", g);
        prop_set_bool(p, e, "bUnlockedAtGameStart", 1);
        prop_set_bool(p, e, "bRequirePurchase", 0);
        save(p, argv[3]);
        return 0;
    }
    if (!strcmp(cmd, "analyze") || !strcmp(cmd, "patch")) {
        wchar_t *pak = utf8_to_w(argv[2]);
        wchar_t *addons[MAX_ADDONS];
        int naddons = 0;
        while (argc > 3 && argv[3][0] == '+') {                 // add-ons: drop them from the argument list
            if (naddons < MAX_ADDONS) addons[naddons++] = utf8_to_w(argv[3] + 1);
            memmove(&argv[3], &argv[4], sizeof(char *) * (argc - 3));
            argc--;
        }
        wchar_t *game = argc > 3 && !strcmp(cmd, "analyze") ? utf8_to_w(argv[3]) : NULL;
        analysis *a = analyze_pak(pak, (const wchar_t *const *)addons, naddons, game);
        printf("%s\n", a->summary);
        for (int i = 0; i < a->npal; i++)
            printf("  palette %d: %s (was \"%s\", auto name \"%s\", colour %02x%02x%02x)\n", i + 1, a->pal[i].pkg,
                   a->pal[i].old_name ? a->pal[i].old_name : "", a->pal[i].auto_name, a->pal[i].rgb[0],
                   a->pal[i].rgb[1], a->pal[i].rgb[2]);
        if (!strcmp(cmd, "patch")) {
            if (argc < 5) sr_fail("patch needs a name and a rarity");
            patch_options o = {0};
            o.name = argv[3];
            o.rarity = atoi(argv[4]);
            for (int i = 5; i < argc && i - 5 < MAX_PALETTES; i++) o.pal_names[i - 5] = argv[i];
            patch_result r = run_patch(a, &o);
            printf("written: %ls\n", r.out_path);
        }
        return 0;
    }
    fprintf(stderr, "unknown command\n");
    return 2;
}
