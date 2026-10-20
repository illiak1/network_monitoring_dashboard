/* page_traffic.c - Traffic page: bandwidth chart, top talkers, link stats.
 *
 * Top: the bandwidth card (panels.c) at large size. Bottom: top talkers on
 * the left and a "Link statistics" card on the right with the utilisation
 * meter and a current/average/peak/P95 table for the selected time window.
 * Called from dashboard_frame via pages.h.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "anim.h"
#include "pages.h"
#include "panels.h"

/* ---- Window statistics ---- */

/* Summary of one direction's throughput over a time window, in Mbps. */
typedef struct {
    float current, average, peak, p95;
} RateStats;

/* qsort comparator for ascending floats. Uses two comparisons instead of
 * a subtraction, which would truncate when converted to int. */
static int compare_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Statistics over the samples inside the selected time window.
 * Uses the newest window_s seconds of the ring (or all samples when the
 * history is shorter). All values are 0 for an empty ring. */
static RateStats window_stats(const Ring *ring, float window_s)
{
    /* 1. Take the newest m samples of a linear copy. */
    float v[RING_CAP];
    int n = ring_copy(ring, v);
    int m = (int)(window_s / SIM_TICK_S);
    if (m > n)
        m = n;
    RateStats s = { 0 };
    if (m == 0)
        return s;

    /* 2. Current, average and peak in one pass over the window. */
    float *w = v + (n - m);
    float sum = 0.0f;
    s.current = w[m - 1];
    for (int i = 0; i < m; i++) {
        sum += w[i];
        s.peak = fmaxf(s.peak, w[i]);
    }
    s.average = sum / (float)m;

    /* 3. P95: sort the window (after current was taken, since sorting
     * destroys the time order) and pick the element at 95% of the range. */
    qsort(w, (size_t)m, sizeof w[0], compare_float);
    s.p95 = w[(int)((float)(m - 1) * 0.95f)];
    return s;
}

/* ---- Link panel ---- */

/* Draws the utilisation block: percentage readout, "of N Mbps" capacity
 * note and a bar with markers at 70% (warning) and 85% (alarm). The same
 * limits drive the zone colour; 85% is also where data.c raises its
 * utilisation alert. The value is eased at rate 6. */
static void draw_utilization(Dashboard *d, Rectangle area)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    const Palette *c = &t->c;

    float util = ui_anim(ui, ui_key("util_value"), d->summary.utilization, 6.0f);
    Color col = chart_zone_color(t, util, 1.0f, 0.70f, 0.85f);

    char big[16];
    snprintf(big, sizeof big, "%.0f%%", (double)(util * 100.0f));
    theme_text(t, FONT_CAPTION_BOLD, "LINK UTILISATION", area.x, area.y, c->text_faint);
    theme_text(t, FONT_MONO_LARGE, big, area.x, area.y + 16.0f, col);
    char cap[32];
    snprintf(cap, sizeof cap, "of %.0f Mbps", (double)LINK_CAPACITY_MBPS);
    theme_text_right(t, FONT_SMALL, cap, area.x + area.width, area.y + 34.0f, c->text_faint);

    /* The fill is at least 8 px (the bar height) so its rounded caps
     * stay intact at near-zero utilisation. */
    Rectangle bar = { area.x, area.y + 62.0f, area.width, 8.0f };
    ui_rect(bar, 4.0f, c->border);
    float w = fmaxf(8.0f, bar.width * anim_clamp01(util));
    ui_rect((Rectangle){ bar.x, bar.y, w, bar.height }, 4.0f, col);
    for (int i = 0; i < 2; i++) {
        float x = floorf(bar.x + bar.width * (i == 0 ? 0.70f : 0.85f));
        DrawRectangle((int)x, (int)bar.y - 2, 1, (int)bar.height + 4, c->text_faint);
    }
    if (ui_hover(ui, (Rectangle){ area.x, area.y, area.width, 72.0f })) {
        ui_tip_begin(ui, "Link utilisation");
        ui_tip_row(ui, &c->warn, "Warning at", "70 %");
        ui_tip_row(ui, &c->crit, "Alarm at", "85 %");
    }
}

/* Draws the throughput table: one row per direction with CURRENT, AVERAGE,
 * PEAK and P95 columns (right-aligned numbers) for the same window as the
 * bandwidth chart (range_idx). */
static void draw_rate_table(Dashboard *d, Rectangle area)
{
    const Theme *t = &d->theme;
    const Palette *c = &t->c;
    float window = RANGE_SECONDS[d->range_idx];

    static const char *const cols[4] = { "CURRENT", "AVERAGE", "PEAK", "P95" };
    float label_w = 84.0f;
    float col_w = (area.width - label_w) / 4.0f;

    /* 1. Title, column headings and the divider under them. */
    theme_text(t, FONT_CAPTION_BOLD, "THROUGHPUT, MBPS", area.x, area.y, c->text_faint);
    float hy = area.y + 18.0f;
    for (int i = 0; i < 4; i++)
        theme_text_right(t, FONT_CAPTION, cols[i], area.x + label_w + col_w * (float)(i + 1), hy, c->text_faint);
    DrawRectangle((int)area.x, (int)hy + 18, (int)area.width, 1, c->border);

    /* 2. One row per direction; the CURRENT value is emphasised. */
    const Ring *rings[2] = { &d->data.bw_in, &d->data.bw_out };
    const char *names[2] = { "Inbound", "Outbound" };
    Color colors[2] = { c->series_in, c->series_out };
    for (int r = 0; r < 2; r++) {
        RateStats s = window_stats(rings[r], window);
        float vals[4] = { s.current, s.average, s.peak, s.p95 };
        float y = hy + 27.0f + (float)r * 28.0f;
        DrawCircleV((Vector2){ area.x + 4.0f, y + 8.0f }, 3.5f, colors[r]);
        theme_text(t, FONT_BODY, names[r], area.x + 16.0f, y, c->text);
        for (int i = 0; i < 4; i++) {
            char buf[16];
            snprintf(buf, sizeof buf, "%.0f", (double)vals[i]);
            theme_text_right(t, FONT_MONO, buf, area.x + label_w + col_w * (float)(i + 1), y, i == 0 ? c->text : c->text_dim);
        }
    }
}

/* Link statistics card: utilisation block on top, rate table below it
 * when at least 92 px of height remain (the table needs about that). */
static void panel_link_stats(Dashboard *d, Rectangle r)
{
    Rectangle content = ui_panel(&d->ui, r, "Link statistics", ui_key("link_stats"));
    Rectangle util = ui_cut_top(&content, 74.0f);
    ui_cut_top(&content, 10.0f);
    draw_utilization(d, util);
    if (content.height > 92.0f)
        draw_rate_table(d, content);
}

/* ---- Page ---- */

/* Lays out the Traffic page inside area a: bandwidth card on top (52% of
 * the height within [260, 520] px, leaving at least 220 px below), then
 * top talkers and link statistics side by side. */
void page_traffic(Dashboard *d, Rectangle a)
{
    const float gap = SP_LG;

    float top_h = anim_clamp(a.height * 0.52f, 260.0f, 520.0f);
    if (a.height - top_h - gap < 220.0f)
        top_h = a.height - gap - 220.0f;
    Rectangle top = ui_cut_top(&a, top_h);
    ui_cut_top(&a, gap);
    panel_bandwidth(d, top, &d->chart_traffic, "bw_traffic");

    Rectangle talkers = ui_cut_left(&a, floorf((a.width - gap) * 0.5f));
    ui_cut_left(&a, gap);
    panel_talkers(d, talkers);
    panel_link_stats(d, a);
}
