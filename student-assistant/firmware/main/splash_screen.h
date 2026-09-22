#pragma once

/**
 * Cue splash screen — animated "C" arc + "ue" wordmark.
 * Call splash_screen_show() at boot, it invokes the done callback
 * when the animation finishes and the app should load the home screen.
 */

typedef void (*splash_done_cb_t)(void);

void splash_screen_set_done_cb(splash_done_cb_t cb);
void splash_screen_show(void);
