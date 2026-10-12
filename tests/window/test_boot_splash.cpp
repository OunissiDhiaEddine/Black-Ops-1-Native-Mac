// Draws the boot credit into a PNG without opening a window (P8 / T-065).
//   build/test_boot_splash out.png [width height]
#include <cstdio>
#include <cstdlib>

#include "boot_splash.h"

int main(int argc, char **argv) {
    if (argc < 2) return fprintf(stderr, "usage: test_boot_splash out.png [width height]\n"), 2;
    int w = argc > 3 ? atoi(argv[2]) : 1280, h = argc > 3 ? atoi(argv[3]) : 800;
    if (!bo1::win::write_boot_splash_png(argv[1], w, h)) return fprintf(stderr, "could not write %s\n", argv[1]), 1;
    printf("boot splash \"%s\" drawn at %dx%d -> %s\n", bo1::win::kBootSplashText, w, h, argv[1]);
    return 0;
}
