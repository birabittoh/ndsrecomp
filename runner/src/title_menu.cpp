#include "title_menu.h"

#include <mutex>
#include <utility>

namespace {

struct State {
    std::mutex mutex;
    std::vector<NdsTitleMenuRow> rows;
    std::vector<NdsTitleMenuRequest> requests;
};

State& state() {
    static State s;
    return s;
}

}  // namespace

std::vector<NdsTitleMenuRow> nds_title_menu_rows() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.rows;
}

void nds_title_menu_set(const std::string& key, int value) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.requests.push_back({key, false, value});
}

void nds_title_menu_run(const std::string& key) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.requests.push_back({key, true, 0});
}

void nds_title_menu_publish(std::vector<NdsTitleMenuRow> rows) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.rows = std::move(rows);
}

std::vector<NdsTitleMenuRequest> nds_title_menu_take_requests() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return std::exchange(s.requests, {});
}
