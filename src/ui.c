/* ui.c - immediate-mode UI toolkit implementation.
 *
 * Sections: Lifecycle (per-frame input snapshot, tooltip drawing), Keys
 * and slots (hashed animation storage with LRU eviction), Clipping and
 * layout (scissor stack, rect cutting), Primitives, Widgets (panel card)
 * and Tooltip API.
 * Declared in ui.h; the widgets draw with the Theme bound in ui_init and
 * are called from dashboard.c. Interaction is resolved in the same call
 * that draws, using the input captured by ui_begin.
 */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "anim.h"

/* Slots probed per lookup. A small window bounds the cost of a lookup and
 * limits which entries a new key may evict. */
#define SLOT_PROBE 8

/* -------------------------------------------------------- Lifecycle */

/* Clears the whole state (including all animation slots) and binds the theme. */
void ui_init(Ui *ui, const Theme *theme)
{
    memset(ui, 0, sizeof *ui);
    ui->theme = theme;
}

/* Starts a frame: samples time and input and resets per-frame state.
 * dt is capped at 50 ms so a stall (window drag, debugger) cannot make
 * animations jump or run the simulation far ahead. When the cursor is
 * outside the window the mouse is parked far away, so hover tests fail
 * (raylib keeps reporting the last position inside the window). */
void ui_begin(Ui *ui)
{
    ui->dt = fminf(GetFrameTime(), 0.05f);
    ui->time = GetTime();
    ui->frame++;

    ui->mouse = IsCursorOnScreen() ? GetMousePosition() : (Vector2){ -10000.0f, -10000.0f };
    ui->pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

    ui->clip_depth = 0;
    ui->cursor_hand = false;
    ui->tip.active = false;
}

/* Draws the tooltip requested this frame (see ui_tip_begin) next to the
 * cursor. Called from ui_end so it is drawn above everything else. */
static void draw_tooltip(Ui *ui)
{
    const Theme *t = ui->theme;
    const Palette *c = &t->c;
    const UiTooltip *tip = &ui->tip;

    /* 1. Fade in while a tip is requested; reset when none so the next
     * one fades in again. Rate 22 is a 45 ms time constant. */
    if (!tip->active) {
        ui->tip_alpha = 0.0f;
        return;
    }
    ui->tip_alpha = anim_approach(ui->tip_alpha, 1.0f, 22.0f, ui->dt);
    float a = ui->tip_alpha;

    /* 2. Size from content: widest of title and rows (label + gap + value,
     * plus the swatch column), height from the row count. 18 is the title
     * line, row_h the pitch of the rows. */
    const float pad = 10.0f, row_h = 18.0f, gap = 18.0f;
    float w = theme_measure(t, FONT_BODY_BOLD, tip->title).x;
    for (int i = 0; i < tip->rows; i++) {
        float rw = theme_measure(t, FONT_SMALL, tip->row[i].label).x + gap
                 + theme_measure(t, FONT_MONO_SMALL, tip->row[i].value).x
                 + (tip->row[i].has_swatch ? 16.0f : 0.0f);
        if (rw > w)
            w = rw;
    }
    float h = pad * 2.0f + 18.0f + (float)tip->rows * row_h;
    w += pad * 2.0f;

    /* 3. Place below-right of the cursor (rising 4 px while fading in);
     * flip to the other side of the cursor when it would leave the window
     * (8 px margin). Whole pixels keep the text sharp. */
    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    Rectangle r = { ui->mouse.x + 16.0f, ui->mouse.y + 18.0f + (1.0f - a) * 4.0f, w, h };
    if (r.x + r.width > sw - 8.0f)
        r.x = ui->mouse.x - 16.0f - r.width;
    if (r.y + r.height > sh - 8.0f)
        r.y = ui->mouse.y - 18.0f - r.height;
    r.x = floorf(r.x);
    r.y = floorf(r.y);

    /* 4. Soft shadow (offset down), body and outline. */
    ui_rect((Rectangle){ r.x - 1.0f, r.y + 3.0f, r.width + 2.0f, r.height + 2.0f }, RADIUS_SM + 2, theme_alpha(BLACK, 0.35f * a));
    ui_rect(r, RADIUS_SM, theme_alpha(c->tooltip_bg, a));
    ui_rect_outline(r, RADIUS_SM, 1.0f, theme_alpha(c->border_strong, a));

    /* 5. Title, then rows: optional swatch + label left, value right-aligned. */
    float y = r.y + pad;
    theme_text(t, FONT_BODY_BOLD, tip->title, r.x + pad, y, theme_alpha(c->text, a));
    y += 22.0f;
    for (int i = 0; i < tip->rows; i++) {
        float x = r.x + pad;
        if (tip->row[i].has_swatch) {
            DrawCircleV((Vector2){ x + 4.0f, y + 8.0f }, 3.5f, theme_alpha(tip->row[i].swatch, a));
            x += 16.0f;
        }
        theme_text(t, FONT_SMALL, tip->row[i].label, x, y, theme_alpha(c->text_dim, a));
        theme_text_right(t, FONT_MONO_SMALL, tip->row[i].value, r.x + r.width - pad, y, theme_alpha(c->text, a));
        y += row_h;
    }
}

/* Ends the frame: draws the tooltip on top and updates the mouse cursor.
 * The cursor is only set when the requested shape changed. */
void ui_end(Ui *ui)
{
    draw_tooltip(ui);
    if (ui->cursor_hand != ui->cursor_hand_applied) {
        SetMouseCursor(ui->cursor_hand ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT);
        ui->cursor_hand_applied = ui->cursor_hand;
    }
}

/* ------------------------------------------------------ Keys, slots */

/* 32-bit FNV-1a hash of a string. Key 0 marks an empty slot, so a zero
 * hash is remapped to 1. */
uint32_t ui_key(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1u;
}

/* Derives a key from base and an index (hash_combine-style mix with the
 * golden-ratio constant, then a multiply and shift to spread the bits).
 * Used for per-row and per-state sub-keys; never returns 0. */
uint32_t ui_key_i(uint32_t base, uint32_t index)
{
    uint32_t h = (base ^ (index + 0x9E3779B9u + (base << 6) + (base >> 2))) * 16777619u;
    h ^= h >> 15;
    return h ? h : 1u;
}

/* Open addressing with a short probe window. When the window is full, the
 * least recently touched slot is evicted, so live widgets keep their state.
 * Returns a pointer to the slot's float; *fresh (optional) is set to true
 * when the slot was just created (value reset to 0) so callers can start
 * an animation at its target. Touching a slot refreshes its last_frame. */
float *ui_slot(Ui *ui, uint32_t key, bool *fresh)
{
    uint32_t start = key & (UI_SLOT_COUNT - 1);
    UiSlot *victim = NULL;

    /* 1. Scan the window: hit returns, an empty slot ends the scan, and the
     * oldest occupied slot is remembered as the eviction candidate. */
    for (int i = 0; i < SLOT_PROBE; i++) {
        UiSlot *s = &ui->slots[(start + (uint32_t)i) & (UI_SLOT_COUNT - 1)];
        if (s->key == key) {
            s->last_frame = ui->frame;
            if (fresh)
                *fresh = false;
            return &s->v;
        }
        if (s->key == 0) {
            victim = s;
            break;
        }
        if (!victim || s->last_frame < victim->last_frame)
            victim = s;
    }

    /* 2. Miss: claim the empty or least recently used slot. */
    victim->key = key;
    victim->last_frame = ui->frame;
    victim->v = 0.0f;
    if (fresh)
        *fresh = true;
    return &victim->v;
}

/* Eases the value stored under key toward target at the given rate (1/s,
 * see anim_approach) and returns it. The first call returns target, so
 * newly shown widgets do not animate in from 0. */
float ui_anim(Ui *ui, uint32_t key, float target, float rate)
{
    bool fresh;
    float *v = ui_slot(ui, key, &fresh);
    *v = fresh ? target : anim_approach(*v, target, rate, ui->dt);
    return *v;
}

/* ------------------------------------------------- Clipping, layout */

/* Intersection of two rectangles; zero-sized when they do not overlap. */
static Rectangle rect_intersect(Rectangle a, Rectangle b)
{
    float x0 = fmaxf(a.x, b.x), y0 = fmaxf(a.y, b.y);
    float x1 = fminf(a.x + a.width, b.x + b.width);
    float y1 = fminf(a.y + a.height, b.y + b.height);
    return (Rectangle){ x0, y0, fmaxf(0.0f, x1 - x0), fmaxf(0.0f, y1 - y0) };
}

/* Sets the GPU scissor rectangle (raylib needs integer pixels). */
static void apply_scissor(Rectangle r)
{
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
}

/* Pushes a clip region. Raylib's scissor does not nest (a new call
 * replaces the old rectangle), so the stack stores the intersection with
 * the parent. A push beyond UI_CLIP_DEPTH is ignored; ui_clip_pop then
 * unwinds one level too many for such a push, so keep nesting shallow. */
void ui_clip_push(Ui *ui, Rectangle r)
{
    if (ui->clip_depth >= UI_CLIP_DEPTH)
        return;
    if (ui->clip_depth > 0)
        r = rect_intersect(r, ui->clip[ui->clip_depth - 1]);
    ui->clip[ui->clip_depth++] = r;
    apply_scissor(r);
}

/* Pops the clip region and restores the parent scissor, or leaves
 * scissor mode when the stack becomes empty. */
void ui_clip_pop(Ui *ui)
{
    if (ui->clip_depth == 0)
        return;
    ui->clip_depth--;
    if (ui->clip_depth > 0)
        apply_scissor(ui->clip[ui->clip_depth - 1]);
    else
        EndScissorMode();
}

/* True when the mouse is inside r and also inside the innermost clip
 * region, so clipped-away content cannot be hovered. */
bool ui_hover(const Ui *ui, Rectangle r)
{
    if (!CheckCollisionPointRec(ui->mouse, r))
        return false;
    return ui->clip_depth == 0 || CheckCollisionPointRec(ui->mouse, ui->clip[ui->clip_depth - 1]);
}

/* ui_hover plus a request for the pointer cursor (applied in ui_end). */
bool ui_hot(Ui *ui, Rectangle r)
{
    bool h = ui_hover(ui, r);
    if (h)
        ui->cursor_hand = true;
    return h;
}

/* Rect cutting: each function returns a strip of the given size from one
 * side of *r and shrinks *r by it. Cutting in sequence splits an area
 * without manual coordinate arithmetic. */
Rectangle ui_cut_left(Rectangle *r, float w)
{
    Rectangle out = { r->x, r->y, w, r->height };
    r->x += w;
    r->width -= w;
    return out;
}

Rectangle ui_cut_right(Rectangle *r, float w)
{
    Rectangle out = { r->x + r->width - w, r->y, w, r->height };
    r->width -= w;
    return out;
}

Rectangle ui_cut_top(Rectangle *r, float h)
{
    Rectangle out = { r->x, r->y, r->width, h };
    r->y += h;
    r->height -= h;
    return out;
}

Rectangle ui_cut_bottom(Rectangle *r, float h)
{
    Rectangle out = { r->x, r->y + r->height - h, r->width, h };
    r->height -= h;
    return out;
}

/* r shrunk by p on every side; the size never goes below 0. */
Rectangle ui_pad(Rectangle r, float p)
{
    return (Rectangle){ r.x + p, r.y + p, fmaxf(0.0f, r.width - 2.0f * p), fmaxf(0.0f, r.height - 2.0f * p) };
}

/* -------------------------------------------------------- Primitives */

/* Converts a corner radius in px to raylib's "roundness" (0..1 of the
 * shorter side; radius = roundness * min(w, h) / 2). */
static float roundness_for(Rectangle r, float radius)
{
    float m = fminf(r.width, r.height);
    return m > 0.0f ? fminf(1.0f, 2.0f * radius / m) : 0.0f;
}

/* Filled rectangle; radius <= 0 draws square corners. */
void ui_rect(Rectangle r, float radius, Color c)
{
    if (radius <= 0.0f)
        DrawRectangleRec(r, c);
    else
        DrawRectangleRounded(r, roundness_for(r, radius), 10, c);
}

/* Outlined rectangle with the given line thickness (px). */
void ui_rect_outline(Rectangle r, float radius, float thickness, Color c)
{
    if (radius <= 0.0f)
        DrawRectangleLinesEx(r, thickness, c);
    else
        DrawRectangleRoundedLinesEx(r, roundness_for(r, radius), 10, thickness, c);
}

/* ----------------------------------------------------------- Widgets */

/* Header band of a panel: inside the side padding, UI_PANEL_HEADER_H tall.
 * The title is drawn at its left by ui_panel; callers place controls in
 * the remaining space (usually cut from the right). */
Rectangle ui_panel_header(Rectangle panel)
{
    return (Rectangle){ panel.x + UI_PANEL_PAD, panel.y, panel.width - 2.0f * UI_PANEL_PAD, UI_PANEL_HEADER_H };
}

/* Draws a card with an optional title and returns its content area.
 * key identifies the hover animation slot (background and border lift
 * toward the hover colours at rate 14). Without a title the content area
 * is just the padded rectangle. */
Rectangle ui_panel(Ui *ui, Rectangle r, const char *title, uint32_t key)
{
    const Palette *c = &ui->theme->c;
    float h = ui_anim(ui, key, ui_hover(ui, r) ? 1.0f : 0.0f, 14.0f);

    ui_rect(r, RADIUS_MD, theme_mix(c->panel, c->panel_hover, h));
    ui_rect_outline(r, RADIUS_MD, 1.0f, theme_mix(c->border, c->border_strong, h));

    Rectangle content = ui_pad(r, UI_PANEL_PAD);
    if (title) {
        theme_text(ui->theme, FONT_HEADING, title, r.x + UI_PANEL_PAD, r.y + 15.0f, c->text);
        /* Content starts at UI_PANEL_HEADER_H from the panel top (pad + cut). */
        ui_cut_top(&content, UI_PANEL_HEADER_H - UI_PANEL_PAD);
    }
    return content;
}

/* ----------------------------------------------------------- Tooltip */

/* Starts (or replaces) this frame's tooltip. The tooltip is drawn later by
 * ui_end, near the cursor. Title is truncated to the buffer. */
void ui_tip_begin(Ui *ui, const char *title)
{
    ui->tip.active = true;
    ui->tip.rows = 0;
    snprintf(ui->tip.title, sizeof ui->tip.title, "%s", title);
}

/* Appends a label/value row; swatch (may be NULL) adds a colour dot.
 * Ignored when no tooltip is active or UI_TIP_ROWS is reached. */
void ui_tip_row(Ui *ui, const Color *swatch, const char *label, const char *value)
{
    if (!ui->tip.active || ui->tip.rows >= UI_TIP_ROWS)
        return;
    UiTipRow *row = &ui->tip.row[ui->tip.rows++];
    row->has_swatch = swatch != NULL;
    if (swatch)
        row->swatch = *swatch;
    snprintf(row->label, sizeof row->label, "%s", label);
    snprintf(row->value, sizeof row->value, "%s", value);
}
