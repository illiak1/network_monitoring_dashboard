/* dashboard.h - application state and shared helpers.
 *
 * Dashboard owns everything the program keeps between frames: Theme,
 * Dataset (simulation), the latest Summary and the pause flag. dashboard.c
 * runs the frame loop and draws the basic page; main.c only creates the
 * window and calls dashboard_frame.
 */
#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include "data.h"
#include "theme.h"

/* Whole application state. Large, so main.c keeps it in static storage. */
typedef struct {
    Theme theme;
    Dataset data;
    Summary summary;           /* refreshed once per frame from data */

    bool paused;               /* freezes the simulation */
} Dashboard;

/* Loads the theme, seeds the simulation from the clock, sets the defaults. */
void dashboard_init(Dashboard *d);
/* Releases GPU resources (fonts). */
void dashboard_shutdown(Dashboard *d);
/* Runs one frame: input, simulation step and drawing. */
void dashboard_frame(Dashboard *d);

/* Palette colour for a device status / alert severity. */
Color status_color(const Palette *c, DeviceStatus s);
Color severity_color(const Palette *c, AlertSeverity s);
/* "412 Mbps" or "1.24 Gbps". */
void format_rate(char *buf, size_t cap, float mbps);
/* Local time as HH:MM:SS. */
void format_clock(char *buf, size_t cap, time_t t);

#endif
