#include "chip8.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

/*
 * CHIP-8 hex keypad → keyboard mapping:
 *
 *   Keypad    Keyboard
 *   1 2 3 C   1 2 3 4
 *   4 5 6 D   Q W E R
 *   7 8 9 E   A S D F
 *   A 0 B F   Z X C V
 */
static const SDL_Scancode key_map[CHIP8_NUM_KEYS] = {
    SDL_SCANCODE_X,  /* 0 */
    SDL_SCANCODE_1,  /* 1 */
    SDL_SCANCODE_2,  /* 2 */
    SDL_SCANCODE_3,  /* 3 */
    SDL_SCANCODE_Q,  /* 4 */
    SDL_SCANCODE_W,  /* 5 */
    SDL_SCANCODE_E,  /* 6 */
    SDL_SCANCODE_A,  /* 7 */
    SDL_SCANCODE_S,  /* 8 */
    SDL_SCANCODE_D,  /* 9 */
    SDL_SCANCODE_Z,  /* A */
    SDL_SCANCODE_C,  /* B */
    SDL_SCANCODE_4,  /* C */
    SDL_SCANCODE_R,  /* D */
    SDL_SCANCODE_F,  /* E */
    SDL_SCANCODE_V,  /* F */
};

struct Chip8Input {
    uint8_t keys[CHIP8_NUM_KEYS];
};

Chip8Input *chip8_input_create(void) {
    if (SDL_InitSubSystem(SDL_INIT_EVENTS) < 0) return NULL;
    return calloc(1, sizeof(Chip8Input));
}

bool chip8_input_poll(Chip8Input *inp, uint8_t keys[CHIP8_NUM_KEYS], bool *quit) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            *quit = true; return false;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            SDL_Scancode sc  = ev.key.keysym.scancode;
            uint8_t      val = (ev.type == SDL_KEYDOWN) ? 1 : 0;
            if (sc == SDL_SCANCODE_ESCAPE && val) { *quit = true; return false; }
            for (int k = 0; k < CHIP8_NUM_KEYS; k++)
                if (sc == key_map[k]) { inp->keys[k] = val; break; }
            break;
        }
        }
    }
    memcpy(keys, inp->keys, CHIP8_NUM_KEYS);
    return true;
}

void chip8_input_destroy(Chip8Input *inp) {
    free(inp);
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
}
