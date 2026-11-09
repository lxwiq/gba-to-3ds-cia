# Testing the player without a console

`headless.c` is a tiny libretro frontend that boots a title in the Azahar
3DS emulator core (`azahar_libretro`, from the Azahar releases) without a
window, and writes the emulator output (both screens, 400x480) as PPM files.

```sh
clang -O2 -o headless headless.c
gba2cia build game.gba --mode dual --cxi \
  --test-script "800:press:START,1000:press:A,2050:touch:133:218,2400:exit" -o test.cxi
./headless azahar_libretro.dylib test.cxi out/ 4400 200 citra_graphics_api=Software
```

The `--test-script` events run inside the player (frame numbers count from
the end of loading): `press:KEY[+KEY]`, `touch:x:y` on the bottom screen,
`shot` (BMPs written to the virtual SD card under `3ds/gba2cia/test/`) and
`exit`.
