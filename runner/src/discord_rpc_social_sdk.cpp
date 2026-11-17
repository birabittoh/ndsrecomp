// Discord Rich Presence through Discord's Social SDK (discord_rpc.h), for
// Android, where there is no IPC socket: the SDK binds to the installed
// Discord app's RPC service. "Rich presence without authentication": the
// application id and UpdateRichPresence, no Connect().
//
// The SDK's Java side needs the activity first
// (com.discord.socialsdk.DiscordSocialSdkInit.setEngineActivity, called by
// the title's activity in onCreate). The Client is only touched from the
// worker thread, which also runs the SDK's callbacks.
#include "discord_rpc.h"

#if NDS_DISCORD_SOCIAL_SDK

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"

namespace nds::discord {
namespace {

// Discord allows 5 activity updates per 20 seconds: one per 4 s at most,
// the latest wins. A refused update is tried again after kRetryInterval
// (the Discord app missing, signed out, not running yet).
constexpr auto kSendInterval = std::chrono::seconds(4);
constexpr auto kRetryInterval = std::chrono::seconds(15);
constexpr auto kPollInterval = std::chrono::milliseconds(250);

struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    bool running = false;
    std::string application_id;
    Activity activity;
    bool enabled = true;
};

State& state() {
    static State s;
    return s;
}

void Worker() {
    State& s = state();
    using Clock = std::chrono::steady_clock;
    const uint64_t start_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    uint64_t application_id = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        application_id = std::strtoull(s.application_id.c_str(), nullptr, 10);
    }
    if (application_id == 0) {
        std::fprintf(stderr, "[discord] bad application id\n");
        return;
    }

    discordpp::Client client;
    // The SDK's own warnings and errors, each message once in a row.
    std::string last_log;
    client.AddLogCallback(
        [&last_log](std::string message, discordpp::LoggingSeverity) {
            if (message == last_log) return;
            last_log = message;
            std::fprintf(stderr, "[discord] sdk: %s\n", message.c_str());
        },
        discordpp::LoggingSeverity::Error);
    client.SetApplicationId(application_id);

    // What Discord shows, as far as the last answer told.
    bool shown_valid = false;
    bool shown_enabled = false;
    Activity shown;
    bool in_flight = false;
    bool ok_reported = false;
    std::string failure_reported;
    Clock::time_point next_send = Clock::now();

    std::unique_lock<std::mutex> lock(s.mutex);
    while (s.running) {
        const bool enabled = s.enabled;
        const Activity wanted = s.activity;
        lock.unlock();

        if (!in_flight && Clock::now() >= next_send &&
            (!shown_valid || enabled != shown_enabled ||
             (enabled && !(wanted == shown)))) {
            next_send = Clock::now() + kSendInterval;
            if (!enabled) {
                client.ClearRichPresence();
                shown_valid = true;
                shown_enabled = false;
            } else {
                discordpp::Activity activity;
                activity.SetType(discordpp::ActivityTypes::Playing);
                if (!wanted.name.empty()) activity.SetName(wanted.name);
                if (!wanted.details.empty()) activity.SetDetails(wanted.details);
                if (!wanted.state.empty()) activity.SetState(wanted.state);
                discordpp::ActivityTimestamps timestamps;
                timestamps.SetStart(start_ms);
                activity.SetTimestamps(timestamps);
                if (!wanted.large_image.empty()) {
                    discordpp::ActivityAssets assets;
                    assets.SetLargeImage(wanted.large_image);
                    if (!wanted.large_text.empty())
                        assets.SetLargeText(wanted.large_text);
                    activity.SetAssets(assets);
                }
                in_flight = true;
                client.UpdateRichPresence(
                    activity, [&, wanted](discordpp::ClientResult result) {
                        in_flight = false;
                        if (result.Successful()) {
                            shown_valid = true;
                            shown_enabled = true;
                            shown = wanted;
                            failure_reported.clear();
                            if (!ok_reported)
                                std::fprintf(stderr, "[discord] connected\n");
                            ok_reported = true;
                            return;
                        }
                        // Discord missing or not signed in: try again later,
                        // saying so once.
                        shown_valid = false;
                        next_send = Clock::now() + kRetryInterval;
                        const std::string error = result.Error();
                        if (error != failure_reported) {
                            std::fprintf(stderr,
                                         "[discord] activity not shown (%s); "
                                         "trying again every %llds\n",
                                         error.c_str(),
                                         static_cast<long long>(
                                             kRetryInterval.count()));
                            failure_reported = error;
                        }
                        ok_reported = false;
                    });
            }
        }
        discordpp::RunCallbacks();

        lock.lock();
        s.wake.wait_for(lock, kPollInterval, [&] { return !s.running; });
    }
    lock.unlock();

    client.ClearRichPresence();
    for (int i = 0; i < 4; ++i) {
        discordpp::RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

}  // namespace

void Start(const std::string& application_id, const Activity& initial) {
    if (application_id.empty()) return;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.activity = initial;
    if (s.running || s.thread.joinable()) return;
    s.application_id = application_id;
    s.running = true;
    s.thread = std::thread(Worker);
}

void SetActivity(const Activity& activity) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.activity = activity;
}

void SetEnabled(bool enabled) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.enabled = enabled;
}

void Stop() {
    State& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.running = false;
    }
    s.wake.notify_all();
    if (s.thread.joinable()) s.thread.join();
}

}  // namespace nds::discord

#endif  // NDS_DISCORD_SOCIAL_SDK
