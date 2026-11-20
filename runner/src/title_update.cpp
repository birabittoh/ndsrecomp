#include "title_update.h"

#include <mutex>
#include <utility>

#ifndef NDS_RELEASE_VERSION
#define NDS_RELEASE_VERSION ""
#endif

namespace {

struct State {
    std::mutex mutex;
    bool published = false;
    NdsTitleUpdateStatus status;
    bool install_requested = false;
    std::string download_text;
};

State& state() {
    static State s;
    return s;
}

}  // namespace

NdsTitleUpdateStatus nds_title_update_status() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.published) return s.status;
    NdsTitleUpdateStatus status;
    status.current_version = NDS_RELEASE_VERSION;
    return status;
}

void nds_title_update_install() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.install_requested = true;
}

void nds_title_update_set_download_text(std::string text) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.download_text = std::move(text);
}

std::string nds_title_update_download_text() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.download_text;
}

void nds_title_update_publish(NdsTitleUpdateStatus status) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.status = std::move(status);
    s.published = true;
}

bool nds_title_update_take_install_request() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return std::exchange(s.install_requested, false);
}
