/* pages.h - entry points of the four dashboard pages.
 *
 * dashboard_frame (dashboard.c) calls the function of the active page with
 * the content rectangle (right of the sidebar, below the header). Each
 * page only arranges panels and page-specific widgets inside that area;
 * the shared cards are in panels.h. One implementation file per page:
 * page_overview.c, page_devices.c, page_traffic.c, page_alerts.c.
 */
#ifndef PAGES_H
#define PAGES_H

#include "dashboard.h"

void page_overview(Dashboard *d, Rectangle area);
void page_devices(Dashboard *d, Rectangle area);
void page_traffic(Dashboard *d, Rectangle area);
void page_alerts(Dashboard *d, Rectangle area);

#endif
