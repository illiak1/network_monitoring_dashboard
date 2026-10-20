/* shell.c - application chrome: sidebar navigation and top header.
 *
 * Sidebar: brand, two page buttons with an animated active indicator,
 * collapse toggle, FPS/"simulated data" note. Header: page
 * title, summary statistics (dropped by priority when space runs out),
 * LIVE/PAUSED pill, pause button and clock. Both are drawn after the page
 * by dashboard_frame and read Dashboard.summary computed for the frame.
 */
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "anim.h"
#include "dashboard.h"

#define NAV_ITEM_H 40.0f    /* height of one sidebar button, px */
#define NAV_ITEM_GAP 4.0f   /* vertical gap between buttons, px */

/* Labels and icons indexed by Page. */
static const char *const PAGE_NAMES[PAGE_COUNT] = { "Overview", "Traffic" };
static const IconId PAGE_ICONS[PAGE_COUNT] = { ICON_DASHBOARD, ICON_TRAFFIC };

/* ----------------------------------------------------------- Sidebar */

/* Draws the logo tile and, when the sidebar is wide enough, the product
 * name. expanded (0..1) fades the text during the width animation. */
static void draw_brand(Dashboard *d, Rectangle a, float expanded)
{
    const Theme *t = &d->theme;
    const Palette *c = &t->c;

    Rectangle logo = { a.x + 16.0f, 14.0f, 36.0f, 36.0f };
    ui_rect(logo, 10.0f, c->accent);
    ui_icon(ICON_TRAFFIC, (Vector2){ logo.x + 18.0f, logo.y + 18.0f }, 20.0f, c->bg);

    if (expanded > 0.05f) {
        theme_text(t, FONT_HEADING, "NetMon", a.x + 62.0f, 15.0f, theme_alpha(c->text, expanded));
        theme_text(t, FONT_CAPTION, "Network dashboard", a.x + 62.0f, 34.0f, theme_alpha(c->text_faint, expanded));
    }
}

/* Draws the sidebar into area a (its width is animated by dashboard.c)
 * and handles navigation and collapse clicks. */
void shell_draw_sidebar(Dashboard *d, Rectangle a)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    const Palette *c = &t->c;
    /* 0 = collapsed width, 1 = full width; drives the label fade while the
     * width animates. */
    float expanded = anim_clamp01((a.width - SIDEBAR_COLLAPSED_W) / (float)(SIDEBAR_W - SIDEBAR_COLLAPSED_W));

    /* 1. Background, right-hand divider and brand. */
    DrawRectangleRec(a, c->sidebar);
    DrawRectangle((int)(a.x + a.width) - 1, 0, 1, (int)a.height, c->border);
    draw_brand(d, a, expanded);

    /* 2. Active-page indicator: a tinted bar with an accent strip that
     * slides between items (rate 18). top is the y of the first item,
     * below the brand block. */
    const float top = 84.0f;
    float ind_y = ui_anim(ui, ui_key("nav_indicator"), top + (float)d->page * (NAV_ITEM_H + NAV_ITEM_GAP), 18.0f);
    Rectangle ind = { a.x + 10.0f, ind_y, a.width - 20.0f, NAV_ITEM_H };
    ui_rect(ind, RADIUS_SM + 2, theme_alpha(c->accent, 0.13f));
    ui_rect((Rectangle){ a.x + 10.0f, ind_y + 9.0f, 3.0f, NAV_ITEM_H - 18.0f }, 1.5f, c->accent);

    /* 3. Nav items: hover tint, icon, label with key hint, click. */
    for (int i = 0; i < PAGE_COUNT; i++) {
        Rectangle item = { a.x + 10.0f, top + (float)i * (NAV_ITEM_H + NAV_ITEM_GAP), a.width - 20.0f, NAV_ITEM_H };
        bool hot = ui_hot(ui, item);
        float hv = ui_anim(ui, ui_key_i(ui_key("nav"), (uint32_t)i), hot ? 1.0f : 0.0f, 16.0f);
        bool active = (int)d->page == i;

        if (!active && hv > 0.01f)
            ui_rect(item, RADIUS_SM + 2, theme_alpha(c->text, 0.05f * hv));

        Color fg = active ? c->accent : theme_mix(c->text_dim, c->text, hv);
        ui_icon(PAGE_ICONS[i], (Vector2){ item.x + 23.0f, item.y + item.height * 0.5f }, 18.0f, fg);

        if (expanded > 0.05f) {
            theme_text(t, FONT_BODY_BOLD, PAGE_NAMES[i], item.x + 46.0f, item.y + 11.0f,
                       theme_alpha(active ? c->text : theme_mix(c->text_dim, c->text, hv), expanded));
            char key[2] = { (char)('1' + i), '\0' };
            theme_text_right(t, FONT_MONO_SMALL, key, item.x + item.width - 12.0f, item.y + 12.0f,
                             theme_alpha(c->text_faint, 0.7f * expanded));
        }

        if (hot && ui->pressed)
            dashboard_navigate(d, (Page)i);
    }

    /* 4. Collapse toggle, 128 px above the bottom; only when it would not
     * collide with the nav items in a short window. */
    float fy = a.height - 128.0f;
    if (fy > top + (float)PAGE_COUNT * (NAV_ITEM_H + NAV_ITEM_GAP)) {
        Rectangle toggle = { a.x + 10.0f, fy, a.width - 20.0f, 36.0f };
        bool hot = ui_hot(ui, toggle);
        float hv = ui_anim(ui, ui_key("nav_toggle"), hot ? 1.0f : 0.0f, 16.0f);
        ui_rect(toggle, RADIUS_SM + 2, theme_alpha(c->text, 0.05f * hv));
        ui_icon(ICON_MENU, (Vector2){ toggle.x + 23.0f, toggle.y + 18.0f }, 16.0f, theme_mix(c->text_faint, c->text, hv));
        if (expanded > 0.05f)
            theme_text(t, FONT_SMALL, "Collapse", toggle.x + 46.0f, toggle.y + 10.0f, theme_alpha(theme_mix(c->text_faint, c->text, hv), expanded));
        if (hot && ui->pressed)
            d->sidebar_collapsed = !d->sidebar_collapsed;
    }

    /* 5. Bottom note: data-source notice and live FPS counter. The
     * collapsed form shows only the FPS number. */
    Rectangle note = { a.x + 10.0f, a.height - 72.0f, a.width - 20.0f, 56.0f };
    ui_rect(note, RADIUS_SM + 2, c->panel);
    ui_rect_outline(note, RADIUS_SM + 2, 1.0f, c->border);
    char fps[24];
    snprintf(fps, sizeof fps, "%d FPS", GetFPS());
    if (expanded > 0.5f) {
        theme_text(t, FONT_CAPTION_BOLD, "SIMULATED DATA", note.x + 12.0f, note.y + 12.0f, theme_alpha(c->text_dim, expanded));
        theme_text(t, FONT_MONO_SMALL, fps, note.x + 12.0f, note.y + 29.0f, theme_alpha(c->text_faint, expanded));
    } else {
        theme_text_center(t, FONT_CAPTION, "FPS", note.x + note.width * 0.5f, note.y + 10.0f, c->text_faint);
        snprintf(fps, sizeof fps, "%d", GetFPS());
        theme_text_center(t, FONT_MONO_SMALL, fps, note.x + note.width * 0.5f, note.y + 28.0f, c->text_dim);
    }
}

/* ------------------------------------------------------------ Header */

/* One summary statistic in the header. width is the larger of label and
 * value width in px, used to pack the statistics. */
typedef struct {
    const char *label;
    char value[32];
    Color color;
    float width;
} HeaderStat;

/* Statistic ids; also the left-to-right drawing order. */
enum { STAT_DEVICES, STAT_THROUGHPUT, STAT_LATENCY, STAT_LOSS, STAT_COUNT };

/* Lower index = kept longer when the header runs out of room. Device
 * count and throughput rank above the latency and loss averages (the
 * gauges on the Overview page show those too). */
static const int STAT_PRIORITY[STAT_COUNT] = { STAT_DEVICES, STAT_THROUGHPUT, STAT_LATENCY, STAT_LOSS };

/* Fills stats[] from the current summary: text, status colour and width.
 * Colours reuse the thresholds of the gauges and the utilisation alarm so
 * header and panels agree. ONLINE counts devices that are not DOWN. */
static void build_stats(const Dashboard *d, HeaderStat stats[STAT_COUNT])
{
    const Palette *c = &d->theme.c;
    const Summary *s = &d->summary;

    stats[STAT_DEVICES].label = "ONLINE";
    snprintf(stats[STAT_DEVICES].value, sizeof stats[STAT_DEVICES].value, "%d/%d", s->up + s->warning, DEVICE_COUNT);
    stats[STAT_DEVICES].color = s->down > 0 ? c->crit : (s->warning > 0 ? c->warn : c->ok);

    stats[STAT_THROUGHPUT].label = "IN / OUT (MBPS)";
    snprintf(stats[STAT_THROUGHPUT].value, sizeof stats[STAT_THROUGHPUT].value, "%.0f / %.0f", (double)s->in_mbps, (double)s->out_mbps);
    stats[STAT_THROUGHPUT].color = s->utilization > 0.85f ? c->warn : c->text;

    stats[STAT_LATENCY].label = "AVG LATENCY";
    snprintf(stats[STAT_LATENCY].value, sizeof stats[STAT_LATENCY].value, "%.1f ms", (double)s->avg_latency_ms);
    stats[STAT_LATENCY].color = chart_zone_color(&d->theme, s->avg_latency_ms, GAUGE_LATENCY_MAX_MS, GAUGE_LATENCY_WARN_MS, GAUGE_LATENCY_CRIT_MS);

    stats[STAT_LOSS].label = "PACKET LOSS";
    snprintf(stats[STAT_LOSS].value, sizeof stats[STAT_LOSS].value, "%.2f %%", (double)s->avg_loss_pct);
    stats[STAT_LOSS].color = chart_zone_color(&d->theme, s->avg_loss_pct, GAUGE_LOSS_MAX_PCT, GAUGE_LOSS_WARN_PCT, GAUGE_LOSS_CRIT_PCT);

    for (int i = 0; i < STAT_COUNT; i++) {
        float lw = theme_measure(&d->theme, FONT_CAPTION, stats[i].label).x;
        float vw = theme_measure(&d->theme, FONT_MONO_BOLD, stats[i].value).x;
        stats[i].width = fmaxf(lw, vw);
    }
}

/* Subtitle under the page title. Returns a string literal. */
static const char *page_subtitle(const Dashboard *d)
{
    switch (d->page) {
    case PAGE_OVERVIEW: return "Live network status at a glance";
    case PAGE_TRAFFIC: return "Bandwidth, utilisation and top talkers";
    default: return "";
    }
}

/* Draws the wall-clock time and date right-aligned at `right`. Returns
 * the x of the block's left edge so the caller can lay out leftwards. */
static float draw_clock(Dashboard *d, float right)
{
    const Theme *t = &d->theme;
    char time_buf[16], date_buf[32];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) {
        strftime(time_buf, sizeof time_buf, "%H:%M:%S", tm);
        strftime(date_buf, sizeof date_buf, "%a %d %b %Y", tm);
    } else {
        snprintf(time_buf, sizeof time_buf, "--:--:--");
        date_buf[0] = '\0';
    }
    float w = fmaxf(theme_measure(t, FONT_MONO_BOLD, time_buf).x, theme_measure(t, FONT_CAPTION, date_buf).x);
    theme_text_right(t, FONT_MONO_BOLD, time_buf, right, 14.0f, t->c.text);
    theme_text_right(t, FONT_CAPTION, date_buf, right, 36.0f, t->c.text_faint);
    return right - w;
}

/* Draws the header into area a. The layout runs right to left (clock,
 * pause button, LIVE pill), then the title on the left; whatever width
 * remains in between is given to the summary statistics. */
void shell_draw_header(Dashboard *d, Rectangle a)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    const Palette *c = &t->c;

    /* 1. Background and bottom divider. */
    DrawRectangleRec(a, c->bg);
    DrawRectangle((int)a.x, (int)a.height - 1, (int)a.width, 1, c->border);

    float left = a.x + 20.0f;
    float right = a.x + a.width - 20.0f;

    /* 2. Clock. */
    right = draw_clock(d, right) - 20.0f;

    /* 3. Pause/resume button with tooltip; the click toggles the simulation. */
    Rectangle pause = { right - 34.0f, 15.0f, 34.0f, 34.0f };
    bool hot = ui_hot(ui, pause);
    float hv = ui_anim(ui, ui_key("pause_btn"), hot ? 1.0f : 0.0f, 16.0f);
    ui_rect(pause, RADIUS_SM + 2, theme_mix(c->panel, c->panel_raised, hv));
    ui_rect_outline(pause, RADIUS_SM + 2, 1.0f, theme_mix(c->border, c->border_strong, hv));
    ui_icon(d->paused ? ICON_PLAY : ICON_PAUSE, (Vector2){ pause.x + 17.0f, pause.y + 17.0f }, 16.0f, theme_mix(c->text_dim, c->text, hv));
    if (hot) {
        ui_tip_begin(ui, d->paused ? "Resume (Space)" : "Pause (Space)");
        if (ui->pressed)
            d->paused = !d->paused;
    }
    right = pause.x - 12.0f;

    /* 4. LIVE / PAUSED pill; the dot pulses only while live. */
    const char *live = d->paused ? "PAUSED" : "LIVE";
    Color live_color = d->paused ? c->warn : c->ok;
    float lw = theme_measure(t, FONT_CAPTION_BOLD, live).x + 34.0f;
    Rectangle pill = { right - lw, 20.0f, lw, 24.0f };
    ui_rect(pill, 12.0f, theme_alpha(live_color, 0.13f));
    ui_status_dot(ui, (Vector2){ pill.x + 13.0f, pill.y + 12.0f }, 3.5f, live_color, !d->paused);
    theme_text(t, FONT_CAPTION_BOLD, live, pill.x + 24.0f, pill.y + 5.0f, live_color);
    right = pill.x - 24.0f;

    /* 5. Title and subtitle on the left; title_end is where the statistics
     * area begins (32 px breathing room). */
    const char *title = PAGE_NAMES[d->page];
    const char *subtitle = page_subtitle(d);
    theme_text(t, FONT_TITLE, title, left, 9.0f, c->text);
    theme_text(t, FONT_SMALL, subtitle, left, 38.0f, c->text_faint);
    float title_end = left + fmaxf(theme_measure(t, FONT_TITLE, title).x, theme_measure(t, FONT_SMALL, subtitle).x) + 32.0f;

    /* 6. Summary stats take whatever room is left, dropped by priority.
     * Admission stops at the first one that does not fit, so a
     * lower-priority stat is never shown while a higher one is hidden.
     * Each stat needs its width plus 22 px padding on both sides. */
    HeaderStat stats[STAT_COUNT];
    build_stats(d, stats);
    const float pad = 22.0f;
    bool visible[STAT_COUNT] = { false };
    float used = 0.0f, avail = right - title_end;
    for (int p = 0; p < STAT_COUNT; p++) {
        int i = STAT_PRIORITY[p];
        float need = stats[i].width + 2.0f * pad;
        if (used + need > avail)
            break;
        visible[i] = true;
        used += need;
    }

    /* 7. Draw the admitted stats right-aligned in index order, separated
     * by 1 px dividers. */
    float x = right - used;
    bool first = true;
    for (int i = 0; i < STAT_COUNT; i++) {
        if (!visible[i])
            continue;
        if (!first)
            DrawRectangle((int)x, 18, 1, 28, c->border);
        first = false;
        float cx = x + pad;
        theme_text(t, FONT_CAPTION, stats[i].label, cx, 13.0f, c->text_faint);
        theme_text(t, FONT_MONO_BOLD, stats[i].value, cx, 30.0f, stats[i].color);
        x += stats[i].width + 2.0f * pad;
    }
}
