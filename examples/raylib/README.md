# Minimal raylib binding

Files: `raylib.tzb` (the Tamizhi header), `tz_raylib.h` / `tz_raylib.c` (C wrappers),
`window.tz` (example).

Build (needs raylib installed, e.g. `apt install libraylib-dev` or build from source):

    tamizhi window.tz tz_raylib.c -lraylib -lm -lpthread -ldl -lGL -lX11 -o window
    # macOS: ... -lraylib -framework OpenGL -framework Cocoa -framework IOKit
    # add -I<dir> / -L<dir> if raylib is not in the default paths

Functions: `init_window(w, h, title)`, `close_window()`, `window_should_close()` (returns
0/1), `set_fps(n)`, `begin_drawing()`, `end_drawing()`, `clear_background(r,g,b,a)`,
`draw_rect(x, y, w, h, r, g, b, a)`. Colours are 0-255 numbers.

To add more raylib calls: add a wrapper in `tz_raylib.c` + `tz_raylib.h`, then one
`veli seyal ...;` line in `raylib.tzb`. Rename the Tamizhi side freely (the name before `=`).
