/* page_overview.c - Overview page layout.
 *
 * Top band: bandwidth chart (left) and latency/loss gauges (right).
 * Bottom band: three equal columns with the compact device list, top
 * talkers and recent alerts. Only arranges rectangles; the cards
 * themselves are in panels.c. Called from dashboard_frame via pages.h.
 */
#include "anim.h"
#include "pages.h"
#include "panels.h"

/* Lays out and draws the Overview page inside area a. */
void page_overview(Dashboard *d, Rectangle a)
{
    const float gap = SP_LG;

    /* 1. Top band height: 47% of the area within [250, 360] px, but the
     * bottom band keeps at least 230 px even in a short window (this
     * takes priority over the 250 px minimum). */
    float top_h = anim_clamp(a.height * 0.47f, 250.0f, 360.0f);
    if (a.height - top_h - gap < 230.0f)
        top_h = a.height - gap - 230.0f;
    Rectangle top = ui_cut_top(&a, top_h);
    ui_cut_top(&a, gap);

    /* 2. Gauges get 34% of the width, 300-400 px; the chart takes the rest. */
    float gauge_w = anim_clamp(top.width * 0.34f, 300.0f, 400.0f);
    Rectangle gauges = ui_cut_right(&top, gauge_w);
    ui_cut_right(&top, gap);
    panel_bandwidth(d, top, &d->chart_overview, "bw_overview");
    panel_gauges(d, gauges);

    /* 3. Bottom band: three columns; floor keeps column edges on whole
     * pixels, the last column (alerts) absorbs the remainder. */
    float col_w = floorf((a.width - 2.0f * gap) / 3.0f);
    Rectangle devices = ui_cut_left(&a, col_w);
    ui_cut_left(&a, gap);
    Rectangle talkers = ui_cut_left(&a, col_w);
    ui_cut_left(&a, gap);
    panel_devices_compact(d, devices);
    panel_talkers(d, talkers);
    panel_alerts_compact(d, a);
}
