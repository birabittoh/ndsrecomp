#include "title_dls.h"

#include <atomic>

namespace {
// Constant-initialized, so a title's static initializer may set it first.
std::atomic<NdsTitleDlsQuery> g_query{nullptr};
}  // namespace

void nds_title_set_dls_query(NdsTitleDlsQuery query) {
    g_query.store(query, std::memory_order_release);
}

std::vector<NdsDlsFile> nds_title_dls_files(const NdsDlsRequest& request) {
    std::vector<NdsDlsFile> files;
    if (const NdsTitleDlsQuery query = g_query.load(std::memory_order_acquire))
        query(request, &files);
    return files;
}
