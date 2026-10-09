// The window: Browse to a suit mod pak, type a name, pick a rarity, optionally name the palettes and add add-on paks,
// Patch.
// Plain Win32 (works the same on Windows and under Wine/Proton). Reading and patching run on a worker thread.
#include "patcher.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

HINSTANCE sr_self;

enum {
    ID_PATH = 100, ID_BROWSE, ID_SUMMARY, ID_NAME, ID_RARITY, ID_PATCH, ID_LOG, ID_PALGROUP, ID_PALHINT,
    ID_PAL = 200, ID_SWATCH = 300, ID_PALLABEL = 400, ID_ADDON = 500, ID_ADDON_BROWSE = 510, ID_ADDON_CLEAR = 520,
    ID_ADDONGROUP = 530,
};
#define WM_LOGLINE (WM_APP + 1)
#define WM_ANALYZED (WM_APP + 2)
#define WM_PATCHED (WM_APP + 3)

static const char *RARITY_TEXT[4] = {"Legendary", "Epic", "Rare", "Common"};
static const int RARITY_VALUE[4] = {3, 2, 1, 0};

static HWND wnd, h_path, h_browse, h_summary, h_name, h_rarity, h_patch, h_log, h_palgroup, h_palhint;
static HWND h_pal[MAX_PALETTES], h_swatch[MAX_PALETTES], h_pallabel[MAX_PALETTES];
static HWND h_addon[MAX_ADDONS], h_addon_browse[MAX_ADDONS], h_addon_clear[MAX_ADDONS];
static HFONT font, font_title, font_small;
static analysis *cur;                                           // the analysed pak (GUI thread owns it when idle)
static volatile LONG busy;
static wchar_t *pending_path;
static wchar_t *main_src;                                       // the mod pak to read (its backup once patched)
static wchar_t *addon_sel[MAX_ADDONS];                          // chosen add-on paks (NULL = empty slot)
static double scale = 1.0;

static int S(int v) { return (int)(v * scale + 0.5); }

// ---------------------------------------------------------------- log

static void gui_log(const char *line)
{
    wchar_t *w = utf8_to_w(line);
    if (!PostMessageW(wnd, WM_LOGLINE, 0, (LPARAM)w)) free(w);
}

static void log_append(const wchar_t *s)
{
    int n = GetWindowTextLengthW(h_log);
    if (n > 60000) { SendMessageW(h_log, EM_SETSEL, 0, 20000); SendMessageW(h_log, EM_REPLACESEL, FALSE, (LPARAM)L""); n = GetWindowTextLengthW(h_log); }
    SendMessageW(h_log, EM_SETSEL, n, n);
    if (n) SendMessageW(h_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(h_log, EM_REPLACESEL, FALSE, (LPARAM)s);
}

// ---------------------------------------------------------------- state

static char *get_text_utf8(HWND h)
{
    int n = GetWindowTextLengthW(h);
    wchar_t *w = xcalloc(n + 2, sizeof(wchar_t));
    GetWindowTextW(h, w, n + 1);
    // trim
    wchar_t *s = w;
    while (*s == L' ' || *s == L'\t') s++;
    size_t l = wcslen(s);
    while (l && (s[l - 1] == L' ' || s[l - 1] == L'\t')) s[--l] = 0;
    char *u = w_to_utf8(s);
    free(w);
    return u;
}

static void update_buttons(void)
{
    char *name = get_text_utf8(h_name);
    int ok = cur && !busy && name[0] && SendMessageW(h_rarity, CB_GETCURSEL, 0, 0) != CB_ERR;
    free(name);
    EnableWindow(h_patch, ok);
    EnableWindow(h_browse, !busy);
    for (int i = 0; i < MAX_ADDONS; i++) {
        EnableWindow(h_addon_browse[i], !busy && main_src);
        EnableWindow(h_addon_clear[i], !busy && addon_sel[i]);
    }
}

static void show_addons(void)
{
    for (int i = 0; i < MAX_ADDONS; i++) SetWindowTextW(h_addon[i], addon_sel[i] ? addon_sel[i] : L"");
}

// the add-ons the analysis really read (backups once patched), in their slots
static void take_addons(const analysis *a)
{
    for (int i = 0; i < MAX_ADDONS; i++) { free(addon_sel[i]); addon_sel[i] = NULL; }
    for (int i = 0; i < a->naddons; i++) addon_sel[i] = _wcsdup(a->addon_path[i]);
    free(main_src);
    main_src = _wcsdup(a->pak_path);
    show_addons();
}

static void set_summary(const char *s)
{
    wchar_t *w = utf8_to_w(s);
    SetWindowTextW(h_summary, w);
    free(w);
}

static void show_palettes(void)
{
    int n = cur ? cur->npal : 0;
    for (int i = 0; i < MAX_PALETTES; i++) {
        int show = i < n;
        ShowWindow(h_pal[i], show ? SW_SHOW : SW_HIDE);
        ShowWindow(h_swatch[i], show ? SW_SHOW : SW_HIDE);
        ShowWindow(h_pallabel[i], show ? SW_SHOW : SW_HIDE);
        if (!show) continue;
        SetWindowTextW(h_pal[i], L"");
        wchar_t cue[160];
        wchar_t *wc = utf8_to_w(cur->pal[i].auto_name);
        swprintf(cue, 160, L"%ls (auto)", wc);
        free(wc);
        SendMessageW(h_pal[i], EM_SETCUEBANNER, TRUE, (LPARAM)cue);
        InvalidateRect(h_swatch[i], NULL, TRUE);
    }
    const wchar_t *hint = !cur ? L"Browse to a suit mod to see its palettes."
                        : n ? L"Leave a box empty to name that palette after its main colour."
                            : L"This suit has no palettes.";
    SetWindowTextW(h_palhint, hint);
}

// ---------------------------------------------------------------- worker

typedef struct { wchar_t *pak, *game, *addons[MAX_ADDONS]; int naddons; } analyze_job;

static DWORD WINAPI analyze_thread(LPVOID p)
{
    analyze_job *j = p;
    analysis *volatile a = NULL;
    jmp_buf jb;
    sr_jmp = &jb;
    if (!setjmp(jb)) a = analyze_pak(j->pak, (const wchar_t *const *)j->addons, j->naddons, j->game);
    else { log_msg("error: %s", sr_error); a = NULL; }
    sr_jmp = NULL;
    free(j->pak); free(j->game);
    for (int i = 0; i < j->naddons; i++) free(j->addons[i]);
    free(j);
    PostMessageW(wnd, WM_ANALYZED, patcher_need_game, (LPARAM)a);
    return 0;
}

static void start_analysis(const wchar_t *pak, const wchar_t *game)
{
    if (busy) return;
    if (cur) { analysis_free(cur); cur = NULL; }
    show_palettes();
    InterlockedExchange(&busy, 1);
    update_buttons();
    SetWindowTextW(h_path, pak);
    set_summary("Reading the mod and the game's files...");
    analyze_job *j = xcalloc(1, sizeof *j);
    j->pak = _wcsdup(pak);
    j->game = game ? _wcsdup(game) : NULL;
    for (int i = 0; i < MAX_ADDONS; i++) if (addon_sel[i]) j->addons[j->naddons++] = _wcsdup(addon_sel[i]);
    free(pending_path);
    pending_path = _wcsdup(pak);
    if (pak != main_src) { free(main_src); main_src = _wcsdup(pak); }
    HANDLE t = CreateThread(NULL, 4 << 20, analyze_thread, j, 0, NULL);
    if (t) CloseHandle(t);
}

typedef struct { patch_options o; char *name, *pals[MAX_PALETTES]; } patch_job;

static DWORD WINAPI patch_thread(LPVOID p)
{
    patch_job *j = p;
    volatile int ok = 0;
    static patch_result r;
    jmp_buf jb;
    sr_jmp = &jb;
    if (!setjmp(jb)) { r = run_patch(cur, &j->o); ok = 1; }
    else log_msg("error: %s", sr_error);
    sr_jmp = NULL;
    free(j->name);
    for (int i = 0; i < MAX_PALETTES; i++) free(j->pals[i]);
    free(j);
    PostMessageW(wnd, WM_PATCHED, ok, ok ? (LPARAM)&r : 0);
    return 0;
}

static void start_patch(void)
{
    if (busy || !cur) return;
    patch_job *j = xcalloc(1, sizeof *j);
    j->name = get_text_utf8(h_name);
    j->o.name = j->name;
    LRESULT sel = SendMessageW(h_rarity, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR || !j->name[0]) { free(j->name); free(j); return; }
    j->o.rarity = RARITY_VALUE[sel];
    for (int i = 0; i < cur->npal; i++) { j->pals[i] = get_text_utf8(h_pal[i]); j->o.pal_names[i] = j->pals[i]; }
    InterlockedExchange(&busy, 1);
    update_buttons();
    log_append(L"");
    HANDLE t = CreateThread(NULL, 4 << 20, patch_thread, j, 0, NULL);
    if (t) CloseHandle(t);
}

// ---------------------------------------------------------------- dialogs

// another suit mod: its add-ons start empty (a pak patched before brings back its own)
static void new_mod(const wchar_t *file)
{
    if (busy) return;
    for (int i = 0; i < MAX_ADDONS; i++) { free(addon_sel[i]); addon_sel[i] = NULL; }
    show_addons();
    start_analysis(file, NULL);
}

static void browse(void)
{
    wchar_t file[MAX_PATH * 4] = L"";
    wchar_t *init = cur ? _wcsdup(cur->paks_dir) : game_find_paks(NULL);
    OPENFILENAMEW ofn = {sizeof ofn};
    ofn.hwndOwner = wnd;
    ofn.lpstrFilter = L"Suit mod paks (*.pak)\0*.pak\0Backups of patched paks (*.pak.bak)\0*.pak.bak\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrInitialDir = init;
    ofn.lpstrTitle = L"Choose the suit mod's .pak file";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) new_mod(file);
    free(init);
}

static void browse_addon(int slot)
{
    wchar_t file[MAX_PATH * 4] = L"";
    wchar_t *init = cur ? _wcsdup(cur->paks_dir) : game_find_paks(NULL);
    OPENFILENAMEW ofn = {sizeof ofn};
    ofn.hwndOwner = wnd;
    ofn.lpstrFilter = L"Add-on paks (*.pak)\0*.pak\0Backups (*.pak.bak)\0*.pak.bak\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrInitialDir = init;
    ofn.lpstrTitle = L"Choose an add-on .pak for this suit (hair, weapon, ...)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn) && main_src) {
        free(addon_sel[slot]);
        addon_sel[slot] = _wcsdup(file);
        show_addons();
        wchar_t *m = _wcsdup(main_src);
        start_analysis(m, NULL);
        free(m);
    }
    free(init);
}

static void clear_addon(int slot)
{
    if (busy || !addon_sel[slot] || !main_src) return;
    free(addon_sel[slot]);
    addon_sel[slot] = NULL;
    show_addons();
    wchar_t *m = _wcsdup(main_src);
    start_analysis(m, NULL);
    free(m);
}

static wchar_t *ask_game_folder(void)
{
    MessageBoxW(wnd, L"The Midnight Suns game folder wasn't found.\n\nIn the next window, choose the folder the game is "
                     L"installed in (the one with MidnightSuns.exe).", L"Where is Midnight Suns?", MB_OK | MB_ICONINFORMATION);
    BROWSEINFOW bi = {0};
    wchar_t name[MAX_PATH];
    bi.hwndOwner = wnd;
    bi.pszDisplayName = name;
    bi.lpszTitle = L"Choose the Midnight Suns game folder (with MidnightSuns.exe)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST id = SHBrowseForFolderW(&bi);
    if (!id) return NULL;
    wchar_t dir[MAX_PATH * 2];
    int ok = SHGetPathFromIDListW(id, dir);
    CoTaskMemFree(id);
    if (!ok) return NULL;
    static const wchar_t *subs[] = {L"", L"MidnightSuns\\Content\\Paks", L"Content\\Paks", L"Paks"};
    for (int i = 0; i < 4; i++) {
        wchar_t *p = *subs[i] ? wpath(dir, subs[i]) : _wcsdup(dir);
        if (game_paks_valid(p)) { game_remember_paks(p); return p; }
        free(p);
    }
    MessageBoxW(wnd, L"That folder doesn't contain the game's files (MidnightSuns\\Content\\Paks\\pakchunk0-WindowsNoEditor.pak).",
                L"Not the game folder", MB_OK | MB_ICONWARNING);
    return NULL;
}

static int registry_installed(const wchar_t *paks)
{
    wchar_t *p = wpath(paks, L"..\\..\\Binaries\\Win64\\version.dll");
    int ok = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    free(p);
    return ok;
}

// ---------------------------------------------------------------- developer test hook
// SUITPATCHER_SNAPSHOT=<file.bmp> saves the window once a pak has been read (and again as <file>.2.bmp after patching);
// SUITPATCHER_AUTOFILL=<name>|<rarity index>|<palette 1 name> fills the form and presses Patch.

static void snapshot(const char *suffix)
{
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("SUITPATCHER_SNAPSHOT", path, MAX_PATH - 8);
    if (!n || n >= MAX_PATH - 8) return;
    strcat(path, suffix);
    RedrawWindow(wnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    RECT r;
    GetWindowRect(wnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
    HBITMAP bm = CreateCompatibleBitmap(sdc, w, h);
    SelectObject(mdc, bm);
    BitBlt(mdc, 0, 0, w, h, sdc, r.left, r.top, SRCCOPY);
    BITMAPINFOHEADER bi = {sizeof bi, w, h, 1, 24, BI_RGB};
    int stride = (w * 3 + 3) & ~3;
    uint8_t *px = xmalloc((size_t)stride * h);
    GetDIBits(mdc, bm, 0, h, px, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER bf = {0x4D42, (DWORD)(sizeof bf + sizeof bi + stride * h), 0, 0, sizeof bf + sizeof bi};
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(&bf, sizeof bf, 1, f); fwrite(&bi, sizeof bi, 1, f); fwrite(px, (size_t)stride * h, 1, f); fclose(f); }
    free(px);
    DeleteObject(bm); DeleteDC(mdc); ReleaseDC(NULL, sdc);
}

static void autofill(void)
{
    char v[512];
    if (!GetEnvironmentVariableA("SUITPATCHER_AUTOFILL", v, sizeof v)) return;
    char *name = strtok(v, "|"), *rar = strtok(NULL, "|"), *p1 = strtok(NULL, "|");
    if (name) { wchar_t *w = utf8_to_w(name); SetWindowTextW(h_name, w); free(w); }
    if (rar) SendMessageW(h_rarity, CB_SETCURSEL, atoi(rar), 0);
    if (p1 && cur && cur->npal > 1) { wchar_t *w = utf8_to_w(p1); SetWindowTextW(h_pal[1], w); free(w); }
    update_buttons();
    snapshot(".1.bmp");
    if (IsWindowEnabled(h_patch)) start_patch();
}

// ---------------------------------------------------------------- window

static HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, DWORD ex)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), wnd,
                             (HMENU)(INT_PTR)id, sr_self, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
    return c;
}

static void create_controls(void)
{
    const int L = 16, W = 608;
    HWND t = make(L"STATIC", L"Midnight Suns Suit Patcher", 0, L, 12, W, 26, 0, 0);
    SendMessageW(t, WM_SETFONT, (WPARAM)font_title, FALSE);
    HWND st = make(L"STATIC", L"Turns a suit mod into a suit of its own that works with the SuitRegistry.", 0, L, 38, W, 18, 0, 0);
    SendMessageW(st, WM_SETFONT, (WPARAM)font_small, FALSE);

    make(L"STATIC", L"Suit mod (.pak):", 0, L, 70, W, 18, 0, 0);
    h_path = make(L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY, L, 90, W - 104, 24, ID_PATH, WS_EX_CLIENTEDGE);
    h_browse = make(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, L + W - 96, 89, 96, 26, ID_BROWSE, 0);
    h_summary = make(L"STATIC", L"Choose the .pak file of the suit mod (or drop it on this window).", SS_LEFT, L, 122, W, 48,
                     ID_SUMMARY, 0);

    make(L"STATIC", L"Suit name:", 0, L, 180, 300, 18, 0, 0);
    h_name = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, L, 200, 420, 24, ID_NAME, WS_EX_CLIENTEDGE);
    SendMessageW(h_name, EM_SETLIMITTEXT, 60, 0);
    SendMessageW(h_name, EM_SETCUEBANNER, TRUE, (LPARAM)L"Name shown in the game");
    make(L"STATIC", L"Rarity:", 0, L + 436, 180, 172, 18, 0, 0);
    h_rarity = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, L + 436, 200, 172, 200, ID_RARITY, 0);
    for (int i = 0; i < 4; i++) {
        wchar_t *w = utf8_to_w(RARITY_TEXT[i]);
        SendMessageW(h_rarity, CB_ADDSTRING, 0, (LPARAM)w);
        free(w);
    }
    SendMessageW(h_rarity, CB_SETCUEBANNER, 0, (LPARAM)L"Choose...");

    h_palgroup = make(L"BUTTON", L"Alt palette names (optional)", BS_GROUPBOX, L, 236, W, 205, ID_PALGROUP, 0);
    h_palhint = make(L"STATIC", L"", 0, L + 12, 256, W - 24, 18, ID_PALHINT, 0);
    SendMessageW(h_palhint, WM_SETFONT, (WPARAM)font_small, FALSE);
    for (int i = 0; i < MAX_PALETTES; i++) {
        int col = i / 6, row = i % 6;
        int x = L + 12 + col * 196, y = 278 + row * 26;
        wchar_t num[8];
        swprintf(num, 8, L"%d.", i + 1);
        h_pallabel[i] = make(L"STATIC", num, SS_RIGHT, x, y + 3, 22, 18, ID_PALLABEL + i, 0);
        h_swatch[i] = make(L"STATIC", L"", SS_OWNERDRAW, x + 26, y + 3, 18, 18, ID_SWATCH + i, 0);
        h_pal[i] = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, x + 48, y, 140, 23, ID_PAL + i, WS_EX_CLIENTEDGE);
        SendMessageW(h_pal[i], EM_SETLIMITTEXT, 40, 0);
        ShowWindow(h_pal[i], SW_HIDE); ShowWindow(h_swatch[i], SW_HIDE); ShowWindow(h_pallabel[i], SW_HIDE);
    }

    make(L"BUTTON", L"Add-ons (optional): extra paks for this suit, like hair or a weapon", BS_GROUPBOX, L, 449, W, 112,
         ID_ADDONGROUP, 0);
    for (int i = 0; i < MAX_ADDONS; i++) {
        int y = 471 + i * 28;
        wchar_t num[8];
        swprintf(num, 8, L"%d.", i + 1);
        make(L"STATIC", num, SS_RIGHT, L + 12, y + 3, 22, 18, 0, 0);
        h_addon[i] = make(L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY, L + 40, y, W - 218, 24, ID_ADDON + i, WS_EX_CLIENTEDGE);
        SendMessageW(h_addon[i], EM_SETCUEBANNER, TRUE, (LPARAM)L"No add-on");
        h_addon_browse[i] = make(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, L + W - 172, y - 1, 90, 26,
                                 ID_ADDON_BROWSE + i, 0);
        h_addon_clear[i] = make(L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, L + W - 76, y - 1, 64, 26,
                                ID_ADDON_CLEAR + i, 0);
    }

    h_patch = make(L"BUTTON", L"Patch", BS_DEFPUSHBUTTON | WS_TABSTOP, L + W - 140, 571, 140, 32, ID_PATCH, 0);
    HWND need = make(L"STATIC", L"Needs a .pak, a name and a rarity.", 0, L, 579, W - 160, 18, 0, 0);
    SendMessageW(need, WM_SETFONT, (WPARAM)font_small, FALSE);
    h_log = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, L, 613, W, 88, ID_LOG, WS_EX_CLIENTEDGE);
    SendMessageW(h_log, WM_SETFONT, (WPARAM)font_small, FALSE);
    show_palettes();
    update_buttons();
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        wnd = h;
        create_controls();
        DragAcceptFiles(h, TRUE);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_BROWSE && code == BN_CLICKED) browse();
        else if (id >= ID_ADDON_BROWSE && id < ID_ADDON_BROWSE + MAX_ADDONS && code == BN_CLICKED) browse_addon(id - ID_ADDON_BROWSE);
        else if (id >= ID_ADDON_CLEAR && id < ID_ADDON_CLEAR + MAX_ADDONS && code == BN_CLICKED) clear_addon(id - ID_ADDON_CLEAR);
        else if (id == ID_PATCH && code == BN_CLICKED) start_patch();
        else if ((id == ID_NAME && code == EN_CHANGE) || (id == ID_RARITY && code == CBN_SELCHANGE)) update_buttons();
        return 0;
    }
    case WM_DROPFILES: {
        wchar_t file[MAX_PATH * 4];
        if (DragQueryFileW((HDROP)wp, 0, file, MAX_PATH * 4)) new_mod(file);
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
        int i = (int)d->CtlID - ID_SWATCH;
        if (i < 0 || i >= MAX_PALETTES || !cur || i >= cur->npal) return FALSE;
        HBRUSH b = CreateSolidBrush(RGB(cur->pal[i].rgb[0], cur->pal[i].rgb[1], cur->pal[i].rgb[2]));
        FillRect(d->hDC, &d->rcItem, b);
        DeleteObject(b);
        FrameRect(d->hDC, &d->rcItem, (HBRUSH)GetStockObject(GRAY_BRUSH));
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_LOGLINE:
        log_append((wchar_t *)lp);
        free((wchar_t *)lp);
        return 0;
    case WM_ANALYZED: {
        InterlockedExchange(&busy, 0);
        cur = (analysis *)lp;
        if (cur) {
            take_addons(cur);
            set_summary(cur->summary);
            show_palettes();
            if (!GetWindowTextLengthW(h_name)) SetFocus(h_name);
            snapshot(".0.bmp");
            autofill();
        } else if (wp) {
            set_summary("The game folder is needed to read the suit's original files.");
            wchar_t *g = ask_game_folder();
            if (g && pending_path) { wchar_t *p = _wcsdup(pending_path); start_analysis(p, g); free(p); }
            free(g);
        } else {
            set_summary("This pak can't be patched - see the messages below.");
        }
        update_buttons();
        return 0;
    }
    case WM_PATCHED: {
        InterlockedExchange(&busy, 0);
        update_buttons();
        if (wp) {
            patch_result *r = (patch_result *)lp;
            take_addons(cur);
            wchar_t msg[2048];
            const wchar_t *fn = wcsrchr(r->out_path, L'\\');
            const wchar_t *bn = wcsrchr(r->backup_path, L'\\');
            swprintf(msg, 2048, L"Done! %ls is now a suit of its own.\n\nThe original mod is kept as %ls%ls (the game ignores "
                                L"it).\n\nStart the game: the SuitRegistry registers the new suit by itself.%ls",
                     fn ? fn + 1 : r->out_path, bn ? bn + 1 : r->backup_path,
                     cur->naddons ? L", and the add-ons as .bak files too" : L"",
                     registry_installed(cur->paks_dir) ? L""
                     : L"\n\nNote: the SuitRegistry (version.dll) doesn't seem to be installed in this game. Install it, "
                       L"or the new suit won't show up.");
            snapshot(".2.bmp");
            if (GetEnvironmentVariableW(L"SUITPATCHER_AUTOFILL", NULL, 0)) { PostQuitMessage(0); return 0; }
            MessageBoxW(h, msg, L"Suit patched", MB_OK | MB_ICONINFORMATION);
        } else {
            MessageBoxW(h, L"Patching failed - see the messages at the bottom of the window. Nothing was changed.",
                        L"Suit Patcher", MB_OK | MB_ICONWARNING);
        }
        return 0;
    }
    case WM_CLOSE:
        if (busy && MessageBoxW(h, L"Still working. Quit anyway?", L"Suit Patcher", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev;
    sr_self = inst;
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HMODULE user = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *dpi_t)(void);
    dpi_t aware = user ? (dpi_t)GetProcAddress(user, "SetProcessDPIAware") : NULL;
    if (aware) aware();
    HDC dc = GetDC(NULL);
    scale = GetDeviceCaps(dc, LOGPIXELSY) / 96.0;
    ReleaseDC(NULL, dc);
    if (scale < 1.0) scale = 1.0;
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -S(13);
    font = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(12);
    font_small = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(20);
    lf.lfWeight = FW_BOLD;
    font_title = CreateFontIndirectW(&lf);
    log_open();
    log_hook = gui_log;

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"MidnightSunsSuitPatcher";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = {0, 0, S(640), S(715)};
    AdjustWindowRect(&rc, style, FALSE);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Midnight Suns Suit Patcher", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, inst, NULL);
    ShowWindow(h, show);
    UpdateWindow(h);
    log_msg("Suit Patcher %s", PATCHER_VERSION);
    // a pak given on the command line (or dropped on the exe)
    if (cmd && *cmd) {
        wchar_t *p = _wcsdup(cmd);
        size_t n = wcslen(p);
        if (p[0] == L'"') { memmove(p, p + 1, n * sizeof(wchar_t)); n--; if (n && p[n - 1] == L'"') p[--n] = 0; }
        if (*p) new_mod(p);
        free(p);
    }
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
