// Developer test tool (not shipped): exercises the package editor and the patcher from the command line.
//   names <pkg.uasset>                      name map + hash check
//   replace <in.uasset> <out.uasset> a=b..  substring replace in all names (like uassettool clone)
//   rename <in.uasset> <out.uasset> /Game/X=/Game/Y..  reference rename (first pair = this package)
//   props <pkg.uasset>                      tags of the main export
//   edit <in.uasset> <out.uasset>           set name/rarity/guid/bools on an outfit or palette
//   analyze <mod.pak> [game Paks folder]    run the patcher's analysis
//   patch <mod.pak> <name> <rarity 0-3> [palette names...]   analyze + patch
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
        wchar_t *game = argc > 3 && !strcmp(cmd, "analyze") ? utf8_to_w(argv[3]) : NULL;
        analysis *a = analyze_pak(pak, game);
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
