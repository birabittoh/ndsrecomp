#include "release_recipe.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include "sha1.h"
#include "title_banks.h"
#include "toml.hpp"
#include "runtime_arm.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kVersion = 1;
constexpr uint32_t kNitroFooter = 0xDEC00621u;
enum Op : uint8_t { kKnown, kShift, kCopy, kWord, kBl, kLit };

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

uint32_t u32(const uint8_t* d) {
    return uint32_t{d[0]} | (uint32_t{d[1]} << 8) | (uint32_t{d[2]} << 16) |
           (uint32_t{d[3]} << 24);
}

uint32_t u32(const std::vector<uint8_t>& d, size_t at) { return u32(&d[at]); }

void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) d[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

std::string hex_digest(const uint8_t* d) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 20; ++i) {
        out.push_back(digits[d[i] >> 4]);
        out.push_back(digits[d[i] & 15]);
    }
    return out;
}

std::string hex(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%llX",
                  static_cast<unsigned long long>(value));
    return buffer;
}

std::string toml_string(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out + "\"";
}

bool read_file(const fs::path& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0);
    out->resize(static_cast<size_t>(size));
    return size == 0 ||
           static_cast<bool>(in.read(reinterpret_cast<char*>(out->data()),
                                     size));
}

bool write_file(const fs::path& path, const std::vector<uint8_t>& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!data.empty())
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

uint16_t crc16(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return static_cast<uint16_t>(crc);
}

// --- recipe reading --------------------------------------------------------

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    bool ok = true;

    bool need(size_t n) {
        if (!ok || size - pos < n) ok = false;
        return ok;
    }
    uint16_t u16v() {
        if (!need(2)) return 0;
        const uint16_t v = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        return v;
    }
    uint32_t u32v() {
        if (!need(4)) return 0;
        const uint32_t v = u32(data + pos);
        pos += 4;
        return v;
    }
    const uint8_t* bytes(size_t n) {
        if (!need(n)) return nullptr;
        const uint8_t* p = data + pos;
        pos += n;
        return p;
    }
    std::string text(size_t n) {
        const uint8_t* p = bytes(n);
        return p ? std::string(reinterpret_cast<const char*>(p), n) : "";
    }
    uint64_t leb() {
        uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (!need(1)) return 0;
            const uint8_t byte = data[pos++];
            value |= uint64_t{byte & 0x7Fu} << shift;
            if (!(byte & 0x80)) return value;
        }
        ok = false;
        return 0;
    }
    int64_t zigzag() {
        const uint64_t v = leb();
        return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
    }
};

struct Segment {
    uint32_t offset, address, size;
};

struct RecipeFile {
    std::string name;
    std::string bank;
    uint32_t size = 0;
    const uint8_t* sha1 = nullptr;
    std::vector<Segment> segments;
    const uint8_t* ops = nullptr;
    uint32_t ops_size = 0;
};

struct Recipe {
    NdsReleaseRecipeInfo info;
    std::vector<RecipeFile> files;
};

bool parse(const uint8_t* data, size_t size, Recipe* recipe,
           std::string* error) {
    Reader r{data, size};
    const uint8_t* magic = r.bytes(4);
    if (!magic || std::memcmp(magic, "NRCP", 4) != 0)
        return fail(error, "not a release recipe");
    if (r.u32v() != kVersion)
        return fail(error, "a release recipe of another version");
    const uint8_t* runs_as = r.bytes(20);
    const uint8_t* applies_to = r.bytes(20);
    const uint32_t count = r.u32v();
    if (!r.ok) return fail(error, "truncated release recipe");
    recipe->info = {hex_digest(runs_as), hex_digest(applies_to)};
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        RecipeFile f;
        f.name = r.text(r.u16v());
        f.bank = r.text(r.u16v());
        f.size = r.u32v();
        f.sha1 = r.bytes(20);
        const uint32_t segments = r.u32v();
        for (uint32_t s = 0; s < segments && r.ok; ++s) {
            Segment seg;
            seg.offset = r.u32v();
            seg.address = r.u32v();
            seg.size = r.u32v();
            if (uint64_t{seg.offset} + seg.size > f.size) r.ok = false;
            f.segments.push_back(seg);
        }
        f.ops_size = r.u32v();
        f.ops = r.bytes(f.ops_size);
        recipe->files.push_back(std::move(f));
    }
    if (!r.ok) return fail(error, "truncated release recipe");
    return true;
}

// --- validation copies -----------------------------------------------------

using Rows = std::map<std::string, std::set<const NdsStaticValidation*>>;

void collect(const char* bank, const NdsDispatchEntry* table, unsigned length,
             void* context) {
    auto& rows = (*static_cast<Rows*>(context))[bank];
    for (unsigned i = 0; i < length; ++i)
        if (table[i].validation) rows.insert(table[i].validation);
}

// The bytes of a file the rows give; a byte two rows disagree on is unknown
// (tools/release_recipe.py known_bytes).
void known_bytes(const std::set<const NdsStaticValidation*>& rows,
                 const RecipeFile& f, std::vector<uint8_t>* known,
                 std::vector<uint8_t>* have) {
    known->assign(f.size, 0);
    have->assign(f.size, 0);
    std::vector<uint8_t> bad(f.size, 0);
    for (const NdsStaticValidation* row : rows) {
        if (!row->expected) continue;
        for (const Segment& seg : f.segments) {
            const uint64_t lo = std::max<uint64_t>(row->addr, seg.address);
            const uint64_t hi = std::min<uint64_t>(uint64_t{row->addr} + row->size,
                                                   uint64_t{seg.address} + seg.size);
            for (uint64_t a = lo; a < hi; ++a) {
                const size_t i = seg.offset + (a - seg.address);
                const uint8_t b = row->expected[a - row->addr];
                if ((*have)[i] && (*known)[i] != b) bad[i] = 1;
                (*known)[i] = b;
                (*have)[i] = 1;
            }
        }
    }
    for (size_t i = 0; i < f.size; ++i)
        if (bad[i]) (*have)[i] = 0;
}

bool apply(const RecipeFile& f, const std::vector<uint8_t>& other,
           const std::vector<uint8_t>& known, const std::vector<uint8_t>& have,
           std::vector<uint8_t>* out, std::string* error) {
    out->assign(f.size, 0);
    Reader r{f.ops, f.ops_size};
    uint64_t pos = 0;
    int64_t delta = 0;
    auto source = [&](uint64_t n) -> const uint8_t* {
        const int64_t at = static_cast<int64_t>(pos) + delta;
        if (at < 0 || static_cast<uint64_t>(at) + n > other.size()) return nullptr;
        return other.data() + at;
    };
    const std::string where = f.name + ": ";
    while (r.ok && r.pos < r.size) {
        const uint8_t kind = r.data[r.pos++];
        if (kind == kShift) {
            delta = r.zigzag();
            continue;
        }
        if (kind == kWord || kind == kBl) {
            const int64_t k = r.zigzag();
            const uint8_t* s = source(4);
            if (!r.ok || pos + 4 > f.size || !s)
                return fail(error, where + "recipe reads past a file");
            uint32_t w = u32(s);
            w = kind == kWord
                ? static_cast<uint32_t>(w + static_cast<uint32_t>(k))
                : (w & 0xFF000000u) |
                      ((w + static_cast<uint32_t>(k)) & 0x00FFFFFFu);
            put32(*out, pos, w);
            pos += 4;
            continue;
        }
        const uint64_t n = r.leb();
        if (!r.ok || pos + n > f.size)
            return fail(error, where + "recipe runs past the file");
        if (kind == kKnown) {
            for (uint64_t i = pos; i < pos + n; ++i)
                if (!have[i])
                    return fail(error, where + "the banks lack bytes the recipe needs");
            std::copy(known.begin() + pos, known.begin() + pos + n,
                      out->begin() + pos);
        } else if (kind == kCopy) {
            const uint8_t* s = source(n);
            if (!s) return fail(error, where + "recipe reads past a file");
            std::copy(s, s + n, out->begin() + pos);
        } else if (kind == kLit) {
            const uint8_t* s = r.bytes(n);
            if (!s) return fail(error, where + "truncated release recipe");
            std::copy(s, s + n, out->begin() + pos);
        } else {
            return fail(error, where + "unknown recipe op");
        }
        pos += n;
    }
    if (!r.ok || pos != f.size)
        return fail(error, where + "recipe does not cover the file");
    if (std::memcmp(gba::sha1(out->data(), out->size()).bytes.data(), f.sha1, 20) != 0)
        return fail(error, where + "rebuilt file has the wrong SHA-1");
    return true;
}

// --- the source folder -----------------------------------------------------

struct Folder {
    toml::table manifest;
    std::vector<uint8_t> header;
    std::vector<uint8_t> y9;
};

// The ARM9 footer (module params offset) sits right after the ARM9 binary;
// nds_folder keeps it as an extra/ file.
bool arm9_params(const fs::path& dir, const Folder& folder, uint32_t* params,
                 std::string* error) {
    const uint32_t end = u32(folder.header, 0x20) + u32(folder.header, 0x2C);
    if (const toml::table* extra = folder.manifest["extra"].as_table()) {
        for (const auto& [key, value] : *extra) {
            if (value.value<int64_t>().value_or(-1) != int64_t{end}) continue;
            std::vector<uint8_t> footer;
            if (read_file(dir / std::string(key.str()), &footer) &&
                footer.size() >= 8 && u32(footer, 0) == kNitroFooter) {
                *params = u32(footer, 4);
                return true;
            }
        }
    }
    return fail(error, "the ARM9 has no footer naming its module params");
}

bool read_code(const fs::path& dir, const Folder& folder,
               const std::string& name, std::vector<uint8_t>* out,
               std::string* error) {
    std::vector<uint8_t> raw;
    if (!read_file(dir / name, &raw))
        return fail(error, "cannot read " + (dir / name).string());
    if (name == "arm9.bin") {
        uint32_t params = 0;
        if (!arm9_params(dir, folder, &params, error)) return false;
        if (uint64_t{params} + 0x18 > raw.size())
            return fail(error, "arm9.bin is shorter than its module params");
        const uint32_t ram = u32(folder.header, 0x28);
        const uint32_t end = u32(raw, params + 0x14);
        if (!end) {
            *out = std::move(raw);
            return true;
        }
        if (end < ram || end - ram > raw.size())
            return fail(error, "arm9.bin: bad compressed end");
        std::vector<uint8_t> head(raw.begin(), raw.begin() + (end - ram));
        if (!nds_code_decompress(head, out, error)) return false;
        out->insert(out->end(), raw.begin() + (end - ram), raw.end());
        return true;
    }
    unsigned id = 0;
    if (std::sscanf(name.c_str(), "overlay/overlay9_%u.bin", &id) != 1)
        return fail(error, "a recipe names " + name + ", which is not code");
    for (size_t at = 0; at + 32 <= folder.y9.size(); at += 32) {
        if (u32(folder.y9, at) != id) continue;
        if (folder.y9[at + 0x1F] & 1) return nds_code_decompress(raw, out, error);
        *out = std::move(raw);
        return true;
    }
    return fail(error, name + " is not in the overlay table");
}

bool link_or_copy(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::create_hard_link(from, to, ec);
    if (!ec) return true;
    return fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
}

NdsReleasePrepare g_prepare = nullptr;

}  // namespace

void nds_release_set_prepare(NdsReleasePrepare prepare) { g_prepare = prepare; }

bool nds_release_recipe_info(const uint8_t* recipe, size_t size,
                             NdsReleaseRecipeInfo* info, std::string* error) {
    Recipe parsed;
    if (!parse(recipe, size, &parsed, error)) return false;
    *info = parsed.info;
    return true;
}

bool nds_code_decompress(const std::vector<uint8_t>& data,
                         std::vector<uint8_t>* out, std::string* error) {
    const size_t len = data.size();
    if (len < 8) return fail(error, "compressed code is too short");
    const uint32_t extra = u32(data, len - 4);
    if (extra == 0) {
        *out = data;
        return true;
    }
    const uint32_t header = u32(data, len - 8);
    const size_t header_len = header >> 24;
    size_t compressed_len = header & 0xFFFFFFu;
    if (header_len < 8 || header_len > len || compressed_len > len)
        return fail(error, "bad code compression footer");
    const size_t passthrough = len - compressed_len;
    // Compressed bytes, read from the end backwards.
    const uint8_t* comp = data.data() + passthrough;
    const size_t comp_size = compressed_len - header_len;
    std::vector<uint8_t> dec(len + extra - passthrough);
    const size_t dec_len = dec.size();
    size_t written = 0, read = 0;
    uint8_t flags = 0, mask = 1;
    auto next = [&](uint8_t* byte) {
        if (read >= comp_size) return false;
        *byte = comp[comp_size - 1 - read++];
        return true;
    };
    while (written < dec_len) {
        if (mask == 1) {
            if (!next(&flags)) return fail(error, "compressed code runs short");
            mask = 0x80;
        } else {
            mask >>= 1;
        }
        if (flags & mask) {
            uint8_t b1, b2;
            if (!next(&b1) || !next(&b2))
                return fail(error, "compressed code runs short");
            const size_t length = (b1 >> 4) + 3;
            size_t disp = ((size_t{b1} & 0x0F) << 8 | b2) + 3;
            if (disp > written) {
                if (written < 2)
                    return fail(error, "compressed code reaches before its start");
                disp = 2;  // as ndspy and DSDecmp
            }
            size_t from = written - disp;
            for (size_t i = 0; i < length && written < dec_len; ++i)
                dec[dec_len - 1 - written++] = dec[dec_len - 1 - from++];
        } else {
            uint8_t b;
            if (!next(&b)) return fail(error, "compressed code runs short");
            dec[dec_len - 1 - written++] = b;
        }
    }
    out->assign(data.begin(), data.begin() + passthrough);
    out->insert(out->end(), dec.begin(), dec.end());
    return true;
}

bool nds_code_compress(const std::vector<uint8_t>& data, size_t prefix,
                       std::vector<uint8_t>* out) {
    if (prefix > data.size()) return false;
    // ndspy's _lzCommon.compress over the body reversed (window 0x1002,
    // matches 3..18 that end before the current byte, the longest and of
    // those the nearest), with hash chains in place of its substring search.
    std::vector<uint8_t> rev(data.rbegin(), data.rend() - prefix);
    const size_t n = rev.size();
    constexpr size_t kWindow = 0x1002, kMaxLen = 18, kHashSize = 1u << 16;
    auto hash3 = [&](size_t i) {
        return ((size_t{rev[i]} << 8) ^ (size_t{rev[i + 1]} << 4) ^ rev[i + 2]) &
               (kHashSize - 1);
    };
    std::vector<int64_t> head(kHashSize, -1), prev(n, -1);
    for (size_t i = 0; i + 3 <= n; ++i) {
        const size_t h = hash3(i);
        prev[i] = head[h];
        head[h] = static_cast<int64_t>(i);
    }
    auto search = [&](size_t pos, size_t* match_pos) {
        const size_t cap = std::min(kMaxLen, n - pos);
        const size_t start = pos > kWindow ? pos - kWindow : 0;
        size_t best = 0;
        if (cap < 3) return best;
        for (int64_t c = prev[pos]; c >= 0 && size_t(c) >= start; c = prev[c]) {
            const size_t p = size_t(c);
            if (p + 3 > pos) continue;
            const size_t limit = std::min(cap, pos - p);
            size_t len = 0;
            while (len < limit && rev[p + len] == rev[pos + len]) ++len;
            if (len > best) {
                best = len;
                *match_pos = p;
                if (best == cap) break;
            }
        }
        return best;
    };
    std::vector<uint8_t> result;
    size_t current = 0, ignorable_data = 0, ignorable_comp = 0;
    int64_t best_savings = 0;
    while (current < n) {
        const size_t flags_at = result.size();
        result.push_back(0);
        ++ignorable_comp;
        uint8_t flags = 0;
        for (int i = 0; i < 8; ++i) {
            if (current >= n) continue;
            size_t match_pos = 0;
            const size_t len = search(current, &match_pos);
            if (len > 2) {
                const size_t disp = current - match_pos - 3;
                flags |= static_cast<uint8_t>(1u << (7 - i));
                result.push_back(static_cast<uint8_t>(((len - 3) << 4) | (disp >> 8)));
                result.push_back(static_cast<uint8_t>(disp));
                current += len;
                ignorable_data += len;
                ignorable_comp += 2;
            } else {
                result.push_back(rev[current++]);
                ++ignorable_data;
                ++ignorable_comp;
            }
            const int64_t savings = int64_t(current) - int64_t(result.size());
            if (savings > best_savings) {
                ignorable_data = ignorable_comp = 0;
                best_savings = savings;
            }
        }
        result[flags_at] = flags;
    }
    std::reverse(result.begin(), result.end());
    if (result.empty() || n + 4 < ((result.size() + 3) & ~size_t{4}) + 8)
        return false;
    // ndspy leaves the stream's last bytes (the first ones in the file) raw
    // when they save nothing. Nintendo's compressor does too, except when
    // they are only literals under the last flag byte: then it keeps them in
    // the stream, which changes nothing but the footer's compressed length.
    if (ignorable_data == ignorable_comp) ignorable_data = ignorable_comp = 0;
    std::vector<uint8_t> body(data.begin() + prefix,
                              data.begin() + prefix + ignorable_data);
    body.insert(body.end(), result.begin() + ignorable_comp, result.end());
    const size_t comp_len = result.size() - ignorable_comp;
    size_t header_len = 8;
    const size_t extra = n - body.size();
    while (body.size() % 4) {
        body.push_back(0xFF);
        ++header_len;
    }
    out->assign(data.begin(), data.begin() + prefix);
    out->insert(out->end(), body.begin(), body.end());
    out->resize(out->size() + 8);
    put32(*out, out->size() - 8,
          static_cast<uint32_t>((comp_len + header_len) | (header_len << 24)));
    put32(*out, out->size() - 4, static_cast<uint32_t>(extra - header_len));
    return true;
}

bool nds_release_convert_folder(const fs::path& unpacked, const fs::path& dest,
                                const uint8_t* recipe_data, size_t recipe_size,
                                std::string* error) {
    Recipe recipe;
    if (!parse(recipe_data, recipe_size, &recipe, error)) return false;

    // The title's own step first, if it has one for this release.
    fs::path prepared_dir = dest;
    prepared_dir += ".prepared";
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code ec;
            if (!dir.empty()) fs::remove_all(dir, ec);
        }
    } cleanup;
    fs::path source = unpacked;
    if (g_prepare) {
        std::error_code ec;
        fs::remove_all(prepared_dir, ec);
        cleanup.dir = prepared_dir;
        bool prepared = false;
        if (!g_prepare(unpacked, prepared_dir, &prepared, error)) return false;
        if (prepared) source = prepared_dir;
    }

    Folder folder;
    try {
        folder.manifest = toml::parse_file((source / "rom.toml").string());
    } catch (const std::exception& e) {
        return fail(error, (source / "rom.toml").string() + ": " + e.what());
    }
    if (folder.manifest["rom"]["sha1"].value_or(std::string()) !=
        recipe.info.applies_to)
        return fail(error, source.string() + " is not the release this recipe "
                           "rebuilds from");
    if (!read_file(source / "header.bin", &folder.header) ||
        folder.header.size() < 0x200 ||
        !read_file(source / "y9.bin", &folder.y9))
        return fail(error, "cannot read the header and overlay table in " +
                               source.string());

    Rows rows;
    nds_visit_configured_title_banks(recipe.info.runs_as.c_str(), collect, &rows);

    std::map<std::string, std::vector<uint8_t>> rebuilt;
    for (const RecipeFile& f : recipe.files) {
        std::vector<uint8_t> other, known, have;
        if (f.name == "header.bin") {
            other = folder.header;
        } else if (!read_code(source, folder, f.name, &other, error)) {
            return false;
        }
        if (!f.bank.empty()) {
            const auto bank = rows.find(f.bank);
            if (bank == rows.end())
                return fail(error, "this build has no bank " + f.bank +
                                   " for " + recipe.info.runs_as);
            known_bytes(bank->second, f, &known, &have);
        } else {
            known.assign(f.size, 0);
            have.assign(f.size, 0);
        }
        if (!apply(f, other, known, have, &rebuilt[f.name], error)) return false;
    }

    // ARM9: compressed again and put back where the source's was. A game
    // can read its own ARM9 from the card (DQ9's camp menu never opens when
    // those card bytes are blank), so the card has to hold the bytes the
    // runs_as image does there. Code compression is deterministic, and its
    // output rebuilt from YDQE matches YDQP's arm9.bin byte for byte.
    std::vector<uint8_t>& arm9 = rebuilt["arm9.bin"];
    if (arm9.empty()) return fail(error, "the recipe rebuilds no ARM9");
    uint32_t params = 0;
    if (!arm9_params(source, folder, &params, error)) return false;
    if (uint64_t{params} + 0x18 > arm9.size() || params + 0x18 > 0x4000)
        return fail(error, "the rebuilt ARM9's module params are out of place");
    {
        std::vector<uint8_t> packed, check;
        if (!nds_code_compress(arm9, 0x4000, &packed))
            return fail(error, "the rebuilt ARM9 does not compress");
        // Its compressed end, in the part left raw.
        put32(packed, params + 0x14,
              u32(folder.header, 0x28) + static_cast<uint32_t>(packed.size()));
        put32(arm9, params + 0x14, u32(packed, params + 0x14));
        if (!nds_code_decompress(packed, &check, error) || check != arm9)
            return fail(error, "the rebuilt ARM9 does not compress back to itself");
        arm9 = std::move(packed);
    }
    const uint32_t arm9_offset = u32(folder.header, 0x20);
    const uint32_t old_footer = arm9_offset + u32(folder.header, 0x2C);
    const uint32_t new_footer = arm9_offset + static_cast<uint32_t>(arm9.size());
    std::string old_footer_name;
    std::vector<uint8_t> footer;
    char footer_name[32];
    std::snprintf(footer_name, sizeof footer_name, "extra/%08X.bin", new_footer);

    // Overlay table: rebuilt overlays are uncompressed.
    std::vector<uint8_t> y9 = folder.y9;
    for (size_t at = 0; at + 32 <= y9.size(); at += 32) {
        char name[64];
        std::snprintf(name, sizeof name, "overlay/overlay9_%04u.bin", u32(y9, at));
        const auto it = rebuilt.find(name);
        if (it == rebuilt.end()) continue;
        if (it->second.size() > 0xFFFFFFu)
            return fail(error, std::string(name) + " is too large");
        put32(y9, at + 0x1C, static_cast<uint32_t>(it->second.size()));
    }

    // Layout: the ARM9 and its footer (moved to the ARM9's new end) have to
    // fit before whatever the source places after its ARM9.
    uint64_t room = UINT64_MAX;
    for (const char* table : {"regions", "extra"}) {
        if (const toml::table* t = folder.manifest[table].as_table()) {
            for (const auto& [key, value] : *t) {
                const std::string name(key.str());
                const int64_t offset = value.value<int64_t>().value_or(0);
                if (name == "arm9.bin") continue;
                if (offset == int64_t{old_footer}) {
                    old_footer_name = name;
                    if (!read_file(source / name, &footer))
                        return fail(error, "cannot read " + (source / name).string());
                    continue;
                }
                if (offset > int64_t{arm9_offset})
                    room = std::min<uint64_t>(room, uint64_t(offset));
            }
        }
    }
    if (old_footer_name.empty())
        return fail(error, "the ARM9 has no footer naming its module params");
    const toml::array* fat = folder.manifest["files"]["fat"].as_array();
    if (!fat) return fail(error, (source / "rom.toml").string() + " has no FAT");
    for (const toml::node& node : *fat) {
        const toml::array* entry = node.as_array();
        if (!entry || entry->size() != 3)
            return fail(error, (source / "rom.toml").string() + ": bad FAT entry");
        const int64_t offset = (*entry)[1].value<int64_t>().value_or(0);
        if (offset > int64_t{arm9_offset})
            room = std::min<uint64_t>(room, uint64_t(offset));
    }
    if (uint64_t{new_footer} + footer.size() > room)
        return fail(error, "the rebuilt ARM9 does not fit where the source's was");

    std::vector<uint8_t> header = folder.header;
    std::copy(rebuilt["header.bin"].begin(), rebuilt["header.bin"].end(),
              header.begin());
    put32(header, 0x2C, static_cast<uint32_t>(arm9.size()));
    const uint16_t crc = crc16(header.data(), 0x15E);
    header[0x15E] = static_cast<uint8_t>(crc);
    header[0x15F] = static_cast<uint8_t>(crc >> 8);

    fs::path tmp = dest;
    tmp += ".partial";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    auto abandon = [&](const std::string& message) {
        fs::remove_all(tmp, ec);
        return fail(error, message);
    };

    std::map<std::string, const std::vector<uint8_t>*> written = {
        {"header.bin", &header}, {"y9.bin", &y9}, {footer_name, &footer}};
    for (const auto& [name, data] : rebuilt)
        if (name != "header.bin") written[name] = &data;
    for (const auto& [name, data] : written)
        if (!write_file(tmp / name, *data))
            return abandon("cannot write " + (tmp / name).string());

    auto copy_rest = [&](const std::string& name) {
        if (written.count(name) || name == old_footer_name) return true;
        return link_or_copy(source / name, tmp / name);
    };
    for (const char* table : {"regions", "extra"})
        if (const toml::table* t = folder.manifest[table].as_table())
            for (const auto& [key, value] : *t)
                if (!copy_rest(std::string(key.str())))
                    return abandon("cannot copy " + std::string(key.str()));
    for (const toml::node& node : *fat) {
        const std::string name = (*node.as_array())[0].value_or(std::string());
        if (!copy_rest(name)) return abandon("cannot copy " + name);
    }

    // rom.toml: the source's, with the ARM9 footer moved and the identity
    // named.
    std::ostringstream m;
    std::string game_code(reinterpret_cast<const char*>(&header[0x0C]), 4);
    m << "# An unpacked Nintendo DS ROM (ndsrecomp tools/nds_folder.py),\n"
      << "# its ARM9 code rebuilt from this release by a release recipe\n"
      << "# (ndsrecomp tools/release_recipe.py) to run as `identity`.\n"
      << "\n[rom]\nformat = 1\n"
      << "game_code = " << toml_string(game_code) << "\n"
      << "revision = " << folder.manifest["rom"]["revision"].value_or(int64_t{0}) << "\n"
      << "sha1 = \"" << recipe.info.applies_to << "\"\n"
      << "identity = \"" << recipe.info.runs_as << "\"\n"
      << "size = 0x" << hex(folder.manifest["rom"]["size"].value_or(int64_t{0})) << "\n"
      << "fill = 0x" << hex(folder.manifest["rom"]["fill"].value_or(int64_t{0xFF})) << "\n"
      << "fat = 0x" << hex(folder.manifest["rom"]["fat"].value_or(int64_t{0})) << "\n";
    for (const char* table : {"regions", "extra"}) {
        m << "\n[" << table << "]\n";
        if (const toml::table* t = folder.manifest[table].as_table()) {
            for (const auto& [key, value] : *t) {
                const std::string name(key.str());
                if (name == old_footer_name) {
                    m << toml_string(footer_name) << " = 0x" << hex(new_footer) << "\n";
                    continue;
                }
                m << toml_string(name) << " = 0x"
                  << hex(value.value<int64_t>().value_or(0)) << "\n";
            }
        }
    }
    m << "\n[files]\nfat = [\n";
    for (const toml::node& node : *fat) {
        const toml::array& e = *node.as_array();
        m << "  [" << toml_string(e[0].value_or(std::string())) << ", 0x"
          << hex(e[1].value_or(int64_t{0})) << ", 0x"
          << hex(e[2].value_or(int64_t{0})) << "],\n";
    }
    m << "]\n";
    const std::string text = m.str();
    if (!write_file(tmp / "rom.toml",
                    std::vector<uint8_t>(text.begin(), text.end())))
        return abandon("cannot write " + (tmp / "rom.toml").string());

    fs::rename(tmp, dest, ec);
    if (ec) return abandon("cannot create " + dest.string() + ": " + ec.message());
    return true;
}
