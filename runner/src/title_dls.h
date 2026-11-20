// title_dls.h -- the files a title's download server (Nintendo's DLS1,
// dls1.nintendowifi.net/download) hands out, for the built-in WFC server
// (net/wfc_local_server.h, [network.wfc] provider = "built-in").
//
// The server asks the registered query on every download request, on the
// emulation thread, and does the rest itself: the count / list / contents
// actions and the attribute filters. The query builds the files there and
// then (they are small), so it can pick them by the date or by the console.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct NdsDlsRequest {
    std::string gamecd;  // e.g. "YDQE"
    uint8_t mac[6];      // the console's MAC address
};

struct NdsDlsFile {
    std::string name;
    std::string attr1, attr2, attr3;
    std::vector<uint8_t> data;
};

typedef void (*NdsTitleDlsQuery)(const NdsDlsRequest& request,
                                 std::vector<NdsDlsFile>* files);
// Call from a static initializer, like a setting. nullptr removes it.
void nds_title_set_dls_query(NdsTitleDlsQuery query);
// For the server: the files for this request (none with no query).
std::vector<NdsDlsFile> nds_title_dls_files(const NdsDlsRequest& request);
