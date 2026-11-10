#ifndef TZ_RAYLIB_H
#define TZ_RAYLIB_H
void tz_init_window(int w, int h, const char *title);
void tz_close_window(void);
int  tz_window_should_close(void);
void tz_set_fps(int fps);
void tz_begin_drawing(void);
void tz_end_drawing(void);
void tz_clear_background(int r, int g, int b, int a);
void tz_draw_rect(int x, int y, int w, int h, int r, int g, int b, int a);
#endif
