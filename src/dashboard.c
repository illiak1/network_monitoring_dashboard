/* dashboard.c - application state handling and the per-frame pipeline.
 *
 * Sections: formatting and colour helpers, init/shutdown, the basic page and
 * dashboard_frame (input -> simulation -> page). Everything is drawn with
 * the theme text helpers. Called once per frame from main.c.
 */
#include "dashboard.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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

/* Initialises all state: loads fonts (needs the GL context) and seeds the
 * simulation from the clock (a different run every second; the XOR
 * constant only scrambles the bits). */
void dashboard_init(Dashboard *d)
{
    memset(d, 0, sizeof *d);
    theme_load(&d->theme);
    data_init(&d->data, (uint32_t)time(NULL) ^ 0xA5F1C3D7u);
}

/* Releases GPU resources owned by the dashboard. */
void dashboard_shutdown(Dashboard *d)
{
    theme_unload(&d->theme);
}

/* ---------------------------------------------------------- Basic page */

/* Draws the basic page with the theme text helpers: title, one summary
 * line, the device list (status dot, name, address, latency) on the left
 * and the newest alerts on the right. */
static void draw_basic_page(Dashboard *d)
{
    const Theme *t = &d->theme;
    const Palette *c = &t->c;
    const Summary *s = &d->summary;
    char buf[128], in[24], out[24];

    /* 1. Title and summary: device states and aggregate rates. */
    theme_text(t, FONT_TITLE, "Network Monitoring Dashboard", 32.0f, 24.0f, c->text);
    format_rate(in, sizeof in, s->in_mbps);
    format_rate(out, sizeof out, s->out_mbps);
    snprintf(buf, sizeof buf, "%d up   %d warning   %d down   in %s   out %s%s", s->up, s->warning, s->down, in, out,
             d->paused ? "   (paused)" : "");
    theme_text(t, FONT_BODY, buf, 32.0f, 58.0f, c->text_dim);

    /* 2. Device list, one 24 px line per device, dot coloured by status. */
    theme_text(t, FONT_CAPTION_BOLD, "DEVICES", 32.0f, 94.0f, c->text_faint);
    float y = 116.0f;
    for (int i = 0; i < DEVICE_COUNT; i++) {
        const Device *dev = &d->data.devices[i];
        DrawCircleV((Vector2){ 40.0f, y + 8.0f }, 4.0f, status_color(c, dev->status));
        theme_text(t, FONT_BODY_BOLD, dev->name, 56.0f, y, c->text);
        theme_text(t, FONT_MONO_SMALL, dev->ip, 190.0f, y + 1.0f, c->text_faint);
        if (dev->status == STATUS_DOWN)
            snprintf(buf, sizeof buf, "down");
        else
            snprintf(buf, sizeof buf, "%.0f ms", (double)dev->latency_ms);
        theme_text_right(t, FONT_MONO_SMALL, buf, 440.0f, y + 1.0f, dev->status == STATUS_DOWN ? c->crit : c->text_dim);
        y += 24.0f;
    }

    /* 3. Newest alerts: time, severity, source and message. The message
     * is cut to the width left on the right of the 510 px start column. */
    theme_text(t, FONT_CAPTION_BOLD, "RECENT ALERTS", 510.0f, 94.0f, c->text_faint);
    int shown = data_alert_count(&d->data) < 12 ? data_alert_count(&d->data) : 12;
    float msg_w = (float)GetScreenWidth() - 510.0f - 250.0f;
    y = 116.0f;
    for (int i = 0; i < shown; i++) {
        const Alert *a = data_alert(&d->data, i);
        Color sev = severity_color(c, a->severity);
        format_clock(buf, sizeof buf, a->when);
        theme_text(t, FONT_MONO_SMALL, buf, 510.0f, y + 1.0f, c->text_faint);
        theme_text(t, FONT_CAPTION_BOLD, alert_severity_name(a->severity), 590.0f, y + 2.0f, sev);
        theme_text(t, FONT_BODY_BOLD, a->source, 660.0f, y, c->text);
        theme_text_fit(t, FONT_BODY, a->message, 760.0f, y, msg_w, c->text_dim);
        y += 24.0f;
    }
}

/* ----------------------------------------------------------- Frame */

/* Runs one complete frame inside BeginDrawing/EndDrawing. */
void dashboard_frame(Dashboard *d)
{
    /* 1. Input and simulation. Space toggles the pause flag; dt is capped
     * at 50 ms so a stall (window drag, debugger) cannot run the simulation
     * far ahead. The summary is refreshed even while paused. */
    float dt = fminf(GetFrameTime(), 0.05f);
    if (IsKeyPressed(KEY_SPACE))
        d->paused = !d->paused;
    if (!d->paused)
        data_update(&d->data, (double)dt);
    data_summary(&d->data, &d->summary);

    /* 2. Draw the basic page on the theme background. */
    ClearBackground(d->theme.c.bg);
    draw_basic_page(d);
}
