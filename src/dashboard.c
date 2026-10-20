/* dashboard.c - application state handling and the per-frame pipeline.
 *
 * Sections: formatting and colour helpers, init/shutdown, the basic page and
 * dashboard_frame (input -> simulation -> page -> tooltip). The page is
 * drawn with the ui.h widgets. Called once per frame from main.c.
 */
#include "dashboard.h"

#include <stdio.h>
#include <string.h>

#define CONTENT_PAD 20   /* margin around the page content, px */

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

/* Initialises all state: loads fonts (needs the GL context), binds the UI
 * toolkit to the theme and seeds the simulation from the clock (a
 * different run every second; the XOR constant only scrambles the bits). */
void dashboard_init(Dashboard *d)
{
    memset(d, 0, sizeof *d);
    theme_load(&d->theme);
    ui_init(&d->ui, &d->theme);
    data_init(&d->data, (uint32_t)time(NULL) ^ 0xA5F1C3D7u);
}

/* Releases GPU resources owned by the dashboard. */
void dashboard_shutdown(Dashboard *d)
{
    theme_unload(&d->theme);
}

/* ---------------------------------------------------------- Basic page */

/* Row pitch of both lists on the basic page, px. */
#define BASIC_ROW_H 26.0f

/* Draws the basic page inside area a with ui.h widgets: title and summary
 * line, a "Devices" card with one row per device (hover highlight and
 * tooltip) and a "Recent alerts" card with the newest alerts. */
static void draw_basic_page(Dashboard *d, Rectangle a)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    const Palette *c = &t->c;
    const Summary *s = &d->summary;
    char buf[128], in[24], out[24];

    /* 1. Title and summary: device states and aggregate rates. */
    theme_text(t, FONT_TITLE, "Network Monitoring Dashboard", a.x, a.y, c->text);
    format_rate(in, sizeof in, s->in_mbps);
    format_rate(out, sizeof out, s->out_mbps);
    snprintf(buf, sizeof buf, "%d up   %d warning   %d down   in %s   out %s%s", s->up, s->warning, s->down, in, out,
             d->paused ? "   (paused)" : "");
    theme_text(t, FONT_BODY, buf, a.x, a.y + 34.0f, c->text_dim);
    ui_cut_top(&a, 68.0f);

    /* 2. Two cards: devices at a fixed 460 px, alerts take the rest. */
    Rectangle devices = ui_cut_left(&a, 460.0f);
    ui_cut_left(&a, SP_LG);
    Rectangle content = ui_panel(ui, devices, "Devices", ui_key("basic_devices"));

    /* 3. Device rows: hover wash (eased at rate 16), status dot, name,
     * address and latency; hovering a row shows its metrics in a tooltip. */
    for (int i = 0; i < DEVICE_COUNT; i++) {
        const Device *dev = &d->data.devices[i];
        Rectangle row = { content.x, content.y + (float)i * BASIC_ROW_H, content.width, BASIC_ROW_H - 2.0f };
        bool hot = ui_hover(ui, row);
        float hv = ui_anim(ui, ui_key_i(ui_key("basic_row"), (uint32_t)i), hot ? 1.0f : 0.0f, 16.0f);
        ui_rect(row, RADIUS_SM, theme_alpha(c->text, 0.055f * hv));

        float ty = row.y + (row.height - t->font_px[FONT_BODY]) * 0.5f;
        DrawCircleV((Vector2){ row.x + 12.0f, row.y + row.height * 0.5f }, 3.5f, status_color(c, dev->status));
        theme_text(t, FONT_BODY_BOLD, dev->name, row.x + 28.0f, ty, c->text);
        theme_text(t, FONT_MONO_SMALL, dev->ip, row.x + 160.0f, ty + 1.0f, c->text_faint);
        if (dev->status == STATUS_DOWN)
            snprintf(buf, sizeof buf, "down");
        else
            snprintf(buf, sizeof buf, "%.0f ms", (double)dev->latency_ms);
        theme_text_right(t, FONT_MONO_SMALL, buf, row.x + row.width - 8.0f, ty + 1.0f,
                         dev->status == STATUS_DOWN ? c->crit : c->text_dim);

        if (hot) {
            ui_tip_begin(ui, dev->name);
            ui_tip_row(ui, NULL, "Status", device_status_name(dev->status));
            snprintf(buf, sizeof buf, "%.2f %%", (double)dev->loss_pct);
            ui_tip_row(ui, NULL, "Packet loss", buf);
            format_rate(buf, sizeof buf, dev->rx_mbps + dev->tx_mbps);
            ui_tip_row(ui, NULL, "Traffic", buf);
        }
    }

    /* 4. Alerts card: as many of the newest alerts as fit the height. */
    content = ui_panel(ui, a, "Recent alerts", ui_key("basic_alerts"));
    int fit = (int)(content.height / BASIC_ROW_H);
    int shown = data_alert_count(&d->data) < fit ? data_alert_count(&d->data) : fit;
    for (int i = 0; i < shown; i++) {
        const Alert *al = data_alert(&d->data, i);
        Color sev = severity_color(c, al->severity);
        float y = content.y + (float)i * BASIC_ROW_H;
        format_clock(buf, sizeof buf, al->when);
        theme_text(t, FONT_MONO_SMALL, buf, content.x, y + 1.0f, c->text_faint);
        theme_text(t, FONT_CAPTION_BOLD, alert_severity_name(al->severity), content.x + 74.0f, y + 2.0f, sev);
        theme_text_fit(t, FONT_BODY, al->message, content.x + 148.0f, y, content.width - 148.0f, c->text_dim);
    }
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

    /* 3. Basic page on the whole window, inset by CONTENT_PAD. */
    ClearBackground(d->theme.c.bg);
    draw_basic_page(d, (Rectangle){ CONTENT_PAD, CONTENT_PAD, sw - 2.0f * CONTENT_PAD, sh - 2.0f * CONTENT_PAD });

    /* 4. Tooltip on top of everything (ui_end). */
    ui_end(ui);
}
