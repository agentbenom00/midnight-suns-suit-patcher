// The patcher: analyse a suit mod pak, then turn it into a standalone suit with a SuitRegistry manifest.
#pragma once
#include "upkg.h"

#define MAX_PALETTES 16
#define PATCHER_VERSION "1.0.0"

// ---- game files (game.c)
typedef struct gindex gindex;
// the game's Paks folder: next to `beside` (a file path) if it is the Paks folder, the saved choice, Steam or Epic.
// NULL if not found.
wchar_t *game_find_paks(const wchar_t *beside);
void game_remember_paks(const wchar_t *paks);
int game_paks_valid(const wchar_t *paks);
// index of the game's own paks: suit-related folders plus `extra_dirs` (pak paths like "CodaGame/Content/X/")
gindex *game_open(const wchar_t *paks, char **extra_dirs, int nextra);
void game_close(gindex *g);
int game_has(gindex *g, const char *path);                      // pak path, e.g. CodaGame/Content/A/B.uasset
uint8_t *game_read(gindex *g, const char *path, size_t *n);     // NULL if absent
typedef void (*game_each_fn)(const char *path, void *ctx);
void game_each(gindex *g, game_each_fn f, void *ctx);           // every indexed file

// "/Game/A/B" <-> "CodaGame/Content/A/B" (+ extension)
char *game_to_pak(const char *game_path, const char *ext);
char *pak_to_game(const char *pak_path);                        // NULL if not under */Content/; extension removed

// ---- palette colours (colors.c)
int palette_colormap(upkg *p, float cm[5][4]);                  // bit i set = channel i (R,G,B,A,Emissive) found
// names nearest to a colour, best first; returns how many were written
int color_names(const float srgb[3], const char **out, int max);
void linear_to_srgb8(const float lin[3], uint8_t out[3]);

// ---- analysis and patching (patcher.c)
enum { MODE_REPLACER = 1, MODE_STANDALONE = 2 };

typedef struct {
    char *pkg;                                                  // /Game path
    char *old_name;                                             // in-game name now (may be NULL)
    char auto_name[64];                                         // used when the user leaves the box empty
    uint8_t rgb[3];                                             // its main colour (sRGB), for the swatch
    const char *cand[8]; int ncand;                             // colour names, nearest first
} pal_info;

typedef struct mod_file mod_file;
typedef struct mod_pkg mod_pkg;
typedef struct clone_item clone_item;

typedef struct {
    int mode;
    wchar_t *pak_path;                                          // the pak that is read (the original)
    wchar_t *out_path;                                          // where the patched pak is written
    wchar_t *paks_dir;                                          // the game's Paks folder
    char summary[1024];                                         // for the window
    char hero[16];                                              // e.g. MAGK
    char *outfit;                                               // /Game path of the suit's outfit
    char *outfit_name;                                          // its in-game name
    pal_info pal[MAX_PALETTES]; int npal;
    // internals
    mod_file *files; int nfiles;
    mod_pkg *pkgs; int npkgs;
    char *source_name;                                          // file name of the original pak
    clone_item *items; int nitems;
    gindex *game;
    pak mod;
} analysis;

typedef struct {
    const char *name;                                           // the suit's name (UTF-8)
    int rarity;                                                 // 0 Common, 1 Rare, 2 Epic, 3 Legendary
    const char *pal_names[MAX_PALETTES];                        // NULL / "" = automatic
} patch_options;

typedef struct { wchar_t *out_path; wchar_t *backup_path; char id[96]; int nfiles; } patch_result;

// both fail via sr_fail (run them inside a sr_jmp handler)
analysis *analyze_pak(const wchar_t *pak, const wchar_t *game_paks);   // game_paks NULL = find it
patch_result run_patch(analysis *a, const patch_options *o);
void analysis_free(analysis *a);

// set when the game's Paks folder could not be found (the window then asks for it)
extern int patcher_need_game;
