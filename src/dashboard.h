/* dashboard.h - application state and shared UI helpers.
 *
 * Dashboard owns everything the program keeps between frames: Theme, Ui
 * (input and animation slots), Dataset (simulation) and the user's view
 * state (page, filters, sort, selection). dashboard.c runs the frame loop
 * and navigation; shell.c draws sidebar and header; pages.h/page_*.c lay
 * out the four pages from the cards in panels.h. main.c only creates the
 * window and calls dashboard_frame.
 */
#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include "charts.h"
#include "data.h"
#include "theme.h"
#include "ui.h"

/* Pages in sidebar order; the keys 1-4 map to this order. */
typedef enum {
    PAGE_OVERVIEW,
    PAGE_DEVICES,
    PAGE_TRAFFIC,
    PAGE_ALERTS,
    PAGE_COUNT
} Page;

/* Sort column of the device table. */
typedef enum {
    SORT_STATUS,
    SORT_NAME,
    SORT_LATENCY,
    SORT_LOSS,
    SORT_TRAFFIC
} SortKey;

#define RANGE_COUNT 3        /* selectable bandwidth windows: 30 s, 1 m, 2 m */
#define FILTER_ALL (-1)      /* "no filter"; outside the DeviceStatus/AlertSeverity value range */

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
    LineChartState chart_device;
    BarChartState talkers;

    int selected_device;       /* index into data.devices, -1 for none */
    int device_filter;         /* FILTER_ALL or a DeviceStatus */
    SortKey sort_key;
    bool sort_desc;

    int alert_filter;          /* FILTER_ALL or an AlertSeverity */
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
/* Sets the selected device (-1 clears) and re-snaps the detail chart axis. */
void dashboard_select_device(Dashboard *d, int index);
/* Selects a device and opens the Devices page. */
void dashboard_open_device(Dashboard *d, int index);

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
