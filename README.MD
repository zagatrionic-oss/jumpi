# ULTRA HARD Obby Game (SDL2 + SDL2_ttf)

This is a small obby style game made with SDL2 and SDL2_ttf.  
It is single file C program. Player can move with WASD and mouse, jump with space.  
There is simple menu overlay and JSON-like map loader.

## Features
- First person controls (WASD + mouse look)
- Jump and sprint
- Collisions with cubes and wedges
- End tile to finish level
- Menu with Resume, Load, Settings, Credits, Quit
- Load map from JSON-like file
- Demo map included if no file given

## Build on Linux
Make sure SDL2 and SDL2_ttf are installed.

```bash
gcc -O2 -o obby_full_game obby_full_game.c `sdl2-config --cflags --libs` $(pkg-config --cflags --libs SDL2_ttf) -lm

-"Zac was here"