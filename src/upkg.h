// Cooked UE 4.26 packages (.uasset header + .uexp exports): just enough to rename package references, and to read and
// change a few tagged properties of an export.
#pragma once
#include "sr.h"

typedef struct {
    char *s;                                                    // narrow: raw bytes; wide: UTF-8
    int wide;
    uint16_t h1, h2;                                            // serialized hashes (recomputed when changed)
    int changed;
} pname;

typedef struct { int32_t class_pkg, class_name, outer, name; size_t at; } pimport;
typedef struct { int32_t class_idx, outer, name; int64_t size, off; size_t at; } pexport;

typedef struct {
    uint8_t *ua; size_t ua_n;                                   // original header (offsets refer to it)
    uint8_t *ue; size_t ue_n;                                   // exports (edited in place)
    int split;                                                  // 1 = came as .uasset + .uexp
    int ver, hashes;
    uint32_t flags;
    int32_t total, name_off;
    size_t name_end;
    // summary fields that hold header offsets (positions in ua; 0 = absent)
    size_t f_total, f_namecount, f_off[12], f_bulk;
    int nf_off;
    int64_t bulk;
    pname *names; int nnames, cap_names, nnames0;
    pimport *imp; int nimp;
    pexport *exp; int nexp;
} upkg;

upkg *upkg_load(uint8_t *ua, size_t ua_n, uint8_t *ue, size_t ue_n);   // takes ownership; ue may be NULL
upkg *upkg_load_ex(uint8_t *ua, size_t ua_n, uint8_t *ue, size_t ue_n, int header_only);   // header only: no .uexp needed
void upkg_free(upkg *p);
void upkg_save(upkg *p, uint8_t **ua, size_t *ua_n, uint8_t **ue, size_t *ue_n);   // *ue NULL if not split

int upkg_find_name(upkg *p, const char *s);                     // -1 if absent
int upkg_name(upkg *p, const char *s);                          // index, added if absent
const char *upkg_name_at(upkg *p, int32_t i);                   // "" if out of range
void upkg_set_name(upkg *p, int i, const char *s);              // change an entry in place
int upkg_main_export(upkg *p);                                  // the asset export (outer 0), -1 if none
const char *upkg_class_of(upkg *p, int exp);                    // class name of an export
const char *upkg_short(const char *pkg_path);                   // "/Game/A/B" -> "B" (pointer into the string)

// every "/Game/..." package this one refers to (imports and soft references), malloc'd strings
char **upkg_refs(upkg *p, int *n);

// package renames: old -> new package path, e.g. /Game/A/X -> /Game/A/X_Tag
typedef struct { char **from, **to; int n; } rename_map;
void rmap_add(rename_map *m, const char *from, const char *to);
const char *rmap_get(const rename_map *m, const char *from);   // NULL if not renamed
// Point every reference to a renamed package at its new name (soft paths, package imports and the object imports in
// them), and rename this package's own asset export if self_old/self_new are given.
void upkg_rename(upkg *p, const rename_map *m, const char *self_old, const char *self_new);
// test helper: substring replace in every name entry (what uassettool "clone" does)
int upkg_replace_names(upkg *p, const char *from, const char *to);

// ---- tagged properties of an export
typedef struct {
    size_t at, size_at, val;                                    // tag start, its Size field, the value (in ue)
    int32_t size;
    const char *name, *type, *sub;                              // sub: struct name / array inner type
    size_t bool_at;
} ptag;

int prop_find(upkg *p, int exp, const char *name, ptag *t);     // 1 if found
void prop_set_bool(upkg *p, int exp, const char *name, int v);
void prop_set_text(upkg *p, int exp, const char *name, const char *key, const char *utf8);
char *prop_get_text(upkg *p, int exp, const char *name);        // source string, malloc'd, NULL if none
void prop_set_softpath(upkg *p, int exp, const char *name, const char *path);
void prop_set_guid(upkg *p, int exp, const char *name, const uint8_t guid[16]);
int prop_get_guid(upkg *p, int exp, const char *name, uint8_t guid[16]);
// soft-path array: keep only the entries equal to `keep`; returns how many entries were removed
int prop_filter_softarray(upkg *p, int exp, const char *name, const char *keep);
// linear colours of a LinearColor array property (e.g. CustomInventoryIconColors); count, up to max
int prop_get_colors(upkg *p, int exp, const char *name, float (*rgba)[4], int max);
