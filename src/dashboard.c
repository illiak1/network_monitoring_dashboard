/* dashboard.c - application state handling and the per-frame pipeline.
 *
 * Sections: formatting and colour helpers, init/shutdown and dashboard_frame
 * (input -> simulation -> page -> tooltip). The Overview page is in
 * page_overview.c, shared cards in panels.c. Called once per frame from
 * main.c.
 */
#include "dashboard.h"

#include <stdio.h>
#include <string.h>

#include "pages.h"

#define CONTENT_PAD 20   /* margin around the page content, px */

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

    d->range_idx = 1;
    d->series_on[0] = d->series_on[1] = true;
}

/* Releases GPU resources owned by the dashboard. */
void dashboard_shutdown(Dashboard *d)
{
    theme_unload(&d->theme);
}

/* ----------------------------------------------------------- Frame */

/* Runs one complete frame inside BeginDrawing/EndDrawing. */
void dashboard_frame(Dashboard *d)
{
    Ui *ui = &d->ui;

    /* 1. Input snapshot, shortcut, simulation. Space toggles the pause
     * flag; the summary is refreshed even while paused. */
    ui_begin(ui);
    if (IsKeyPressed(KEY_SPACE))
        d->paused = !d->paused;
    if (!d->paused)
        data_update(&d->data, ui->dt);
    data_summary(&d->data, &d->summary);

    /* 2. A (nearly) minimised window has no usable area: skip drawing. */
    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    if (sw < 200.0f || sh < 200.0f) {
        ui_end(ui);
        return;
    }

    /* 3. Overview page on the whole window, inset by CONTENT_PAD. */
    ClearBackground(d->theme.c.bg);
    page_overview(d, (Rectangle){ CONTENT_PAD, CONTENT_PAD, sw - 2.0f * CONTENT_PAD, sh - 2.0f * CONTENT_PAD });

    /* 4. Tooltip on top of everything (ui_end). */
    ui_end(ui);
}
