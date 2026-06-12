#pragma once

typedef enum {
    GEMU_DISPLAY_SDL,
    GEMU_DISPLAY_GTK,
    GEMU_DISPLAY_CURSES,
    GEMU_DISPLAY_NONE,
} GemuDisplayType;

typedef enum {
    GEMU_RENDERER_AUTO,
    GEMU_RENDERER_ACCELERATED,
    GEMU_RENDERER_SOFTWARE,
} GemuRendererType;
