// Cooked UE 4.26 package editing (FPackageFileSummary, name map, import/export maps, tagged properties).
//
// The header is rebuilt on save: summary + name map + the rest of the header as it was. Growing the name map shifts
// every table after it, so all summary offsets past the name map, the exports' SerialOffset and BulkDataStartOffset
// move by the same amount. Property edits change the .uexp: the export's SerialSize, the SerialOffset of the exports
// after it and BulkDataStartOffset follow.
#include "upkg.h"

#define PKG_TAG 0x9E2A83C1u
#define PKG_UNVERSIONED_PROPS 0x00002000u
#define PKG_FILTER_EDITOR_ONLY 0x80000000u
#define IMPORT_SIZE 28
#define EXPORT_SIZE 104                                         // 4.26 (FileVersionUE4 522)

// ---------------------------------------------------------------- name hashes (FNameEntrySerialized)

static uint32_t crc_deprecated[256], crc_sb8[256];

static void crc_init(void)
{
    if (crc_sb8[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
        crc_deprecated[i] = c;
        c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        crc_sb8[i] = c;
    }
}

// chars of a name entry as UTF-16 code units (narrow entries: their raw bytes)
static int name_units(const pname *n, uint16_t **out)
{
    if (!n->wide) {
        size_t l = strlen(n->s);
        *out = xmalloc(2 * (l + 1));
        for (size_t i = 0; i < l; i++) (*out)[i] = (uint8_t)n->s[i];
        return (int)l;
    }
    wchar_t *w = utf8_to_w(n->s);
    *out = (uint16_t *)w;
    return (int)wcslen(w);
}

static void name_hash(pname *n)
{
    crc_init();
    uint16_t *u;
    int len = name_units(n, &u);
    uint32_t h = 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = u[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!n->wide) {
            h = ((h >> 8) & 0x00FFFFFF) ^ crc_deprecated[(h ^ (uint8_t)c) & 0xFF];
        } else {
            h = ((h >> 8) & 0x00FFFFFF) ^ crc_deprecated[(h ^ (c & 0xFF)) & 0xFF];
            h = ((h >> 8) & 0x00FFFFFF) ^ crc_deprecated[(h ^ (c >> 8)) & 0xFF];
        }
    }
    uint32_t c32 = ~0u;
    for (int i = 0; i < len; i++) {
        uint32_t ch = u[i];
        for (int k = 0; k < 4; k++) { c32 = (c32 >> 8) ^ crc_sb8[(c32 ^ ch) & 0xFF]; ch >>= 8; }
    }
    free(u);
    n->h1 = (uint16_t)h;
    n->h2 = (uint16_t)~c32;
}

// ---------------------------------------------------------------- load / save

static int needs_wide(const char *s)
{
    for (; *s; s++) if ((uint8_t)*s >= 0x80) return 1;
    return 0;
}

static void add_name_raw(upkg *p, char *s, int wide)
{
    if (p->nnames == p->cap_names) {
        p->cap_names = p->cap_names ? 2 * p->cap_names : 64;
        p->names = xrealloc(p->names, sizeof(pname) * p->cap_names);
    }
    pname *n = &p->names[p->nnames++];
    memset(n, 0, sizeof *n);
    n->s = s;
    n->wide = wide;
}

upkg *upkg_load_ex(uint8_t *ua, size_t ua_n, uint8_t *ue, size_t ue_n, int header_only)
{
    upkg *p = xcalloc(1, sizeof *p);
    p->ua = ua; p->ua_n = ua_n;
    rd r = {ua, ua_n, 0};
    if (rd_u32(&r) != PKG_TAG) sr_fail("not an Unreal package");
    int32_t legacy = rd_i32(&r);
    if (legacy != -6 && legacy != -7) sr_fail("unsupported package format (%d)", legacy);
    rd_i32(&r);                                                 // LegacyUE3Version
    p->ver = rd_i32(&r);
    if (p->ver == 0) p->ver = 522;                              // unversioned cook: the engine's own version
    if (p->ver < 507 || p->ver > 522) sr_fail("unsupported package version %d (expected UE 4.26)", p->ver);
    rd_i32(&r);                                                 // licensee version
    int32_t ncv = rd_i32(&r);
    if (ncv < 0 || ncv > 10000) sr_fail("bad custom version count");
    rd_skip(&r, 20ull * ncv);
    p->f_total = r.at;
    p->total = rd_i32(&r);
    rd_skip_fstring(&r);                                        // FolderName
    p->flags = rd_u32(&r);
    if (p->flags & PKG_UNVERSIONED_PROPS) sr_fail("packages with unversioned properties are not supported");
    p->f_namecount = r.at;
    int32_t nn = rd_i32(&r);
    p->name_off = rd_i32(&r);
    if (!(p->flags & PKG_FILTER_EDITOR_ONLY)) rd_skip_fstring(&r);   // LocalizationId
    rd_i32(&r); p->f_off[p->nf_off++] = r.at; rd_i32(&r);      // gatherable text
    size_t f_expcount = r.at;
    p->nexp = rd_i32(&r);
    p->f_off[p->nf_off++] = r.at;
    int32_t exp_off = rd_i32(&r);
    p->nimp = rd_i32(&r);
    p->f_off[p->nf_off++] = r.at;
    int32_t imp_off = rd_i32(&r);
    (void)f_expcount;
    p->f_off[p->nf_off++] = r.at; rd_i32(&r);                  // DependsOffset
    rd_i32(&r); p->f_off[p->nf_off++] = r.at; rd_i32(&r);      // soft package references
    if (p->ver >= 510) { p->f_off[p->nf_off++] = r.at; rd_i32(&r); }   // searchable names
    p->f_off[p->nf_off++] = r.at; rd_i32(&r);                  // thumbnails
    rd_skip(&r, 16);                                            // guid
    int32_t gens = rd_i32(&r);
    if (gens < 0 || gens > 100000) sr_fail("bad generation count");
    rd_skip(&r, 8ull * gens);
    rd_skip(&r, 10); rd_skip_fstring(&r);                       // SavedByEngineVersion
    rd_skip(&r, 10); rd_skip_fstring(&r);                       // CompatibleWithEngineVersion
    rd_u32(&r);                                                 // compression flags
    if (rd_i32(&r)) sr_fail("compressed packages are not supported");
    rd_u32(&r);                                                 // package source
    int32_t extra = rd_i32(&r);
    for (int32_t i = 0; i < extra; i++) rd_skip_fstring(&r);
    if (legacy > -7) rd_i32(&r);                                // NumTextureAllocations
    p->f_off[p->nf_off++] = r.at; rd_i32(&r);                  // AssetRegistryDataOffset
    p->f_bulk = r.at;
    p->bulk = (int64_t)rd_u64(&r);
    p->f_off[p->nf_off++] = r.at; rd_i32(&r);                  // WorldTileInfoDataOffset
    int32_t nchunks = rd_i32(&r);
    if (nchunks < 0 || nchunks > 100000) sr_fail("bad chunk id count");
    rd_skip(&r, 4ull * nchunks);
    rd_i32(&r); p->f_off[p->nf_off++] = r.at; rd_i32(&r);      // preload dependencies
    if (p->total <= 0 || (size_t)p->total > ua_n) sr_fail("bad header size");
    // a package that isn't split in .uasset + .uexp: split it here, join it again on save
    p->split = ue != NULL;
    if (!ue) {
        ue_n = ua_n - (size_t)p->total;
        ue = xmalloc(ue_n + 1);
        memcpy(ue, ua + p->total, ue_n);
        p->ua_n = ua_n = (size_t)p->total;
    }
    p->ue = ue; p->ue_n = ue_n;
    // names
    p->hashes = p->ver >= 504;
    rd n = {ua, (size_t)p->total, (size_t)p->name_off};
    if (nn < 0 || nn > 1000000) sr_fail("bad name count");
    for (int32_t i = 0; i < nn; i++) {
        int32_t len = rd_i32(&n);
        n.at -= 4;
        char *s = rd_fstring(&n);
        size_t l = strlen(s);
        if (l && s[l - 1] == 0) s[l - 1] = 0;
        add_name_raw(p, s, len < 0);
        if (p->hashes) {
            uint16_t a = (uint16_t)(rd_u8(&n) | rd_u8(&n) << 8);
            uint16_t b = (uint16_t)(rd_u8(&n) | rd_u8(&n) << 8);
            p->names[i].h1 = a; p->names[i].h2 = b;
        }
    }
    p->nnames0 = p->nnames;
    p->name_end = n.at;
    // imports / exports
    if (p->nimp < 0 || p->nexp < 0 || (size_t)imp_off + (size_t)p->nimp * IMPORT_SIZE > (size_t)p->total ||
        (size_t)exp_off + (size_t)p->nexp * EXPORT_SIZE > (size_t)p->total || (size_t)imp_off < p->name_end ||
        (size_t)exp_off < p->name_end)
        sr_fail("bad import/export tables");
    p->imp = xcalloc(p->nimp + 1, sizeof(pimport));
    for (int i = 0; i < p->nimp; i++) {
        const uint8_t *q = ua + imp_off + (size_t)i * IMPORT_SIZE;
        pimport *im = &p->imp[i];
        im->at = imp_off + (size_t)i * IMPORT_SIZE;
        im->class_pkg = (int32_t)rd32(q);
        im->class_name = (int32_t)rd32(q + 8);
        im->outer = (int32_t)rd32(q + 16);
        im->name = rd32(q + 24) ? -1 : (int32_t)rd32(q + 20);  // (numbered names never match a package name)
        if (rd32(q + 24)) im->name = -2 - (int32_t)rd32(q + 20);
    }
    p->exp = xcalloc(p->nexp + 1, sizeof(pexport));
    for (int i = 0; i < p->nexp; i++) {
        const uint8_t *q = ua + exp_off + (size_t)i * EXPORT_SIZE;
        pexport *e = &p->exp[i];
        e->at = exp_off + (size_t)i * EXPORT_SIZE;
        e->class_idx = (int32_t)rd32(q);
        e->outer = (int32_t)rd32(q + 12);
        e->name = rd32(q + 20) ? -2 - (int32_t)rd32(q + 16) : (int32_t)rd32(q + 16);
        e->size = (int64_t)rd64(q + 28);
        e->off = (int64_t)rd64(q + 36);
        if (!header_only && (e->off < p->total || e->size < 0 || (uint64_t)(e->off - p->total) + (uint64_t)e->size > ue_n))
            sr_fail("bad export table");
    }
    return p;
}

upkg *upkg_load(uint8_t *ua, size_t ua_n, uint8_t *ue, size_t ue_n) { return upkg_load_ex(ua, ua_n, ue, ue_n, 0); }

void upkg_free(upkg *p)
{
    if (!p) return;
    for (int i = 0; i < p->nnames; i++) free(p->names[i].s);
    free(p->names); free(p->imp); free(p->exp); free(p->ua); free(p->ue);
    free(p);
}

static void put_name_entry(buf *b, pname *n, int hashes)
{
    if (!n->wide) {
        uint32_t l = (uint32_t)strlen(n->s) + 1;
        buf_u32(b, l);
        buf_put(b, n->s, l);
    } else {
        wchar_t *w = utf8_to_w(n->s);
        int32_t l = (int32_t)wcslen(w) + 1;
        buf_u32(b, (uint32_t)-l);
        buf_put(b, w, 2 * (size_t)l);
        free(w);
    }
    if (hashes) {
        if (n->changed) name_hash(n);
        uint8_t t[4] = {n->h1 & 0xff, n->h1 >> 8, n->h2 & 0xff, n->h2 >> 8};
        buf_put(b, t, 4);
    }
}

static void w32(uint8_t *b, size_t at, int32_t v) { memcpy(b + at, &v, 4); }
static void w64(uint8_t *b, size_t at, int64_t v) { memcpy(b + at, &v, 8); }

void upkg_save(upkg *p, uint8_t **ua, size_t *ua_n, uint8_t **ue, size_t *ue_n)
{
    buf names = {0};
    for (int i = 0; i < p->nnames; i++) put_name_entry(&names, &p->names[i], p->hashes);
    size_t old_len = p->name_end - (size_t)p->name_off;
    int64_t d = (int64_t)names.n - (int64_t)old_len;
    buf h = {0};
    buf_put(&h, p->ua, (size_t)p->name_off);
    buf_put(&h, names.p, names.n);
    buf_put(&h, p->ua + p->name_end, (size_t)p->total - p->name_end);
    uint8_t *o = h.p;
    w32(o, p->f_total, (int32_t)(p->total + d));
    w32(o, p->f_namecount, p->nnames);
    for (int k = 0; k < p->nf_off; k++) {
        int32_t v = (int32_t)rd32(o + p->f_off[k]);
        if (v && (size_t)v >= p->name_end) w32(o, p->f_off[k], (int32_t)(v + d));
    }
    w64(o, p->f_bulk, p->bulk + d);
    for (int i = 0; i < p->nimp; i++) {
        size_t at = p->imp[i].at + d;
        if (p->imp[i].name >= 0) { w32(o, at + 20, p->imp[i].name); w32(o, at + 24, 0); }
    }
    for (int i = 0; i < p->nexp; i++) {
        size_t at = p->exp[i].at + d;
        if (p->exp[i].name >= 0) { w32(o, at + 16, p->exp[i].name); w32(o, at + 20, 0); }
        w64(o, at + 28, p->exp[i].size);
        w64(o, at + 36, p->exp[i].off + d);
    }
    buf_free(&names);
    if (p->split) {
        *ua = h.p; *ua_n = h.n;
        *ue = xmalloc(p->ue_n + 1);
        memcpy(*ue, p->ue, p->ue_n);
        *ue_n = p->ue_n;
    } else {
        buf_put(&h, p->ue, p->ue_n);
        *ua = h.p; *ua_n = h.n;
        *ue = NULL; *ue_n = 0;
    }
}

// ---------------------------------------------------------------- names, imports, exports

int upkg_find_name(upkg *p, const char *s)
{
    for (int i = 0; i < p->nnames; i++) if (!strcmp(p->names[i].s, s)) return i;
    return -1;
}

int upkg_name(upkg *p, const char *s)
{
    int i = upkg_find_name(p, s);
    if (i >= 0) return i;
    add_name_raw(p, xstrdup(s), needs_wide(s));
    p->names[p->nnames - 1].changed = 1;
    return p->nnames - 1;
}

const char *upkg_name_at(upkg *p, int32_t i) { return i >= 0 && i < p->nnames ? p->names[i].s : ""; }

void upkg_set_name(upkg *p, int i, const char *s)
{
    free(p->names[i].s);
    p->names[i].s = xstrdup(s);
    p->names[i].wide = needs_wide(s);
    p->names[i].changed = 1;
}

const char *upkg_short(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

int upkg_main_export(upkg *p)
{
    int first = -1;
    for (int i = 0; i < p->nexp; i++) {
        if (p->exp[i].outer != 0) continue;
        if (first < 0) first = i;
        int is_asset = (int)rd32(p->ua + p->exp[i].at + 80);    // bIsAsset
        if (is_asset) return i;
    }
    return first;
}

const char *upkg_class_of(upkg *p, int exp)
{
    int32_t c = p->exp[exp].class_idx;
    if (c < 0 && -c - 1 < p->nimp) return upkg_name_at(p, p->imp[-c - 1].name);
    return "";
}

static int is_package_import(upkg *p, int i)
{
    return !strcmp(upkg_name_at(p, p->imp[i].class_name), "Package");
}

char **upkg_refs(upkg *p, int *n)
{
    char **v = NULL;
    int k = 0;
    for (int i = 0; i < p->nnames; i++) {
        const char *s = p->names[i].s;
        if (strncmp(s, "/Game/", 6)) continue;
        char *t = xstrdup(s);
        char *dot = strchr(t + 6, '.');
        if (dot) *dot = 0;
        char *colon = strchr(t + 6, ':');
        if (colon) *colon = 0;
        int dup = 0;
        for (int j = 0; j < k && !dup; j++) dup = !_stricmp(v[j], t);
        if (dup) { free(t); continue; }
        v = xrealloc(v, sizeof(char *) * (k + 1));
        v[k++] = t;
    }
    *n = k;
    return v;
}

void rmap_add(rename_map *m, const char *from, const char *to)
{
    m->from = xrealloc(m->from, sizeof(char *) * (m->n + 1));
    m->to = xrealloc(m->to, sizeof(char *) * (m->n + 1));
    m->from[m->n] = xstrdup(from);
    m->to[m->n] = xstrdup(to);
    m->n++;
}

const char *rmap_get(const rename_map *m, const char *from)
{
    for (int i = 0; i < m->n; i++) if (!_stricmp(m->from[i], from)) return m->to[i];
    return NULL;
}

// "/Game/A/X" -> "/Game/A/X.X"
static char *object_path(const char *pkg)
{
    const char *s = upkg_short(pkg);
    size_t a = strlen(pkg), b = strlen(s);
    char *o = xmalloc(a + b + 2);
    memcpy(o, pkg, a); o[a] = '.'; memcpy(o + a + 1, s, b + 1);
    return o;
}

void upkg_rename(upkg *p, const rename_map *m, const char *self_old, const char *self_new)
{
    // object imports inside a renamed package import: they take the new short name (looked up before the package
    // import's own name changes below)
    for (int i = 0; i < p->nimp; i++) {
        int32_t outer = p->imp[i].outer;
        if (outer >= 0 || -outer - 1 >= p->nimp || p->imp[i].name < 0) continue;
        pimport *pk = &p->imp[-outer - 1];
        if (!is_package_import(p, -outer - 1) || pk->name < 0) continue;
        const char *to = rmap_get(m, upkg_name_at(p, pk->name));
        if (!to) continue;
        const char *from = upkg_name_at(p, pk->name);
        if (!_stricmp(upkg_name_at(p, p->imp[i].name), upkg_short(from)))
            p->imp[i].name = upkg_name(p, upkg_short(to));
    }
    // the package's own asset export (and other top-level exports with its name)
    if (self_old && self_new) {
        for (int i = 0; i < p->nexp; i++)
            if (p->exp[i].outer == 0 && p->exp[i].name >= 0 && !_stricmp(upkg_name_at(p, p->exp[i].name), upkg_short(self_old)))
                p->exp[i].name = upkg_name(p, upkg_short(self_new));
    }
    // full paths: "/Game/A/X" (package imports, soft package references) and "/Game/A/X.X" (soft object paths),
    // also "/Game/A/X.X:Sub" style subobject paths
    for (int i = 0; i < p->nnames; i++) {
        const char *s = p->names[i].s;
        if (strncmp(s, "/Game/", 6)) continue;
        size_t pl = strcspn(s, ".:");
        char *pkg = xmalloc(pl + 1);
        memcpy(pkg, s, pl); pkg[pl] = 0;
        const char *to = rmap_get(m, pkg);
        if (to) {
            buf b = {0};
            buf_put(&b, to, strlen(to));
            const char *rest = s + pl;
            if (*rest == '.') {
                const char *obj = rest + 1;
                size_t ol = strcspn(obj, ":.");
                if (ol == strlen(upkg_short(pkg)) && !_strnicmp(obj, upkg_short(pkg), ol)) {
                    buf_u8(&b, '.');
                    buf_put(&b, upkg_short(to), strlen(upkg_short(to)));
                    rest = obj + ol;
                }
            }
            buf_put(&b, rest, strlen(rest) + 1);
            upkg_set_name(p, i, (char *)b.p);
            buf_free(&b);
        }
        free(pkg);
    }
}

int upkg_replace_names(upkg *p, const char *from, const char *to)
{
    int changed = 0;
    size_t lf = strlen(from), lt = strlen(to);
    for (int i = 0; i < p->nnames; i++) {
        const char *s = p->names[i].s;
        if (!strstr(s, from)) continue;
        buf b = {0};
        for (const char *q = strstr(s, from); q; q = strstr(s, from)) {
            buf_put(&b, s, q - s);
            buf_put(&b, to, lt);
            s = q + lf;
        }
        buf_put(&b, s, strlen(s) + 1);
        upkg_set_name(p, i, (char *)b.p);
        buf_free(&b);
        changed++;
    }
    return changed;
}

// ---------------------------------------------------------------- tagged properties

static const char *fname_at(upkg *p, const uint8_t *q)
{
    if (rd32(q + 4)) return "";                                 // numbered: never one we look for
    return upkg_name_at(p, (int32_t)rd32(q));
}

static size_t exp_start(upkg *p, int e) { return (size_t)(p->exp[e].off - p->total); }

// read the tag at *at; 0 at "None" (then *at is past it)
static int tag_read(upkg *p, int e, size_t *at, ptag *t)
{
    size_t end = exp_start(p, e) + (size_t)p->exp[e].size;
    rd r = {p->ue, end, *at};
    memset(t, 0, sizeof *t);
    t->at = r.at;
    rd_need(&r, 8);
    t->name = fname_at(p, p->ue + r.at);
    r.at += 8;
    if (!strcmp(t->name, "None")) { *at = r.at; return 0; }
    rd_need(&r, 8);
    t->type = fname_at(p, p->ue + r.at);
    r.at += 8;
    t->size_at = r.at;
    t->size = rd_i32(&r);
    rd_i32(&r);                                                 // array index
    if (!strcmp(t->type, "StructProperty")) {
        rd_need(&r, 8); t->sub = fname_at(p, p->ue + r.at); r.at += 8;
        rd_skip(&r, 16);
    } else if (!strcmp(t->type, "BoolProperty")) {
        t->bool_at = r.at;
        rd_u8(&r);
    } else if (!strcmp(t->type, "ByteProperty") || !strcmp(t->type, "EnumProperty") ||
               !strcmp(t->type, "ArrayProperty") || !strcmp(t->type, "SetProperty")) {
        rd_need(&r, 8); t->sub = fname_at(p, p->ue + r.at); r.at += 8;
    } else if (!strcmp(t->type, "MapProperty")) {
        rd_skip(&r, 16);
    }
    if (rd_u8(&r)) rd_skip(&r, 16);
    t->val = r.at;
    if (t->size < 0) sr_fail("bad property size");
    rd_skip(&r, (size_t)t->size);
    *at = r.at;
    return 1;
}

int prop_find(upkg *p, int e, const char *name, ptag *t)
{
    size_t at = exp_start(p, e);
    while (tag_read(p, e, &at, t))
        if (!strcmp(t->name, name)) return 1;
    return 0;
}

static size_t none_at(upkg *p, int e)
{
    size_t at = exp_start(p, e);
    ptag t;
    while (tag_read(p, e, &at, &t)) {}
    return at - 8;
}

// replace ue[at, at+del) with ins; export e grows by the difference
static void ue_splice(upkg *p, int e, size_t at, size_t del, const void *ins, size_t n)
{
    int64_t d = (int64_t)n - (int64_t)del;
    if (d > 0) p->ue = xrealloc(p->ue, p->ue_n + (size_t)d + 1);
    memmove(p->ue + at + n, p->ue + at + del, p->ue_n - at - del);
    memcpy(p->ue + at, ins, n);
    p->ue_n += d;
    int64_t start = p->exp[e].off;
    p->exp[e].size += d;
    for (int i = 0; i < p->nexp; i++) if (i != e && p->exp[i].off > start) p->exp[i].off += d;
    if (p->bulk > start) p->bulk += d;
}

static void put_fname(buf *b, upkg *p, const char *s) { buf_u32(b, (uint32_t)upkg_name(p, s)); buf_u32(b, 0); }

static void put_fstring(buf *b, const char *utf8)
{
    if (!*utf8) { buf_u32(b, 0); return; }
    if (!needs_wide(utf8)) { buf_fstring(b, utf8); return; }
    wchar_t *w = utf8_to_w(utf8);
    int32_t l = (int32_t)wcslen(w) + 1;
    buf_u32(b, (uint32_t)-l);
    buf_put(b, w, 2 * (size_t)l);
    free(w);
}

// insert a new tag (header built by the caller up to the Size field) before None
static void add_tag(upkg *p, int e, const char *name, const char *type, const void *hdr_extra, size_t hn,
                    const void *val, size_t vn)
{
    buf b = {0};
    put_fname(&b, p, name);
    put_fname(&b, p, type);
    buf_u32(&b, (uint32_t)vn);
    buf_u32(&b, 0);
    buf_put(&b, hdr_extra, hn);
    buf_u8(&b, 0);                                              // no property guid
    buf_put(&b, val, vn);
    ue_splice(p, e, none_at(p, e), 0, b.p, b.n);
    buf_free(&b);
}

void prop_set_bool(upkg *p, int e, const char *name, int v)
{
    ptag t;
    if (prop_find(p, e, name, &t)) {
        if (strcmp(t.type, "BoolProperty")) sr_fail("%s is not a bool", name);
        p->ue[t.bool_at] = v ? 1 : 0;
        return;
    }
    uint8_t bv = v ? 1 : 0;
    add_tag(p, e, name, "BoolProperty", &bv, 1, NULL, 0);
}

static void text_value(buf *b, uint32_t flags, const char *key, const char *utf8)
{
    buf_u32(b, flags);
    buf_u8(b, 0);                                               // history: Base
    buf_u32(b, 1); buf_u8(b, 0);                                // namespace "" (as the game's packages store it)
    put_fstring(b, key);
    put_fstring(b, utf8);
}

void prop_set_text(upkg *p, int e, const char *name, const char *key, const char *utf8)
{
    ptag t;
    buf b = {0};
    if (prop_find(p, e, name, &t)) {
        if (strcmp(t.type, "TextProperty")) sr_fail("%s is not a text", name);
        uint32_t flags = t.size >= 4 ? rd32(p->ue + t.val) : 0;
        text_value(&b, flags, key, utf8);
        int32_t sz = (int32_t)b.n;
        memcpy(p->ue + t.size_at, &sz, 4);
        ue_splice(p, e, t.val, (size_t)t.size, b.p, b.n);
    } else {
        text_value(&b, 0, key, utf8);
        add_tag(p, e, name, "TextProperty", NULL, 0, b.p, b.n);
    }
    buf_free(&b);
}

char *prop_get_text(upkg *p, int e, const char *name)
{
    ptag t;
    if (!prop_find(p, e, name, &t) || strcmp(t.type, "TextProperty")) return NULL;
    rd r = {p->ue, t.val + (size_t)t.size, t.val};
    rd_u32(&r);
    int8_t hist = (int8_t)rd_u8(&r);
    if (hist == 0) {
        rd_skip_fstring(&r);
        rd_skip_fstring(&r);
        return rd_fstring(&r);
    }
    if (hist == -1 && rd_i32(&r)) return rd_fstring(&r);
    return NULL;
}

void prop_set_softpath(upkg *p, int e, const char *name, const char *path)
{
    ptag t;
    int idx = upkg_name(p, path);
    if (prop_find(p, e, name, &t)) {
        if (strcmp(t.type, "SoftObjectProperty") || t.size < 8) sr_fail("%s is not a soft object path", name);
        int32_t z = 0;
        memcpy(p->ue + t.val, &idx, 4);
        memcpy(p->ue + t.val + 4, &z, 4);
        return;
    }
    uint8_t v[12] = {0};
    memcpy(v, &idx, 4);
    add_tag(p, e, name, "SoftObjectProperty", NULL, 0, v, 12);
}

void prop_set_guid(upkg *p, int e, const char *name, const uint8_t g[16])
{
    ptag t;
    if (prop_find(p, e, name, &t)) {
        if (strcmp(t.type, "StructProperty") || t.size != 16) sr_fail("%s is not a guid", name);
        memcpy(p->ue + t.val, g, 16);
        return;
    }
    buf h = {0};
    put_fname(&h, p, "Guid");
    uint8_t z[16] = {0};
    buf_put(&h, z, 16);
    add_tag(p, e, name, "StructProperty", h.p, h.n, g, 16);
    buf_free(&h);
}

int prop_get_guid(upkg *p, int e, const char *name, uint8_t g[16])
{
    ptag t;
    if (!prop_find(p, e, name, &t) || strcmp(t.type, "StructProperty") || t.size != 16) return 0;
    memcpy(g, p->ue + t.val, 16);
    return 1;
}

int prop_filter_softarray(upkg *p, int e, const char *name, const char *keep)
{
    ptag t;
    if (!prop_find(p, e, name, &t) || strcmp(t.type, "ArrayProperty") || !t.sub || strcmp(t.sub, "SoftObjectProperty"))
        return 0;
    rd r = {p->ue, t.val + (size_t)t.size, t.val};
    int32_t n = rd_i32(&r);
    buf b = {0};
    buf_u32(&b, 0);
    int kept = 0;
    for (int32_t i = 0; i < n; i++) {
        size_t s = r.at;
        rd_need(&r, 8);
        const char *nm = fname_at(p, p->ue + r.at);
        r.at += 8;
        rd_skip_fstring(&r);
        if (!_stricmp(nm, keep)) { buf_put(&b, p->ue + s, r.at - s); kept++; }
    }
    if (kept == n) { buf_free(&b); return 0; }
    memcpy(b.p, &kept, 4);
    int32_t sz = (int32_t)b.n;
    memcpy(p->ue + t.size_at, &sz, 4);
    ue_splice(p, e, t.val, (size_t)t.size, b.p, b.n);
    buf_free(&b);
    return n - kept;
}

int prop_get_colors(upkg *p, int e, const char *name, float (*rgba)[4], int max)
{
    ptag t;
    if (!prop_find(p, e, name, &t) || strcmp(t.type, "ArrayProperty") || !t.sub || strcmp(t.sub, "StructProperty"))
        return 0;
    rd r = {p->ue, t.val + (size_t)t.size, t.val};
    int32_t n = rd_i32(&r);
    ptag inner;
    size_t at = r.at;
    if (!tag_read(p, e, &at, &inner) || !inner.sub || strcmp(inner.sub, "LinearColor")) return 0;
    r.at = inner.val;
    int k = 0;
    for (int32_t i = 0; i < n && k < max; i++, k++) {
        rd_need(&r, 16);
        memcpy(rgba[k], p->ue + r.at, 16);
        r.at += 16;
    }
    return k;
}
