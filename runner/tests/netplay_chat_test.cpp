// netplay_chat_test.cpp -- line cleaning and send routing of the netplay
// chat (net/netplay_chat.h). Built without NDS_ONLINE: no masking.

#include "net/netplay_chat.h"

#include <cstdio>
#include <string>

namespace {

bool require(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

bool TestClean() {
    bool ok = true;
    ok = require(nds_netplay_chat_clean("  hi  ", 240) == "hi", "trim") && ok;
    ok = require(nds_netplay_chat_clean("a\nb\tc\x01" "d", 240) == "a b cd",
                 "controls") && ok;
    ok = require(nds_netplay_chat_clean("\xC2\x85x", 240) == "x",
                 "C1 control") && ok;
    ok = require(nds_netplay_chat_clean("a\xFF" "b\xC3", 240) == "ab",
                 "malformed UTF-8") && ok;
    // "é" is two bytes: cut before it rather than through it.
    ok = require(nds_netplay_chat_clean("ab\xC3\xA9", 3) == "ab",
                 "cut on a character boundary") && ok;
    ok = require(nds_netplay_chat_clean("\xE3\x81\x82", 240) ==
                     "\xE3\x81\x82", "kana kept") && ok;
    ok = require(nds_netplay_chat_clean(" \n ", 240).empty(), "blank") && ok;
    return ok;
}

bool TestRouting() {
    bool ok = true;
    std::string from, text;
    nds_netplay_chat_set_name("Hero");
    ok = require(nds_netplay_chat_send("hello") == NdsChatSendResult::NoOne,
                 "nobody in range") && ok;
    ok = require(nds_netplay_chat_send("   ") == NdsChatSendResult::Empty,
                 "empty line") && ok;

    // LAN only: queued for the transport and listed at once.
    nds_netplay_chat_set_lan_peers(1);
    const uint64_t before = nds_netplay_chat_last_id();
    ok = require(nds_netplay_chat_send(" hi ") == NdsChatSendResult::Sent,
                 "lan send") && ok;
    ok = require(nds_netplay_chat_take_lan(&from, &text) && from == "Hero" &&
                     text == "hi", "lan outbox") && ok;
    ok = require(!nds_netplay_chat_take_online(&text), "no online line") && ok;
    auto lines = nds_netplay_chat_since(before);
    ok = require(lines.size() == 1 && lines[0].self &&
                     lines[0].source == NdsChatSource::Lan &&
                     lines[0].text == "hi", "own lan line listed") && ok;

    // Online room too: the server echoes the line, so nothing is listed.
    nds_netplay_chat_set_online_room(true);
    const uint64_t mid = nds_netplay_chat_last_id();
    ok = require(nds_netplay_chat_send("both") == NdsChatSendResult::Sent,
                 "both send") && ok;
    ok = require(nds_netplay_chat_take_online(&text) && text == "both",
                 "online outbox") && ok;
    ok = require(nds_netplay_chat_take_lan(&from, &text) && text == "both",
                 "lan outbox too") && ok;
    ok = require(nds_netplay_chat_last_id() == mid, "waits for echo") && ok;

    // Leaving drops what was still queued.
    nds_netplay_chat_send("late");
    nds_netplay_chat_set_online_room(false);
    nds_netplay_chat_set_lan_peers(0);
    ok = require(!nds_netplay_chat_take_online(&text) &&
                     !nds_netplay_chat_take_lan(&from, &text),
                 "outboxes cleared") && ok;

    // Received lines: cleaned, empty ones dropped, ids increase.
    NdsChatLine in;
    in.source = NdsChatSource::Lan;
    in.from = "Ally\n";
    in.text = "yo\x07";
    nds_netplay_chat_push(in);
    in.text = "\n";
    nds_netplay_chat_push(in);
    lines = nds_netplay_chat_since(mid);
    ok = require(lines.size() == 1 && lines[0].from == "Ally" &&
                     lines[0].text == "yo" && !lines[0].self &&
                     lines[0].id == mid + 1, "received line") && ok;
    return ok;
}

}  // namespace

int main() {
    bool ok = TestClean();
    ok = TestRouting() && ok;
    if (!ok) return 1;
    std::printf("netplay_chat_test: ok\n");
    return 0;
}
