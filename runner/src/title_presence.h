// title_presence.h -- what a title shows as its Discord activity
// (discord_rpc.h).
//
// The frontend asks the registered query about once a second, on the
// frontend thread: the query reads guest memory (bus_read_*) to tell what
// the player is doing and fills in the activity's two lines. It returns
// false when it has nothing to say (the frontend then shows the game's name
// alone). Like the other title queries (title_settings.h), it only reads.
#pragma once

#include <string>

struct NdsTitlePresence {
    std::string details;  // first line, e.g. "In battle"
    std::string state;    // second line, may be empty
};

typedef bool (*NdsTitlePresenceQuery)(NdsTitlePresence* out);
// Call from a static initializer, like a setting. nullptr removes it.
void nds_title_set_presence_query(NdsTitlePresenceQuery query);
// For the frontend: the query's answer (false with none registered).
bool nds_title_presence(NdsTitlePresence* out);
