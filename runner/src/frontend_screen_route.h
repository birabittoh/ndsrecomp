#pragma once

#include <cstdint>

// Single-screen layout: which physical LCD (0 top, 1 bottom) is the "main"
// one, i.e. where engine A is routed. Games flip POWCNT1 during transitions
// and fades, so a new routing is followed only after it has held for
// `hold_frames` consecutive presented frames. Frames where both LCDs are
// blank neither advance nor reset that count: nothing is visible to switch
// on. The first observed frame is taken as-is.
struct NdsMainScreenDebounce {
    int main = 0;
    int candidate = 0;
    uint32_t held = 0;
    bool primed = false;
};

// Returns true when `main` changed on this frame.
inline bool nds_main_screen_update(NdsMainScreenDebounce& d, int routed,
                                   bool both_blank,
                                   uint32_t hold_frames = 4) {
    routed &= 1;
    if (!d.primed) {
        if (both_blank) return false;
        d = {routed, routed, 0, true};
        return false;
    }
    if (both_blank) return false;
    if (routed == d.main) {
        d.candidate = d.main;
        d.held = 0;
        return false;
    }
    if (routed == d.candidate) {
        ++d.held;
    } else {
        d.candidate = routed;
        d.held = 1;
    }
    if (d.held < hold_frames) return false;
    d.main = routed;
    d.held = 0;
    return true;
}

// The LCD the single window shows. Holding the virtual stylus peeks at the
// other LCD (the bottom one while engine A is on top, the top one while it is
// on the bottom), bypassing the debounce both ways; the toggle hotkey shows
// the other LCD until the main screen next changes.
inline int nds_single_shown_screen(int main, bool toggled,
                                   bool stylus_peek) {
    if (stylus_peek) return main ^ 1;
    return toggled ? (main ^ 1) : main;
}
