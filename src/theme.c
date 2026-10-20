/* theme.c - palette, font loading and text drawing helpers.
 *
 * Sections: font table (role -> file and size), palette (dark theme
 * colours), font loading (asset lookup with default-font fallback) and
 * colour/text helpers. Declared in theme.h; the Theme built here is owned
 * by Dashboard and read by ui.c, charts.c, panels.c and pages.
 */
#include "theme.h"

#include <math.h>
#include <string.h>

/* ---- Font table ---- */

/* Font file and pixel size of one role. */
typedef struct {
    const char *file;
    float px;
} FontSpec;

/* Indexed by FontId (sizes mirror the comments in theme.h). Inter is used
 * for text, JetBrains Mono for numbers: its digits are tabular, so values
 * that update every tick do not shift their neighbours. */
static const FontSpec FONT_SPECS[FONT_COUNT] = {
    [FONT_CAPTION]    = { "Inter-Regular.ttf",        11.0f },
    [FONT_CAPTION_BOLD] = { "Inter-SemiBold.ttf",     11.0f },
    [FONT_SMALL]      = { "Inter-Regular.ttf",        12.0f },
    [FONT_BODY]       = { "Inter-Regular.ttf",        13.0f },
    [FONT_BODY_BOLD]  = { "Inter-SemiBold.ttf",       13.0f },
    [FONT_HEADING]    = { "Inter-SemiBold.ttf",       15.0f },
    [FONT_TITLE]      = { "Inter-SemiBold.ttf",       22.0f },
    [FONT_MONO_SMALL] = { "JetBrainsMono-Medium.ttf", 12.0f },
    [FONT_MONO]       = { "JetBrainsMono-Medium.ttf", 13.0f },
    [FONT_MONO_BOLD]  = { "JetBrainsMono-Bold.ttf",   15.0f },
    [FONT_MONO_LARGE] = { "JetBrainsMono-Bold.ttf",   30.0f },
};

/* ---- Palette ---- */

/* Converts 0xRRGGBB to an opaque Color. */
static Color rgb(unsigned hex)
{
    return (Color){ (unsigned char)(hex >> 16), (unsigned char)(hex >> 8), (unsigned char)hex, 255 };
}

/* Builds the dark palette. Surfaces step up in brightness with elevation
 * (bg -> sidebar -> panel -> hover -> raised); status colours (ok, warn,
 * crit) are saturated so they stand out on every surface level. */
static Palette make_palette(void)
{
    return (Palette){
        .bg            = rgb(0x0A0D13),
        .sidebar       = rgb(0x0D1119),
        .panel         = rgb(0x121722),
        .panel_hover   = rgb(0x161C29),
        .panel_raised  = rgb(0x1A2130),
        .border        = rgb(0x1F2736),
        .border_strong = rgb(0x2D3850),
        .grid          = rgb(0x1A2130),
        .text          = rgb(0xE8ECF4),
        .text_dim      = rgb(0x9BA5BA),
        .text_faint    = rgb(0x646F88),
        .accent        = rgb(0x5B8CFF),
        .ok            = rgb(0x34D399),
        .warn          = rgb(0xF5B83D),
        .crit          = rgb(0xF2555F),
        .series_in     = rgb(0x38BDF8),
        .series_out    = rgb(0xB08CFF),
        .tooltip_bg    = rgb(0x1B2232),
    };
}

/* ---- Font loading ---- */

/* Probes the usual asset locations relative to the executable and the CWD.
 * Order: next to the executable (CMake copies assets there), one and two
 * directories above it (Makefile builds into build/), then the working
 * directory. Writes the first existing path into out (size cap) and
 * returns true; returns false if the file is nowhere to be found. */
static bool find_font(const char *name, char *out, size_t cap)
{
    const char *app = GetApplicationDirectory();
    const char *patterns[] = {
        "%sassets/fonts/%s",
        "%s../assets/fonts/%s",
        "%s../../assets/fonts/%s",
    };
    for (size_t i = 0; i < sizeof patterns / sizeof patterns[0]; i++) {
        const char *p = TextFormat(patterns[i], app, name);
        if (FileExists(p)) {
            strncpy(out, p, cap - 1);
            out[cap - 1] = '\0';
            return true;
        }
    }
    const char *cwd_patterns[] = { "assets/fonts/%s", "../assets/fonts/%s" };
    for (size_t i = 0; i < sizeof cwd_patterns / sizeof cwd_patterns[0]; i++) {
        const char *p = TextFormat(cwd_patterns[i], name);
        if (FileExists(p)) {
            strncpy(out, p, cap - 1);
            out[cap - 1] = '\0';
            return true;
        }
    }
    return false;
}

/* Builds the palette and loads every font role. If any font file is
 * missing, all roles switch to the raylib default font so the set stays
 * consistent. Needs a GL context (fonts become textures). */
void theme_load(Theme *t)
{
    memset(t, 0, sizeof *t);
    t->c = make_palette();

    /* Basic Latin plus Latin-1 supplement (degree sign, micro sign, middle dot). */
    int codepoints[224];
    for (int i = 0; i < 224; i++)
        codepoints[i] = 32 + i;

    /* 1. Load each role at its exact pixel size; stop at the first failure. */
    bool all_loaded = true;
    for (int i = 0; i < FONT_COUNT; i++) {
        char path[512];
        t->font_px[i] = FONT_SPECS[i].px;
        if (find_font(FONT_SPECS[i].file, path, sizeof path)) {
            t->fonts[i] = LoadFontEx(path, (int)FONT_SPECS[i].px, codepoints, 224);
            SetTextureFilter(t->fonts[i].texture, TEXTURE_FILTER_BILINEAR);
            t->spacing[i] = 0.0f;
        } else {
            all_loaded = false;
            break;
        }
    }

    /* 2. Fallback: release what was loaded and use the default font for all
     * roles, with 1 px glyph spacing to keep the bitmap font legible.
     * NOTE: font_px was only filled up to the failed role, so later roles
     * keep size 0 on this path. */
    if (!all_loaded) {
        TraceLog(LOG_WARNING, "THEME: font assets not found, using default font");
        for (int i = 0; i < FONT_COUNT; i++) {
            if (t->fonts[i].texture.id != 0)
                UnloadFont(t->fonts[i]);
            t->fonts[i] = GetFontDefault();
            t->spacing[i] = 1.0f;
        }
        t->custom_fonts = false;
    } else {
        t->custom_fonts = true;
    }
}

/* Unloads the font textures. The default font is owned by raylib and is
 * not released (custom_fonts is false then). */
void theme_unload(Theme *t)
{
    if (!t->custom_fonts)
        return;
    for (int i = 0; i < FONT_COUNT; i++)
        UnloadFont(t->fonts[i]);
    t->custom_fonts = false;
}

/* ---- Colour and text helpers ---- */

/* Multiplies the alpha of c by a (clamped to 0..1); +0.5 rounds to nearest. */
Color theme_alpha(Color c, float a)
{
    float v = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    c.a = (unsigned char)((float)c.a * v + 0.5f);
    return c;
}

/* Linear interpolation a -> b in RGBA, t clamped to 0..1. Blends in gamma
 * space, which is adequate for the small steps used by hover effects. */
Color theme_mix(Color a, Color b, float t)
{
    float k = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return (Color){
        (unsigned char)((float)a.r + ((float)b.r - (float)a.r) * k + 0.5f),
        (unsigned char)((float)a.g + ((float)b.g - (float)a.g) * k + 0.5f),
        (unsigned char)((float)a.b + ((float)b.b - (float)a.b) * k + 0.5f),
        (unsigned char)((float)a.a + ((float)b.a - (float)a.a) * k + 0.5f),
    };
}

/* Width and height in px of text drawn with font f. */
Vector2 theme_measure(const Theme *t, FontId f, const char *text)
{
    return MeasureTextEx(t->fonts[f], text, t->font_px[f], t->spacing[f]);
}

/* Draws text with its top-left corner at (x, y). The origin is rounded to
 * whole pixels: a fractional origin would resample the glyph bitmaps and
 * blur them. */
void theme_text(const Theme *t, FontId f, const char *text, float x, float y, Color c)
{
    Vector2 pos = { floorf(x + 0.5f), floorf(y + 0.5f) };
    DrawTextEx(t->fonts[f], text, pos, t->font_px[f], t->spacing[f], c);
}

/* Draws text so that its right edge is at `right`. */
void theme_text_right(const Theme *t, FontId f, const char *text, float right, float y, Color c)
{
    theme_text(t, f, text, right - theme_measure(t, f, text).x, y, c);
}

/* Draws text horizontally centred on cx. */
void theme_text_center(const Theme *t, FontId f, const char *text, float cx, float y, Color c)
{
    theme_text(t, f, text, cx - theme_measure(t, f, text).x * 0.5f, y, c);
}

/* Draws text at (x, y); if it is wider than max_w, cuts it and appends
 * "...". Truncation is byte-wise (all UI strings are ASCII) and works on a
 * local 128-byte copy, so on this path input is cut at 124 characters at most. */
void theme_text_fit(const Theme *t, FontId f, const char *text, float x, float y, float max_w, Color c)
{
    if (theme_measure(t, f, text).x <= max_w) {
        theme_text(t, f, text, x, y, c);
        return;
    }

    /* 1. Copy, leaving room for the ellipsis and the terminator. */
    char buf[128];
    size_t len = strlen(text);
    if (len >= sizeof buf - 4)
        len = sizeof buf - 4;
    memcpy(buf, text, len);

    /* 2. Drop one character at a time until text + "..." fits. */
    while (len > 0) {
        len--;
        memcpy(buf + len, "...", 4);
        if (theme_measure(t, f, buf).x <= max_w)
            break;
    }
    theme_text(t, f, buf, x, y, c);
}
