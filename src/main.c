/* main.c - program entry point.
 *
 * Creates the window, owns the main loop and hands every frame to
 * dashboard_frame (dashboard.c), which does all input handling, simulation
 * and drawing. No application logic lives here.
 */
#include "dashboard.h"
#include "raylib.h"

/* Initial window size and the smallest size the resizable window may take
 * (the layouts in the page files are designed down to this size). */
#define WINDOW_W 1280
#define WINDOW_H 720
#define MIN_W 960
#define MIN_H 600

/* Opens the window, runs the frame loop until the window is closed, then
 * releases resources. Returns 0. */
int main(void)
{
    /* 1. Window: resizable, 4x MSAA for smooth arcs and rounded shapes,
     * vsync hint to avoid tearing. ESC is not bound to quit (the exit key
     * is disabled); only the window close button ends the program. */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(WINDOW_W, WINDOW_H, "Network Monitoring Dashboard");
    SetWindowMinSize(MIN_W, MIN_H);
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    /* 2. Application state. Static: it is large and starts zero-initialised,
     * and a static keeps it off the stack. dashboard_init needs the GL
     * context created above (it loads fonts). */
    static Dashboard app;
    dashboard_init(&app);

    /* 3. One frame per iteration. */
    while (!WindowShouldClose()) {
        BeginDrawing();
        dashboard_frame(&app);
        EndDrawing();
    }

    /* 4. Cleanup in reverse order: fonts before the GL context goes away. */
    dashboard_shutdown(&app);
    CloseWindow();
    return 0;
}
