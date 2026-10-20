/* ui.c - immediate-mode UI toolkit implementation.
 *
 * Sections: Lifecycle (per-frame input snapshot, tooltip drawing), Keys
 * and slots (hashed animation storage with LRU eviction), Clipping and
 * layout (scissor stack, rect cutting), Primitives, Widgets (panel, chip,
 * segmented control, button, badge, icons), Scroll and Tooltip API.
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
#define SCROLL_STEP 56.0f   /* px scrolled per wheel notch */

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
    ui->down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    ui->wheel = GetMouseWheelMove();
    /* A drag capture ends with the button release. */
    if (!ui->down)
        ui->active = 0;

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
 * region, so content scrolled out of its viewport cannot be hovered. */
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

/* Top y that vertically centres a line of font f inside r. */
static float text_top(const Ui *ui, FontId f, Rectangle r)
{
    return r.y + (r.height - ui->theme->font_px[f]) * 0.5f;
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

/* Pill-shaped toggle button. Two animation slots (key, key+1): hover and
 * active. The active state tints fill, outline and text with `tint`.
 * Returns true on the frame the button is clicked. */
bool ui_chip(Ui *ui, uint32_t key, Rectangle r, const char *label, bool active, Color tint)
{
    const Palette *c = &ui->theme->c;
    bool hot = ui_hot(ui, r);
    float hv = ui_anim(ui, key, hot ? 1.0f : 0.0f, 16.0f);
    float av = ui_anim(ui, ui_key_i(key, 1), active ? 1.0f : 0.0f, 14.0f);

    Color fill = theme_mix(c->panel_raised, theme_mix(c->panel_raised, tint, 0.20f), av);
    fill = theme_mix(fill, c->text, 0.05f * hv);
    ui_rect(r, r.height * 0.5f, fill);
    ui_rect_outline(r, r.height * 0.5f, 1.0f, theme_mix(c->border, theme_alpha(tint, 0.7f), av));

    Color tc = theme_mix(theme_mix(c->text_dim, c->text, hv), tint, av);
    float tw = theme_measure(ui->theme, FONT_SMALL, label).x;
    theme_text(ui->theme, FONT_SMALL, label, r.x + (r.width - tw) * 0.5f, text_top(ui, FONT_SMALL, r), tc);
    return hot && ui->pressed;
}

/* Chip with a leading colour dot, used as a chart legend entry. The dot is
 * filled when active and shows a hollow ring when off, so the series
 * colour stays recognisable. Returns true on click. */
bool ui_legend_chip(Ui *ui, uint32_t key, Rectangle r, const char *label, Color color, bool active)
{
    const Palette *c = &ui->theme->c;
    bool hot = ui_hot(ui, r);
    float hv = ui_anim(ui, key, hot ? 1.0f : 0.0f, 16.0f);
    float av = ui_anim(ui, ui_key_i(key, 1), active ? 1.0f : 0.0f, 14.0f);

    ui_rect(r, r.height * 0.5f, theme_mix(c->panel_raised, c->border_strong, 0.5f * hv));
    ui_rect_outline(r, r.height * 0.5f, 1.0f, theme_mix(c->border, c->border_strong, hv));

    Vector2 dot = { r.x + 13.0f, r.y + r.height * 0.5f };
    DrawCircleV(dot, 4.0f, theme_alpha(color, av));
    DrawCircleLinesV(dot, 4.0f, theme_alpha(color, 0.5f + 0.5f * av));
    theme_text(ui->theme, FONT_SMALL, label, r.x + 24.0f, text_top(ui, FONT_SMALL, r),
               theme_mix(c->text_faint, theme_mix(c->text_dim, c->text, hv), av));
    return hot && ui->pressed;
}

/* Segmented control with n equal segments. A thumb slides to the active
 * segment (rate 20); slots: key for the thumb, key+1+i for hover of
 * segment i. Returns the active index, updated on click. */
int ui_segmented(Ui *ui, uint32_t key, Rectangle r, const char *const *labels, int n, int active)
{
    const Palette *c = &ui->theme->c;
    const float inset = 2.0f;
    float seg_w = (r.width - 2.0f * inset) / (float)n;

    ui_rect(r, RADIUS_SM + 1, c->bg);
    ui_rect_outline(r, RADIUS_SM + 1, 1.0f, c->border);

    float tx = ui_anim(ui, key, (float)active * seg_w, 20.0f);
    Rectangle thumb = { r.x + inset + tx, r.y + inset, seg_w, r.height - 2.0f * inset };
    ui_rect(thumb, RADIUS_SM - 1, c->panel_raised);
    ui_rect_outline(thumb, RADIUS_SM - 1, 1.0f, c->border_strong);

    int result = active;
    for (int i = 0; i < n; i++) {
        Rectangle s = { r.x + inset + seg_w * (float)i, r.y + inset, seg_w, r.height - 2.0f * inset };
        bool hot = ui_hot(ui, s);
        float hv = ui_anim(ui, ui_key_i(key, (uint32_t)i + 1), hot ? 1.0f : 0.0f, 16.0f);
        Color tc = i == active ? c->text : theme_mix(c->text_dim, c->text, hv);
        float tw = theme_measure(ui->theme, FONT_SMALL, labels[i]).x;
        theme_text(ui->theme, FONT_SMALL, labels[i], s.x + (s.width - tw) * 0.5f, text_top(ui, FONT_SMALL, s), tc);
        if (hot && ui->pressed)
            result = i;
    }
    return result;
}

/* Text button: primary uses the accent fill with dark text, secondary a
 * raised surface with an outline. Returns true on click. */
bool ui_button(Ui *ui, uint32_t key, Rectangle r, const char *label, bool primary)
{
    const Palette *c = &ui->theme->c;
    bool hot = ui_hot(ui, r);
    float hv = ui_anim(ui, key, hot ? 1.0f : 0.0f, 16.0f);

    Color fill = primary ? theme_mix(c->accent, WHITE, 0.14f * hv) : theme_mix(c->panel_raised, c->border_strong, hv);
    Color tc = primary ? c->bg : c->text;
    ui_rect(r, RADIUS_SM + 1, fill);
    if (!primary)
        ui_rect_outline(r, RADIUS_SM + 1, 1.0f, theme_mix(c->border_strong, c->text_faint, hv));

    float tw = theme_measure(ui->theme, FONT_BODY_BOLD, label).x;
    theme_text(ui->theme, FONT_BODY_BOLD, label, r.x + (r.width - tw) * 0.5f, text_top(ui, FONT_BODY_BOLD, r), tc);
    return hot && ui->pressed;
}

/* Width in px a badge needs for text: text width plus 7 px padding per side. */
float ui_badge_width(const Ui *ui, const char *text)
{
    return theme_measure(ui->theme, FONT_CAPTION_BOLD, text).x + 14.0f;
}

/* Pill with a 14%-alpha tint of `color` and the text in full colour,
 * centred in r. */
void ui_badge(Ui *ui, Rectangle r, const char *text, Color color)
{
    ui_rect(r, r.height * 0.5f, theme_alpha(color, 0.14f));
    float tw = theme_measure(ui->theme, FONT_CAPTION_BOLD, text).x;
    theme_text(ui->theme, FONT_CAPTION_BOLD, text, r.x + (r.width - tw) * 0.5f, text_top(ui, FONT_CAPTION_BOLD, r), color);
}

/* Filled dot centred at c. With pulse, a ring expands from the dot once
 * every ~1.1 s (0.9 Hz) to 2.8 x radius while fading from 35% to 0. */
void ui_status_dot(Ui *ui, Vector2 c, float radius, Color color, bool pulse)
{
    if (pulse) {
        float phase = (float)fmod(ui->time * 0.9, 1.0);
        DrawCircleV(c, radius + phase * radius * 1.8f, theme_alpha(color, 0.35f * (1.0f - phase)));
    }
    DrawCircleV(c, radius, color);
}

/* Draws an icon centred at c, fitted in a square of `size` px, as lines
 * and triangles (no textures). s is the half size; coordinates below are
 * fractions of s tuned by eye. th is the stroke width. */
void ui_icon(IconId id, Vector2 c, float size, Color color)
{
    const float th = 1.7f;
    const float s = size * 0.5f;

    switch (id) {
    case ICON_DASHBOARD: {
        /* 2 x 2 grid of outlined cells. */
        float cell = s * 0.88f, gap = s * 0.24f;
        for (int iy = 0; iy < 2; iy++) {
            for (int ix = 0; ix < 2; ix++) {
                Rectangle q = { c.x - s + (float)ix * (cell + gap) + gap * 0.5f,
                                c.y - s + (float)iy * (cell + gap) + gap * 0.5f, cell, cell };
                ui_rect_outline(q, 2.0f, th, color);
            }
        }
        break;
    }
    case ICON_DEVICES: {
        /* Monitor: screen outline, stand and base. */
        Rectangle m = { c.x - s, c.y - s * 0.85f, s * 2.0f, s * 1.35f };
        ui_rect_outline(m, 2.5f, th, color);
        DrawLineEx((Vector2){ c.x, c.y + s * 0.5f }, (Vector2){ c.x, c.y + s * 0.9f }, th, color);
        DrawLineEx((Vector2){ c.x - s * 0.5f, c.y + s * 0.92f }, (Vector2){ c.x + s * 0.5f, c.y + s * 0.92f }, th, color);
        break;
    }
    case ICON_TRAFFIC: {
        /* Zigzag line with dots on the inner vertices. */
        Vector2 p[5] = {
            { c.x - s, c.y + s * 0.35f }, { c.x - s * 0.4f, c.y - s * 0.25f },
            { c.x + s * 0.05f, c.y + s * 0.2f }, { c.x + s * 0.6f, c.y - s * 0.7f },
            { c.x + s, c.y - s * 0.15f },
        };
        for (int i = 0; i < 4; i++)
            DrawLineEx(p[i], p[i + 1], th, color);
        for (int i = 1; i < 4; i++)
            DrawCircleV(p[i], th * 0.5f, color);
        break;
    }
    case ICON_ALERTS: {
        /* Warning triangle with an exclamation mark. */
        Vector2 a = { c.x, c.y - s * 0.95f }, b = { c.x - s * 1.05f, c.y + s * 0.8f }, d = { c.x + s * 1.05f, c.y + s * 0.8f };
        DrawLineEx(a, b, th, color);
        DrawLineEx(b, d, th, color);
        DrawLineEx(d, a, th, color);
        DrawLineEx((Vector2){ c.x, c.y - s * 0.3f }, (Vector2){ c.x, c.y + s * 0.25f }, th, color);
        DrawCircleV((Vector2){ c.x, c.y + s * 0.55f }, th * 0.65f, color);
        break;
    }
    case ICON_MENU:
        /* Three horizontal bars. */
        for (int i = -1; i <= 1; i++)
            DrawLineEx((Vector2){ c.x - s * 0.9f, c.y + (float)i * s * 0.6f }, (Vector2){ c.x + s * 0.9f, c.y + (float)i * s * 0.6f }, th, color);
        break;
    case ICON_PAUSE:
        ui_rect((Rectangle){ c.x - s * 0.55f, c.y - s * 0.7f, s * 0.42f, s * 1.4f }, 1.5f, color);
        ui_rect((Rectangle){ c.x + s * 0.13f, c.y - s * 0.7f, s * 0.42f, s * 1.4f }, 1.5f, color);
        break;
    case ICON_PLAY:
        DrawTriangle((Vector2){ c.x - s * 0.5f, c.y - s * 0.75f }, (Vector2){ c.x - s * 0.5f, c.y + s * 0.75f },
                     (Vector2){ c.x + s * 0.75f, c.y }, color);
        break;
    case ICON_CHECK:
        DrawLineEx((Vector2){ c.x - s * 0.7f, c.y }, (Vector2){ c.x - s * 0.2f, c.y + s * 0.5f }, th, color);
        DrawLineEx((Vector2){ c.x - s * 0.2f, c.y + s * 0.5f }, (Vector2){ c.x + s * 0.75f, c.y - s * 0.5f }, th, color);
        break;
    case ICON_ARROW_UP:
        DrawTriangle((Vector2){ c.x, c.y - s * 0.6f }, (Vector2){ c.x - s * 0.7f, c.y + s * 0.5f },
                     (Vector2){ c.x + s * 0.7f, c.y + s * 0.5f }, color);
        break;
    case ICON_ARROW_DOWN:
        DrawTriangle((Vector2){ c.x - s * 0.7f, c.y - s * 0.5f }, (Vector2){ c.x, c.y + s * 0.6f },
                     (Vector2){ c.x + s * 0.7f, c.y - s * 0.5f }, color);
        break;
    }
}

/* ------------------------------------------------------------ Scroll */

/* Scrollbar thumb rectangle for a view showing a window of content_h at
 * the given offset. The track is inset 4 px top and bottom; thumb length
 * is proportional to the visible fraction with a 28 px minimum so it
 * stays grabbable. */
static Rectangle scroll_thumb(Rectangle view, float content_h, float offset)
{
    float max_off = content_h - view.height;
    float track_h = view.height - 8.0f;
    float thumb_h = fmaxf(28.0f, track_h * view.height / content_h);
    float t = max_off > 0.0f ? offset / max_off : 0.0f;
    return (Rectangle){ view.x + view.width - 7.0f, view.y + 4.0f + t * (track_h - thumb_h), 4.0f, thumb_h };
}

/* Starts a scrolling viewport: handles wheel and scrollbar drag, eases the
 * displayed offset toward the target and pushes a clip region. Returns
 * the whole-pixel offset to subtract from content y. key owns three
 * slots: target offset, displayed offset and drag grab position. Must be
 * paired with ui_scroll_end using the same arguments. */
float ui_scroll_begin(Ui *ui, uint32_t key, Rectangle view, float content_h)
{
    float max_off = fmaxf(0.0f, content_h - view.height);
    float *target = ui_slot(ui, key, NULL);
    float *shown = ui_slot(ui, ui_key_i(key, 1), NULL);
    float *grab = ui_slot(ui, ui_key_i(key, 2), NULL);

    /* 1. Wheel, unless another widget is capturing a drag. */
    if (ui->active == 0 && ui_hover(ui, view) && ui->wheel != 0.0f)
        *target -= ui->wheel * SCROLL_STEP;

    /* 2. Scrollbar drag. The hit area is 8 px wider on each side than the
     * thumb. Pressing on the thumb keeps the grab point; pressing the
     * track centres the thumb under the cursor. While dragging, the
     * offset follows the mouse directly (no easing lag). */
    if (max_off > 0.0f) {
        Rectangle thumb = scroll_thumb(view, content_h, *shown);
        Rectangle hit = { thumb.x - 8.0f, view.y, thumb.width + 16.0f, view.height };
        if (ui->pressed && ui_hover(ui, hit) && ui->active == 0) {
            ui->active = key;
            *grab = CheckCollisionPointRec(ui->mouse, (Rectangle){ hit.x, thumb.y, hit.width, thumb.height })
                  ? ui->mouse.y - thumb.y : thumb.height * 0.5f;
        }
        if (ui->active == key && ui->down) {
            float track_h = view.height - 8.0f;
            float span = track_h - thumb.height;
            float t = span > 0.0f ? (ui->mouse.y - *grab - (view.y + 4.0f)) / span : 0.0f;
            *target = anim_clamp01(t) * max_off;
            *shown = *target;
        }
    }

    /* 3. Clamp, ease (rate 20) and snap the last 0.1 px so the motion
     * ends instead of creeping. */
    *target = fminf(fmaxf(*target, 0.0f), max_off);
    *shown = anim_approach(*shown, *target, 20.0f, ui->dt);
    if (fabsf(*shown - *target) < 0.1f)
        *shown = *target;

    /* 4. Clip content to the view; floor keeps rows on whole pixels. */
    ui_clip_push(ui, view);
    return floorf(*shown);
}

/* Ends the viewport started by ui_scroll_begin: pops the clip and draws
 * the scrollbar thumb when the content is taller than the view. The thumb
 * widens slightly while hovered or dragged. */
void ui_scroll_end(Ui *ui, uint32_t key, Rectangle view, float content_h)
{
    ui_clip_pop(ui);
    if (content_h <= view.height)
        return;

    const Palette *c = &ui->theme->c;
    float shown = *ui_slot(ui, ui_key_i(key, 1), NULL);
    Rectangle thumb = scroll_thumb(view, content_h, shown);
    Rectangle hit = { thumb.x - 8.0f, view.y, thumb.width + 16.0f, view.height };
    bool engaged = ui->active == key || ui_hover(ui, hit);
    float a = ui_anim(ui, ui_key_i(key, 3), engaged ? 1.0f : 0.0f, 14.0f);
    thumb.x -= a * 1.0f;
    thumb.width += a * 2.0f;
    ui_rect(thumb, thumb.width * 0.5f, theme_mix(c->border_strong, c->text_faint, a));
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
