/* ui.h - immediate-mode UI toolkit.
 *
 * Widgets are plain function calls made every frame: they draw and return
 * the interaction result, with no retained widget tree. State that must
 * persist (hover and selection animations) lives in hashed
 * "slots" inside Ui, addressed by a 32-bit key. Parts: input snapshot and
 * frame lifecycle, slots, clip stack, rect-cutting layout, primitives, the
 * panel card and the tooltip. Implemented in ui.c on top of theme.h;
 * dashboard.c draws through it.
 */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>
#include "raylib.h"
#include "theme.h"

#define UI_SLOT_COUNT 2048   /* animation slots; must be a power of two (index mask) */
#define UI_CLIP_DEPTH 4      /* nested scissor regions */
#define UI_TIP_ROWS 6       /* rows per tooltip */

#define UI_PANEL_HEADER_H 46 /* px from panel top to content: title band */
#define UI_PANEL_PAD 16      /* inner padding of a panel, px */

/* Persistent per-widget animation value, addressed by a 32-bit key. */
typedef struct {
    uint32_t key;          /* 0 = unused slot */
    uint32_t last_frame;   /* frame of last access, for eviction */
    float v;
} UiSlot;

typedef struct {
    bool has_swatch;
    Color swatch;          /* series colour dot before the label */
    char label[32];
    char value[24];
} UiTipRow;

/* Tooltip content, rebuilt every frame by ui_tip_begin/ui_tip_row. */
typedef struct {
    bool active;
    char title[32];
    int rows;
    UiTipRow row[UI_TIP_ROWS];
} UiTooltip;

/* Per-frame input snapshot plus persistent UI state. */
typedef struct {
    const Theme *theme;

    Vector2 mouse;       /* far off-screen when the cursor is outside the window */
    bool pressed;        /* left button went down this frame */
    float dt;            /* frame time in s, capped at 50 ms */
    double time;
    uint32_t frame;      /* frame counter, drives slot eviction */

    Rectangle clip[UI_CLIP_DEPTH];  /* effective (intersected) clip rects */
    int clip_depth;

    UiSlot slots[UI_SLOT_COUNT];

    bool cursor_hand;           /* a widget asked for the pointer cursor this frame */
    bool cursor_hand_applied;   /* cursor shape last sent to raylib */
    UiTooltip tip;
    float tip_alpha;     /* tooltip fade, 0..1 */
} Ui;

/* ---- Lifecycle ---- */

/* Clears all state and binds the theme. */
void ui_init(Ui *ui, const Theme *theme);
/* Starts a frame: samples input, resets clip stack, cursor request, tooltip. */
void ui_begin(Ui *ui);
/* Draws the pending tooltip and applies the cursor shape. Call last. */
void ui_end(Ui *ui);

/* ---- Keys and slots ---- */

/* Hashes a string to a non-zero widget key. */
uint32_t ui_key(const char *s);
/* Derives a sub-key from base and an index (e.g. per row or per state). */
uint32_t ui_key_i(uint32_t base, uint32_t index);

/* True if the mouse is over r and inside the active clip region. */
bool ui_hover(const Ui *ui, Rectangle r);
/* Like ui_hover, and requests the pointer cursor. */
bool ui_hot(Ui *ui, Rectangle r);

/* Returns the float stored for key, creating it (zeroed) if needed;
 * *fresh (optional) tells whether it was just created. */
float *ui_slot(Ui *ui, uint32_t key, bool *fresh);
/* Smooths a stored value toward target; the first call snaps to target. */
float ui_anim(Ui *ui, uint32_t key, float target, float rate);

/* ---- Clipping and layout ---- */

/* Pushes a scissor rectangle, intersected with the current one. */
void ui_clip_push(Ui *ui, Rectangle r);
/* Restores the previous scissor rectangle. */
void ui_clip_pop(Ui *ui);

/* Rect cutting layout: carves a strip off r and shrinks r accordingly. */
Rectangle ui_cut_left(Rectangle *r, float w);
Rectangle ui_cut_right(Rectangle *r, float w);
Rectangle ui_cut_top(Rectangle *r, float h);
Rectangle ui_cut_bottom(Rectangle *r, float h);
/* r inset by p on every side. */
Rectangle ui_pad(Rectangle r, float p);

/* ---- Primitives ---- */

/* Filled / outlined rectangle; radius is the corner radius in px. */
void ui_rect(Rectangle r, float radius, Color c);
void ui_rect_outline(Rectangle r, float radius, float thickness, Color c);

/* ---- Widgets ---- */

/* Draws a card and its title. Returns the content area below the header. */
Rectangle ui_panel(Ui *ui, Rectangle r, const char *title, uint32_t key);
/* Header band of a panel (inside the side padding). Callers cut controls
 * off its right end; the title occupies the left. */
Rectangle ui_panel_header(Rectangle panel);

/* ---- Tooltip ---- */

/* Starts the tooltip for this frame; rows beyond UI_TIP_ROWS are dropped. */
void ui_tip_begin(Ui *ui, const char *title);
/* Adds a label/value row; swatch (optional) draws a colour dot. */
void ui_tip_row(Ui *ui, const Color *swatch, const char *label, const char *value);

#endif
