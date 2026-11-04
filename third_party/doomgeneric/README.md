# doomgeneric for CosmosEx (CE_DOOM.PRG)

`src/` is [doomgeneric](https://github.com/ozkl/doomgeneric) at commit `e257a09527062bfb986b6e5e0780839b6cbd5b71`
(only the files its Makefile compiles - the other platform ports are left out), GPL-2 (`LICENSE`).
`ce/doomgeneric_ce.c` is the CosmosEx port: ce_media starts `ce_doomgeneric` and talks to it through pipes.

Changes to the upstream files:
- `src/i_sound.c`: `<SDL_mixer.h>` not included when `CE_DOOMGENERIC` is defined (the port has its own sound module)

`ce_doomgeneric` is a separate program - CosmosEx itself (PolyForm Noncommercial) doesn't link with it.
