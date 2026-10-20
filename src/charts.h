/* charts.h - chart renderers: line chart, gauge, stacked bars, sparkline.
 *
 * Immediate-mode drawing functions with no knowledge of Dashboard or
 * Dataset: callers hand in arrays and a Rectangle. Persistent animation
 * state (axis scale, bar positions) lives in the small *State structs the
 * caller owns. Implemented in charts.c on top of theme.h and anim.h.

 */
#ifndef CHARTS_H
#define CHARTS_H

#include <stdbool.h>
#include "raylib.h"
#include "theme.h"

#define CHART_MAX_SERIES 4   /* series drawn per line chart; bounds the stack buffers in chart_line */
#define BAR_MAX_IDS 32       /* distinct bar ids with animation state; ids are used modulo this */

/* ------------------------------------------------------- Line chart */

typedef struct {
    const char *name;      /* label for the caller's legend; not drawn by the chart */
    Color color;
    const float *values;   /* oldest to newest, LineChartParams.count entries */
    float alpha;           /* 0..1 visibility, animated by the caller */
} LineSeries;

typedef struct {
    float y_max;           /* smoothed axis maximum */
    bool initialized;      /* false: next draw snaps y_max instead of animating */
} LineChartState;

typedef struct {
    Rectangle area;        /* includes the axis label gutters */
    int count;             /* samples per series */
    float interval_s;      /* spacing between samples */
    float window_s;        /* visible time span */
    float scroll_s;        /* time since the newest sample was produced */
    const char *axis_fmt;  /* printf format for y labels, e.g. "%.0f" */
    float min_y_max;       /* lower bound for the axis maximum */
} LineChartParams;

/* Result of the hover test. */
typedef struct {
    bool active;
    int index;             /* sample index nearest to the cursor */
    float x;               /* screen x of that sample */
} ChartHover;

/* Draws grid, area fills, spline lines and the hover crosshair. Returns the
 * hovered sample when mouse_ok and the cursor is inside the plot. */
ChartHover chart_line(const Theme *t, LineChartState *st, const LineChartParams *p,
                      const LineSeries *series, int n_series,
                      Vector2 mouse, bool mouse_ok, float dt);

/* -------------------------------------------------------------- Gauge */

/* Zone colour with a short blend across each threshold. */
Color chart_zone_color(const Theme *t, float value, float max, float warn, float crit);

/* 270-degree arc gauge with ok/warn/crit zones and a value readout.
 * value is the displayed (already animated) reading; max/warn/crit share
 * its unit; value_text, unit and label are drawn inside and below the arc. */
void chart_gauge(const Theme *t, Rectangle area, float value, float max, float warn, float crit,
                 const char *value_text, const char *unit, const char *label);

/* --------------------------------------------------------- Bar chart */

typedef struct {
    int id;                /* stable identity so rows can animate when re-ranked */
    const char *label;
    const char *value_text;
    float in;              /* inbound segment, same unit as max_value */
    float out;             /* outbound segment */
} BarItem;

/* Per-id animated row position and segment widths (px). */
typedef struct {
    float y[BAR_MAX_IDS];
    float in[BAR_MAX_IDS];
    float out[BAR_MAX_IDS];
    bool seen[BAR_MAX_IDS];   /* false until first drawn; first draw snaps instead of animating */
} BarChartState;

/* Stacked in/out horizontal bars. Items are drawn in the given order, one
 * row_h per row; max_value is the full-bar value. Returns the hovered item
 * id or -1. */
int chart_bars(const Theme *t, BarChartState *st, Rectangle area, const BarItem *items, int n,
               float max_value, float row_h, Vector2 mouse, bool mouse_ok, float dt);

/* ---------------------------------------------------------- Sparkline */

/* Minimal trend line without axes; values are scaled from [lo, hi] to the
 * rectangle height. Optional gradient fill, dot on the newest sample. */
void chart_sparkline(Rectangle area, const float *values, int count, float lo, float hi,
                     Color color, bool fill);

#endif
