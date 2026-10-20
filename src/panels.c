/* panels.c - dashboard cards shared by several pages.
 *
 * Sections: helpers, Bandwidth (line chart card), Gauges (latency and loss
 * with sparklines) and Top talkers (animated bars). Each panel_* function
 * draws one titled card with ui_panel, reads Dashboard state, and handles
 * its own hover, tooltip and click. Declared in panels.h; composed into
 * pages by page_overview.c and page_traffic.c.
 */
#include "panels.h"

#include <math.h>
#include <stdio.h>

#include "anim.h"

#define TALKER_ROWS 8              /* most bars shown; also the size of the local arrays */

/* ----------------------------------------------------------- Helpers */

/* Minimum, average and maximum of all samples in the ring (all 0 when the
 * ring is empty). */
static void ring_stats(const Ring *r, float *lo, float *avg, float *hi)
{
    float mn = 1e30f, mx = -1e30f, sum = 0.0f;
    for (int i = 0; i < r->count; i++) {
        float v = ring_at(r, i);
        mn = fminf(mn, v);
        mx = fmaxf(mx, v);
        sum += v;
    }
    *lo = r->count ? mn : 0.0f;
    *hi = r->count ? mx : 0.0f;
    *avg = r->count ? sum / (float)r->count : 0.0f;
}

/* Draws "dot label" and returns its width. Positioned by right edge.
 * The caller subtracts the returned width (plus a gap) to place the next
 * legend entry to the left. */
static float header_legend(const Dashboard *d, float right, float y, Color color, const char *label)
{
    const Theme *t = &d->theme;
    float w = theme_measure(t, FONT_SMALL, label).x;
    theme_text(t, FONT_SMALL, label, right - w, y, t->c.text_dim);
    DrawCircleV((Vector2){ right - w - 10.0f, y + 7.0f }, 3.5f, color);
    return w + 18.0f;
}

/* ---------------------------------------------------------- Bandwidth */

/* Bandwidth card: inbound/outbound line chart for the selected window.
 * r is the card rectangle, chart the caller's axis state, id a string
 * that keeps widget keys unique when the card appears on more than one
 * page. Header controls: range selector (only when the header is wider
 * than 400 px) and legend chips that toggle each series. */
void panel_bandwidth(Dashboard *d, Rectangle r, LineChartState *chart, const char *id)
{
    Ui *ui = &d->ui;
    const Palette *c = &d->theme.c;
    uint32_t key = ui_key(id);
    Rectangle content = ui_panel(ui, r, "Bandwidth", key);

    /* 1. Header controls, cut from the right: range selector, then the
     * legend chips (built back to front so Inbound ends up left of
     * Outbound). A series can only be switched off while the other is on. */
    Rectangle hdr = ui_panel_header(r);
    hdr.y += 10.0f;
    hdr.height = 26.0f;
    if (hdr.width > 400.0f) {
        Rectangle seg = ui_cut_right(&hdr, 120.0f);
        d->range_idx = ui_segmented(ui, ui_key_i(key, 1), seg, RANGE_LABELS, RANGE_COUNT, d->range_idx);
        ui_cut_right(&hdr, 12.0f);
    }
    static const char *const names[2] = { "Inbound", "Outbound" };
    const Color colors[2] = { c->series_in, c->series_out };
    for (int s = 1; s >= 0; s--) {
        Rectangle chip = ui_cut_right(&hdr, 28.0f + theme_measure(&d->theme, FONT_SMALL, names[s]).x);
        ui_cut_right(&hdr, 6.0f);
        if (ui_legend_chip(ui, ui_key_i(key, 20 + (uint32_t)s), chip, names[s], colors[s], d->series_on[s])) {
            bool others = d->series_on[1 - s];
            if (others)
                d->series_on[s] = !d->series_on[s];
        }
    }

    /* 2. Linearise both histories (oldest first, as the chart expects). */
    float in_v[RING_CAP], out_v[RING_CAP];
    int n = ring_copy(&d->data.bw_in, in_v);
    ring_copy(&d->data.bw_out, out_v);

    /* 3. Chart. Series alpha is eased so toggling fades the line; scroll_s
     * is the time since the last tick (sub-tick scrolling); min_y_max of
     * 100 Mbps keeps idle noise from zooming the axis in. */
    LineSeries series[2] = {
        { names[0], c->series_in,  in_v,  ui_anim(ui, ui_key_i(key, 10), d->series_on[0] ? 1.0f : 0.0f, 14.0f) },
        { names[1], c->series_out, out_v, ui_anim(ui, ui_key_i(key, 11), d->series_on[1] ? 1.0f : 0.0f, 14.0f) },
    };
    LineChartParams p = {
        .area = content, .count = n, .interval_s = SIM_TICK_S, .window_s = RANGE_SECONDS[d->range_idx],
        .scroll_s = d->data.tick_acc, .axis_fmt = "%.0f", .min_y_max = 100.0f,
    };
    ChartHover h = chart_line(&d->theme, chart, &p, series, 2, ui->mouse, ui_hover(ui, content), ui->dt);
    if (!h.active)
        return;

    /* 4. Tooltip: age of the hovered sample, using the same one-interval
     * offset as the chart's sample_x. "Now" below 1 s. */
    float behind = (float)(n - 1 - h.index) * SIM_TICK_S + d->data.tick_acc - SIM_TICK_S;
    char title[24], value[24];
    if (behind < 1.0f)
        snprintf(title, sizeof title, "Now");
    else
        snprintf(title, sizeof title, "%.0f s ago", (double)behind);
    ui_tip_begin(ui, title);
    for (int s = 0; s < 2; s++) {
        if (!d->series_on[s])
            continue;
        snprintf(value, sizeof value, "%.1f Mbps", (double)series[s].values[h.index]);
        ui_tip_row(ui, &colors[s], names[s], value);
    }
}

/* ------------------------------------------------------------ Gauges */

/* Description of one gauge: captions, printf format of its readouts and
 * the scale/zone limits in the metric's own unit. */
typedef struct {
    const char *label;
    const char *unit;
    const char *value_fmt;
    float max, warn, crit;
} GaugeSpec;

/* Draws one gauge cell: the arc gauge, an "avg / peak" line and, when the
 * cell is tall enough, a sparkline of the history. shown is the eased
 * value for the arc, live the real current value (tooltip, sparkline
 * colour), hist the metric's history ring. */
static void gauge_cell(Dashboard *d, Rectangle cell, const GaugeSpec *g, float shown, float live, const Ring *hist)
{
    Ui *ui = &d->ui;
    const Palette *c = &d->theme.c;
    Rectangle whole = cell;

    float lo, avg, hi;
    ring_stats(hist, &lo, &avg, &hi);

    /* 1. Layout. Gauge, stats line and sparkline form one group, centred vertically in the cell. */
    const float stats_h = 16.0f, spark_h = 26.0f, gap = 8.0f;
    bool with_spark = cell.height >= 205.0f;
    float extras = gap + stats_h + (with_spark ? gap + spark_h : 0.0f);
    /* Same 1.72 x radius + 24 px label band geometry that chart_gauge uses. */
    float radius = fminf(cell.width * 0.5f - 2.0f, (cell.height - extras - 24.0f) / 1.72f);
    float gauge_h = radius * 1.72f + 24.0f;
    float y0 = cell.y + fmaxf(0.0f, (cell.height - gauge_h - extras) * 0.5f);
    Rectangle gauge = { cell.x, y0, cell.width, gauge_h };
    Rectangle stats = { cell.x, y0 + gauge_h + gap, cell.width, stats_h };
    Rectangle spark = { cell.x, stats.y + stats_h + gap, cell.width, spark_h };

    /* 2. Gauge and the avg/peak line. */
    char value[16], line[64];
    snprintf(value, sizeof value, g->value_fmt, (double)shown);
    chart_gauge(&d->theme, gauge, shown, g->max, g->warn, g->crit, value, g->unit, g->label);

    snprintf(line, sizeof line, g->value_fmt, (double)avg);
    char peak[16];
    snprintf(peak, sizeof peak, g->value_fmt, (double)hi);
    char text[96];
    snprintf(text, sizeof text, "avg %s   peak %s", line, peak);
    theme_text_center(&d->theme, FONT_CAPTION, text, stats.x + stats.width * 0.5f, stats.y + 1.0f, c->text_faint);

    /* 3. Sparkline over the full gauge scale, coloured by the live zone;
     * inset to 80% width so it stays under the arc. */
    if (with_spark) {
        float values[RING_CAP];
        int n = ring_copy(hist, values);
        Color col = chart_zone_color(&d->theme, live, g->max, g->warn, g->crit);
        Rectangle s = { spark.x + spark.width * 0.1f, spark.y, spark.width * 0.8f, spark.height };
        chart_sparkline(s, values, n, 0.0f, g->max, col, true);
    }

    /* 4. Tooltip with the live value, history stats (the ring holds 2
     * minutes) and the zone limits. */
    if (ui_hover(ui, whole)) {
        char row[24];
        ui_tip_begin(ui, g->label);
        snprintf(row, sizeof row, g->value_fmt, (double)live);
        ui_tip_row(ui, NULL, "Current", row);
        snprintf(row, sizeof row, g->value_fmt, (double)avg);
        ui_tip_row(ui, NULL, "Average (2 min)", row);
        snprintf(row, sizeof row, g->value_fmt, (double)hi);
        ui_tip_row(ui, NULL, "Peak (2 min)", row);
        snprintf(row, sizeof row, g->value_fmt, (double)g->warn);
        ui_tip_row(ui, &c->warn, "Warning at", row);
        snprintf(row, sizeof row, g->value_fmt, (double)g->crit);
        ui_tip_row(ui, &c->crit, "Critical at", row);
    }
}

/* Network health card: latency gauge (left half) and loss gauge (right
 * half). The displayed values are eased at rate 6 so the arcs glide
 * between 2 Hz updates instead of stepping. */
void panel_gauges(Dashboard *d, Rectangle r)
{
    Ui *ui = &d->ui;
    const Palette *c = &d->theme.c;
    Rectangle content = ui_panel(ui, r, "Network health", ui_key("gauges"));
    Rectangle hdr = ui_panel_header(r);
    theme_text_right(&d->theme, FONT_SMALL, "network average", hdr.x + hdr.width, hdr.y + 16.0f, c->text_faint);

    d->gauge_latency = anim_approach(d->gauge_latency, d->summary.avg_latency_ms, 6.0f, ui->dt);
    d->gauge_loss = anim_approach(d->gauge_loss, d->summary.avg_loss_pct, 6.0f, ui->dt);

    static const GaugeSpec latency = { "Latency", "ms", "%.0f", GAUGE_LATENCY_MAX_MS, GAUGE_LATENCY_WARN_MS, GAUGE_LATENCY_CRIT_MS };
    static const GaugeSpec loss = { "Packet loss", "%", "%.2f", GAUGE_LOSS_MAX_PCT, GAUGE_LOSS_WARN_PCT, GAUGE_LOSS_CRIT_PCT };

    Rectangle left = ui_cut_left(&content, floorf(content.width * 0.5f));
    gauge_cell(d, ui_pad(left, 2.0f), &latency, d->gauge_latency, d->summary.avg_latency_ms, &d->data.net_latency);
    gauge_cell(d, ui_pad(content, 2.0f), &loss, d->gauge_loss, d->summary.avg_loss_pct, &d->data.net_loss);
}

/* ------------------------------------------------------ Top talkers */

/* Top talkers card: the busiest devices as stacked in/out bars. As many
 * rows as fit (1..TALKER_ROWS). Hovering shows a tooltip with the share
 * of total traffic. */
void panel_talkers(Dashboard *d, Rectangle r)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    const Palette *c = &t->c;
    uint32_t key = ui_key("talkers");
    Rectangle content = ui_panel(ui, r, "Top talkers", key);

    /* 1. Colour legend in the header. */
    Rectangle hdr = ui_panel_header(r);
    float x = hdr.x + hdr.width;
    x -= header_legend(d, x, hdr.y + 15.0f, c->series_out, "Out") + 10.0f;
    header_legend(d, x, hdr.y + 15.0f, c->series_in, "In");

    /* 2. Number of rows from the available height, then the ranking. */
    const float row_h = 32.0f;
    int rows = (int)(content.height / row_h);
    rows = rows < 1 ? 1 : (rows > TALKER_ROWS ? TALKER_ROWS : rows);

    int idx[TALKER_ROWS];
    int n = data_top_talkers(&d->data, idx, rows);

    /* 3. Bar items; the device index is the stable id for row animation. */
    BarItem items[TALKER_ROWS];
    char labels[TALKER_ROWS][24];
    float top = 0.0f;
    for (int i = 0; i < n; i++) {
        const Device *dev = &d->data.devices[idx[i]];
        format_rate(labels[i], sizeof labels[i], dev->rx_mbps + dev->tx_mbps);
        items[i] = (BarItem){ idx[i], dev->name, labels[i], dev->rx_mbps, dev->tx_mbps };
        top = fmaxf(top, dev->rx_mbps + dev->tx_mbps);
    }
    /* Full-bar value: the leader plus 8% headroom, eased slowly (rate 4)
     * so bars do not all rescale at once on a new leader, and at least
     * 120 Mbps so near-idle traffic does not fill the bars. */
    float scale = ui_anim(ui, ui_key_i(key, 1), fmaxf(top * 1.08f, 120.0f), 4.0f);

    int hovered = chart_bars(t, &d->talkers, content, items, n, scale, row_h, ui->mouse, ui_hover(ui, content), ui->dt);
    if (hovered < 0)
        return;

    /* 4. Tooltip for the hovered device. */
    const Device *dev = &d->data.devices[hovered];
    char in[24], out[24], total[24], share[24];
    format_rate(in, sizeof in, dev->rx_mbps);
    format_rate(out, sizeof out, dev->tx_mbps);
    format_rate(total, sizeof total, dev->rx_mbps + dev->tx_mbps);
    float all = d->summary.in_mbps + d->summary.out_mbps;
    snprintf(share, sizeof share, "%.1f %%", all > 0.0f ? (double)((dev->rx_mbps + dev->tx_mbps) / all * 100.0f) : 0.0);
    ui_tip_begin(ui, dev->name);
    ui_tip_row(ui, &c->series_in, "Inbound", in);
    ui_tip_row(ui, &c->series_out, "Outbound", out);
    ui_tip_row(ui, NULL, "Total", total);
    ui_tip_row(ui, NULL, "Share of traffic", share);
}
