// discord_rpc.h -- Discord Rich Presence over the local Discord client's IPC
// (a named pipe on Windows, a Unix socket on Linux and macOS), with no
// third-party code. Android has no Discord client to talk to, so the calls
// do nothing there (kNdsDiscordRpcSupported is false).
//
// A background thread connects, keeps looking for Discord while it is not
// running and sends the activity whenever it changes. It logs once when it
// connects, once when Discord cannot be found (not on every retry) and once
// when the connection drops.
//
// The application id is a Discord application's (Developer Portal); the
// activity shows as "Playing <application name>", and the image keys name
// assets uploaded to that application (or https URLs).
#pragma once

#include <string>

#if defined(__ANDROID__) || !(defined(_WIN32) || defined(__linux__) || \
                              defined(__APPLE__))
inline constexpr bool kNdsDiscordRpcSupported = false;
#else
inline constexpr bool kNdsDiscordRpcSupported = true;
#endif

namespace nds::discord {

struct Activity {
    std::string details;      // first line, e.g. "In battle"
    std::string state;        // second line; empty leaves it out
    std::string large_image;  // asset key or https URL
    std::string large_text;   // the image's tooltip
    bool operator==(const Activity&) const = default;
};

// Starts the worker (once; later calls only change the activity). Empty
// application id: nothing starts.
void Start(const std::string& application_id, const Activity& initial);
// Any thread. Sent when it differs from the last one sent.
void SetActivity(const Activity& activity);
// Any thread. Off clears the activity and keeps the connection.
void SetEnabled(bool enabled);
// Clears the activity, closes the connection and joins the worker.
void Stop();

}  // namespace nds::discord
