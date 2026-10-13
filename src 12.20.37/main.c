/* main.c - program entry point.
 *
 * Creates the window, loads the theme (theme.h) and owns the main loop.
 * Each frame draws the title and a row of palette swatches with the theme
 * fonts and colours, which shows that fonts and palette load correctly.
 */
#include "raylib.h"
#include "theme.h"

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

    /* 2. Theme: palette and fonts. Needs the GL context created above
     * (fonts become textures). */
    Theme theme;
    theme_load(&theme);

    /* 3. One frame per iteration: title, caption and one swatch per accent
     * and status colour (40 px squares, 52 px pitch). */
    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(theme.c.bg);
        theme_text(&theme, FONT_TITLE, "Network Monitoring Dashboard", 32.0f, 28.0f, theme.c.text);
        theme_text(&theme, FONT_SMALL, "Theme and fonts loaded", 32.0f, 62.0f, theme.c.text_dim);

        const Color swatches[] = {
            theme.c.accent, theme.c.ok, theme.c.warn, theme.c.crit, theme.c.series_in, theme.c.series_out,
        };
        for (int i = 0; i < (int)(sizeof swatches / sizeof swatches[0]); i++)
            DrawRectangle(32 + i * 52, 96, 40, 40, swatches[i]);
        EndDrawing();
    }

    /* 4. Cleanup in reverse order: fonts before the GL context goes away. */
    theme_unload(&theme);
    CloseWindow();
    return 0;
}
