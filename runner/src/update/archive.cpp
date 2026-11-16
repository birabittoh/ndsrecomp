// update/archive.cpp -- see update/archive.h.
#include "update/archive.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <thread>
#include <vector>

#include <miniz.h>

namespace nds::archive {

namespace {

// Entry names are UTF-8 with forward slashes whatever the host.
std::filesystem::path ToPath(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// Rejects an absolute path or any ".." segment.
bool IsSafeRelativePath(std::string_view entry_name) {
    if (entry_name.empty()) return false;
    if (entry_name.front() == '/' || entry_name.front() == '\\') return false;
    // A drive-letter prefix ("C:") is absolute on Windows too.
    if (entry_name.size() >= 2 && entry_name[1] == ':') return false;
    size_t start = 0;
    while (start <= entry_name.size()) {
        const size_t end = entry_name.find_first_of("/\\", start);
        const std::string_view segment = entry_name.substr(
            start,
            (end == std::string_view::npos ? entry_name.size() : end) - start);
        if (segment == "..") return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

// Decompresses a gzip-wrapped (RFC 1952) deflate stream. Parses only enough
// of the header to find where the raw deflate data starts; the CRC32 and
// size footer (and anything after it, e.g. another member) is left alone,
// since tinfl's deflate decoder ends by itself.
bool GunzipToHeap(const std::vector<uint8_t>& data, std::vector<uint8_t>& out,
                  std::string& error) {
    if (data.size() < 18 || data[0] != 0x1f || data[1] != 0x8b) {
        error = "not a gzip file (bad magic)";
        return false;
    }
    if (data[2] != 8) {
        error = "unsupported gzip compression method";
        return false;
    }
    const uint8_t flags = data[3];
    size_t pos = 10;

    if (flags & 0x04) {  // FEXTRA
        if (pos + 2 > data.size()) {
            error = "truncated gzip header (FEXTRA)";
            return false;
        }
        const uint16_t xlen =
            static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2u + xlen;
    }
    auto skip_cstring = [&](const char* what) -> bool {
        while (pos < data.size() && data[pos] != 0) ++pos;
        if (pos >= data.size()) {
            error = std::string("truncated gzip header (") + what + ")";
            return false;
        }
        ++pos;
        return true;
    };
    if ((flags & 0x08) && !skip_cstring("FNAME")) return false;
    if ((flags & 0x10) && !skip_cstring("FCOMMENT")) return false;
    if (flags & 0x02) pos += 2;  // FHCRC
    if (pos >= data.size()) {
        error = "truncated gzip header";
        return false;
    }

    size_t out_len = 0;
    void* decompressed = tinfl_decompress_mem_to_heap(
        data.data() + pos, data.size() - pos, &out_len, 0);
    if (!decompressed) {
        error = "gzip payload is corrupt (deflate decode failed)";
        return false;
    }
    out.assign(static_cast<uint8_t*>(decompressed),
               static_cast<uint8_t*>(decompressed) + out_len);
    mz_free(decompressed);
    return true;
}

struct UstarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};
static_assert(sizeof(UstarHeader) == 512);

uint64_t ParseOctal(const char* field, size_t len) {
    uint64_t value = 0;
    for (size_t i = 0; i < len && field[i] != 0 && field[i] != ' '; ++i) {
        if (field[i] < '0' || field[i] > '7') break;
        value = value * 8 + static_cast<uint64_t>(field[i] - '0');
    }
    return value;
}

bool IsZeroBlock(const uint8_t* block) {
    for (size_t i = 0; i < 512; ++i) {
        if (block[i] != 0) return false;
    }
    return true;
}

}  // namespace

bool ExtractZip(const std::filesystem::path& archive,
                const std::filesystem::path& dest_dir, std::string& error) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, archive.string().c_str(), 0)) {
        error = "failed to open archive (corrupt or not a zip file)";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        mz_zip_reader_end(&zip);
        error = "failed to create destination directory: " + ec.message();
        return false;
    }

    bool ok = true;
    const mz_uint file_count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < file_count && ok; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            ok = false;
            error = "failed to read archive entry metadata";
            break;
        }
        const std::string_view name(stat.m_filename);
        if (!IsSafeRelativePath(name)) {
            ok = false;
            error = "archive entry has an unsafe path: " + std::string(name);
            break;
        }
        const std::filesystem::path dest_path = dest_dir / ToPath(name);

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            std::filesystem::create_directories(dest_path, ec);
            continue;
        }

        std::filesystem::create_directories(dest_path.parent_path(), ec);
        if (ec) {
            ok = false;
            error = "failed to create directory for " + std::string(name) +
                    ": " + ec.message();
            break;
        }

        // Each entry goes to a heap buffer, then out to its file: a release
        // is a few hundred MB at most, small enough to skip miniz's
        // streaming callback API.
        size_t extracted_size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &extracted_size, 0);
        if (!data) {
            ok = false;
            error = "failed to extract " + std::string(name);
            break;
        }
        std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            mz_free(data);
            ok = false;
            error = "failed to write " + dest_path.string();
            break;
        }
        out.write(static_cast<const char*>(data),
                  static_cast<std::streamsize>(extracted_size));
        mz_free(data);
    }

    mz_zip_reader_end(&zip);
    return ok;
}

bool ExtractTarGz(const std::filesystem::path& archive,
                  const std::filesystem::path& dest_dir, std::string& error) {
    std::ifstream in(archive, std::ios::binary);
    if (!in) {
        error = "failed to open archive";
        return false;
    }
    const std::vector<uint8_t> compressed((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    in.close();

    std::vector<uint8_t> tar;
    if (!GunzipToHeap(compressed, tar, error)) return false;

    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        error = "failed to create destination directory: " + ec.message();
        return false;
    }

    size_t pos = 0;
    std::string pending_long_name;  // a GNU long name ('L') entry's data
    while (pos + 512 <= tar.size()) {
        const uint8_t* block = tar.data() + pos;
        if (IsZeroBlock(block)) break;  // end of archive
        UstarHeader header;
        std::memcpy(&header, block, sizeof(UstarHeader));
        pos += 512;

        const uint64_t size = ParseOctal(header.size, sizeof(header.size));
        const size_t data_blocks = static_cast<size_t>((size + 511) / 512);
        if (pos + data_blocks * 512 > tar.size()) {
            error = "truncated tar archive";
            return false;
        }
        const uint8_t* entry_data = tar.data() + pos;
        pos += data_blocks * 512;

        if (header.typeflag == 'L') {
            // GNU long name: this entry's data is the next entry's real name
            // (NUL-terminated), in place of its truncated 100-byte field.
            pending_long_name.assign(
                reinterpret_cast<const char*>(entry_data),
                strnlen(reinterpret_cast<const char*>(entry_data),
                        static_cast<size_t>(size)));
            continue;
        }
        if (header.typeflag == 'x' || header.typeflag == 'g') {
            // A pax extended header: skipped, and the following entry's
            // ustar name and size used as they are, as tar readers that
            // ignore pax records do.
            continue;
        }

        std::string name;
        if (!pending_long_name.empty()) {
            name = pending_long_name;
            pending_long_name.clear();
        } else {
            const size_t prefix_len = strnlen(header.prefix, sizeof(header.prefix));
            const size_t name_len = strnlen(header.name, sizeof(header.name));
            if (prefix_len > 0) {
                name.assign(header.prefix, prefix_len);
                name += '/';
                name.append(header.name, name_len);
            } else {
                name.assign(header.name, name_len);
            }
        }
        if (name.empty()) continue;
        if (!IsSafeRelativePath(name)) {
            error = "archive entry has an unsafe path: " + name;
            return false;
        }

        const std::filesystem::path dest_path = dest_dir / ToPath(name);
        if (header.typeflag == '5') {  // directory
            std::filesystem::create_directories(dest_path, ec);
            continue;
        }
        if (header.typeflag != '0' && header.typeflag != '\0')
            continue;  // symlink, device, fifo, ...: not needed for an install

        std::filesystem::create_directories(dest_path.parent_path(), ec);
        if (ec) {
            error = "failed to create directory for " + name + ": " + ec.message();
            return false;
        }
        std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "failed to write " + dest_path.string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(entry_data),
                  static_cast<std::streamsize>(size));
    }

    return true;
}

bool MoveOrCopyDirectory(const std::filesystem::path& from,
                         const std::filesystem::path& to, std::string& error) {
    constexpr int kMaxAttempts = 5;
    constexpr auto kRetryDelay = std::chrono::milliseconds(100);

    std::error_code ec;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        ec.clear();
        std::filesystem::rename(from, to, ec);
        if (!ec) return true;
        if (attempt < kMaxAttempts) std::this_thread::sleep_for(kRetryDelay);
    }

    // A rename across file systems fails every time, not only transiently,
    // so it gets no more tries; the copy fallback has its own retry loop
    // for the same transient-lock reason as the rename above.
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        ec.clear();
        std::filesystem::create_directories(to, ec);
        if (!ec) {
            std::filesystem::copy(from, to,
                                  std::filesystem::copy_options::recursive |
                                      std::filesystem::copy_options::overwrite_existing,
                                  ec);
        }
        if (!ec) {
            std::filesystem::remove_all(from, ec);
            return true;
        }
        if (attempt < kMaxAttempts) std::this_thread::sleep_for(kRetryDelay);
    }

    error = "failed to move the extracted update: " + ec.message();
    return false;
}

}  // namespace nds::archive
