// Boot credit (P8 / T-065): a black screen with one line of text, shown once when the game's display comes up.
#pragma once

namespace bo1::win {

extern const char *const kBootSplashText;

// Covers the window's Metal layer with the credit for `seconds`, then fades it out. Returns at once; the work
// happens on the main thread. Does nothing without a layer (headless runs) or with BO1_SPLASH=off.
// Returns true when the credit is being shown, so that the caller can hold the game back for as long.
bool show_boot_splash(void *metal_layer, double seconds);

// The same picture drawn into a PNG file, without a window (for looking at it: build/test_boot_splash).
bool write_boot_splash_png(const char *path, int width, int height);

} // namespace bo1::win

namespace bo1::win {
// A native alert with one OK button; returns when it is dismissed. Call from a thread that is not the main
// thread, with the window service running (the game's fatal-error message, T-076).
void show_error_alert(const char *title, const char *text);
} // namespace bo1::win
