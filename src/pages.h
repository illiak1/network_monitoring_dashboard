/* pages.h - entry point of the Overview page.
 *
 * dashboard_frame (dashboard.c) calls page_overview with the content
 * rectangle (the window inset by a margin). The page only arranges panels
 * inside that area; the shared cards are in panels.h. Implemented in
 * page_overview.c.
 */
#ifndef PAGES_H
#define PAGES_H

#include "dashboard.h"

void page_overview(Dashboard *d, Rectangle area);

#endif
