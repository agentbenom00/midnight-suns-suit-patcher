// Palette colours: read a palette's tints and name a colour (nearest named colour in CIELAB).
#include "patcher.h"
#include <math.h>

// MaterialSlotsData[].BaseColorTint.ColorMap: EMaterialParamType::Red/Green/Blue/Alpha/Emissive -> FLinearColor,
// serialized as (FName, 4 floats). Found by the FName, as the game's own map isn't readable as tagged properties.
int palette_colormap(upkg *p, float cm[5][4])
{
    static const char *ch[5] = {"EMaterialParamType::Red", "EMaterialParamType::Green", "EMaterialParamType::Blue",
                                "EMaterialParamType::Alpha", "EMaterialParamType::Emissive"};
    int e = upkg_main_export(p), have = 0;
    if (e < 0) return 0;
    size_t s = (size_t)(p->exp[e].off - p->total), end = s + (size_t)p->exp[e].size;
    for (int c = 0; c < 5; c++) {
        int idx = upkg_find_name(p, ch[c]);
        if (idx < 0) continue;
        for (size_t at = s; at + 24 <= end; at++) {
            if (rd32(p->ue + at) != (uint32_t)idx || rd32(p->ue + at + 4) != 0) continue;
            float v[4];
            memcpy(v, p->ue + at + 8, 16);
            int ok = 1;
            for (int k = 0; k < 4; k++) ok &= isfinite(v[k]) && v[k] >= -0.01f && v[k] <= 100.0f;
            if (!ok) continue;
            memcpy(cm[c], v, 16);
            have |= 1 << c;
            break;
        }
    }
    return have;
}

static float to_srgb(float x)
{
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    return x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1 / 2.4f) - 0.055f;
}

void linear_to_srgb8(const float lin[3], uint8_t out[3])
{
    for (int i = 0; i < 3; i++) out[i] = (uint8_t)lroundf(to_srgb(lin[i]) * 255.0f);
}

static void srgb_to_lab(const float c[3], float lab[3])
{
    float l[3];
    for (int i = 0; i < 3; i++) l[i] = c[i] <= 0.04045f ? c[i] / 12.92f : powf((c[i] + 0.055f) / 1.055f, 2.4f);
    float x = (0.4124f * l[0] + 0.3576f * l[1] + 0.1805f * l[2]) / 0.95047f;
    float y = 0.2126f * l[0] + 0.7152f * l[1] + 0.0722f * l[2];
    float z = (0.0193f * l[0] + 0.1192f * l[1] + 0.9505f * l[2]) / 1.08883f;
    float f[3], v[3] = {x, y, z};
    for (int i = 0; i < 3; i++) f[i] = v[i] > 0.008856f ? cbrtf(v[i]) : 7.787f * v[i] + 16.0f / 116.0f;
    lab[0] = 116 * f[1] - 16;
    lab[1] = 500 * (f[0] - f[1]);
    lab[2] = 200 * (f[1] - f[2]);
}

static const struct { const char *name; uint8_t r, g, b; } NAMES[] = {
    {"Black", 12, 12, 12},        {"Onyx", 40, 40, 44},          {"Charcoal", 64, 64, 68},
    {"Slate", 100, 110, 125},     {"Gray", 128, 128, 128},       {"Silver", 192, 192, 196},
    {"White", 245, 245, 245},     {"Ivory", 240, 234, 214},      {"Bone", 220, 210, 185},
    {"Red", 200, 20, 20},         {"Crimson", 160, 15, 40},      {"Scarlet", 235, 40, 20},
    {"Blood Red", 110, 5, 10},    {"Maroon", 90, 15, 25},        {"Ruby", 175, 20, 75},
    {"Orange", 245, 120, 20},     {"Rust", 160, 70, 30},         {"Amber", 245, 175, 20},
    {"Gold", 212, 170, 50},       {"Yellow", 245, 225, 40},      {"Bronze", 160, 110, 50},
    {"Copper", 185, 100, 60},     {"Brown", 100, 60, 30},        {"Chocolate", 70, 40, 25},
    {"Tan", 205, 170, 125},       {"Olive", 110, 115, 40},       {"Khaki", 170, 160, 110},
    {"Lime", 140, 220, 40},       {"Green", 30, 150, 50},        {"Emerald", 20, 155, 95},
    {"Forest Green", 25, 80, 35}, {"Toxic Green", 90, 255, 40},  {"Mint", 150, 230, 180},
    {"Teal", 20, 125, 125},       {"Turquoise", 50, 210, 195},   {"Cyan", 30, 220, 240},
    {"Sky Blue", 110, 180, 235},  {"Azure", 30, 130, 230},       {"Blue", 30, 60, 200},
    {"Cobalt", 20, 70, 160},      {"Denim", 75, 95, 150},      {"Navy", 15, 25, 80},          {"Midnight Blue", 15, 20, 50},
    {"Indigo", 60, 30, 130},      {"Purple", 110, 30, 160},      {"Violet", 140, 70, 210},
    {"Lavender", 180, 160, 225},  {"Plum", 100, 35, 80},         {"Magenta", 220, 30, 180},
    {"Pink", 245, 120, 175},      {"Hot Pink", 250, 40, 140},    {"Rose", 215, 90, 120},
};
#define NNAMES ((int)(sizeof NAMES / sizeof NAMES[0]))

int color_names(const float srgb[3], const char **out, int max)
{
    float lab[3];
    srgb_to_lab(srgb, lab);
    float d[NNAMES];
    int order[NNAMES];
    for (int i = 0; i < NNAMES; i++) {
        float c[3] = {NAMES[i].r / 255.0f, NAMES[i].g / 255.0f, NAMES[i].b / 255.0f}, l2[3];
        srgb_to_lab(c, l2);
        // lightness counts a bit less than hue/chroma: tints are shown over a shaded texture
        float dl = (lab[0] - l2[0]) * 0.75f, da = lab[1] - l2[1], db = lab[2] - l2[2];
        d[i] = dl * dl + da * da + db * db;
        order[i] = i;
    }
    for (int a = 1; a < NNAMES; a++)
        for (int b = a; b > 0 && d[order[b - 1]] > d[order[b]]; b--) { int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t; }
    int n = max < NNAMES ? max : NNAMES;
    for (int i = 0; i < n; i++) out[i] = NAMES[order[i]].name;
    return n;
}
