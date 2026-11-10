/* Thin wrappers so Tamizhi never has to pass raylib's Color struct by value. */
#include <raylib.h>
#include "tz_raylib.h"

static Color col(int r, int g, int b, int a) {
    return (Color){ (unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a };
}
void tz_init_window(int w, int h, const char *title) { InitWindow(w, h, title); }
void tz_close_window(void)         { CloseWindow(); }
int  tz_window_should_close(void)  { return WindowShouldClose(); }
void tz_set_fps(int fps)           { SetTargetFPS(fps); }
void tz_begin_drawing(void)        { BeginDrawing(); }
void tz_end_drawing(void)          { EndDrawing(); }
void tz_clear_background(int r, int g, int b, int a) { ClearBackground(col(r, g, b, a)); }
void tz_draw_rect(int x, int y, int w, int h, int r, int g, int b, int a) {
    DrawRectangle(x, y, w, h, col(r, g, b, a));
}
