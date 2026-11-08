// Strict-loader coverage for the mid-instruction hook manifest
// (docs/midasm-hooks.md). Emitter-side checks that need discovered
// functions live in test_hooks.py.
#include "config.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

const std::string kHeader =
    "version = 1\n"
    "[program]\n"
    "bank = \"fixture_arm9\"\n"
    "sha1 = \"16e6c9168e34f267cb427bacb388e4783dadc584\"\n";

const std::string kPlain =
    "[[hook]]\n"
    "address = 0x02000100\n"
    "mode = \"arm\"\n"
    "name = \"FixturePlain\"\n"
    "registers = [\"r0\", \"sp\", \"lr\", \"cpsr\"]\n";

bool parses(const std::filesystem::path& path, const std::string& text,
            ndsrecomp::HookManifest* out = nullptr) {
    {
        std::ofstream stream(path, std::ios::binary);
        stream << text;
        if (!stream.good()) return false;
    }
    ndsrecomp::HookManifest manifest;
    const bool ok = ndsrecomp::load_hook_manifest(path.string(), manifest);
    if (out) *out = manifest;
    return ok;
}

std::string hook(const std::string& body) {
    return "[[hook]]\n" + body;
}

}  // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      "ndsrecomp_hook_manifest_test.toml";
    int failures = 0;
    auto expect = [&](bool want, const std::string& text, const char* what) {
        if (parses(path, text) != want) {
            std::fprintf(stderr, "%s: expected %s\n", what,
                         want ? "accept" : "reject");
            ++failures;
        }
    };

    ndsrecomp::HookManifest manifest;
    if (!parses(path, kHeader + kPlain, &manifest) ||
        manifest.hooks.size() != 1u ||
        manifest.hooks[0].registers.size() != 4u ||
        manifest.hooks[0].registers[1] != 13u ||
        manifest.hooks[0].registers[2] != 14u ||
        manifest.hooks[0].registers[3] != ndsrecomp::kHookArgCpsr ||
        manifest.hooks[0].when != ndsrecomp::HookWhen::Before ||
        manifest.hooks[0].returns_bool ||
        manifest.hooks[0].action != ndsrecomp::HookAction::Continue) {
        std::fprintf(stderr, "plain hook did not parse to the defaults\n");
        ++failures;
    }

    const std::string cond = hook(
        "address = 0x02000200\nmode = \"thumb\"\nname = \"FixtureCond\"\n"
        "on_false = \"jump\"\njump_address = 0x02000210\n");
    if (!parses(path, kHeader + cond, &manifest) ||
        !manifest.hooks[0].returns_bool ||
        manifest.hooks[0].on_true != ndsrecomp::HookAction::Continue ||
        manifest.hooks[0].on_false != ndsrecomp::HookAction::Jump ||
        manifest.hooks[0].jump_address != 0x02000210u) {
        std::fprintf(stderr, "conditional hook did not parse\n");
        ++failures;
    }

    expect(true, kHeader + hook(
        "address = 0x02000300\nmode = \"arm\"\nname = \"FixtureRet\"\n"
        "action = \"return\"\n"), "unconditional return");
    expect(true, kHeader + kPlain + hook(
        "address = 0x02000100\nmode = \"arm\"\nname = \"FixtureAfter\"\n"
        "when = \"after\"\n"), "before and after at one site");
    expect(true, kHeader + kPlain + hook(
        "address = 0x02000100\nmode = \"thumb\"\nname = \"FixtureThumb\"\n"),
        "same address in the other mode");

    expect(false, kHeader, "no hooks");
    expect(false, kHeader + kPlain + "extra = 1\n", "unknown hook key");
    expect(false, "unknown = 1\n" + kHeader + kPlain, "unknown top-level key");
    expect(false, "version = 2\n" + kHeader.substr(kHeader.find('\n') + 1) +
                  kPlain, "wrong version");
    expect(false, kHeader + kPlain + kPlain, "duplicate site");
    expect(false, kHeader + hook(
        "address = 0x02000102\nmode = \"arm\"\nname = \"Unaligned\"\n"),
        "unaligned ARM address");
    expect(false, kHeader + hook(
        "address = 0x02000100\nmode = \"arm\"\nname = \"1bad\"\n"),
        "non-identifier name");
    expect(false, kHeader + hook(
        "address = 0x02000100\nmode = \"arm\"\nname = \"Pc\"\n"
        "registers = [\"r15\"]\n"), "r15 argument");
    expect(false, kHeader + hook(
        "address = 0x02000100\nmode = \"arm\"\nname = \"Pc\"\n"
        "registers = [\"pc\"]\n"), "pc argument");
    expect(false, kHeader + hook(
        "address = 0x02000100\nmode = \"arm\"\nname = \"Dup\"\n"
        "registers = [\"r1\", \"r1\"]\n"), "repeated register");
    expect(false, kHeader + hook(
        "address = 0x02000100\nmode = \"arm\"\nname = \"Mixed\"\n"
        "action = \"return\"\non_true = \"jump\"\njump_address = 0x02000104\n"),
        "action together with on_true");
    expect(false, kHeader + hook(
        "address = 0x02000100\nmode = \"arm\"\nname = \"NoTarget\"\n"
        "action = \"jump\"\n"), "jump without jump_address");
    expect(false, kHeader + hook(
        "address = 0x02000100\nmode = \"arm\"\nname = \"Stray\"\n"
        "jump_address = 0x02000104\n"), "jump_address without a jump");
    expect(false, kHeader + hook(
        "address = 0x02000100\nmode = \"arm\"\nname = \"TwoJumps\"\n"
        "on_true = \"jump\"\non_false = \"jump\"\n"
        "jump_address = 0x02000104\n"), "both outcomes jump");
    expect(false, kHeader + hook(
        "address = 0x02000100\nmode = \"arm\"\nname = \"Same\"\n"
        "on_true = \"continue\"\non_false = \"continue\"\n"),
        "both outcomes continue");
    expect(false, kHeader + hook(
        "address = 0x02000100\nmode = \"arm\"\nname = \"AfterRet\"\n"
        "when = \"after\"\naction = \"return\"\n"), "after hook that returns");
    expect(false, kHeader + hook(
        "address = 0x02000100\nmode = \"arm\"\nname = \"AfterCond\"\n"
        "when = \"after\"\non_true = \"return\"\n"),
        "after hook with a bool outcome");
    expect(false, kHeader + hook(
        "address = 0x02000200\nmode = \"thumb\"\nname = \"Odd\"\n"
        "action = \"jump\"\njump_address = 0x02000211\n"),
        "unaligned jump_address");
    expect(false, kHeader + kPlain + hook(
        "address = 0x02000104\nmode = \"arm\"\nname = \"FixturePlain\"\n"),
        "handler reused with a different signature");
    expect(true, kHeader + kPlain + hook(
        "address = 0x02000104\nmode = \"arm\"\nname = \"FixturePlain\"\n"
        "registers = [\"r1\", \"r2\", \"r3\", \"r4\"]\n"),
        "handler reused with the same arity");

    std::filesystem::remove(path);
    if (failures == 0) std::puts("hook_manifest_test: ok");
    return failures == 0 ? 0 : 1;
}
