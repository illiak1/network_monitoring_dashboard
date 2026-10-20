/* dashboard.c - application state handling and the per-frame pipeline.
 *
 * Sections: formatting and colour helpers, init/shutdown, navigation,
 * keyboard shortcuts and layout state, and dashboard_frame (input ->
 * simulation -> page -> shell -> tooltip). Pages are in page_*.c,
 * sidebar and header in shell.c, shared cards in panels.c. Called once
 * per frame from main.c.
 */
#include "dashboard.h"

#include <stdio.h>
#include <string.h>

#include "anim.h"
#include "pages.h"

#define CONTENT_PAD 20   /* margin between the shell and page content, px */

/* Bandwidth window in seconds per range index; RANGE_LABELS are the
 * matching button captions. 120 s equals the history length (RING_CAP). */
const float RANGE_SECONDS[RANGE_COUNT] = { 30.0f, 60.0f, 120.0f };
const char *const RANGE_LABELS[RANGE_COUNT] = { "30s", "1m", "2m" };

/* ------------------------------------------------------------ Helpers */

/* Palette colour for a device status; text_dim for an unknown value. */
Color status_color(const Palette *c, DeviceStatus s)
{
    switch (s) {
    case STATUS_UP:      return c->ok;
    case STATUS_WARNING: return c->warn;
    case STATUS_DOWN:    return c->crit;
    }
    return c->text_dim;
}

/* Palette colour for an alert severity (Info uses the accent colour). */
Color severity_color(const Palette *c, AlertSeverity s)
{
    switch (s) {
    case SEV_INFO:     return c->accent;
    case SEV_WARNING:  return c->warn;
    case SEV_CRITICAL: return c->crit;
    default:           return c->text_dim;
    }
}

/* Formats a rate for display: 2 decimals in Gbps, whole Mbps below 1000. */
void format_rate(char *buf, size_t cap, float mbps)
{
    if (mbps >= 1000.0f)
        snprintf(buf, cap, "%.2f Gbps", (double)(mbps / 1000.0f));
    else
        snprintf(buf, cap, "%.0f Mbps", (double)mbps);
}

/* Formats t as local time HH:MM:SS; "--:--:--" if it cannot be converted. */
void format_clock(char *buf, size_t cap, time_t t)
{
    struct tm *tm = localtime(&t);
    if (tm)
        strftime(buf, cap, "%H:%M:%S", tm);
    else
        snprintf(buf, cap, "--:--:--");
}

/* ------------------------------------------------------ Init, shutdown */

/* Initialises all state: loads fonts (needs the GL context), seeds the
 * simulation from the clock (a different run every second; the XOR
 * constant only scrambles the bits) and sets the view defaults. */
void dashboard_init(Dashboard *d)
{
    memset(d, 0, sizeof *d);
    theme_load(&d->theme);
    ui_init(&d->ui, &d->theme);
    data_init(&d->data, (uint32_t)time(NULL) ^ 0xA5F1C3D7u);

    d->page = PAGE_OVERVIEW;
    d->page_t = 1.0f;
    d->sidebar_w = SIDEBAR_W;
    d->range_idx = 1;
    d->series_on[0] = d->series_on[1] = true;
}

/* Releases GPU resources owned by the dashboard. */
void dashboard_shutdown(Dashboard *d)
{
    theme_unload(&d->theme);
}

/* ---------------------------------------------------------- Navigation */

/* Switches to page and restarts the fade-in (page_t = 0). Re-selecting
 * the current page does nothing. */
void dashboard_navigate(Dashboard *d, Page page)
{
    if (page == d->page)
        return;
    d->page = page;
    d->page_t = 0.0f;
}

/* ------------------------------------------------- Input, layout state */

/* Global shortcuts: 1-2 pages, Space pause, B sidebar, F11 borderless. */
static void handle_keys(Dashboard *d)
{
    static const int page_keys[PAGE_COUNT] = { KEY_ONE, KEY_TWO };
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (IsKeyPressed(page_keys[i]))
            dashboard_navigate(d, (Page)i);
    }
    if (IsKeyPressed(KEY_SPACE))
        d->paused = !d->paused;
    if (IsKeyPressed(KEY_B))
        d->sidebar_collapsed = !d->sidebar_collapsed;
    if (IsKeyPressed(KEY_F11))
        ToggleBorderlessWindowed();
}

/* Advances the animated layout values by one frame: sidebar width toward
 * its expanded/collapsed target (rate 16, collapsed also when the window
 * is narrower than SIDEBAR_AUTO_COLLAPSE_BELOW) and the page fade-in
 * (rate 6 per s, so a transition takes ~170 ms). The width is rounded to
 * whole pixels because it positions text and borders. */
static void update_layout_state(Dashboard *d, float screen_w)
{
    bool collapsed = d->sidebar_collapsed || screen_w < SIDEBAR_AUTO_COLLAPSE_BELOW;
    float target = collapsed ? SIDEBAR_COLLAPSED_W : SIDEBAR_W;
    d->sidebar_w = anim_approach(d->sidebar_w, target, 16.0f, d->ui.dt);
    if (fabsf(d->sidebar_w - target) < 0.5f)
        d->sidebar_w = target;
    d->sidebar_w = floorf(d->sidebar_w + 0.5f);

    d->page_t = fminf(1.0f, d->page_t + d->ui.dt * 6.0f);
}

/* ----------------------------------------------------------- Frame */

/* Runs one complete frame inside BeginDrawing/EndDrawing. */
void dashboard_frame(Dashboard *d)
{
    Ui *ui = &d->ui;
    const Palette *c = &d->theme.c;

    /* 1. Input snapshot, shortcuts, simulation. The summary is refreshed
     * even while paused so UI interaction (acknowledging alerts) shows. */
    ui_begin(ui);
    handle_keys(d);
    if (!d->paused)
        data_update(&d->data, ui->dt);
    data_summary(&d->data, &d->summary);

    /* 2. A (nearly) minimised window has no usable area: skip drawing. */
    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    if (sw < 200.0f || sh < 200.0f) {
        ui_end(ui);
        return;
    }
    update_layout_state(d, sw);

    /* 3. Content rectangle: right of the sidebar and below the header,
     * inset by CONTENT_PAD on the left, right and bottom and by 4 px at
     * the top. */
    ClearBackground(c->bg);

    Rectangle body = { d->sidebar_w, HEADER_H, sw - d->sidebar_w, sh - HEADER_H };
    Rectangle content = { body.x + CONTENT_PAD, body.y + 4.0f, body.width - 2.0f * CONTENT_PAD, body.height - CONTENT_PAD - 4.0f };

    /* 4. Active page. */
    switch (d->page) {
    case PAGE_OVERVIEW: page_overview(d, content); break;
    case PAGE_TRAFFIC:  page_traffic(d, content); break;
    default: break;
    }

    /* 5. Page fade-in: a background-coloured veil over the body that
     * becomes transparent (ease-out), drawn before the shell so sidebar
     * and header are not dimmed. */
    if (d->page_t < 1.0f)
        DrawRectangleRec(body, theme_alpha(c->bg, 1.0f - anim_ease_out_cubic(d->page_t)));

    /* 6. Shell, then the tooltip on top of everything (ui_end). */
    shell_draw_sidebar(d, (Rectangle){ 0.0f, 0.0f, d->sidebar_w, sh });
    shell_draw_header(d, (Rectangle){ d->sidebar_w, 0.0f, sw - d->sidebar_w, HEADER_H });

    ui_end(ui);
}
