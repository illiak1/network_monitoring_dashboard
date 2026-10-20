/* theme.h - visual identity: palette, spacing scale, fonts and text helpers.
 *
 * theme.c implements it. One Theme instance lives in Dashboard and is
 * referenced (const) by Ui, so every widget in ui.c draws with the
 * same colours and metrics. Text is always drawn through the theme_text*
 * helpers so glyph placement stays pixel-aligned.
 */
#ifndef THEME_H
#define THEME_H

#include <stdbool.h>
#include "raylib.h"

/* Spacing scale (px), multiples of a 4 px grid. */
enum {
    SP_XS = 4,
    SP_SM = 8,
    SP_MD = 12,
    SP_LG = 16,   /* gap between panels */
    SP_XL = 24
};

/* Corner radii (px). SM: chips, buttons, rows; MD: panels and cards. */
enum {
    RADIUS_SM = 6,
    RADIUS_MD = 10
};

/* Font roles. Each role maps to a font file and pixel size in theme.c
 * (FONT_SPECS); sizes are rasterised at exactly that size, never scaled. */
typedef enum {
    FONT_CAPTION,      /* 11 px, labels and axis ticks   */
    FONT_CAPTION_BOLD, /* 11 px semibold, badges         */
    FONT_SMALL,        /* 12 px, secondary text          */
    FONT_BODY,         /* 13 px, default text            */
    FONT_BODY_BOLD,    /* 13 px semibold                 */
    FONT_HEADING,      /* 15 px semibold, panel titles   */
    FONT_TITLE,        /* 22 px semibold, page titles    */
    FONT_MONO_SMALL,   /* 12 px, tabular numerals        */
    FONT_MONO,         /* 13 px, tabular numerals        */
    FONT_MONO_BOLD,    /* 15 px bold, header statistics  */
    FONT_MONO_LARGE,   /* 30 px bold, gauge readouts     */
    FONT_COUNT         /* number of roles; array size    */
} FontId;

/* Colour tokens of the dark theme. Surfaces get lighter in the order
 * bg < sidebar < panel < panel_hover < panel_raised. */
typedef struct {
    Color bg;            /* window background */
    Color sidebar;
    Color panel;         /* card background */
    Color panel_hover;   /* card background under the cursor */
    Color panel_raised;  /* controls on a card: chips, buttons, thumbs */
    Color border;
    Color border_strong; /* hover and emphasised outlines */
    Color grid;          /* chart gridlines */
    Color text;          /* primary text */
    Color text_dim;      /* secondary text */
    Color text_faint;    /* tertiary text: axis ticks, hints (~3.6:1 contrast on panel) */
    Color accent;        /* selection, links, primary button */
    Color ok;            /* status colours: up / warning / down, also gauge zones */
    Color warn;
    Color crit;
    Color series_in;     /* inbound traffic */
    Color series_out;    /* outbound traffic */
    Color tooltip_bg;
} Palette;

/* Palette plus loaded fonts. */
typedef struct {
    Palette c;
    Font fonts[FONT_COUNT];
    float font_px[FONT_COUNT];   /* nominal size in px, used for vertical centring */
    float spacing[FONT_COUNT];   /* extra glyph spacing: 0 for TTF fonts, 1 for the default font */
    bool custom_fonts;           /* false when running on the raylib default font */
} Theme;

/* Loads palette and fonts; falls back to the raylib default font if assets
 * are missing. Requires an initialized window (GL context). */
void theme_load(Theme *t);
/* Releases the font textures. Safe to call when the fallback font is used. */
void theme_unload(Theme *t);

/* Returns c with its alpha multiplied by a (clamped to 0..1). */
Color theme_alpha(Color c, float a);
/* Linear blend a -> b per channel including alpha; t is clamped to 0..1. */
Color theme_mix(Color a, Color b, float t);

/* Text helpers. Positions are snapped to whole pixels to keep glyphs crisp. */
/* Size of text in px when drawn with font f. */
Vector2 theme_measure(const Theme *t, FontId f, const char *text);
/* Left-aligned at x; y is the top of the line box. */
void theme_text(const Theme *t, FontId f, const char *text, float x, float y, Color c);
/* Right edge of the text at `right`. */
void theme_text_right(const Theme *t, FontId f, const char *text, float right, float y, Color c);
/* Horizontal centre of the text at cx. */
void theme_text_center(const Theme *t, FontId f, const char *text, float cx, float y, Color c);
/* Draws text truncated with an ellipsis to fit max_w. */
void theme_text_fit(const Theme *t, FontId f, const char *text, float x, float y, float max_w, Color c);

#endif
