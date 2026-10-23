/* page_devices.c - Devices page: sortable table and detail panel.
 *
 * Left (63% of the width): table of all devices with status filter chips,
 * sortable column headers and per-row hover/selection animation. Columns
 * are dropped by priority when the table gets narrow. Right: detail panel
 * for the selected device with six metric tiles and a latency chart.
 * Selection lives in Dashboard.selected_device and is also set from other
 * pages (dashboard_open_device). Called from dashboard_frame via pages.h.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "anim.h"
#include "pages.h"
#include "panels.h"

#define ROW_H 40.0f    /* table row pitch incl. 2 px gap, px */
#define HEAD_H 28.0f   /* table header height, px */

/* ---- Columns ---- */

/* Static description of a table column. */
typedef struct {
    const char *label;
    float width;       /* 0 = takes the remaining width */
    bool right;        /* right-aligned (numeric) */
    bool sortable;
    SortKey sort;      /* key applied on click; unused when not sortable */
} Column;

enum { COL_STATUS, COL_DEVICE, COL_LATENCY, COL_LOSS, COL_TRAFFIC, COL_TREND, COL_ADDRESS, COL_COUNT };

/* Column definitions indexed by the COL_* ids; the order is the on-screen
 * left-to-right order. */
static const Column COLUMNS[COL_COUNT] = {
    [COL_STATUS]  = { "STATUS",  84.0f, false, true,  SORT_STATUS },
    [COL_DEVICE]  = { "DEVICE",   0.0f, false, true,  SORT_NAME },
    [COL_LATENCY] = { "LATENCY", 76.0f, true,  true,  SORT_LATENCY },
    [COL_LOSS]    = { "LOSS",    66.0f, true,  true,  SORT_LOSS },
    [COL_TRAFFIC] = { "TRAFFIC", 96.0f, true,  true,  SORT_TRAFFIC },
    [COL_TREND]   = { "TREND",   92.0f, false, false, SORT_STATUS },
    [COL_ADDRESS] = { "ADDRESS", 100.0f, false, false, SORT_STATUS },
};

/* Narrowest the flexible DEVICE column may get before optional columns
 * are dropped (px; room for the device name and its kind line). */
static const float DEVICE_COL_MIN = 120.0f;

/* Resolved horizontal layout: visibility and x/w (relative to the table
 * left edge) for each column. */
typedef struct {
    bool visible[COL_COUNT];
    float x[COL_COUNT];
    float w[COL_COUNT];
} ColumnLayout;

/* Computes which columns fit into width px and where they go. The DEVICE
 * column is flexible: it gets whatever the fixed-width columns leave over.
 * Optional columns are dropped in the fixed order ADDRESS, TREND, TRAFFIC
 * until it reaches DEVICE_COL_MIN. */
static ColumnLayout layout_columns(float width)
{
    ColumnLayout l;
    memset(&l, 0, sizeof l);
    for (int i = 0; i < COL_COUNT; i++)
        l.visible[i] = true;

    /* 1. Drop optional columns one at a time until the device column has
     * enough room (after three drops nothing more can go). */
    const int drop_order[3] = { COL_ADDRESS, COL_TREND, COL_TRAFFIC };
    for (int k = 0; k <= 3; k++) {
        float fixed = 0.0f;
        for (int i = 0; i < COL_COUNT; i++) {
            if (l.visible[i] && i != COL_DEVICE)
                fixed += COLUMNS[i].width;
        }
        if (width - fixed >= DEVICE_COL_MIN || k == 3)
            break;
        l.visible[drop_order[k]] = false;
    }

    /* 2. Assign x and width to the surviving columns. */
    float fixed = 0.0f;
    for (int i = 0; i < COL_COUNT; i++) {
        if (l.visible[i] && i != COL_DEVICE)
            fixed += COLUMNS[i].width;
    }
    float x = 0.0f;
    for (int i = 0; i < COL_COUNT; i++) {
        if (!l.visible[i])
            continue;
        l.w[i] = i == COL_DEVICE ? width - fixed : COLUMNS[i].width;
        l.x[i] = x;
        x += l.w[i];
    }
    return l;
}

/* ------------------------------------------------------------ Sorting */

/* Numeric sort value of a device for key (SORT_NAME is handled in
 * precedes since it is a string compare). */
static float sort_value(const Device *dev, SortKey key)
{
    switch (key) {
    case SORT_STATUS:  return (float)dev->status;
    case SORT_LATENCY: return dev->latency_ms;
    case SORT_LOSS:    return dev->loss_pct;
    case SORT_TRAFFIC: return dev->rx_mbps + dev->tx_mbps;
    default:           return 0.0f;
    }
}

/* Strict ordering of devices a and b under the current sort key and
 * direction. Ties fall back to the device index so the order is total and
 * rows do not swap back and forth between frames. */
static bool precedes(const Dashboard *d, int a, int b)
{
    const Device *da = &d->data.devices[a], *db = &d->data.devices[b];
    int cmp;
    if (d->sort_key == SORT_NAME) {
        cmp = strcmp(da->name, db->name);
    } else {
        float va = sort_value(da, d->sort_key), vb = sort_value(db, d->sort_key);
        cmp = va < vb ? -1 : (va > vb ? 1 : 0);
    }
    if (d->sort_desc)
        cmp = -cmp;
    return cmp != 0 ? cmp < 0 : a < b;
}

/* Fills order[] with the indices of the devices that pass the status
 * filter, sorted by precedes (insertion sort: 14 elements, runs every
 * frame). Returns the number of entries. */
static int build_order(const Dashboard *d, int *order)
{
    int n = 0;
    for (int i = 0; i < DEVICE_COUNT; i++) {
        if (d->device_filter != FILTER_ALL && (int)d->data.devices[i].status != d->device_filter)
            continue;
        int pos = n++;
        while (pos > 0 && precedes(d, i, order[pos - 1])) {
            order[pos] = order[pos - 1];
            pos--;
        }
        order[pos] = i;
    }
    return n;
}

/* -------------------------------------------------------------- Table */

/* Draws the column headings in head and handles sort clicks. Clicking the
 * active column flips the direction; clicking another column selects it,
 * descending for numeric metrics (largest first) and ascending for status
 * and name. */
static void draw_table_header(Dashboard *d, Rectangle head, const ColumnLayout *l)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    const Palette *c = &t->c;

    for (int i = 0; i < COL_COUNT; i++) {
        if (!l->visible[i])
            continue;
        const Column *col = &COLUMNS[i];
        Rectangle cell = { head.x + l->x[i], head.y, l->w[i], head.height };
        bool active = col->sortable && d->sort_key == col->sort;
        bool hot = col->sortable && ui_hot(ui, cell);
        Color tc = active ? c->text : (hot ? c->text_dim : c->text_faint);

        /* Text x: right-aligned columns reserve 14 px for the sort arrow;
         * the small offsets line the heading up with the cell content. */
        float tw = theme_measure(t, FONT_CAPTION_BOLD, col->label).x;
        float tx = col->right ? cell.x + cell.width - tw - (active ? 14.0f : 0.0f) : cell.x;
        if (i == COL_STATUS)
            tx += 4.0f;
        else if (i == COL_TREND || i == COL_ADDRESS)
            tx += 12.0f;
        theme_text(t, FONT_CAPTION_BOLD, col->label, tx, head.y + 8.0f, tc);
        if (active) {
            IconId arrow = d->sort_desc ? ICON_ARROW_DOWN : ICON_ARROW_UP;
            ui_icon(arrow, (Vector2){ tx + tw + 8.0f, head.y + 14.0f }, 9.0f, c->accent);
        }
        if (hot && ui->pressed) {
            if (d->sort_key == col->sort) {
                d->sort_desc = !d->sort_desc;
            } else {
                d->sort_key = col->sort;
                d->sort_desc = col->sort == SORT_LATENCY || col->sort == SORT_LOSS || col->sort == SORT_TRAFFIC;
            }
        }
    }
    DrawRectangle((int)head.x, (int)(head.y + head.height) - 1, (int)head.width, 1, c->border);
}

/* Draws the row of device i inside row: selection/hover background, then
 * every visible column. A DOWN device shows dashes and dimmed text, since
 * its measured values are not meaningful. Latency and loss turn amber at
 * the device WARNING thresholds. */
static void draw_row(Dashboard *d, Rectangle row, int i, const ColumnLayout *l)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    const Palette *c = &t->c;
    const Device *dev = &d->data.devices[i];
    bool selected = d->selected_device == i;
    bool hot = ui_hot(ui, row);
    float hv = ui_anim(ui, ui_key_i(ui_key("dev_row_hover"), (uint32_t)i), hot ? 1.0f : 0.0f, 16.0f);
    float sv = ui_anim(ui, ui_key_i(ui_key("dev_row_sel"), (uint32_t)i), selected ? 1.0f : 0.0f, 14.0f);

    /* 1. Background: hover wash blended into the selection tint, plus an
     * accent stripe that fades in with the selection. */
    ui_rect(row, RADIUS_SM, theme_mix(theme_alpha(c->text, 0.05f * hv), theme_alpha(c->accent, 0.14f), sv));
    if (sv > 0.01f)
        ui_rect((Rectangle){ row.x, row.y + 9.0f, 3.0f, row.height - 18.0f }, 1.5f, theme_alpha(c->accent, sv));

    Color sc = status_color(c, dev->status);
    float cy = row.y + row.height * 0.5f;
    float ty = row.y + (row.height - t->font_px[FONT_BODY]) * 0.5f;
    bool down = dev->status == STATUS_DOWN;

    /* 2. Status badge. */
    if (l->visible[COL_STATUS]) {
        static const char *const labels[3] = { "UP", "WARN", "DOWN" };
        Rectangle b = { row.x + l->x[COL_STATUS] + 4.0f, cy - 10.0f, 52.0f, 20.0f };
        ui_badge(ui, b, labels[dev->status], sc);
    }

    /* 3. Name with the device kind underneath. */
    Rectangle dc = { row.x + l->x[COL_DEVICE], row.y, l->w[COL_DEVICE], row.height };
    theme_text_fit(t, FONT_BODY_BOLD, dev->name, dc.x, row.y + 5.0f, dc.width - 8.0f, down ? c->text_dim : c->text);
    theme_text_fit(t, FONT_CAPTION, device_kind_name(dev->kind), dc.x, row.y + 22.0f, dc.width - 8.0f, c->text_faint);

    /* 4. Numeric columns, right-aligned 2 px inside their cell. */
    char buf[32];
    if (l->visible[COL_LATENCY]) {
        if (down)
            snprintf(buf, sizeof buf, "-");
        else
            snprintf(buf, sizeof buf, "%.0f ms", (double)dev->latency_ms);
        Color lc = dev->latency_ms >= DEVICE_LATENCY_WARN_MS ? c->warn : c->text;
        theme_text_right(t, FONT_MONO, buf, row.x + l->x[COL_LATENCY] + l->w[COL_LATENCY] - 2.0f, ty, down ? c->text_faint : lc);
    }
    if (l->visible[COL_LOSS]) {
        if (down)
            snprintf(buf, sizeof buf, "100 %%");
        else
            snprintf(buf, sizeof buf, "%.2f %%", (double)dev->loss_pct);
        Color lc = down ? c->crit : (dev->loss_pct >= DEVICE_LOSS_WARN_PCT ? c->warn : c->text_dim);
        theme_text_right(t, FONT_MONO, buf, row.x + l->x[COL_LOSS] + l->w[COL_LOSS] - 2.0f, ty, lc);
    }
    if (l->visible[COL_TRAFFIC]) {
        if (down)
            snprintf(buf, sizeof buf, "-");
        else
            format_rate(buf, sizeof buf, dev->rx_mbps + dev->tx_mbps);
        theme_text_right(t, FONT_MONO, buf, row.x + l->x[COL_TRAFFIC] + l->w[COL_TRAFFIC] - 2.0f, ty, down ? c->text_faint : c->text_dim);
    }

    /* 5. Latency trend (same per-device scale as the compact list) and address. */
    if (l->visible[COL_TREND] && !down) {
        float values[RING_CAP];
        int n = ring_copy(&dev->latency_hist, values);
        Rectangle sp = { row.x + l->x[COL_TREND] + 12.0f, row.y + 11.0f, l->w[COL_TREND] - 28.0f, row.height - 22.0f };
        chart_sparkline(sp, values, n, 0.0f, fmaxf(60.0f, dev->base_latency_ms * 2.5f), sc, false);
    }
    if (l->visible[COL_ADDRESS])
        theme_text(t, FONT_MONO_SMALL, dev->ip, row.x + l->x[COL_ADDRESS] + 12.0f, ty + 1.0f, c->text_faint);
}

/* Table card: status filter chips in the header, column headings, and the
 * scrolling, animated device rows. Clicking a row selects it; clicking the
 * selected row again clears the selection. */
static void panel_device_table(Dashboard *d, Rectangle r)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    const Palette *c = &t->c;
    uint32_t key = ui_key("device_table");
    Rectangle content = ui_panel(ui, r, "All devices", key);

    /* 1. Filter chips: All / Up / Warning / Down. Built right to left so
     * they read left to right. Chip index i maps to FILTER_ALL for 0 and
     * to the DeviceStatus i - 1 otherwise. */
    Rectangle hdr = ui_panel_header(r);
    hdr.y += 10.0f;
    hdr.height = 26.0f;
    static const char *const labels[4] = { "All", "Up", "Warning", "Down" };
    for (int i = 3; i >= 0; i--) {
        int filter = i == 0 ? FILTER_ALL : i - 1;
        Rectangle chip = ui_cut_right(&hdr, theme_measure(t, FONT_SMALL, labels[i]).x + 24.0f);
        ui_cut_right(&hdr, 6.0f);
        Color tint = filter == FILTER_ALL ? c->accent : status_color(c, (DeviceStatus)filter);
        if (ui_chip(ui, ui_key_i(key, 10 + (uint32_t)i), chip, labels[i], d->device_filter == filter, tint))
            d->device_filter = filter;
    }

    /* 2. Column layout and header; 8 px are left free on the right for
     * the scrollbar. */
    ColumnLayout layout = layout_columns(content.width - 8.0f);
    Rectangle head = ui_cut_top(&content, HEAD_H);
    draw_table_header(d, (Rectangle){ head.x, head.y, content.width - 8.0f, head.height }, &layout);
    ui_cut_top(&content, 4.0f);

    /* 3. Sorted, filtered rows in a scroll view. Each device has its own
     * animated y so rows glide when the order changes. */
    int order[DEVICE_COUNT];
    int n = build_order(d, order);
    float content_h = (float)n * ROW_H;
    uint32_t scroll_key = ui_key_i(key, 1);
    float off = ui_scroll_begin(ui, scroll_key, content, content_h);

    for (int pos = 0; pos < n; pos++) {
        int i = order[pos];
        float y = ui_anim(ui, ui_key_i(ui_key("dev_row_y"), (uint32_t)i), (float)pos * ROW_H, 14.0f);
        Rectangle row = { content.x, content.y + y - off, content.width - 8.0f, ROW_H - 2.0f };
        if (row.y + row.height < content.y || row.y > content.y + content.height)
            continue;
        draw_row(d, row, i, &layout);
        /* active == 0: ignore clicks that belong to a scrollbar drag. */
        if (ui_hover(ui, row) && ui->pressed && ui->active == 0)
            dashboard_select_device(d, d->selected_device == i ? -1 : i);
    }
    ui_scroll_end(ui, scroll_key, content, content_h);

    if (n == 0)
        theme_text_center(t, FONT_BODY, "No devices match this filter", content.x + content.width * 0.5f, content.y + 24.0f, c->text_faint);
}

/* ------------------------------------------------------------- Detail */

/* Formats an uptime with the two most significant units: "12d 4h",
 * "5h 32m" or "7m 9s". */
static void format_uptime(char *buf, size_t cap, float seconds)
{
    int s = (int)seconds;
    if (s >= 86400)
        snprintf(buf, cap, "%dd %dh", s / 86400, (s % 86400) / 3600);
    else if (s >= 3600)
        snprintf(buf, cap, "%dh %dm", s / 3600, (s % 3600) / 60);
    else
        snprintf(buf, cap, "%dm %ds", s / 60, s % 60);
}

/* Draws a label (small caption) with its value below, at r.x/r.y. */
static void draw_stat_tile(const Theme *t, Rectangle r, const char *label, const char *value, Color color)
{
    theme_text(t, FONT_CAPTION_BOLD, label, r.x, r.y, t->c.text_faint);
    theme_text(t, FONT_MONO_BOLD, value, r.x, r.y + 17.0f, color);
}

/* Detail card for the selected device: status badge, kind and address,
 * six metric tiles and a 30 s latency chart (when there is room). Shows a
 * hint instead when no valid device is selected. */
static void panel_device_detail(Dashboard *d, Rectangle r)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    const Palette *c = &t->c;
    /* selected_device is -1 when nothing is selected. */
    bool valid = d->selected_device >= 0 && d->selected_device < DEVICE_COUNT;
    const Device *dev = valid ? &d->data.devices[d->selected_device] : NULL;

    Rectangle content = ui_panel(ui, r, valid ? dev->name : "Device details", ui_key("device_detail"));
    if (!valid) {
        theme_text_center(t, FONT_BODY, "Select a device to see details",
                          content.x + content.width * 0.5f, content.y + content.height * 0.4f, c->text_faint);
        return;
    }

    /* 1. Status badge at the right end of the header, kind and address below the title. */
    Color sc = status_color(c, dev->status);
    static const char *const status_labels[3] = { "UP", "WARNING", "DOWN" };
    Rectangle hdr = ui_panel_header(r);
    float bw = ui_badge_width(ui, status_labels[dev->status]);
    ui_badge(ui, (Rectangle){ hdr.x + hdr.width - bw, hdr.y + 13.0f, bw, 20.0f }, status_labels[dev->status], sc);

    char line[64];
    snprintf(line, sizeof line, "%s   %s", device_kind_name(dev->kind), dev->ip);
    theme_text(t, FONT_SMALL, line, content.x, content.y - 6.0f, c->text_dim);
    ui_cut_top(&content, 22.0f);

    /* 2. Metric tiles: values formatted first, then drawn in a 3 x 2 grid
     * (46 px row pitch). Latency is a dash while the device is down. */
    bool down = dev->status == STATUS_DOWN;
    char v[6][32];
    snprintf(v[0], sizeof v[0], down ? "-" : "%.1f ms", (double)dev->latency_ms);
    snprintf(v[1], sizeof v[1], "%.2f %%", (double)dev->loss_pct);
    format_uptime(v[2], sizeof v[2], dev->uptime_s);
    format_rate(v[3], sizeof v[3], dev->rx_mbps);
    format_rate(v[4], sizeof v[4], dev->tx_mbps);
    format_rate(v[5], sizeof v[5], dev->rx_mbps + dev->tx_mbps);

    static const char *const names[6] = { "LATENCY", "PACKET LOSS", "UPTIME", "INBOUND", "OUTBOUND", "TOTAL" };
    Color cols[6] = {
        dev->latency_ms >= DEVICE_LATENCY_WARN_MS ? c->warn : c->text,
        down ? c->crit : (dev->loss_pct >= DEVICE_LOSS_WARN_PCT ? c->warn : c->text),
        c->text, c->series_in, c->series_out, c->text,
    };
    Rectangle grid = ui_cut_top(&content, 2.0f * 46.0f);
    float cw = grid.width / 3.0f;
    for (int i = 0; i < 6; i++) {
        Rectangle tile = { grid.x + cw * (float)(i % 3), grid.y + 46.0f * (float)(i / 3), cw, 40.0f };
        draw_stat_tile(t, tile, names[i], v[i], cols[i]);
    }

    /* 3. Latency chart over the 30 s history held per device; skipped
     * when less than 80 px of height remain. */
    ui_cut_top(&content, 8.0f);
    theme_text(t, FONT_CAPTION_BOLD, "LATENCY, LAST 30 S", content.x, content.y, c->text_faint);
    ui_cut_top(&content, 18.0f);
    if (content.height < 80.0f)
        return;

    float values[RING_CAP];
    int n = ring_copy(&dev->latency_hist, values);
    LineSeries series[1] = { { "Latency", c->accent, values, 1.0f } };
    LineChartParams p = {
        .area = content, .count = n, .interval_s = SIM_TICK_S, .window_s = 30.0f,
        .scroll_s = d->data.tick_acc, .axis_fmt = "%.0f", .min_y_max = 20.0f,
    };
    ChartHover h = chart_line(t, &d->chart_device, &p, series, 1, ui->mouse, ui_hover(ui, content), ui->dt);

    /* 4. Tooltip: age of the hovered sample, as in the bandwidth panel. */
    if (h.active) {
        float behind = (float)(n - 1 - h.index) * SIM_TICK_S + d->data.tick_acc - SIM_TICK_S;
        char title[24], value[24];
        if (behind < 1.0f)
            snprintf(title, sizeof title, "Now");
        else
            snprintf(title, sizeof title, "%.0f s ago", (double)behind);
        snprintf(value, sizeof value, "%.1f ms", (double)values[h.index]);
        ui_tip_begin(ui, title);
        ui_tip_row(ui, &c->accent, "Latency", value);
    }
}

/* ---- Page ---- */

/* Lays out the Devices page inside area a: table (63% of the width) and
 * the detail panel in the remainder. */
void page_devices(Dashboard *d, Rectangle a)
{
    const float gap = SP_LG;
    Rectangle table = ui_cut_left(&a, floorf((a.width - gap) * 0.63f));
    ui_cut_left(&a, gap);
    panel_device_table(d, table);
    panel_device_detail(d, a);
}
