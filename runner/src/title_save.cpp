#include "title_save.h"

#include <cstdio>
#include <mutex>

namespace {

struct State {
    std::mutex mutex;
    bool requested = false;  // waiting for the frontend to take it
    bool busy = false;       // taken, not finished yet
    NdsTitleSaveRequest request;
};

State& state() {
    static State s;
    return s;
}

void request(NdsTitleSaveAction action, NdsTitleSaveConvert convert) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.requested || s.busy) {
        std::fprintf(stderr, "[save] %s ignored: one is already pending\n",
                     action == NdsTitleSaveAction::Import ? "import" : "export");
        return;
    }
    std::fprintf(stderr, "[save] %s requested\n",
                 action == NdsTitleSaveAction::Import ? "import" : "export");
    s.requested = true;
    s.request = {action, convert};
}

}  // namespace

void nds_title_save_import(NdsTitleSaveConvert convert) {
    request(NdsTitleSaveAction::Import, convert);
}

void nds_title_save_export() {
    request(NdsTitleSaveAction::Export, nullptr);
}

bool nds_title_save_take_request(NdsTitleSaveRequest* out) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.requested) return false;
    s.requested = false;
    s.busy = true;
    *out = s.request;
    return true;
}

void nds_title_save_finish() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.busy = false;
}
