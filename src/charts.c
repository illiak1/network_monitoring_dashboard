/* charts.c - line chart, gauge, stacked bar chart and sparkline.
 *
 * Sections: Line chart (axis scaling, Catmull-Rom curve, gradient fill,
 * hover), Gauge (zone colour, 270-degree arc), Bar chart (animated,
 * re-rankable rows) and Sparkline. Everything is immediate mode: callers
 * pass sample arrays and a rectangle; only the small State structs in
 * charts.h persist. Declared in charts.h, used by panels.c. Gradient fills
 * use rlgl directly because raylib has no per-vertex-colour polygon
 * primitive.
 */
#include "charts.h"

#include <math.h>
#include <stdio.h>

#include "anim.h"
#include "rlgl.h"

#define CURVE_SUBDIV 4      /* interpolated points per sample interval */
/* Capacity of a curve buffer: 240 samples (RING_CAP, hard-coded because
 * this file does not include data.h) x CURVE_SUBDIV + the end point + 1 spare. */
#define CURVE_MAX_POINTS (240 * CURVE_SUBDIV + 2)
#define AXIS_LABEL_W 42.0f  /* left gutter for y-axis labels, px */
#define AXIS_LABEL_H 22.0f  /* bottom gutter for time labels, px */
#define GRID_ROWS 4         /* grid divisions per axis */

/* ------------------------------------------------------- Line chart */

/* Axis maximum for GRID_ROWS equal divisions: the smallest 4 x step that is >= v,
 * where step is a 1/1.5/2/2.5/3/4/5/6/8 x 10^k value.
 * Round steps give readable tick labels (0, 250, 500, ...). The 0.9999
 * factor stops float noise from bumping an exact fit to the next step. */
static float nice_ceil(float v)
{
    static const float steps[] = { 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f };
    float need = v > 0.0f ? v / GRID_ROWS : 1.0f;
    float e = powf(10.0f, floorf(log10f(need)));
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        if (steps[i] * e >= need * 0.9999f)
            return steps[i] * e * GRID_ROWS;
    }
    return 10.0f * e * GRID_ROWS;
}

/* Uniform Catmull-Rom on the value axis; x stays linear so the curve is a function of time.
 * Evaluates the segment p1 -> p2 at t in [0, 1) using the neighbours p0
 * and p3. The spline can overshoot the samples; callers clamp the result. */
static float catmull_rom(float p0, float p1, float p2, float p3, float t)
{
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                   + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

/* Clips a segment to the vertical band [l, r]; false if it lies outside.
 * Modifies a and b in place, moving an outside endpoint onto the band edge
 * and interpolating its y. Needed because the curve extends past the plot
 * (samples scroll out on the left) and fills are drawn without scissor. */
static bool clip_segment_x(Vector2 *a, Vector2 *b, float l, float r)
{
    if ((a->x < l && b->x < l) || (a->x > r && b->x > r))
        return false;
    float dx = b->x - a->x;
    if (dx == 0.0f)
        return true;
    Vector2 a0 = *a, b0 = *b;
    if (a0.x < l) { a->x = l; a->y = a0.y + (b0.y - a0.y) * (l - a0.x) / dx; }
    if (a0.x > r) { a->x = r; a->y = a0.y + (b0.y - a0.y) * (r - a0.x) / dx; }
    if (b0.x < l) { b->x = l; b->y = a0.y + (b0.y - a0.y) * (l - a0.x) / dx; }
    if (b0.x > r) { b->x = r; b->y = a0.y + (b0.y - a0.y) * (r - a0.x) / dx; }
    return true;
}

/* Formats an axis value with the caller-supplied printf format. */
static void format_axis(char *buf, size_t cap, const char *fmt, float v)
{
    snprintf(buf, cap, fmt, (double)v);
}

/* Screen x of sample i. The newest sample is placed one interval to the
 * right of the plot edge at scroll_s = 0 and slides left as scroll_s grows;
 * when the next sample arrives (scroll_s wraps to 0) the previous newest is
 * exactly at the edge. This gives continuous scrolling at frame rate from
 * 2 Hz data, at the cost of showing values one interval late. */
static float sample_x(const LineChartParams *p, Rectangle plot, int i)
{
    float behind = (float)(p->count - 1 - i) * p->interval_s + p->scroll_s - p->interval_s;
    return plot.x + plot.width - behind / p->window_s * plot.width;
}

/* Draws the horizontal grid with y labels (k = 0 is the stronger baseline)
 * and the vertical grid with relative time labels ("-30s" ... "now"). */
static void draw_axes(const Theme *t, const LineChartParams *p, Rectangle plot, float y_max)
{
    const Palette *c = &t->c;
    char label[24];

    /* Lines are 1 px wide: floor + 0.5 puts them on pixel centres so they
     * render sharp instead of 2 px grey. */
    for (int k = 0; k <= GRID_ROWS; k++) {
        float y = floorf(plot.y + plot.height - plot.height * (float)k / GRID_ROWS) + 0.5f;
        DrawLineEx((Vector2){ plot.x, y }, (Vector2){ plot.x + plot.width, y }, 1.0f, k == 0 ? c->border_strong : c->grid);
        format_axis(label, sizeof label, p->axis_fmt, y_max * (float)k / GRID_ROWS);
        theme_text_right(t, FONT_CAPTION, label, plot.x - 10.0f, y - 7.0f, c->text_faint);
    }

    for (int k = 0; k <= GRID_ROWS; k++) {
        float x = floorf(plot.x + plot.width * (float)k / GRID_ROWS) + 0.5f;
        if (k > 0 && k < GRID_ROWS)
            DrawLineEx((Vector2){ x, plot.y }, (Vector2){ x, plot.y + plot.height }, 1.0f, theme_alpha(c->grid, 0.6f));
        if (k == GRID_ROWS) {
            theme_text_right(t, FONT_CAPTION, "now", plot.x + plot.width, plot.y + plot.height + 8.0f, c->text_faint);
        } else {
            snprintf(label, sizeof label, "-%.0fs", (double)(p->window_s * (float)(GRID_ROWS - k) / GRID_ROWS));
            float lw = theme_measure(t, FONT_CAPTION, label).x;
            /* First label is left-aligned to the plot edge, the rest centred on their gridline. */
            float lx = k == 0 ? plot.x : x - lw * 0.5f;
            theme_text(t, FONT_CAPTION, label, lx, plot.y + plot.height + 8.0f, c->text_faint);
        }
    }
}

/* Converts samples to a smooth polyline in screen space. Writes up to
 * CURVE_MAX_POINTS points into out and returns the count.
 * Only the visible part is generated; values are clamped to [0, y_max]
 * so spline overshoot cannot leave the plot vertically. */
static int build_curve(Vector2 *out, const LineChartParams *p, Rectangle plot, const float *v, float y_max)
{
    /* 1. Skip segments that lie completely left of the plot. */
    int first = 0;
    while (first < p->count - 2 && sample_x(p, plot, first + 1) < plot.x)
        first++;

    /* 2. Subdivide every segment; the end samples repeat themselves as
     * their missing neighbour. */
    int n = 0;
    for (int i = first; i < p->count - 1; i++) {
        float p0 = v[i > 0 ? i - 1 : i];
        float p1 = v[i];
        float p2 = v[i + 1];
        float p3 = v[i + 2 < p->count ? i + 2 : i + 1];
        float x1 = sample_x(p, plot, i), x2 = sample_x(p, plot, i + 1);

        for (int s = 0; s < CURVE_SUBDIV; s++) {
            float u = (float)s / CURVE_SUBDIV;
            float val = catmull_rom(p0, p1, p2, p3, u);
            val = fminf(fmaxf(val, 0.0f), y_max);
            out[n++] = (Vector2){ x1 + (x2 - x1) * u, plot.y + plot.height - val / y_max * plot.height };
        }
    }
    /* 3. The loop stops before the last sample; add it explicitly. */
    float last = v[p->count - 1];
    out[n++] = (Vector2){ sample_x(p, plot, p->count - 1), plot.y + plot.height - fminf(last, y_max) / y_max * plot.height };
    return n;
}

/* Fills the area under a polyline with a vertical gradient: colour at 30%
 * alpha (scaled by strength) at the line, fading to transparent at the
 * baseline. Each segment is two triangles with per-vertex colours. */
static void draw_fill(const Vector2 *pts, int n, Rectangle plot, Color color, float strength)
{
    float base = plot.y + plot.height;
    Color top = theme_alpha(color, 0.30f * strength);
    Color bot = theme_alpha(color, 0.0f);

    rlSetTexture(0);
    rlBegin(RL_TRIANGLES);
    for (int i = 0; i + 1 < n; i++) {
        Vector2 a = pts[i], b = pts[i + 1];
        if (!clip_segment_x(&a, &b, plot.x, plot.x + plot.width))
            continue;
        rlColor4ub(top.r, top.g, top.b, top.a); rlVertex2f(a.x, a.y);
        rlColor4ub(bot.r, bot.g, bot.b, bot.a); rlVertex2f(a.x, base);
        rlColor4ub(bot.r, bot.g, bot.b, bot.a); rlVertex2f(b.x, base);

        rlColor4ub(top.r, top.g, top.b, top.a); rlVertex2f(a.x, a.y);
        rlColor4ub(bot.r, bot.g, bot.b, bot.a); rlVertex2f(b.x, base);
        rlColor4ub(top.r, top.g, top.b, top.a); rlVertex2f(b.x, b.y);
    }
    rlEnd();
}

/* Strokes the polyline, clipped to the plot's horizontal extent. */
static void draw_polyline(const Vector2 *pts, int n, Rectangle plot, Color color, float thickness)
{
    for (int i = 0; i + 1 < n; i++) {
        Vector2 a = pts[i], b = pts[i + 1];
        if (clip_segment_x(&a, &b, plot.x, plot.x + plot.width))
            DrawLineEx(a, b, thickness, color);
    }
}

/* Draws a time-series chart with up to CHART_MAX_SERIES series sharing
 * one auto-scaled y axis. st carries the smoothed axis between frames;
 * series[].alpha fades a series in/out. mouse_ok must be false when the
 * cursor is blocked or outside the window. Returns the hover result
 * (active == false when nothing is hovered or the plot is too small). */
ChartHover chart_line(const Theme *t, LineChartState *st, const LineChartParams *p,
                      const LineSeries *series, int n_series,
                      Vector2 mouse, bool mouse_ok, float dt)
{
    const Palette *c = &t->c;
    ChartHover hover = { false, 0, 0.0f };

    /* 1. Plot rectangle inside the label gutters; bail out when degenerate. */
    Rectangle plot = { p->area.x + AXIS_LABEL_W, p->area.y + 6.0f,
                       p->area.width - AXIS_LABEL_W, p->area.height - AXIS_LABEL_H - 6.0f };
    if (p->count < 2 || plot.width < 10.0f || plot.height < 10.0f)
        return hover;

    /* 2. Axis target from the peak of the visible samples of visible
     * series (+2 samples for the partially visible edges), with 12%
     * headroom and the caller's floor, rounded up to a nice value. */
    int visible = (int)ceilf(p->window_s / p->interval_s) + 2;
    int from = p->count - visible > 0 ? p->count - visible : 0;
    float peak = 0.0f;
    for (int s = 0; s < n_series; s++) {
        if (series[s].alpha < 0.05f)
            continue;
        for (int i = from; i < p->count; i++)
            peak = fmaxf(peak, series[s].values[i]);
    }
    float target = nice_ceil(fmaxf(peak * 1.12f, p->min_y_max));

    /* 3. Smooth the axis: grow fast (rate 14) so a new peak is not
     * clipped, shrink slowly (rate 4) so the scale does not pump. The
     * first frame snaps. */
    if (!st->initialized) {
        st->y_max = target;
        st->initialized = true;
    }
    st->y_max = anim_approach(st->y_max, target, target > st->y_max ? 14.0f : 4.0f, dt);
    float y_max = st->y_max;

    draw_axes(t, p, plot, y_max);

    /* 4. Build and fill every series, then stroke all lines on top so no
     * fill covers a line. Nearly invisible series are skipped. */
    Vector2 curves[CHART_MAX_SERIES][CURVE_MAX_POINTS];
    int counts[CHART_MAX_SERIES] = { 0 };
    int drawn = n_series < CHART_MAX_SERIES ? n_series : CHART_MAX_SERIES;

    for (int s = 0; s < drawn; s++) {
        if (series[s].alpha < 0.02f)
            continue;
        counts[s] = build_curve(curves[s], p, plot, series[s].values, y_max);
        draw_fill(curves[s], counts[s], plot, series[s].color, series[s].alpha);
    }
    for (int s = 0; s < drawn; s++) {
        if (counts[s] > 0)
            draw_polyline(curves[s], counts[s], plot, theme_alpha(series[s].color, series[s].alpha), 2.0f);
    }

    /* 5. Hover: invert sample_x to find the nearest sample, then draw the
     * crosshair and a marker on every sufficiently visible series. */
    if (mouse_ok && CheckCollisionPointRec(mouse, plot)) {
        float behind = (plot.x + plot.width - mouse.x) / plot.width * p->window_s;
        int idx = p->count - 1 - (int)roundf((behind + p->interval_s - p->scroll_s) / p->interval_s);
        idx = idx < 0 ? 0 : (idx > p->count - 1 ? p->count - 1 : idx);
        float x = sample_x(p, plot, idx);
        if (x >= plot.x && x <= plot.x + plot.width) {
            hover = (ChartHover){ true, idx, x };
            DrawLineEx((Vector2){ floorf(x) + 0.5f, plot.y }, (Vector2){ floorf(x) + 0.5f, plot.y + plot.height }, 1.0f,
                       theme_alpha(c->text_dim, 0.45f));
            for (int s = 0; s < n_series; s++) {
                if (series[s].alpha < 0.3f)
                    continue;
                float v = fminf(series[s].values[idx], y_max);
                Vector2 pt = { x, plot.y + plot.height - v / y_max * plot.height };
                DrawCircleV(pt, 5.5f, c->panel);
                DrawCircleV(pt, 3.5f, series[s].color);
            }
        }
    }
    return hover;
}

/* -------------------------------------------------------------- Gauge */

/* Colour for a value on an ok/warn/crit scale. Instead of a hard switch at
 * the thresholds the colour is blended over a band of +-4% of max around
 * each one (smoothstep), so it changes smoothly as the value moves. */
Color chart_zone_color(const Theme *t, float value, float max, float warn, float crit)
{
    const Palette *c = &t->c;
    float band = max * 0.04f;
    float to_warn = anim_smoothstep((value - (warn - band)) / (2.0f * band));
    float to_crit = anim_smoothstep((value - (crit - band)) / (2.0f * band));
    return theme_mix(theme_mix(c->ok, c->warn, to_warn), c->crit, to_crit);
}

/* Gauge arc in degrees, raylib convention (0 = +x, increasing clockwise on
 * screen): starts at 135 (lower left), sweeps 270 over the top and ends at
 * 405 = 45 (lower right), leaving a 90 degree opening at the bottom. */
#define GAUGE_START 135.0f
#define GAUGE_SWEEP 270.0f

/* Draws a ring segment from a0 to a1 degrees; skips arcs under 0.1 degrees
 * (nothing visible, and avoids a zero-length ring at value 0). One segment
 * per ~3 degrees keeps the curve smooth at gauge sizes. */
static void arc(Vector2 center, float inner, float outer, float a0, float a1, Color color)
{
    if (a1 - a0 < 0.1f)
        return;
    int seg = (int)((a1 - a0) / 3.0f) + 2;
    DrawRing(center, inner, outer, a0, a1, seg, color);
}

/* Point on a circle of the given radius at an angle in degrees. */
static Vector2 arc_point(Vector2 center, float radius, float deg)
{
    float r = deg * ANIM_PI / 180.0f;
    return (Vector2){ center.x + cosf(r) * radius, center.y + sinf(r) * radius };
}

/* Draws a gauge into area: three faint zone arcs, a bright value arc in
 * the zone colour with round caps, end labels, and the readout. value is
 * the already animated reading (the caller smooths it); max/warn/crit
 * share its unit. Fits the largest arc into area. */
void chart_gauge(const Theme *t, Rectangle area, float value, float max, float warn, float crit,
                 const char *value_text, const char *unit, const char *label)
{
    const Palette *c = &t->c;

    /* 1. Geometry. The arc spans 1.707 radii vertically; the rest holds the end labels. */
    float radius = fminf(area.width * 0.5f - 2.0f, (area.height - 24.0f) / 1.72f);
    float thick = fmaxf(8.0f, radius * 0.15f);
    Vector2 center = { floorf(area.x + area.width * 0.5f), floorf(area.y + radius) };
    float outer = radius, inner = radius - thick;
    float mid = (outer + inner) * 0.5f;

    /* 2. Map value and thresholds to angles. */
    float frac = anim_clamp01(value / max);
    float f_warn = anim_clamp01(warn / max), f_crit = anim_clamp01(crit / max);
    float a_warn = GAUGE_START + GAUGE_SWEEP * f_warn;
    float a_crit = GAUGE_START + GAUGE_SWEEP * f_crit;
    float a_end = GAUGE_START + GAUGE_SWEEP;

    /* 3. Zone track: three arcs at 16% alpha show where warn and crit begin. */
    arc(center, inner, outer, GAUGE_START, a_warn, theme_alpha(c->ok, 0.16f));
    arc(center, inner, outer, a_warn, a_crit, theme_alpha(c->warn, 0.16f));
    arc(center, inner, outer, a_crit, a_end, theme_alpha(c->crit, 0.16f));

    /* 4. Value arc, rounded at both ends, with a small knob at the tip. */
    Color fill = chart_zone_color(t, value, max, warn, crit);
    float a_val = GAUGE_START + GAUGE_SWEEP * frac;
    arc(center, inner, outer, GAUGE_START, a_val, fill);
    DrawCircleV(arc_point(center, mid, GAUGE_START), thick * 0.5f, fill);
    DrawCircleV(arc_point(center, mid, a_val), thick * 0.5f, fill);
    DrawCircleV(arc_point(center, mid, a_val), thick * 0.18f, c->panel);

    /* 5. Labels: scale ends under the arc ends (max with one decimal when
     * below 10), name between them, readout and unit inside the arc. */
    char buf[16];
    Vector2 lo = arc_point(center, mid, GAUGE_START);
    Vector2 hi = arc_point(center, mid, a_end);
    theme_text_center(t, FONT_CAPTION, "0", lo.x, lo.y + thick * 0.5f + 4.0f, c->text_faint);
    snprintf(buf, sizeof buf, "%.*f", max < 10.0f ? 1 : 0, (double)max);
    theme_text_center(t, FONT_CAPTION, buf, hi.x, hi.y + thick * 0.5f + 4.0f, c->text_faint);

    theme_text_center(t, FONT_BODY, label, center.x, lo.y + thick * 0.5f + 3.0f, c->text_dim);
    theme_text_center(t, FONT_MONO_LARGE, value_text, center.x, center.y - 22.0f, c->text);
    theme_text_center(t, FONT_SMALL, unit, center.x, center.y + 14.0f, c->text_faint);
}

/* --------------------------------------------------------- Bar chart */

/* Draws one row per item: rank, label, stacked in/out bar, value text.
 * Rows move and bars resize with easing, keyed by BarItem.id, so a device
 * that changes rank slides to its new place instead of jumping.
 * Returns the id of the hovered row or -1. Items are expected sorted by
 * the caller; ids at or above BAR_MAX_IDS share state modulo the size. */
int chart_bars(const Theme *t, BarChartState *st, Rectangle area, const BarItem *items, int n,
               float max_value, float row_h, Vector2 mouse, bool mouse_ok, float dt)
{
    const Palette *c = &t->c;
    const float rank_w = 20.0f, value_w = 74.0f, bar_h = 8.0f;
    int hovered = -1;

    /* Column layout: rank | label | bar | value. */
    float label_w = fminf(104.0f, area.width * 0.32f);
    float bar_x = area.x + rank_w + label_w + 8.0f;
    float bar_w = fmaxf(10.0f, area.x + area.width - value_w - 8.0f - bar_x);

    for (int slot = 0; slot < n; slot++) {
        const BarItem *it = &items[slot];
        int id = it->id % BAR_MAX_IDS;
        float target_y = area.y + (float)slot * row_h;
        /* Target segment widths in px. */
        float k_in = it->in / max_value * bar_w;
        float k_out = it->out / max_value * bar_w;

        /* 1. First appearance snaps to the target, later frames ease
         * toward it (position faster than width). */
        if (!st->seen[id]) {
            st->seen[id] = true;
            st->y[id] = target_y;
            st->in[id] = k_in;
            st->out[id] = k_out;
        }
        st->y[id] = anim_approach(st->y[id], target_y, 12.0f, dt);
        st->in[id] = anim_approach(st->in[id], k_in, 10.0f, dt);
        st->out[id] = anim_approach(st->out[id], k_out, 10.0f, dt);

        /* 2. Hit test and hover highlight on the animated row. */
        Rectangle row = { area.x - 6.0f, st->y[id], area.width + 12.0f, row_h - 2.0f };
        bool hot = mouse_ok && CheckCollisionPointRec(mouse, row);
        if (hot) {
            hovered = it->id;
            DrawRectangleRounded(row, 0.3f, 6, theme_alpha(c->text, 0.05f));
        }

        /* 3. Rank and label. */
        float ty = row.y + (row.height - t->font_px[FONT_BODY]) * 0.5f;
        char rank[12];
        snprintf(rank, sizeof rank, "%d", slot + 1);
        theme_text(t, FONT_MONO_SMALL, rank, area.x, ty, c->text_faint);
        theme_text_fit(t, FONT_BODY, it->label, area.x + rank_w, ty, label_w, hot ? c->text : theme_mix(c->text_dim, c->text, 0.55f));

        /* 4. Bar: track, then the total in the outbound colour, then the
         * inbound part over it. The inbound segment is drawn on a
         * panel-coloured rectangle 1 px larger to leave a thin gap
         * between the two colours. Segments are at least bar_h wide so
         * the rounded caps stay round. */
        float by = floorf(row.y + (row.height - bar_h) * 0.5f);
        float total = st->in[id] + st->out[id];
        DrawRectangleRounded((Rectangle){ bar_x, by, bar_w, bar_h }, 1.0f, 6, theme_alpha(c->border, 0.9f));
        if (total > 1.0f) {
            float w_total = fmaxf(total, bar_h);
            float w_in = total > 0.0f ? w_total * st->in[id] / total : 0.0f;
            DrawRectangleRounded((Rectangle){ bar_x, by, w_total, bar_h }, 1.0f, 6, c->series_out);
            if (w_in > 1.0f) {
                float w = fmaxf(w_in, bar_h);
                DrawRectangleRounded((Rectangle){ bar_x - 1.0f, by - 1.0f, w + 2.0f, bar_h + 2.0f }, 1.0f, 6, c->panel);
                DrawRectangleRounded((Rectangle){ bar_x, by, w, bar_h }, 1.0f, 6, c->series_in);
            }
        }
        theme_text_right(t, FONT_MONO_SMALL, it->value_text, area.x + area.width, ty, hot ? c->text : c->text_dim);
    }
    return hovered;
}

/* ---------------------------------------------------------- Sparkline */

/* Draws a compact trend line of count values (oldest first), mapped from
 * [lo, hi] onto the rectangle height (out-of-range values are clamped).
 * With fill, a faint gradient is added under the line. A dot marks the
 * newest value. Draws nothing for fewer than 2 samples or an empty range. */
void chart_sparkline(Rectangle area, const float *values, int count, float lo, float hi,
                     Color color, bool fill)
{
    if (count < 2 || hi <= lo)
        return;

    /* 1. Map samples to screen points; the buffer size caps the count. */
    Vector2 pts[CURVE_MAX_POINTS];
    if (count > CURVE_MAX_POINTS)
        count = CURVE_MAX_POINTS;
    for (int i = 0; i < count; i++) {
        float f = anim_clamp01((values[i] - lo) / (hi - lo));
        pts[i] = (Vector2){ area.x + area.width * (float)i / (float)(count - 1),
                            area.y + area.height - f * area.height };
    }

    /* 2. Optional gradient fill (same triangle scheme as draw_fill, but
     * without clipping since all points are inside the area). */
    if (fill) {
        float base = area.y + area.height;
        Color top = theme_alpha(color, 0.22f), bot = theme_alpha(color, 0.0f);
        rlSetTexture(0);
        rlBegin(RL_TRIANGLES);
        for (int i = 0; i + 1 < count; i++) {
            Vector2 a = pts[i], b = pts[i + 1];
            rlColor4ub(top.r, top.g, top.b, top.a); rlVertex2f(a.x, a.y);
            rlColor4ub(bot.r, bot.g, bot.b, bot.a); rlVertex2f(a.x, base);
            rlColor4ub(bot.r, bot.g, bot.b, bot.a); rlVertex2f(b.x, base);

            rlColor4ub(top.r, top.g, top.b, top.a); rlVertex2f(a.x, a.y);
            rlColor4ub(bot.r, bot.g, bot.b, bot.a); rlVertex2f(b.x, base);
            rlColor4ub(top.r, top.g, top.b, top.a); rlVertex2f(b.x, b.y);
        }
        rlEnd();
    }

    /* 3. Line and end marker. */
    for (int i = 0; i + 1 < count; i++)
        DrawLineEx(pts[i], pts[i + 1], 1.5f, color);
    DrawCircleV(pts[count - 1], 2.2f, color);
}
