// android_second_display.h -- the bridge to a second built-in display on
// Android (dual-screen handhelds): the app's activity shows a Presentation
// with a SurfaceView on the other panel and hands its surface and touches
// here over JNI. No game logic: the frontend presents the bottom LCD into the
// surface and turns the touches into touchscreen input.
//
// The activity opts in by declaring, as static natives,
//   nativeSecondDisplaySurface(android.view.Surface surface, int w, int h)
//   nativeSecondDisplayTouch(int action, float x, float y, int w, int h)
// (registered by nds_second_display_start) and the instance methods
// startSecondDisplay() / stopSecondDisplay(). Without them nothing happens.
#pragma once

#if defined(__ANDROID__)
#include <cstdint>

struct ANativeWindow;

struct NdsSecondDisplayTouch {
    enum Action : int { Down = 0, Up = 1, Move = 2, Cancel = 3 };
    int action = Down;
    // View pixels, and the view's size when the event happened.
    float x = 0.0f, y = 0.0f;
    int width = 0, height = 0;
};

// Registers the natives on the activity's class and asks it to look for a
// second display. False (logged) when the activity has no such hooks.
bool nds_second_display_start();
// Asks the activity to take the Presentation down and stops waiting on the
// render thread: after it, the activity's surface callbacks never block.
// The caller has let go of every surface first.
void nds_second_display_stop();

// Render thread, once per frame: the current surface (null when none) and a
// generation that changes whenever it does. The pointer stays valid until
// nds_second_display_release is called with a later generation.
ANativeWindow* nds_second_display_surface(uint64_t* generation);
// Render thread: nothing made on surfaces older than `generation` is in use
// any more; they may be released, and a UI thread waiting on that returns.
void nds_second_display_release(uint64_t generation);
// Render thread: the oldest queued touch, false when there is none.
bool nds_second_display_pop_touch(NdsSecondDisplayTouch* touch);
#endif
