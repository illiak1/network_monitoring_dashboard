/* panels.h - dashboard cards that more than one page uses.
 *
 * Each panel_* function draws one titled card into the rectangle it is
 * given, reads Dashboard state and handles its own hover/click. They are
 * composed into layouts by page_overview.c and page_traffic.c; panels.c implements them
 * with ui.h widgets and charts.h.
 */
#ifndef PANELS_H
#define PANELS_H

#include "dashboard.h"

/* Reusable cards shared by several pages. Each draws into the given rect. */
/* Inbound/outbound line chart with range selector and legend toggles.
 * chart is the caller's axis state; id makes widget keys unique per page. */
void panel_bandwidth(Dashboard *d, Rectangle r, LineChartState *chart, const char *id);
/* Network-average latency and packet-loss gauges. */
void panel_gauges(Dashboard *d, Rectangle r);
/* Ranked in/out bars of the busiest devices; click opens the device. */
void panel_talkers(Dashboard *d, Rectangle r);

#endif
