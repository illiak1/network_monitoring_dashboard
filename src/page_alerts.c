/* page_alerts.c - Alerts page: severity KPI cards and the full alert log.
 *
 * Top row: four clickable cards (all / critical / warning / info) that
 * double as severity filters. Below: the scrolling log with one table-style
 * row per alert (drawn by alert_row_draw in panels.c); clicking a row
 * toggles its acknowledgement. Called from dashboard_frame via pages.h.
 */
#include <stdio.h>

#include "anim.h"
#include "pages.h"
#include "panels.h"

#define ROW_H 44.0f   /* height of one log row including its 2 px gap, px */

/* ---- KPI cards ---- */

/* Draws one card showing a count and acts as a filter toggle. filter is
 * FILTER_ALL or a severity; count is the log size for "all" and the number
 * of open (unacknowledged) alerts otherwise. Clicking selects the filter,
 * clicking the active severity card again clears it. A zero count is
 * dimmed (except on the "all" card). */
static void kpi_card(Dashboard *d, Rectangle r, int filter, const char *label, int count, Color color)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    const Palette *c = &t->c;
    /* filter + 1 maps FILTER_ALL (-1) to key index 0. */
    uint32_t key = ui_key_i(ui_key("alert_kpi"), (uint32_t)(filter + 1));

    bool hot = ui_hot(ui, r);
    bool active = d->alert_filter == filter;
    float hv = ui_anim(ui, key, hot ? 1.0f : 0.0f, 16.0f);
    float av = ui_anim(ui, ui_key_i(key, 1), active ? 1.0f : 0.0f, 14.0f);

    ui_rect(r, RADIUS_MD, theme_mix(c->panel, c->panel_hover, hv));
    ui_rect(r, RADIUS_MD, theme_alpha(color, 0.07f * av));
    ui_rect_outline(r, RADIUS_MD, 1.0f, theme_mix(theme_mix(c->border, c->border_strong, hv), theme_alpha(color, 0.8f), av));

    /* Number on the left; the 66 px offset reserves its width for the label. */
    char buf[16];
    snprintf(buf, sizeof buf, "%d", count);
    theme_text(t, FONT_MONO_LARGE, buf, r.x + 18.0f, r.y + (r.height - t->font_px[FONT_MONO_LARGE]) * 0.5f, count > 0 || filter == FILTER_ALL ? color : c->text_faint);
    theme_text(t, FONT_BODY_BOLD, label, r.x + 18.0f + 66.0f, r.y + r.height * 0.5f - 15.0f, c->text);
    theme_text(t, FONT_SMALL, filter == FILTER_ALL ? "in log" : "open", r.x + 18.0f + 66.0f, r.y + r.height * 0.5f + 3.0f, c->text_faint);

    if (hot && ui->pressed)
        d->alert_filter = active && filter != FILTER_ALL ? FILTER_ALL : filter;
}

/* ---- Alert log ---- */

/* True if the alert passes the current severity filter. */
static bool matches(const Dashboard *d, const Alert *a)
{
    return d->alert_filter == FILTER_ALL || (int)a->severity == d->alert_filter;
}

/* Draws the log card: "Acknowledge all" button in the header and the
 * filtered, scrolling list below. Rows are animated per alert (slide-in
 * of new alerts, reflow when the filter changes). */
static void panel_alert_log(Dashboard *d, Rectangle r)
{
    Ui *ui = &d->ui;
    const Theme *t = &d->theme;
    const Palette *c = &t->c;
    uint32_t key = ui_key("alert_log");
    Rectangle content = ui_panel(ui, r, "Alert log", key);

    /* 1. Header button. */
    Rectangle hdr = ui_panel_header(r);
    hdr.y += 9.0f;
    hdr.height = 28.0f;
    Rectangle btn = ui_cut_right(&hdr, 140.0f);
    if (ui_button(ui, ui_key_i(key, 1), btn, "Acknowledge all", false))
        data_alert_ack_all(&d->data);

    /* 2. Collect the age indices (0 = newest) of the alerts that pass the filter. */
    int shown[ALERT_CAP];
    int n = 0;
    for (int i = 0; i < data_alert_count(&d->data); i++) {
        if (matches(d, data_alert(&d->data, i)))
            shown[n++] = i;
    }

    /* 3. Scrolling list; only rows intersecting the view are drawn. */
    float content_h = (float)n * ROW_H;
    uint32_t scroll_key = ui_key_i(key, 2);
    float off = ui_scroll_begin(ui, scroll_key, content, content_h);

    for (int pos = 0; pos < n; pos++) {
        int i = shown[pos];
        const Alert *a = data_alert(&d->data, i);
        /* Animation slots keyed by alert serial (stable while the list
         * shifts); the 1000 offset keeps them apart from the small
         * widget indices above. New alerts (at most 2 ticks old) start
         * one row above and transparent; alerts first seen later are
         * placed directly so opening the page does not animate the list. */
        uint32_t ak = ui_key_i(key, 1000 + a->serial);
        bool is_new = d->data.tick_count - a->tick <= 2;
        bool fresh, fresh_a;
        float *ys = ui_slot(ui, ak, &fresh);
        float *as = ui_slot(ui, ui_key_i(ak, 1), &fresh_a);
        float slot_y = (float)pos * ROW_H;
        if (fresh)
            *ys = is_new ? slot_y - ROW_H : slot_y;
        if (fresh_a)
            *as = is_new ? 0.0f : 1.0f;
        *ys = anim_approach(*ys, slot_y, 14.0f, ui->dt);
        *as = anim_approach(*as, 1.0f, 8.0f, ui->dt);

        Rectangle row = { content.x, content.y + *ys - off, content.width - 10.0f, ROW_H - 2.0f };
        if (row.y + row.height < content.y || row.y > content.y + content.height)
            continue;
        bool hot = ui_hot(ui, row);
        alert_row_draw(d, row, a, *as, true, hot);
        /* active == 0: ignore clicks that belong to a scrollbar drag. */
        if (hot && ui->pressed && ui->active == 0)
            data_alert_toggle_ack(&d->data, i);
    }
    ui_scroll_end(ui, scroll_key, content, content_h);

    if (n == 0)
        theme_text_center(t, FONT_BODY, "No alerts match this filter", content.x + content.width * 0.5f, content.y + 24.0f, c->text_faint);
}

/* ---- Page ---- */

/* Lays out the Alerts page inside area a: a 76 px row of four equal KPI
 * cards, then the log filling the rest. */
void page_alerts(Dashboard *d, Rectangle a)
{
    const float gap = SP_LG;
    const Palette *c = &d->theme.c;
    const Summary *s = &d->summary;

    Rectangle cards = ui_cut_top(&a, 76.0f);
    ui_cut_top(&a, gap);
    float cw = floorf((cards.width - 3.0f * gap) / 4.0f);

    kpi_card(d, ui_cut_left(&cards, cw), FILTER_ALL, "All alerts", data_alert_count(&d->data), c->text);
    ui_cut_left(&cards, gap);
    kpi_card(d, ui_cut_left(&cards, cw), SEV_CRITICAL, "Critical", s->open_alerts[SEV_CRITICAL], c->crit);
    ui_cut_left(&cards, gap);
    kpi_card(d, ui_cut_left(&cards, cw), SEV_WARNING, "Warning", s->open_alerts[SEV_WARNING], c->warn);
    ui_cut_left(&cards, gap);
    kpi_card(d, cards, SEV_INFO, "Info", s->open_alerts[SEV_INFO], c->accent);

    panel_alert_log(d, a);
}
