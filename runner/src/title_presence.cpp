#include "title_presence.h"

#include <atomic>

namespace {
// Constant-initialized, so a title's static initializer may set it first.
std::atomic<NdsTitlePresenceQuery> g_query{nullptr};
}  // namespace

void nds_title_set_presence_query(NdsTitlePresenceQuery query) {
    g_query.store(query, std::memory_order_release);
}

bool nds_title_presence(NdsTitlePresence* out) {
    const NdsTitlePresenceQuery query = g_query.load(std::memory_order_acquire);
    return query && out && query(out);
}
