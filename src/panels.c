/* panels.c - dashboard cards shared by several pages.
 *
 * Sections: helpers, Bandwidth (line chart card), Gauges (latency and loss
 * with sparklines), Top talkers (animated bars), Compact devices, and
 * Alerts (shared alert row plus the compact list). Each panel_* function
 * draws one titled card with ui_panel, reads Dashboard state, and handles
 * its own hover, tooltip and click. Declared in panels.h; composed into
 * pages by page_overview.c and page_traffic.c.
 */
#include "panels.h"

#include <math.h>
#include <stdio.h>

#include "anim.h"

#define ALERT_COMPACT_ROW_H 50.0f  /* two-line row of the compact alert list, px */
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
 * of total traffic; clicking opens the device on the Devices page. */
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

    /* 4. Tooltip and click for the hovered device. */
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
    ui->cursor_hand = true;
    if (ui->pressed)
        dashboard_open_device(d, hovered);
}

/* ---------------------------------------------------- Compact devices */

/* Compact device list: unhealthy devices first, one row each with status
 * dot, name, latency sparkline and latency value. The header shows the
 * count per status. Rows slide to their new place when the order changes;
 * clicking a row opens the device on the Devices page. */
void panel_devices_compact(Dashboard *d, Rectangle r)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    const Palette *c = &t->c;
    uint32_t key = ui_key("devices_compact");
    Rectangle content = ui_panel(ui, r, "Devices", key);

    /* 1. Header: status counts, placed right to left in the order down,
     * warning, up (so "up" ends up leftmost). */
    Rectangle hdr = ui_panel_header(r);
    float x = hdr.x + hdr.width;
    const DeviceStatus order_status[3] = { STATUS_DOWN, STATUS_WARNING, STATUS_UP };
    const int counts[3] = { d->summary.down, d->summary.warning, d->summary.up };
    for (int i = 0; i < 3; i++) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", counts[i]);
        x -= header_legend(d, x, hdr.y + 15.0f, status_color(c, order_status[i]), buf) + 8.0f;
    }

    /* 2. Display order: three passes (down, warning, up) over the device
     * array. Unhealthy devices first, stable within each group. */
    int order[DEVICE_COUNT], n = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < DEVICE_COUNT; i++) {
            if (d->data.devices[i].status == order_status[pass])
                order[n++] = i;
        }
    }

    /* 3. Scrolling list. Each device has its own y slot (keyed by device
     * index) eased toward its position; rows outside the view are skipped. */
    const float row_h = 32.0f;
    float content_h = (float)n * row_h;
    uint32_t scroll_key = ui_key("devices_compact_scroll");
    float off = ui_scroll_begin(ui, scroll_key, content, content_h);

    for (int pos = 0; pos < n; pos++) {
        int i = order[pos];
        const Device *dev = &d->data.devices[i];
        float y = ui_anim(ui, ui_key_i(key, 100 + (uint32_t)i), (float)pos * row_h, 14.0f);
        Rectangle row = { content.x, content.y + y - off, content.width - 8.0f, row_h - 2.0f };
        if (row.y + row.height < content.y || row.y > content.y + content.height)
            continue;

        bool hot = ui_hot(ui, row);
        float hv = ui_anim(ui, ui_key_i(key, 200 + (uint32_t)i), hot ? 1.0f : 0.0f, 16.0f);
        ui_rect(row, RADIUS_SM, theme_alpha(c->text, 0.055f * hv));

        /* Status dot pulses for anything that is not UP. */
        Color sc = status_color(c, dev->status);
        ui_status_dot(ui, (Vector2){ row.x + 12.0f, row.y + row.height * 0.5f }, 3.5f, sc, dev->status != STATUS_UP);

        /* Name takes the width left of the sparkline and latency columns. */
        float lat_w = 58.0f, spark_w = 56.0f;
        float text_y = row.y + (row.height - t->font_px[FONT_BODY]) * 0.5f;
        Color name_col = dev->status == STATUS_DOWN ? c->text_dim : c->text;
        theme_text_fit(t, FONT_BODY, dev->name, row.x + 28.0f, text_y, row.width - 28.0f - lat_w - spark_w - 16.0f, name_col);

        /* Sparkline scale 0..max(60 ms, 2.5 x base latency): a per-device
         * range that makes a spike visible relative to normal. */
        float values[RING_CAP];
        int cnt = ring_copy(&dev->latency_hist, values);
        Rectangle sp = { row.x + row.width - lat_w - spark_w - 8.0f, row.y + 8.0f, spark_w, row.height - 16.0f };
        if (dev->status != STATUS_DOWN)
            chart_sparkline(sp, values, cnt, 0.0f, fmaxf(60.0f, dev->base_latency_ms * 2.5f), sc, false);

        char lat[16];
        if (dev->status == STATUS_DOWN)
            snprintf(lat, sizeof lat, "down");
        else
            snprintf(lat, sizeof lat, "%.0f ms", (double)dev->latency_ms);
        theme_text_right(t, FONT_MONO_SMALL, lat, row.x + row.width - 4.0f, text_y + 1.0f,
                         dev->status == STATUS_DOWN ? c->crit : c->text_dim);

        /* active == 0: ignore clicks that belong to a scrollbar drag. */
        if (hot && ui->pressed && ui->active == 0)
            dashboard_open_device(d, i);
    }
    ui_scroll_end(ui, scroll_key, content, content_h);
}

/* -------------------------------------------------------------- Alerts */

/* Draws one alert row inside row. alpha is the fade-in factor from the
 * list animation; acknowledged alerts are dimmed to 50%. full = false is
 * the compact two-line form (message; severity, source and time),
 * full = true the table form (badge, time, source, message, state).
 * hover highlights the row and turns the state label into "Acknowledge". */
void alert_row_draw(Dashboard *d, Rectangle row, const Alert *a, float alpha, bool full, bool hover)
{
    const Theme *t = &d->theme;
    const Palette *c = &t->c;
    Color sev = severity_color(c, a->severity);
    float dim = a->acknowledged ? 0.5f : 1.0f;
    float al = alpha * dim;
    char clock[16];
    format_clock(clock, sizeof clock, a->when);

    /* Hover wash and the severity stripe on the left edge. */
    if (hover)
        ui_rect(row, RADIUS_SM, theme_alpha(c->text, 0.055f * alpha));
    ui_rect((Rectangle){ row.x + 2.0f, row.y + 8.0f, 3.0f, row.height - 16.0f }, 1.5f, theme_alpha(sev, al));

    /* Compact form. */
    if (!full) {
        float tx = row.x + 16.0f;
        theme_text_fit(t, FONT_BODY, a->message, tx, row.y + 8.0f, row.width - 24.0f, theme_alpha(c->text, al));
        const char *name = alert_severity_name(a->severity);
        theme_text(t, FONT_CAPTION_BOLD, name, tx, row.y + 28.0f, theme_alpha(sev, al));
        char rest[64];
        snprintf(rest, sizeof rest, "%s  -  %s", a->source, clock);
        float nw = theme_measure(t, FONT_CAPTION_BOLD, name).x;
        theme_text_fit(t, FONT_CAPTION, rest, tx + nw + 8.0f, row.y + 28.0f, row.width - nw - 40.0f, theme_alpha(c->text_faint, alpha));
        if (a->acknowledged)
            ui_icon(ICON_CHECK, (Vector2){ row.x + row.width - 14.0f, row.y + 16.0f }, 12.0f, theme_alpha(c->text_faint, alpha));
        return;
    }

    /* Full form: severity badge, time column, optional source column
     * (only when the row is wider than 640 px), message, state label. */
    float ty = row.y + (row.height - t->font_px[FONT_BODY]) * 0.5f;
    const char *name = alert_severity_name(a->severity);
    Rectangle badge = { row.x + 16.0f, row.y + (row.height - 20.0f) * 0.5f, 68.0f, 20.0f };
    ui_rect(badge, 10.0f, theme_alpha(sev, 0.14f * al));
    theme_text_center(t, FONT_CAPTION_BOLD, name, badge.x + badge.width * 0.5f, badge.y + 4.0f, theme_alpha(sev, al));

    float x = badge.x + badge.width + 16.0f;
    theme_text(t, FONT_MONO_SMALL, clock, x, ty + 1.0f, theme_alpha(c->text_dim, al));
    x += 78.0f;

    /* right_w reserves space for the state label so the message never runs under it. */
    float right_w = 124.0f;
    if (row.width > 640.0f) {
        theme_text_fit(t, FONT_BODY_BOLD, a->source, x, ty, 112.0f, theme_alpha(c->text, al));
        x += 130.0f;
    }
    theme_text_fit(t, FONT_BODY, a->message, x, ty, row.x + row.width - right_w - x, theme_alpha(c->text_dim, al * 1.15f > 1.0f ? 1.0f : al * 1.15f));

    const char *state = a->acknowledged ? "Acknowledged" : (hover ? "Acknowledge" : "Open");
    Color sc = a->acknowledged ? c->text_faint : (hover ? c->accent : c->text_dim);
    theme_text_right(t, FONT_SMALL, state, row.x + row.width - 16.0f, ty, theme_alpha(sc, alpha));
}

/* Recent alerts card: the newest 40 alerts in a scrolling list with a
 * "View all" chip. New alerts slide in from above and fade in; clicking
 * a row opens the Alerts page. */
void panel_alerts_compact(Dashboard *d, Rectangle r)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    uint32_t key = ui_key("alerts_compact");
    Rectangle content = ui_panel(ui, r, "Recent alerts", key);

    /* 1. "View all" chip in the header. */
    Rectangle hdr = ui_panel_header(r);
    hdr.y += 10.0f;
    hdr.height = 26.0f;
    const char *label = "View all";
    Rectangle chip = ui_cut_right(&hdr, theme_measure(t, FONT_SMALL, label).x + 24.0f);
    if (ui_chip(ui, ui_key_i(key, 1), chip, label, false, t->c.accent))
        dashboard_navigate(d, PAGE_ALERTS);

    /* 2. Scrolling list, capped at 40 rows. */
    int total = data_alert_count(&d->data);
    int n = total < 40 ? total : 40;
    float content_h = (float)n * ALERT_COMPACT_ROW_H;
    uint32_t scroll_key = ui_key("alerts_compact_scroll");
    float off = ui_scroll_begin(ui, scroll_key, content, content_h);

    for (int i = 0; i < n; i++) {
        const Alert *a = data_alert(&d->data, i);
        /* Per-alert animation slots keyed by serial, so a row keeps its
         * state while its list position shifts. The 1000 offset keeps the
         * keys apart from the small widget indices used above. */
        uint32_t ak = ui_key_i(key, 1000 + a->serial);
        bool is_new = d->data.tick_count - a->tick <= 2;
        bool fresh, fresh_a;
        float *ys = ui_slot(ui, ak, &fresh);
        float *as = ui_slot(ui, ui_key_i(ak, 1), &fresh_a);
        float slot_y = (float)i * ALERT_COMPACT_ROW_H;
        /* A brand-new alert starts one row above and transparent and
         * slides/fades in; an alert first seen later (page opened) is
         * placed directly so the whole list does not animate. */
        if (fresh)
            *ys = is_new ? slot_y - ALERT_COMPACT_ROW_H : slot_y;
        if (fresh_a)
            *as = is_new ? 0.0f : 1.0f;
        *ys = anim_approach(*ys, slot_y, 14.0f, ui->dt);
        *as = anim_approach(*as, 1.0f, 8.0f, ui->dt);

        Rectangle row = { content.x, content.y + *ys - off, content.width - 8.0f, ALERT_COMPACT_ROW_H - 2.0f };
        if (row.y + row.height < content.y || row.y > content.y + content.height)
            continue;
        bool hot = ui_hot(ui, row);
        alert_row_draw(d, row, a, *as, false, hot);
        if (hot && ui->pressed && ui->active == 0)
            dashboard_navigate(d, PAGE_ALERTS);
    }
    ui_scroll_end(ui, scroll_key, content, content_h);

    if (total == 0)
        theme_text_center(t, FONT_BODY, "No alerts", content.x + content.width * 0.5f, content.y + 20.0f, t->c.text_faint);
}
