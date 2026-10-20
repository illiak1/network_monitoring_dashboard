/* dashboard.h - application state and shared UI helpers.
 *
 * Dashboard owns everything the program keeps between frames: Theme, Ui
 * (input and animation slots), Dataset (simulation) and the user's view
 * state (page, chart range, series toggles). dashboard.c runs the frame
 * loop and navigation; shell.c draws sidebar and header; pages.h/page_*.c
 * lay out the two pages from the cards in panels.h. main.c only creates
 * the window and calls dashboard_frame.
 */
#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include "charts.h"
#include "data.h"
#include "theme.h"
#include "ui.h"

/* Pages in sidebar order; the keys 1-2 map to this order. */
typedef enum {
    PAGE_OVERVIEW,
    PAGE_TRAFFIC,
    PAGE_COUNT
} Page;

#define RANGE_COUNT 3        /* selectable bandwidth windows: 30 s, 1 m, 2 m */

/* Whole application state. Large, so main.c keeps it in static storage. */
typedef struct {
    Theme theme;
    Ui ui;
    Dataset data;
    Summary summary;           /* refreshed once per frame from data */

    Page page;
    float page_t;              /* page transition progress, 0..1 */
    bool paused;               /* freezes the simulation, UI stays live */
    bool sidebar_collapsed;    /* user choice; narrow windows also collapse it */
    float sidebar_w;           /* animated width in px */

    /* Displayed (smoothed) gauge readings. */
    float gauge_latency;
    float gauge_loss;

    int range_idx;             /* bandwidth chart time window, index into RANGE_SECONDS */
    bool series_on[2];         /* inbound, outbound */
    LineChartState chart_overview;
    LineChartState chart_traffic;
    BarChartState talkers;
} Dashboard;

/* Bandwidth window per range index, in seconds, and its button label. */
extern const float RANGE_SECONDS[RANGE_COUNT];
extern const char *const RANGE_LABELS[RANGE_COUNT];

/* Loads the theme, seeds the simulation from the clock, sets the defaults. */
void dashboard_init(Dashboard *d);
/* Releases GPU resources (fonts). */
void dashboard_shutdown(Dashboard *d);
/* Runs one frame: input, simulation step and drawing. */
void dashboard_frame(Dashboard *d);

/* Switches page and restarts the fade-in. No-op for the current page. */
void dashboard_navigate(Dashboard *d, Page page);

/* Palette colour for a device status / alert severity. */
Color status_color(const Palette *c, DeviceStatus s);
Color severity_color(const Palette *c, AlertSeverity s);
/* "412 Mbps" or "1.24 Gbps". */
void format_rate(char *buf, size_t cap, float mbps);
/* Local time as HH:MM:SS. */
void format_clock(char *buf, size_t cap, time_t t);

/* Shell geometry in px. The sidebar shrinks to icons when collapsed or when
 * the window is narrower than SIDEBAR_AUTO_COLLAPSE_BELOW. */
#define HEADER_H 64
#define SIDEBAR_W 208
#define SIDEBAR_COLLAPSED_W 68
#define SIDEBAR_AUTO_COLLAPSE_BELOW 1100

/* Shell (shell.c). */
void shell_draw_sidebar(Dashboard *d, Rectangle area);
void shell_draw_header(Dashboard *d, Rectangle area);

#endif
