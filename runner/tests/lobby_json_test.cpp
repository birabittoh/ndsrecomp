// lobby_json_test.cpp -- the online lobby client's JSON (net/lobby_json.h)
// and lobby URL parsing (net/lobby_ws.h).

#include "net/lobby_json.h"
#include "net/lobby_ws.h"

#include <cstdio>
#include <string>

namespace {

using mpnet::Json;

bool require(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

bool TestParse() {
    bool ok = true;
    const Json m = Json::Parse(
        R"({"op":"lobby_update","lobby_id":"L1","max_slots":4,)"
        R"("host_player_id":"p0","all_ready":false,"session_id":1,)"
        R"("slots":[{"slot":0,"player_id":"p0","display_name":"Marco"},)"
        R"({"slot":2,"player_id":"p2","display_name":"Ju\"li\u00e0"}],)"
        R"("match_caps":{"v":1,"nested":[1,2,{"x":null}]},"f":-1.5e2})");
    ok &= require(m.IsObject(), "object parsed");
    ok &= require(m.Str("op", "") == "lobby_update", "string member");
    ok &= require(m.Int("max_slots", 0) == 4, "int member");
    ok &= require(!m.Bool("all_ready", true), "bool member");
    ok &= require(m["slots"].size() == 2, "array size");
    ok &= require(m["slots"][1].Int("slot", -1) == 2, "nested int");
    ok &= require(m["slots"][1].Str("display_name", "") ==
                      "Ju\"li\xC3\xA0",
                  "escapes and \\u decoded to UTF-8");
    ok &= require(m["match_caps"]["nested"][2]["x"].IsNull(), "null value");
    ok &= require(m["f"].Num() == -150.0, "exponent number");
    ok &= require(m["missing"]["deeper"][3].Str("d") == "d",
                  "absent members fall back to the default");
    ok &= require(Json::Parse("{\"a\":1,}").IsNull(), "trailing comma refused");
    ok &= require(Json::Parse("{\"a\":1} x").IsNull(), "trailing junk refused");
    ok &= require(Json::Parse("[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]"
                              "]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]")
                      .IsNull(),
                  "nesting limit");
    ok &= require(Json::Parse("\"\\ud83d\\ude00\"").Str() == "\xF0\x9F\x98\x80",
                  "surrogate pair");
    return ok;
}

bool TestBuild() {
    bool ok = true;
    const std::string text = mpnet::JsonOp("signal")
                                 .Str("text", "a=x\r\n\"q\"\\")
                                 .Int("type", 150)
                                 .Bool("ok", true)
                                 .Text();
    const Json back = Json::Parse(text);
    ok &= require(back.Str("op", "") == "signal", "op round trip");
    ok &= require(back.Str("text", "") == "a=x\r\n\"q\"\\",
                  "escaped string round trip");
    ok &= require(back.Int("type", 0) == 150, "int round trip");
    ok &= require(back.Bool("ok", false), "bool round trip");
    ok &= require(mpnet::JsonQuote(std::string("\x01", 1)) == "\"\\u0001\"",
                  "control character escaped");
    return ok;
}

bool TestUrl() {
    bool ok = true;
    std::string host, path, error;
    uint16_t port = 0;
    ok &= require(mpnet::WsClient::ParseUrl("ws://netplay.example.org:8765",
                                            &host, &port, &path, &error) &&
                      host == "netplay.example.org" && port == 8765 &&
                      path == "/",
                  "host and port");
    ok &= require(mpnet::WsClient::ParseUrl("ws://127.0.0.1/ws", &host, &port,
                                            &path, &error) &&
                      host == "127.0.0.1" && port == 80 && path == "/ws",
                  "default port and path");
    ok &= require(!mpnet::WsClient::ParseUrl("wss://x:443", &host, &port,
                                             &path, &error),
                  "wss refused");
    ok &= require(!mpnet::WsClient::ParseUrl("http://x", &host, &port, &path,
                                             &error),
                  "other schemes refused");
    ok &= require(!mpnet::WsClient::ParseUrl("ws://x:0", &host, &port, &path,
                                             &error),
                  "port 0 refused");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= TestParse();
    ok &= TestBuild();
    ok &= TestUrl();
    std::printf("%s\n", ok ? "lobby_json_test: ok" : "lobby_json_test: FAILED");
    return ok ? 0 : 1;
}
