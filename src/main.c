/* main.c - program entry point.
 *
 * Creates the window and owns the main loop. Each frame clears the screen
 * and draws the application title with the raylib default font.
 */
#include "raylib.h"

/* Initial window size and the smallest size the resizable window may take. */
#define WINDOW_W 1280
#define WINDOW_H 720
#define MIN_W 960
#define MIN_H 600

/* Opens the window, runs the frame loop until the window is closed, then
 * releases resources. Returns 0. */
int main(void)
{
    /* 1. Window: resizable, 4x MSAA for smooth edges, vsync hint to avoid
     * tearing. ESC is not bound to quit (the exit key is disabled); only
     * the window close button ends the program. */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(WINDOW_W, WINDOW_H, "Network Monitoring Dashboard");
    SetWindowMinSize(MIN_W, MIN_H);
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    /* 2. One frame per iteration. The clear colour is 0x0A0D13, a near-black
     * blue. */
    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground((Color){ 10, 13, 19, 255 });
        DrawText("Network Monitoring Dashboard", 32, 28, 28, RAYWHITE);
        EndDrawing();
    }

    /* 3. Release the window and the GL context. */
    CloseWindow();
    return 0;
}
