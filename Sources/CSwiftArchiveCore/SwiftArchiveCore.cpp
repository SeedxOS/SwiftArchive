/*
 * UnRAR source code may be used in any software to handle
 * RAR archives without limitations free of charge, but cannot be
 * used to develop RAR (WinRAR) compatible archiver and to
 * re-create RAR compression algorithm, which is proprietary.
 * Distribution of modified UnRAR source code in separate form
 * or as a part of other software is permitted, provided that
 * full text of this paragraph, starting from "UnRAR source code"
 * words, is included in license, or in documentation if license
 * is not available, and in source code comments of resulting package.
 *
 * See THIRD_PARTY_NOTICES.md and Sources/CUnRAR/LICENSE.UnRAR.
 */

#include "SwiftArchiveCore.h"

#include "CUnRAR.h"
#include <archive.h>
#include <archive_entry.h>
#include "mz.h"
#include "mz_os.h"
#include "mz_strm.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <ctime>
#include <cwchar>
#include <sys/stat.h>
#include <utime.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Entry {
    std::string path;
    SAEntryKind kind = SA_ENTRY_FILE;
    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;
    int64_t modification_time = 0;
    uint32_t crc32 = 0;
    bool encrypted = false;
    bool solid = false;
    bool split_before = false;
    bool split_after = false;
    uint64_t data_offset = 0;
};

struct RarCallbackContext {
    const std::string *password = nullptr;
    std::atomic<bool> *cancelled = nullptr;
    SADataCallback data_callback = nullptr;
    void *data_context = nullptr;
    SAProgressCallback progress_callback = nullptr;
    void *progress_context = nullptr;
    SAProgress progress{};
};

void clear_error(SAError *error) {
    if (!error)
        return;
    std::memset(error, 0, sizeof(*error));
    error->struct_size = sizeof(*error);
}

int32_t fail(SAError *error, SAErrorCode code, int32_t backend_code,
             const std::string &message) {
    if (error) {
        clear_error(error);
        error->code = code;
        error->backend_code = backend_code;
        std::snprintf(error->message, sizeof(error->message), "%s", message.c_str());
    }
    return 0;
}

SAErrorCode minizip_error_code(int32_t code, bool has_password) {
    switch (code) {
    case MZ_PASSWORD_ERROR:
        return has_password ? SA_ERROR_BAD_PASSWORD : SA_ERROR_PASSWORD_REQUIRED;
    case MZ_FORMAT_ERROR:
    case MZ_DATA_ERROR:
    case MZ_CRC_ERROR:
        return SA_ERROR_CORRUPT_ARCHIVE;
    case MZ_SUPPORT_ERROR:
        return SA_ERROR_UNSUPPORTED_FEATURE;
    case MZ_OPEN_ERROR:
        return SA_ERROR_OPEN_FAILED;
    case MZ_READ_ERROR:
    case MZ_WRITE_ERROR:
    case MZ_CLOSE_ERROR:
        return SA_ERROR_IO;
    default:
        return SA_ERROR_INTERNAL;
    }
}

SAErrorCode unrar_error_code(int32_t code, bool has_password) {
    switch (code) {
    case ERAR_MISSING_PASSWORD:
        return SA_ERROR_PASSWORD_REQUIRED;
    case ERAR_BAD_PASSWORD:
        return has_password ? SA_ERROR_BAD_PASSWORD : SA_ERROR_PASSWORD_REQUIRED;
    case ERAR_BAD_DATA:
    case ERAR_BAD_ARCHIVE:
    case ERAR_UNKNOWN_FORMAT:
        return SA_ERROR_CORRUPT_ARCHIVE;
    case ERAR_EOPEN:
        return SA_ERROR_OPEN_FAILED;
    case ERAR_ECREATE:
    case ERAR_ECLOSE:
    case ERAR_EREAD:
    case ERAR_EWRITE:
        return SA_ERROR_IO;
    case ERAR_LARGE_DICT:
        return SA_ERROR_RESOURCE_LIMIT;
    default:
        return SA_ERROR_INTERNAL;
    }
}

bool valid_utf8(const std::string &value) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(value.data());
    size_t i = 0;
    while (i < value.size()) {
        unsigned char c = bytes[i++];
        if (c < 0x80)
            continue;
        size_t trailing = 0;
        uint32_t codepoint = 0;
        if ((c & 0xe0) == 0xc0) {
            trailing = 1;
            codepoint = c & 0x1f;
        } else if ((c & 0xf0) == 0xe0) {
            trailing = 2;
            codepoint = c & 0x0f;
        } else if ((c & 0xf8) == 0xf0) {
            trailing = 3;
            codepoint = c & 0x07;
        } else {
            return false;
        }
        if (i + trailing > value.size())
            return false;
        for (size_t j = 0; j < trailing; ++j) {
            unsigned char t = bytes[i++];
            if ((t & 0xc0) != 0x80)
                return false;
            codepoint = (codepoint << 6) | (t & 0x3f);
        }
        if ((trailing == 1 && codepoint < 0x80) ||
            (trailing == 2 && codepoint < 0x800) ||
            (trailing == 3 && codepoint < 0x10000) ||
            codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
            return false;
    }
    return true;
}

int decoded_name_score(const std::string &value) {
    if (!valid_utf8(value))
        return -100000;
    int score = 0;
    const auto *bytes = reinterpret_cast<const unsigned char *>(value.data());
    size_t i = 0;
    while (i < value.size()) {
        uint32_t cp = bytes[i++];
        if (cp >= 0x80) {
            int trailing = (cp & 0xe0) == 0xc0 ? 1 : (cp & 0xf0) == 0xe0 ? 2 : 3;
            cp &= trailing == 1 ? 0x1f : trailing == 2 ? 0x0f : 0x07;
            while (trailing-- > 0)
                cp = (cp << 6) | (bytes[i++] & 0x3f);
        }
        if (cp < 0x20 || cp == 0x7f)
            score -= 30;
        else if ((cp >= 0x4e00 && cp <= 0x9fff) ||
                 (cp >= 0x3040 && cp <= 0x30ff) ||
                 (cp >= 0xac00 && cp <= 0xd7af))
            score += 6;
        else if (cp >= 0x2500 && cp <= 0x259f)
            score -= 8;
        else if (cp >= 0x80)
            score += 1;
    }
    return score;
}

std::string convert_encoding(const char *value, int32_t encoding) {
    char *converted = mz_os_utf8_string_create(value, encoding);
    if (!converted)
        return {};
    std::string result(converted);
    mz_os_utf8_string_delete(&converted);
    return result;
}

std::string decode_zip_name(const mz_zip_file *info, SAFilenameEncoding encoding) {
    std::string raw(info->filename ? info->filename : "");
    if ((info->flag & MZ_ZIP_FLAG_UTF8) != 0 || encoding == SA_FILENAME_ENCODING_UTF8)
        return raw;
    if (encoding != SA_FILENAME_ENCODING_AUTO)
        return convert_encoding(raw.c_str(), static_cast<int32_t>(encoding));
    if (valid_utf8(raw))
        return raw;

    const int32_t candidates[] = {936, 950, 932, 437};
    std::string best;
    int best_score = -100000;
    for (int32_t candidate : candidates) {
        std::string decoded = convert_encoding(raw.c_str(), candidate);
        int score = decoded_name_score(decoded);
        if (candidate == 437)
            score += 2;
        if (score > best_score) {
            best = std::move(decoded);
            best_score = score;
        }
    }
    return best.empty() ? raw : best;
}

std::string normalized_entry_path(const std::string &input) {
    if (input.empty() || input.find('\0') != std::string::npos || !valid_utf8(input))
        return {};
    std::string value = input;
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.front() == '/' || (value.size() >= 2 && value[1] == ':'))
        return {};

    std::string result;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find('/', start);
        std::string component = value.substr(start, end - start);
        if (component == "..")
            return {};
        if (!component.empty() && component != ".") {
            if (!result.empty())
                result.push_back('/');
            result += component;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return result;
}

std::wstring utf8_to_wide(const std::string &value) {
    if (!valid_utf8(value))
        return {};
    std::wstring result;
    const auto *bytes = reinterpret_cast<const unsigned char *>(value.data());
    size_t index = 0;
    while (index < value.size()) {
        uint32_t codepoint = bytes[index++];
        if (codepoint >= 0x80) {
            int trailing = (codepoint & 0xe0) == 0xc0 ? 1 :
                           (codepoint & 0xf0) == 0xe0 ? 2 : 3;
            codepoint &= trailing == 1 ? 0x1f : trailing == 2 ? 0x0f : 0x07;
            while (trailing-- > 0)
                codepoint = (codepoint << 6) | (bytes[index++] & 0x3f);
        }
        result.push_back(static_cast<wchar_t>(codepoint));
    }
    return result;
}

std::string wide_to_utf8(const wchar_t *value) {
    if (!value)
        return {};
    std::string result;
    for (; *value != 0; ++value) {
        uint32_t codepoint = static_cast<uint32_t>(*value);
        if (codepoint <= 0x7f) {
            result.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0x10ffff) {
            result.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            return {};
        }
    }
    return result;
}

int64_t windows_time_to_unix(uint32_t low, uint32_t high) {
    uint64_t ticks = (static_cast<uint64_t>(high) << 32) | low;
    if (ticks == 0)
        return 0;
    return static_cast<int64_t>(ticks / 10000000ULL) - 11644473600LL;
}

std::string temporary_path_for(const fs::path &destination, uint64_t sequence) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return destination.string() + ".swiftarchive-part-" + std::to_string(now) + "-" +
           std::to_string(sequence);
}

void apply_modification_time(const fs::path &path, int64_t timestamp) {
    if (timestamp <= 0)
        return;
    struct utimbuf times {};
    times.actime = static_cast<time_t>(timestamp);
    times.modtime = static_cast<time_t>(timestamp);
    utime(path.c_str(), &times);
}

bool ensure_safe_directory_chain(const fs::path &root, const fs::path &directory,
                                 std::error_code &ec) {
    ec.clear();
    auto root_status = fs::symlink_status(root, ec);
    if (ec || fs::is_symlink(root_status) || !fs::is_directory(root_status))
        return false;
    fs::path relative = directory.lexically_relative(root);
    if (relative.empty() || relative == ".")
        return true;
    if (*relative.begin() == "..")
        return false;
    fs::path current = root;
    for (const auto &component : relative) {
        current /= component;
        auto status = fs::symlink_status(current, ec);
        if (ec) {
            if (ec != std::errc::no_such_file_or_directory)
                return false;
            ec.clear();
        }
        if (fs::exists(status)) {
            if (fs::is_symlink(status) || !fs::is_directory(status))
                return false;
            continue;
        }
        if (!fs::create_directory(current, ec) || ec)
            return false;
    }
    return true;
}

int32_t prepare_output_path(const fs::path &root, const Entry &entry,
                            SAOverwritePolicy policy, fs::path &output,
                            bool &skip, SAError *error) {
    output = root / fs::path(entry.path);
    skip = false;
    std::error_code ec;
    fs::path required_directory = entry.kind == SA_ENTRY_DIRECTORY ? output : output.parent_path();
    if (!ensure_safe_directory_chain(root, required_directory, ec))
        return fail(error, SA_ERROR_UNSAFE_PATH, 0, "Destination contains a symbolic link");
    if (!fs::exists(fs::symlink_status(output, ec)))
        return 1;
    if (entry.kind == SA_ENTRY_DIRECTORY && fs::is_directory(fs::symlink_status(output, ec)))
        return 1;
    if (policy == SA_OVERWRITE_SKIP) {
        skip = true;
        return 1;
    }
    if (policy == SA_OVERWRITE_ERROR)
        return fail(error, SA_ERROR_DESTINATION_EXISTS, 0, "Destination already exists");
    if (fs::is_directory(fs::symlink_status(output, ec)))
        return fail(error, SA_ERROR_DESTINATION_EXISTS, 0, "Cannot replace a directory with a file");
    return 1;
}

int CALLBACK rar_callback(UINT message, LPARAM user_data, LPARAM p1, LPARAM p2) {
    auto *context = reinterpret_cast<RarCallbackContext *>(user_data);
    if (!context)
        return -1;
    if (context->cancelled && context->cancelled->load())
        return -1;
    switch (message) {
    case UCM_NEEDPASSWORDW: {
        if (!context->password || context->password->empty())
            return -1;
        std::wstring password = utf8_to_wide(*context->password);
        if (password.empty())
            return -1;
        auto *destination = reinterpret_cast<wchar_t *>(p1);
        size_t capacity = static_cast<size_t>(p2);
        std::wcsncpy(destination, password.c_str(), capacity - 1);
        destination[capacity - 1] = 0;
        return 1;
    }
    case UCM_NEEDPASSWORD: {
        if (!context->password || context->password->empty())
            return -1;
        auto *destination = reinterpret_cast<char *>(p1);
        size_t capacity = static_cast<size_t>(p2);
        std::snprintf(destination, capacity, "%s", context->password->c_str());
        return 1;
    }
    case UCM_PROCESSDATA: {
        auto *bytes = reinterpret_cast<const uint8_t *>(p1);
        size_t length = static_cast<size_t>(p2);
        if (context->data_callback && context->data_callback(context->data_context, bytes, length) == 0)
            return -1;
        context->progress.entry_completed += length;
        context->progress.total_completed += length;
        if (context->progress_callback &&
            context->progress_callback(context->progress_context, &context->progress) == 0)
            return -1;
        return 1;
    }
    case UCM_CHANGEVOLUME:
    case UCM_CHANGEVOLUMEW:
        return 1;
    case UCM_LARGEDICT:
        return 1;
    default:
        return 1;
    }
}

} // namespace

struct SAArchive {
    SAArchiveFormat format = SA_ARCHIVE_FORMAT_AUTO;
    std::string path;
    std::string password;
    SAFilenameEncoding encoding = SA_FILENAME_ENCODING_AUTO;
    SAOpenOptions limits{};
    std::vector<Entry> entries;
    std::atomic<bool> cancelled{false};
    bool uses_libarchive = false;
};

namespace {

int32_t validate_entry(SAArchive *archive, Entry &entry, SAError *error) {
    std::string normalized = normalized_entry_path(entry.path);
    if (normalized.empty())
        return fail(error, SA_ERROR_UNSAFE_PATH, 0, "Archive contains an unsafe path");
    entry.path = std::move(normalized);
    if (entry.kind != SA_ENTRY_FILE && entry.kind != SA_ENTRY_DIRECTORY)
        return fail(error, SA_ERROR_UNSAFE_LINK, 0, "Archive contains a link entry");
    if (entry.uncompressed_size > archive->limits.maximum_entry_size)
        return fail(error, SA_ERROR_RESOURCE_LIMIT, 0, "Archive entry exceeds the configured size limit");
    if (entry.compressed_size > 0 && archive->limits.maximum_compression_ratio > 0 &&
        static_cast<double>(entry.uncompressed_size) / static_cast<double>(entry.compressed_size) >
            archive->limits.maximum_compression_ratio)
        return fail(error, SA_ERROR_RESOURCE_LIMIT, 0, "Archive entry exceeds the configured compression ratio");
    return 1;
}

int32_t validate_archive_limits(SAArchive *archive, SAError *error) {
    if (archive->entries.size() > archive->limits.maximum_entry_count)
        return fail(error, SA_ERROR_RESOURCE_LIMIT, 0, "Archive contains too many entries");
    uint64_t total = 0;
    for (auto &entry : archive->entries) {
        if (!validate_entry(archive, entry, error))
            return 0;
        if (UINT64_MAX - total < entry.uncompressed_size)
            return fail(error, SA_ERROR_RESOURCE_LIMIT, 0, "Archive size overflows the configured limit");
        total += entry.uncompressed_size;
        if (total > archive->limits.maximum_total_size)
            return fail(error, SA_ERROR_RESOURCE_LIMIT, 0, "Archive exceeds the configured total size limit");
    }
    return 1;
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

SAErrorCode libarchive_error_code(struct archive *reader, bool has_password) {
    const char *archive_message = archive_error_string(reader);
    std::string message = lowercase_ascii(archive_message ? archive_message : "");
    if (message.find("unsupported") != std::string::npos ||
        message.find("not supported") != std::string::npos)
        return SA_ERROR_UNSUPPORTED_FEATURE;
    if (message.find("passphrase") != std::string::npos ||
        message.find("password") != std::string::npos) {
        if (!has_password || message.find("required") != std::string::npos ||
            message.find("without") != std::string::npos)
            return SA_ERROR_PASSWORD_REQUIRED;
        return SA_ERROR_BAD_PASSWORD;
    }
    if (message.find("encrypted") != std::string::npos)
        return has_password ? SA_ERROR_BAD_PASSWORD : SA_ERROR_PASSWORD_REQUIRED;
    if (message.find("memory") != std::string::npos || archive_errno(reader) == ENOMEM)
        return SA_ERROR_RESOURCE_LIMIT;
    if (message.find("format") != std::string::npos ||
        message.find("corrupt") != std::string::npos ||
        message.find("checksum") != std::string::npos ||
        message.find("truncated") != std::string::npos)
        return SA_ERROR_CORRUPT_ARCHIVE;
    return SA_ERROR_IO;
}

int32_t fail_libarchive(SAError *error, struct archive *reader,
                        bool has_password, const char *fallback) {
    const char *detail = archive_error_string(reader);
    return fail(error, libarchive_error_code(reader, has_password),
                archive_errno(reader), detail && detail[0] ? detail : fallback);
}

bool register_libarchive_readers(struct archive *reader) {
    const int results[] = {
        archive_read_support_filter_none(reader),
        archive_read_support_filter_gzip(reader),
        archive_read_support_filter_bzip2(reader),
        archive_read_support_filter_compress(reader),
        archive_read_support_filter_xz(reader),
        archive_read_support_filter_lzma(reader),
        archive_read_support_filter_lzip(reader),
        archive_read_support_filter_lz4(reader),
        archive_read_support_filter_zstd(reader),
        archive_read_support_filter_rpm(reader),
        archive_read_support_format_7zip(reader),
        archive_read_support_format_ar(reader),
        archive_read_support_format_cab(reader),
        archive_read_support_format_cpio(reader),
        archive_read_support_format_empty(reader),
        archive_read_support_format_iso9660(reader),
        archive_read_support_format_lha(reader),
        archive_read_support_format_raw(reader),
        archive_read_support_format_tar(reader),
        archive_read_support_format_warc(reader),
        archive_read_support_format_zip(reader),
    };
    return std::all_of(std::begin(results), std::end(results), [](int result) {
        return result >= ARCHIVE_WARN;
    });
}

struct archive *open_libarchive_reader(SAArchive *source, SAError *error) {
    struct archive *reader = archive_read_new();
    if (!reader) {
        fail(error, SA_ERROR_INTERNAL, ENOMEM, "Unable to allocate libarchive reader");
        return nullptr;
    }
    if (!register_libarchive_readers(reader)) {
        fail_libarchive(error, reader, !source->password.empty(),
                        "Unable to configure libarchive reader");
        archive_read_free(reader);
        return nullptr;
    }
    if (!source->password.empty() &&
        archive_read_add_passphrase(reader, source->password.c_str()) < ARCHIVE_OK) {
        fail_libarchive(error, reader, true, "Unable to configure archive password");
        archive_read_free(reader);
        return nullptr;
    }
    int result = archive_read_open_filename(reader, source->path.c_str(), 128 * 1024);
    if (result < ARCHIVE_OK) {
        fail_libarchive(error, reader, !source->password.empty(),
                        "Unable to open archive with libarchive");
        archive_read_free(reader);
        return nullptr;
    }
    return reader;
}

SAArchiveFormat filter_archive_format(struct archive *reader) {
    for (int index = 0; index < archive_filter_count(reader); ++index) {
        switch (archive_filter_code(reader, index)) {
        case ARCHIVE_FILTER_GZIP: return SA_ARCHIVE_FORMAT_GZIP;
        case ARCHIVE_FILTER_BZIP2: return SA_ARCHIVE_FORMAT_BZIP2;
        case ARCHIVE_FILTER_LZMA: return SA_ARCHIVE_FORMAT_LZMA;
        case ARCHIVE_FILTER_XZ: return SA_ARCHIVE_FORMAT_XZ;
        case ARCHIVE_FILTER_LZIP: return SA_ARCHIVE_FORMAT_LZIP;
        case ARCHIVE_FILTER_COMPRESS: return SA_ARCHIVE_FORMAT_COMPRESS;
        case ARCHIVE_FILTER_ZSTD: return SA_ARCHIVE_FORMAT_ZSTD;
        case ARCHIVE_FILTER_LZ4: return SA_ARCHIVE_FORMAT_LZ4;
        default: break;
        }
    }
    return SA_ARCHIVE_FORMAT_AUTO;
}

SAArchiveFormat detected_libarchive_format(struct archive *reader) {
    switch (archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK) {
    case ARCHIVE_FORMAT_7ZIP: return SA_ARCHIVE_FORMAT_7ZIP;
    case ARCHIVE_FORMAT_AR: return SA_ARCHIVE_FORMAT_AR;
    case ARCHIVE_FORMAT_CAB: return SA_ARCHIVE_FORMAT_CAB;
    case ARCHIVE_FORMAT_CPIO: return SA_ARCHIVE_FORMAT_CPIO;
    case ARCHIVE_FORMAT_ISO9660: return SA_ARCHIVE_FORMAT_ISO9660;
    case ARCHIVE_FORMAT_LHA: return SA_ARCHIVE_FORMAT_LHA;
    case ARCHIVE_FORMAT_TAR: return SA_ARCHIVE_FORMAT_TAR;
    case ARCHIVE_FORMAT_WARC: return SA_ARCHIVE_FORMAT_WARC;
    case ARCHIVE_FORMAT_ZIP: return SA_ARCHIVE_FORMAT_ZIP;
    case ARCHIVE_FORMAT_RAW: return filter_archive_format(reader);
    default: return SA_ARCHIVE_FORMAT_AUTO;
    }
}

std::string raw_archive_entry_name(const SAArchive *source) {
    fs::path filename = fs::path(source->path).filename();
    fs::path stem = filename.stem();
    std::string result = stem.string();
    if (result.empty() || result == ".")
        result = "Unpacked";
    return result;
}

int32_t list_libarchive(SAArchive *source, SAError *error) {
    struct archive *reader = open_libarchive_reader(source, error);
    if (!reader)
        return 0;

    int result = ARCHIVE_OK;
    uint64_t native_index = 0;
    struct archive_entry *header = nullptr;
    while ((result = archive_read_next_header(reader, &header)) == ARCHIVE_OK) {
        SAArchiveFormat detected = detected_libarchive_format(reader);
        if (detected == SA_ARCHIVE_FORMAT_AUTO) {
            archive_read_free(reader);
            return fail(error, SA_ERROR_UNSUPPORTED_FORMAT, 0,
                        "Archive format is not supported");
        }
        source->format = detected;

        const char *pathname = archive_entry_pathname_utf8(header);
        if (!pathname)
            pathname = archive_entry_pathname(header);
        std::string path = pathname ? pathname : "";
        if ((archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_RAW)
            path = raw_archive_entry_name(source);

        mode_t file_type = archive_entry_filetype(header);
        if ((path == "." || path == "./") && file_type == AE_IFDIR) {
            result = archive_read_data_skip(reader);
            if (result < ARCHIVE_OK)
                break;
            ++native_index;
            continue;
        }

        Entry entry;
        entry.path = std::move(path);
        entry.data_offset = native_index;
        if (archive_entry_hardlink(header))
            entry.kind = SA_ENTRY_HARD_LINK;
        else if (archive_entry_symlink(header) || file_type == AE_IFLNK)
            entry.kind = SA_ENTRY_SYMBOLIC_LINK;
        else if (file_type == AE_IFDIR)
            entry.kind = SA_ENTRY_DIRECTORY;
        else if (file_type != 0 && file_type != AE_IFREG)
            entry.kind = SA_ENTRY_OTHER_LINK;

        if (archive_entry_size_is_set(header) && archive_entry_size(header) >= 0)
            entry.uncompressed_size = static_cast<uint64_t>(archive_entry_size(header));
        if (archive_entry_mtime_is_set(header))
            entry.modification_time = static_cast<int64_t>(archive_entry_mtime(header));
        entry.encrypted = archive_entry_is_encrypted(header) != 0;
        source->entries.push_back(std::move(entry));

        result = archive_read_data_skip(reader);
        if (result < ARCHIVE_OK)
            break;
        ++native_index;
    }

    if (source->format == SA_ARCHIVE_FORMAT_AUTO)
        source->format = detected_libarchive_format(reader);
    if (result != ARCHIVE_EOF) {
        int32_t failure = fail_libarchive(error, reader, !source->password.empty(),
                                          "Unable to read archive directory");
        archive_read_free(reader);
        return failure;
    }
    archive_read_free(reader);
    if (source->format == SA_ARCHIVE_FORMAT_AUTO)
        return fail(error, SA_ERROR_UNSUPPORTED_FORMAT, 0,
                    "Archive format is not supported");
    source->uses_libarchive = true;
    return validate_archive_limits(source, error);
}

int32_t require_libarchive_password(SAArchive *source, const Entry &entry,
                                    SAError *error) {
    if (source->uses_libarchive && entry.kind == SA_ENTRY_FILE &&
        entry.encrypted && source->password.empty())
        return fail(error, SA_ERROR_PASSWORD_REQUIRED, 0,
                    "A password is required to read this archive entry");
    return 1;
}

int32_t require_zip_password(SAArchive *archive, const Entry &entry, SAError *error) {
    if (entry.kind == SA_ENTRY_FILE && entry.encrypted && archive->password.empty())
        return fail(error, SA_ERROR_PASSWORD_REQUIRED, MZ_PASSWORD_ERROR,
                    "A password is required to read this ZIP entry");
    return 1;
}

int32_t list_zip(SAArchive *archive, SAError *error) {
    void *reader = mz_zip_reader_create();
    if (!reader)
        return fail(error, SA_ERROR_INTERNAL, MZ_MEM_ERROR, "Unable to allocate ZIP reader");
    mz_zip_reader_set_password(reader, archive->password.empty() ? nullptr : archive->password.c_str());
    int32_t result = mz_zip_reader_open_file(reader, archive->path.c_str());
    if (result != MZ_OK) {
        mz_zip_reader_delete(&reader);
        return fail(error, minizip_error_code(result, !archive->password.empty()), result,
                    "Unable to open ZIP archive");
    }
    result = mz_zip_reader_goto_first_entry(reader);
    while (result == MZ_OK) {
        mz_zip_file *info = nullptr;
        result = mz_zip_reader_entry_get_info(reader, &info);
        if (result != MZ_OK || !info)
            break;
        Entry entry;
        entry.path = decode_zip_name(info, archive->encoding);
        if (entry.path.empty()) {
            result = MZ_FORMAT_ERROR;
            break;
        }
        if (mz_zip_attrib_is_symlink(info->external_fa, info->version_madeby) == MZ_OK)
            entry.kind = SA_ENTRY_SYMBOLIC_LINK;
        else if (mz_zip_attrib_is_dir(info->external_fa, info->version_madeby) == MZ_OK ||
                 (!entry.path.empty() && entry.path.back() == '/'))
            entry.kind = SA_ENTRY_DIRECTORY;
        entry.compressed_size = std::max<int64_t>(0, info->compressed_size);
        entry.uncompressed_size = std::max<int64_t>(0, info->uncompressed_size);
        entry.modification_time = static_cast<int64_t>(info->modified_date);
        entry.crc32 = info->crc;
        entry.encrypted = (info->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0;
        archive->entries.push_back(std::move(entry));
        result = mz_zip_reader_goto_next_entry(reader);
    }
    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    if (result != MZ_END_OF_LIST)
        return fail(error, minizip_error_code(result, !archive->password.empty()), result,
                    "Unable to read ZIP directory");
    return validate_archive_limits(archive, error);
}

int32_t list_rar(SAArchive *archive, SAError *error) {
    std::wstring archive_path = utf8_to_wide(archive->path);
    if (archive_path.empty())
        return fail(error, SA_ERROR_INVALID_ARGUMENT, 0, "Archive path is not valid UTF-8");
    RarCallbackContext callback_context{&archive->password, &archive->cancelled};
    RAROpenArchiveDataEx open_data{};
    open_data.ArcNameW = archive_path.data();
    open_data.OpenMode = RAR_OM_LIST;
    open_data.Callback = rar_callback;
    open_data.UserData = reinterpret_cast<LPARAM>(&callback_context);
    HANDLE handle = RAROpenArchiveEx(&open_data);
    if (!handle)
        return fail(error, unrar_error_code(open_data.OpenResult, !archive->password.empty()),
                    static_cast<int32_t>(open_data.OpenResult), "Unable to open RAR archive");

    int32_t result = ERAR_SUCCESS;
    while (true) {
        RARHeaderDataEx header{};
        result = RARReadHeaderEx(handle, &header);
        if (result == ERAR_END_ARCHIVE)
            break;
        if (result != ERAR_SUCCESS)
            break;
        Entry entry;
        entry.path = wide_to_utf8(header.FileNameW);
        entry.compressed_size = (static_cast<uint64_t>(header.PackSizeHigh) << 32) | header.PackSize;
        entry.uncompressed_size = (static_cast<uint64_t>(header.UnpSizeHigh) << 32) | header.UnpSize;
        entry.modification_time = windows_time_to_unix(header.MtimeLow, header.MtimeHigh);
        entry.crc32 = header.FileCRC;
        entry.encrypted = (header.Flags & RHDF_ENCRYPTED) != 0;
        entry.solid = (header.Flags & RHDF_SOLID) != 0;
        entry.split_before = (header.Flags & RHDF_SPLITBEFORE) != 0;
        entry.split_after = (header.Flags & RHDF_SPLITAFTER) != 0;
        if ((header.Flags & RHDF_DIRECTORY) != 0)
            entry.kind = SA_ENTRY_DIRECTORY;
        else if (header.RedirType == 1 || header.RedirType == 2 || header.RedirType == 3)
            entry.kind = SA_ENTRY_SYMBOLIC_LINK;
        else if (header.RedirType == 4)
            entry.kind = SA_ENTRY_HARD_LINK;
        else if (header.RedirType != 0)
            entry.kind = SA_ENTRY_OTHER_LINK;
        archive->entries.push_back(std::move(entry));
        result = RARProcessFileW(handle, RAR_SKIP, nullptr, nullptr);
        if (result != ERAR_SUCCESS)
            break;
    }
    RARCloseArchive(handle);
    if (result != ERAR_END_ARCHIVE)
        return fail(error, unrar_error_code(result, !archive->password.empty()), result,
                    "Unable to read RAR directory");
    return validate_archive_limits(archive, error);
}

bool block_is_zero(const uint8_t *block, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        if (block[index] != 0)
            return false;
    }
    return true;
}

std::string tar_string(const uint8_t *value, size_t length) {
    size_t end = 0;
    while (end < length && value[end] != 0)
        ++end;
    while (end > 0 && value[end - 1] == ' ')
        --end;
    return std::string(reinterpret_cast<const char *>(value), end);
}

bool parse_tar_number(const uint8_t *value, size_t length, uint64_t &result) {
    if (length == 0)
        return false;
    if ((value[0] & 0x80) != 0) {
        result = value[0] & 0x7f;
        for (size_t index = 1; index < length; ++index) {
            if (result > (std::numeric_limits<uint64_t>::max() >> 8))
                return false;
            result = (result << 8) | value[index];
        }
        return true;
    }

    result = 0;
    size_t index = 0;
    while (index < length && (value[index] == 0 || value[index] == ' '))
        ++index;
    bool found_digit = false;
    for (; index < length && value[index] >= '0' && value[index] <= '7'; ++index) {
        found_digit = true;
        if (result > (std::numeric_limits<uint64_t>::max() >> 3))
            return false;
        result = (result << 3) | static_cast<uint64_t>(value[index] - '0');
    }
    return found_digit || result == 0;
}

bool valid_tar_checksum(const uint8_t *header) {
    uint64_t expected = 0;
    if (!parse_tar_number(header + 148, 8, expected))
        return false;
    uint64_t checksum = 0;
    for (size_t index = 0; index < 512; ++index)
        checksum += index >= 148 && index < 156 ? static_cast<uint8_t>(' ') : header[index];
    return checksum == expected;
}

bool read_tar_payload(std::ifstream &stream, uint64_t size, std::string &value) {
    constexpr uint64_t maximum_metadata_size = 16ULL * 1024 * 1024;
    if (size > maximum_metadata_size || size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        return false;
    value.resize(static_cast<size_t>(size));
    if (size > 0)
        stream.read(value.data(), static_cast<std::streamsize>(size));
    return stream.good() || (stream.eof() && stream.gcount() == static_cast<std::streamsize>(size));
}

void parse_pax_records(const std::string &payload, std::string &path,
                       uint64_t &size, bool &has_size, int64_t &modification_time,
                       bool &has_modification_time) {
    size_t offset = 0;
    while (offset < payload.size()) {
        size_t space = payload.find(' ', offset);
        if (space == std::string::npos)
            break;
        uint64_t record_length = 0;
        try {
            record_length = std::stoull(payload.substr(offset, space - offset));
        } catch (...) {
            break;
        }
        if (record_length == 0 || record_length > payload.size() - offset)
            break;
        size_t record_end = offset + static_cast<size_t>(record_length);
        size_t equals = payload.find('=', space + 1);
        if (equals != std::string::npos && equals < record_end) {
            size_t value_end = record_end;
            if (value_end > equals + 1 && payload[value_end - 1] == '\n')
                --value_end;
            std::string key = payload.substr(space + 1, equals - space - 1);
            std::string field = payload.substr(equals + 1, value_end - equals - 1);
            try {
                if (key == "path") {
                    path = field;
                } else if (key == "size") {
                    size = std::stoull(field);
                    has_size = true;
                } else if (key == "mtime") {
                    modification_time = static_cast<int64_t>(std::stod(field));
                    has_modification_time = true;
                }
            } catch (...) {
                // Ignore malformed optional PAX values; the base header remains authoritative.
            }
        }
        offset = record_end;
    }
}

int32_t list_tar(SAArchive *archive, SAError *error) {
    std::ifstream stream(archive->path, std::ios::binary);
    if (!stream)
        return fail(error, SA_ERROR_OPEN_FAILED, errno, "Unable to open TAR archive");
    std::error_code file_error;
    uint64_t file_size = fs::file_size(archive->path, file_error);
    if (file_error)
        return fail(error, SA_ERROR_IO, file_error.value(), "Unable to inspect TAR archive");

    uint64_t offset = 0;
    unsigned int zero_blocks = 0;
    std::string pending_path;
    uint64_t pending_size = 0;
    bool has_pending_size = false;
    int64_t pending_modification_time = 0;
    bool has_pending_modification_time = false;
    bool saw_end_marker = false;
    while (offset + 512 <= file_size) {
        uint8_t header[512]{};
        stream.seekg(static_cast<std::streamoff>(offset));
        stream.read(reinterpret_cast<char *>(header), sizeof(header));
        if (stream.gcount() != sizeof(header))
            return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "TAR header is truncated");
        if (block_is_zero(header, sizeof(header))) {
            saw_end_marker = true;
            if (++zero_blocks >= 2 || offset + 512 == file_size)
                break;
            offset += 512;
            continue;
        }
        zero_blocks = 0;
        if (!valid_tar_checksum(header))
            return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "TAR header checksum is invalid");

        uint64_t header_size = 0;
        uint64_t header_time = 0;
        if (!parse_tar_number(header + 124, 12, header_size) ||
            !parse_tar_number(header + 136, 12, header_time))
            return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "TAR header contains an invalid number");
        uint64_t padded_size = (header_size + 511) & ~uint64_t(511);
        if (padded_size < header_size || offset + 512 > file_size ||
            padded_size > file_size - offset - 512)
            return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "TAR entry data is truncated");

        char type = static_cast<char>(header[156]);
        if (type == 'L' || type == 'x' || type == 'g') {
            std::string payload;
            stream.seekg(static_cast<std::streamoff>(offset + 512));
            if (!read_tar_payload(stream, header_size, payload))
                return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "TAR metadata is invalid");
            if (type == 'L') {
                while (!payload.empty() && (payload.back() == 0 || payload.back() == '\n'))
                    payload.pop_back();
                pending_path = std::move(payload);
            } else if (type == 'x') {
                parse_pax_records(payload, pending_path, pending_size, has_pending_size,
                                  pending_modification_time, has_pending_modification_time);
            }
            offset += 512 + padded_size;
            continue;
        }

        std::string name = tar_string(header, 100);
        std::string prefix = tar_string(header + 345, 155);
        std::string path = pending_path.empty() ? (prefix.empty() ? name : prefix + "/" + name)
                                                : pending_path;
        uint64_t size = has_pending_size ? pending_size : header_size;
        int64_t modification_time = has_pending_modification_time
                                        ? pending_modification_time
                                        : static_cast<int64_t>(header_time);
        pending_path.clear();
        has_pending_size = false;
        has_pending_modification_time = false;

        Entry entry;
        entry.path = std::move(path);
        entry.compressed_size = size;
        entry.uncompressed_size = size;
        entry.modification_time = modification_time;
        entry.data_offset = offset + 512;
        switch (type) {
        case 0:
        case '0':
        case '7':
            entry.kind = SA_ENTRY_FILE;
            break;
        case '5':
            entry.kind = SA_ENTRY_DIRECTORY;
            entry.compressed_size = 0;
            entry.uncompressed_size = 0;
            break;
        case '1':
            entry.kind = SA_ENTRY_HARD_LINK;
            break;
        case '2':
            entry.kind = SA_ENTRY_SYMBOLIC_LINK;
            break;
        default:
            entry.kind = SA_ENTRY_OTHER_LINK;
            break;
        }
        if (archive->entries.size() >= archive->limits.maximum_entry_count)
            return fail(error, SA_ERROR_RESOURCE_LIMIT, 0, "Archive contains too many entries");
        archive->entries.push_back(std::move(entry));

        uint64_t effective_padded_size = (size + 511) & ~uint64_t(511);
        if (effective_padded_size < size || effective_padded_size > file_size - offset - 512)
            return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "TAR entry data is truncated");
        offset += 512 + effective_padded_size;
    }
    if (archive->entries.empty() && !saw_end_marker)
        return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "TAR archive has no valid entries");
    return validate_archive_limits(archive, error);
}

bool read_little_endian_u16(std::ifstream &stream, uint16_t &value) {
    uint8_t bytes[2]{};
    stream.read(reinterpret_cast<char *>(bytes), sizeof(bytes));
    if (stream.gcount() != sizeof(bytes))
        return false;
    value = static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8);
    return true;
}

std::string gzip_output_name(const std::string &archive_path, const std::string &header_name) {
    if (!header_name.empty() && valid_utf8(header_name)) {
        std::string normalized = header_name;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        std::string basename = fs::path(normalized).filename().string();
        if (!basename.empty() && basename != "." && basename != "..")
            return basename;
    }
    fs::path path(archive_path);
    std::string filename = path.filename().string();
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (lower.size() > 5 && lower.substr(lower.size() - 5) == ".gzip")
        filename.resize(filename.size() - 5);
    else if (lower.size() > 3 && lower.substr(lower.size() - 3) == ".gz")
        filename.resize(filename.size() - 3);
    else if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".tgz")
        filename = filename.substr(0, filename.size() - 4) + ".tar";
    return filename.empty() ? "Uncompressed" : filename;
}

int32_t list_gzip(SAArchive *archive, SAError *error) {
    std::ifstream stream(archive->path, std::ios::binary);
    if (!stream)
        return fail(error, SA_ERROR_OPEN_FAILED, errno, "Unable to open GZIP archive");
    std::error_code file_error;
    uint64_t file_size = fs::file_size(archive->path, file_error);
    if (file_error || file_size < 18)
        return fail(error, SA_ERROR_CORRUPT_ARCHIVE, file_error.value(), "GZIP archive is truncated");

    uint8_t header[10]{};
    stream.read(reinterpret_cast<char *>(header), sizeof(header));
    if (stream.gcount() != sizeof(header) || header[0] != 0x1f || header[1] != 0x8b ||
        header[2] != 8 || (header[3] & 0xe0) != 0)
        return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "GZIP header is invalid");
    uint8_t flags = header[3];
    uint64_t header_length = 10;
    if ((flags & 0x04) != 0) {
        uint16_t extra_length = 0;
        if (!read_little_endian_u16(stream, extra_length) || header_length + 2 + extra_length > file_size - 8)
            return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "GZIP extra data is truncated");
        stream.seekg(extra_length, std::ios::cur);
        header_length += 2 + extra_length;
    }
    auto read_zero_terminated = [&](std::string *output) -> bool {
        constexpr uint64_t maximum_field_size = 1024 * 1024;
        for (uint64_t count = 0; count < maximum_field_size && header_length < file_size - 8; ++count) {
            char value = 0;
            stream.get(value);
            ++header_length;
            if (!stream)
                return false;
            if (value == 0)
                return true;
            if (output)
                output->push_back(value);
        }
        return false;
    };
    std::string original_name;
    if ((flags & 0x08) != 0 && !read_zero_terminated(&original_name))
        return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "GZIP filename is invalid");
    if ((flags & 0x10) != 0 && !read_zero_terminated(nullptr))
        return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "GZIP comment is invalid");
    if ((flags & 0x02) != 0) {
        if (header_length + 2 > file_size - 8)
            return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "GZIP header checksum is truncated");
        stream.seekg(2, std::ios::cur);
        header_length += 2;
    }

    stream.seekg(-4, std::ios::end);
    uint8_t size_bytes[4]{};
    stream.read(reinterpret_cast<char *>(size_bytes), sizeof(size_bytes));
    if (stream.gcount() != sizeof(size_bytes))
        return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0, "GZIP footer is truncated");
    uint64_t uncompressed_size = static_cast<uint64_t>(size_bytes[0]) |
                                 (static_cast<uint64_t>(size_bytes[1]) << 8) |
                                 (static_cast<uint64_t>(size_bytes[2]) << 16) |
                                 (static_cast<uint64_t>(size_bytes[3]) << 24);
    uint64_t modification_time = static_cast<uint64_t>(header[4]) |
                                 (static_cast<uint64_t>(header[5]) << 8) |
                                 (static_cast<uint64_t>(header[6]) << 16) |
                                 (static_cast<uint64_t>(header[7]) << 24);
    Entry entry;
    entry.path = gzip_output_name(archive->path, original_name);
    entry.compressed_size = file_size - header_length - 8;
    entry.uncompressed_size = uncompressed_size;
    entry.modification_time = static_cast<int64_t>(modification_time);
    archive->entries.push_back(std::move(entry));
    return validate_archive_limits(archive, error);
}

int32_t stream_tar_entry(SAArchive *archive, uint64_t index,
                         SADataCallback callback, void *data_context,
                         SAProgressCallback progress, void *progress_context,
                         uint64_t total_base, uint64_t total_size, SAError *error) {
    const Entry &entry = archive->entries[index];
    FILE *source = std::fopen(archive->path.c_str(), "rb");
    if (!source)
        return fail(error, SA_ERROR_OPEN_FAILED, errno, "Unable to open TAR archive");
    if (fseeko(source, static_cast<off_t>(entry.data_offset), SEEK_SET) != 0) {
        int backend_error = errno;
        std::fclose(source);
        return fail(error, SA_ERROR_IO, backend_error, "Unable to seek to TAR entry");
    }

    uint64_t completed = 0;
    uint8_t buffer[128 * 1024];
    while (completed < entry.uncompressed_size) {
        if (archive->cancelled.load()) {
            std::fclose(source);
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        }
        size_t requested = static_cast<size_t>(std::min<uint64_t>(
            sizeof(buffer), entry.uncompressed_size - completed));
        size_t read = std::fread(buffer, 1, requested, source);
        if (read != requested) {
            int backend_error = std::ferror(source) ? errno : 0;
            std::fclose(source);
            return fail(error, SA_ERROR_CORRUPT_ARCHIVE, backend_error,
                        "TAR entry data is truncated");
        }
        if (callback && callback(data_context, buffer, read) == 0) {
            archive->cancelled.store(true);
            std::fclose(source);
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        }
        completed += read;
        SAProgress state{sizeof(SAProgress), index, entry.path.c_str(), completed,
                         entry.uncompressed_size, total_base + completed, total_size};
        if (progress && progress(progress_context, &state) == 0) {
            archive->cancelled.store(true);
            std::fclose(source);
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        }
    }
    std::fclose(source);
    return 1;
}

int32_t stream_gzip_entry(SAArchive *archive, uint64_t index,
                          SADataCallback callback, void *data_context,
                          SAProgressCallback progress, void *progress_context,
                          SAError *error) {
    const Entry &entry = archive->entries[index];
    gzFile source = gzopen(archive->path.c_str(), "rb");
    if (!source)
        return fail(error, SA_ERROR_OPEN_FAILED, errno, "Unable to open GZIP archive");
    uint64_t completed = 0;
    uint8_t buffer[128 * 1024];
    while (true) {
        if (archive->cancelled.load()) {
            gzclose(source);
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        }
        int read = gzread(source, buffer, static_cast<unsigned int>(sizeof(buffer)));
        if (read < 0) {
            int backend_error = Z_DATA_ERROR;
            const char *message = gzerror(source, &backend_error);
            std::string description = message ? message : "Unable to decompress GZIP archive";
            gzclose(source);
            return fail(error, SA_ERROR_CORRUPT_ARCHIVE, backend_error, description);
        }
        if (read == 0)
            break;
        uint64_t chunk_size = static_cast<uint64_t>(read);
        uint64_t output_limit = std::min(
            archive->limits.maximum_entry_size,
            archive->limits.maximum_total_size
        );
        if (chunk_size > output_limit || completed > output_limit - chunk_size) {
            gzclose(source);
            return fail(error, SA_ERROR_RESOURCE_LIMIT, 0,
                        "GZIP output exceeds the configured size limit");
        }
        completed += chunk_size;
        if (entry.compressed_size > 0 && archive->limits.maximum_compression_ratio > 0 &&
            static_cast<double>(completed) / static_cast<double>(entry.compressed_size) >
                archive->limits.maximum_compression_ratio) {
            gzclose(source);
            return fail(error, SA_ERROR_RESOURCE_LIMIT, 0,
                        "GZIP output exceeds the configured compression ratio");
        }
        if (callback && callback(data_context, buffer, static_cast<size_t>(read)) == 0) {
            archive->cancelled.store(true);
            gzclose(source);
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        }
        SAProgress state{sizeof(SAProgress), index, entry.path.c_str(), completed,
                         entry.uncompressed_size, completed, entry.uncompressed_size};
        if (progress && progress(progress_context, &state) == 0) {
            archive->cancelled.store(true);
            gzclose(source);
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        }
    }
    int close_result = gzclose(source);
    if (close_result != Z_OK)
        return fail(error, SA_ERROR_CORRUPT_ARCHIVE, close_result,
                    "GZIP checksum validation failed");
    if (static_cast<uint32_t>(completed) != static_cast<uint32_t>(entry.uncompressed_size))
        return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0,
                    "GZIP uncompressed size does not match its footer");
    return 1;
}

struct FileWriteContext {
    FILE *file = nullptr;
    bool failed = false;
    int backend_error = 0;
};

int32_t file_write_callback(void *context, const uint8_t *bytes, size_t length) {
    auto *writer = static_cast<FileWriteContext *>(context);
    if (!writer || !writer->file)
        return 0;
    if (std::fwrite(bytes, 1, length, writer->file) != length) {
        writer->failed = true;
        writer->backend_error = errno;
        return 0;
    }
    return 1;
}

int32_t finish_extracted_file(const std::string &temporary, const fs::path &output,
                              const Entry &entry, const SAExtractionOptions &options,
                              SAError *error) {
    std::error_code ec;
    if (fs::exists(fs::symlink_status(output, ec)))
        fs::remove(output, ec);
    fs::rename(temporary, output, ec);
    if (ec) {
        std::remove(temporary.c_str());
        return fail(error, SA_ERROR_IO, ec.value(), "Unable to finish extracted file");
    }
    if (options.preserve_timestamps)
        apply_modification_time(output, entry.modification_time);
    return 1;
}

uint64_t total_uncompressed_size(const SAArchive *source) {
    uint64_t total = 0;
    for (const auto &entry : source->entries)
        total += entry.uncompressed_size;
    return total;
}

int32_t validate_libarchive_runtime_limits(SAArchive *source,
                                           struct archive *reader,
                                           uint64_t entry_completed,
                                           uint64_t total_completed,
                                           SAError *error) {
    if (entry_completed > source->limits.maximum_entry_size ||
        total_completed > source->limits.maximum_total_size)
        return fail(error, SA_ERROR_RESOURCE_LIMIT, 0,
                    "Archive output exceeds the configured size limit");
    la_int64_t compressed = archive_filter_bytes(reader, -1);
    if (compressed > 0 && total_completed >= 1024 * 1024 &&
        source->limits.maximum_compression_ratio > 0 &&
        static_cast<double>(total_completed) / static_cast<double>(compressed) >
            source->limits.maximum_compression_ratio)
        return fail(error, SA_ERROR_RESOURCE_LIMIT, 0,
                    "Archive output exceeds the configured compression ratio");
    return 1;
}

int32_t stream_active_libarchive_entry(SAArchive *source,
                                       struct archive *reader,
                                       uint64_t entry_index,
                                       SADataCallback callback,
                                       void *data_context,
                                       SAProgressCallback progress,
                                       void *progress_context,
                                       uint64_t total_base,
                                       uint64_t total_size,
                                       SAError *error) {
    const Entry &entry = source->entries[entry_index];
    uint64_t completed = 0;
    uint8_t buffer[128 * 1024];
    while (true) {
        if (source->cancelled.load())
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        la_ssize_t read = archive_read_data(reader, buffer, sizeof(buffer));
        if (read < 0)
            return fail_libarchive(error, reader, !source->password.empty(),
                                   "Unable to read archive entry");
        if (read == 0)
            break;
        uint64_t chunk_size = static_cast<uint64_t>(read);
        if (UINT64_MAX - completed < chunk_size ||
            UINT64_MAX - total_base < completed + chunk_size)
            return fail(error, SA_ERROR_RESOURCE_LIMIT, 0,
                        "Archive output size overflowed");
        completed += chunk_size;
        if (!validate_libarchive_runtime_limits(source, reader, completed,
                                                total_base + completed, error))
            return 0;
        if (callback && callback(data_context, buffer, static_cast<size_t>(read)) == 0) {
            source->cancelled.store(true);
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        }
        SAProgress state{sizeof(SAProgress), entry_index, entry.path.c_str(), completed,
                         entry.uncompressed_size, total_base + completed, total_size};
        if (progress && progress(progress_context, &state) == 0) {
            source->cancelled.store(true);
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        }
    }
    if (entry.uncompressed_size > 0 && completed != entry.uncompressed_size)
        return fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0,
                    "Archive entry size does not match its header");
    return 1;
}

int32_t skip_to_libarchive_entry(SAArchive *source, struct archive *reader,
                                 uint64_t entry_index, SAError *error) {
    uint64_t target = source->entries[entry_index].data_offset;
    uint64_t native_index = 0;
    struct archive_entry *header = nullptr;
    while (true) {
        int result = archive_read_next_header(reader, &header);
        if (result == ARCHIVE_EOF)
            return fail(error, SA_ERROR_ENTRY_NOT_FOUND, 0,
                        "Archive entry was not found");
        if (result < ARCHIVE_OK)
            return fail_libarchive(error, reader, !source->password.empty(),
                                   "Unable to read archive directory");
        if (native_index == target)
            return 1;
        result = archive_read_data_skip(reader);
        if (result < ARCHIVE_OK)
            return fail_libarchive(error, reader, !source->password.empty(),
                                   "Unable to skip archive entry");
        ++native_index;
    }
}

int32_t extract_one_libarchive(SAArchive *source, uint64_t entry_index,
                               const fs::path &root,
                               const SAExtractionOptions &options,
                               SAProgressCallback progress, void *context,
                               SAError *error) {
    const Entry &entry = source->entries[entry_index];
    if (!require_libarchive_password(source, entry, error))
        return 0;
    fs::path output;
    bool skip = false;
    if (!prepare_output_path(root, entry, options.overwrite_policy, output, skip, error))
        return 0;
    if (entry.kind == SA_ENTRY_DIRECTORY || skip) {
        if (entry.kind == SA_ENTRY_DIRECTORY && options.preserve_timestamps)
            apply_modification_time(output, entry.modification_time);
        return 1;
    }
    if (entry.kind != SA_ENTRY_FILE)
        return fail(error, SA_ERROR_UNSAFE_LINK, 0, "Link extraction is disabled");

    struct archive *reader = open_libarchive_reader(source, error);
    if (!reader)
        return 0;
    if (!skip_to_libarchive_entry(source, reader, entry_index, error)) {
        archive_read_free(reader);
        return 0;
    }

    std::string temporary = temporary_path_for(output, entry_index);
    FileWriteContext writer{std::fopen(temporary.c_str(), "wb")};
    if (!writer.file) {
        archive_read_free(reader);
        return fail(error, SA_ERROR_IO, errno, "Unable to create extracted file");
    }
    int32_t success = stream_active_libarchive_entry(
        source, reader, entry_index, file_write_callback, &writer,
        progress, context, 0, entry.uncompressed_size, error);
    if (std::fclose(writer.file) != 0 && success)
        success = fail(error, SA_ERROR_IO, errno, "Unable to close extracted file");
    archive_read_free(reader);
    if (writer.failed) {
        std::remove(temporary.c_str());
        return fail(error, SA_ERROR_IO, writer.backend_error,
                    "Unable to write extracted file");
    }
    if (!success) {
        std::remove(temporary.c_str());
        return 0;
    }
    return finish_extracted_file(temporary, output, entry, options, error);
}

int32_t extract_libarchive(SAArchive *source, const fs::path &root,
                           const SAExtractionOptions &options,
                           SAProgressCallback progress, void *context,
                           SAError *error) {
    for (const auto &entry : source->entries) {
        if (!require_libarchive_password(source, entry, error))
            return 0;
    }
    struct archive *reader = open_libarchive_reader(source, error);
    if (!reader)
        return 0;

    uint64_t total_size = total_uncompressed_size(source);
    uint64_t total_completed = 0;
    uint64_t native_index = 0;
    uint64_t logical_index = 0;
    struct archive_entry *header = nullptr;
    int result = ARCHIVE_OK;
    while ((result = archive_read_next_header(reader, &header)) == ARCHIVE_OK) {
        if (source->cancelled.load()) {
            archive_read_free(reader);
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        }
        if (logical_index >= source->entries.size() ||
            source->entries[logical_index].data_offset != native_index) {
            result = archive_read_data_skip(reader);
            if (result < ARCHIVE_OK)
                break;
            ++native_index;
            continue;
        }

        const Entry &entry = source->entries[logical_index];
        fs::path output;
        bool skip = false;
        if (!prepare_output_path(root, entry, options.overwrite_policy,
                                 output, skip, error)) {
            archive_read_free(reader);
            return 0;
        }
        if (entry.kind == SA_ENTRY_DIRECTORY || skip) {
            result = archive_read_data_skip(reader);
            if (entry.kind == SA_ENTRY_DIRECTORY && options.preserve_timestamps)
                apply_modification_time(output, entry.modification_time);
        } else if (entry.kind != SA_ENTRY_FILE) {
            archive_read_free(reader);
            return fail(error, SA_ERROR_UNSAFE_LINK, 0, "Link extraction is disabled");
        } else {
            std::string temporary = temporary_path_for(output, logical_index);
            FileWriteContext writer{std::fopen(temporary.c_str(), "wb")};
            if (!writer.file) {
                archive_read_free(reader);
                return fail(error, SA_ERROR_IO, errno, "Unable to create extracted file");
            }
            int32_t success = stream_active_libarchive_entry(
                source, reader, logical_index, file_write_callback, &writer,
                progress, context, total_completed, total_size, error);
            if (std::fclose(writer.file) != 0 && success)
                success = fail(error, SA_ERROR_IO, errno, "Unable to close extracted file");
            if (writer.failed) {
                std::remove(temporary.c_str());
                archive_read_free(reader);
                return fail(error, SA_ERROR_IO, writer.backend_error,
                            "Unable to write extracted file");
            }
            if (!success) {
                std::remove(temporary.c_str());
                archive_read_free(reader);
                return 0;
            }
            if (!finish_extracted_file(temporary, output, entry, options, error)) {
                archive_read_free(reader);
                return 0;
            }
            result = ARCHIVE_OK;
        }
        if (result < ARCHIVE_OK)
            break;
        total_completed += entry.uncompressed_size;
        ++logical_index;
        ++native_index;
    }

    if (result != ARCHIVE_EOF || logical_index != source->entries.size()) {
        int32_t failure = result == ARCHIVE_EOF
            ? fail(error, SA_ERROR_CORRUPT_ARCHIVE, 0,
                   "Archive directory changed while extracting")
            : fail_libarchive(error, reader, !source->password.empty(),
                              "Unable to continue reading archive");
        archive_read_free(reader);
        return failure;
    }
    archive_read_free(reader);
    if (options.preserve_timestamps) {
        for (auto iterator = source->entries.rbegin(); iterator != source->entries.rend(); ++iterator) {
            if (iterator->kind == SA_ENTRY_DIRECTORY)
                apply_modification_time(root / iterator->path, iterator->modification_time);
        }
    }
    return 1;
}

int32_t read_libarchive_entry(SAArchive *source, uint64_t entry_index,
                              SADataCallback callback, void *context,
                              SAError *error) {
    const Entry &entry = source->entries[entry_index];
    if (!require_libarchive_password(source, entry, error))
        return 0;
    struct archive *reader = open_libarchive_reader(source, error);
    if (!reader)
        return 0;
    if (!skip_to_libarchive_entry(source, reader, entry_index, error)) {
        archive_read_free(reader);
        return 0;
    }
    int32_t success = stream_active_libarchive_entry(
        source, reader, entry_index, callback, context, nullptr, nullptr,
        0, entry.uncompressed_size, error);
    archive_read_free(reader);
    return success;
}

int32_t extract_tar_entry(SAArchive *archive, uint64_t index, const fs::path &root,
                          const SAExtractionOptions &options,
                          SAProgressCallback progress, void *context,
                          uint64_t total_base, uint64_t total_size, SAError *error) {
    const Entry &entry = archive->entries[index];
    fs::path output;
    bool skip = false;
    if (!prepare_output_path(root, entry, options.overwrite_policy, output, skip, error))
        return 0;
    if (entry.kind == SA_ENTRY_DIRECTORY || skip) {
        if (entry.kind == SA_ENTRY_DIRECTORY && options.preserve_timestamps)
            apply_modification_time(output, entry.modification_time);
        return 1;
    }
    if (entry.kind != SA_ENTRY_FILE)
        return fail(error, SA_ERROR_UNSAFE_LINK, 0, "Link extraction is disabled");

    std::string temporary = temporary_path_for(output, index);
    FileWriteContext writer{std::fopen(temporary.c_str(), "wb")};
    if (!writer.file)
        return fail(error, SA_ERROR_IO, errno, "Unable to create extracted file");
    int32_t success = stream_tar_entry(archive, index, file_write_callback, &writer,
                                       progress, context, total_base, total_size, error);
    if (std::fclose(writer.file) != 0 && success)
        success = fail(error, SA_ERROR_IO, errno, "Unable to close extracted file");
    if (writer.failed) {
        std::remove(temporary.c_str());
        archive->cancelled.store(false);
        return fail(error, SA_ERROR_IO, writer.backend_error, "Unable to write extracted file");
    }
    if (!success) {
        std::remove(temporary.c_str());
        return 0;
    }
    return finish_extracted_file(temporary, output, entry, options, error);
}

int32_t extract_tar(SAArchive *archive, const fs::path &root,
                    const SAExtractionOptions &options,
                    SAProgressCallback progress, void *context, SAError *error) {
    uint64_t total_size = 0;
    for (const auto &entry : archive->entries)
        total_size += entry.uncompressed_size;
    uint64_t total_completed = 0;
    for (uint64_t index = 0; index < archive->entries.size(); ++index) {
        if (!extract_tar_entry(archive, index, root, options, progress, context,
                               total_completed, total_size, error))
            return 0;
        total_completed += archive->entries[index].uncompressed_size;
    }
    if (options.preserve_timestamps) {
        for (auto iterator = archive->entries.rbegin(); iterator != archive->entries.rend(); ++iterator) {
            if (iterator->kind == SA_ENTRY_DIRECTORY)
                apply_modification_time(root / iterator->path, iterator->modification_time);
        }
    }
    return 1;
}

int32_t extract_gzip(SAArchive *archive, const fs::path &root,
                     const SAExtractionOptions &options,
                     SAProgressCallback progress, void *context, SAError *error) {
    const Entry &entry = archive->entries[0];
    fs::path output;
    bool skip = false;
    if (!prepare_output_path(root, entry, options.overwrite_policy, output, skip, error))
        return 0;
    if (skip)
        return 1;
    std::string temporary = temporary_path_for(output, 0);
    FileWriteContext writer{std::fopen(temporary.c_str(), "wb")};
    if (!writer.file)
        return fail(error, SA_ERROR_IO, errno, "Unable to create extracted file");
    int32_t success = stream_gzip_entry(archive, 0, file_write_callback, &writer,
                                        progress, context, error);
    if (std::fclose(writer.file) != 0 && success)
        success = fail(error, SA_ERROR_IO, errno, "Unable to close extracted file");
    if (writer.failed) {
        std::remove(temporary.c_str());
        archive->cancelled.store(false);
        return fail(error, SA_ERROR_IO, writer.backend_error, "Unable to write extracted file");
    }
    if (!success) {
        std::remove(temporary.c_str());
        return 0;
    }
    return finish_extracted_file(temporary, output, entry, options, error);
}

int32_t open_zip_reader(SAArchive *archive, void **reader, SAError *error) {
    *reader = mz_zip_reader_create();
    if (!*reader)
        return fail(error, SA_ERROR_INTERNAL, MZ_MEM_ERROR, "Unable to allocate ZIP reader");
    mz_zip_reader_set_password(*reader, archive->password.empty() ? nullptr : archive->password.c_str());
    int32_t result = mz_zip_reader_open_file(*reader, archive->path.c_str());
    if (result != MZ_OK) {
        mz_zip_reader_delete(reader);
        return fail(error, minizip_error_code(result, !archive->password.empty()), result,
                    "Unable to open ZIP archive");
    }
    return 1;
}

int32_t zip_goto_index(void *reader, uint64_t target, SAError *error) {
    int32_t result = mz_zip_reader_goto_first_entry(reader);
    for (uint64_t index = 0; result == MZ_OK && index < target; ++index)
        result = mz_zip_reader_goto_next_entry(reader);
    if (result != MZ_OK)
        return fail(error, SA_ERROR_ENTRY_NOT_FOUND, result, "Archive entry was not found");
    return 1;
}

int32_t extract_zip(SAArchive *archive, const fs::path &root,
                    const SAExtractionOptions &options,
                    SAProgressCallback progress, void *context, SAError *error) {
    void *reader = nullptr;
    if (!open_zip_reader(archive, &reader, error))
        return 0;
    uint64_t total_size = 0;
    for (const auto &entry : archive->entries)
        total_size += entry.uncompressed_size;
    uint64_t total_completed = 0;
    int32_t result = mz_zip_reader_goto_first_entry(reader);
    for (uint64_t index = 0; index < archive->entries.size() && result == MZ_OK; ++index) {
        if (archive->cancelled.load()) {
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        }
        const Entry &entry = archive->entries[index];
        if (entry.kind != SA_ENTRY_FILE && entry.kind != SA_ENTRY_DIRECTORY) {
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
            return fail(error, SA_ERROR_UNSAFE_LINK, 0, "Link extraction is disabled");
        }
        fs::path output;
        bool skip = false;
        if (!prepare_output_path(root, entry, options.overwrite_policy, output, skip, error)) {
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
            return 0;
        }
        if (entry.kind == SA_ENTRY_DIRECTORY || skip) {
            total_completed += entry.uncompressed_size;
            result = mz_zip_reader_goto_next_entry(reader);
            continue;
        }

        std::string temporary = temporary_path_for(output, index);
        FILE *file = std::fopen(temporary.c_str(), "wb");
        if (!file) {
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
            return fail(error, SA_ERROR_IO, errno, "Unable to create extracted file");
        }
        result = mz_zip_reader_entry_open(reader);
        uint64_t entry_completed = 0;
        uint8_t buffer[128 * 1024];
        while (result == MZ_OK) {
            int32_t read = mz_zip_reader_entry_read(reader, buffer, sizeof(buffer));
            if (read < 0) {
                result = read;
                break;
            }
            if (read == 0)
                break;
            if (std::fwrite(buffer, 1, static_cast<size_t>(read), file) != static_cast<size_t>(read)) {
                result = MZ_WRITE_ERROR;
                break;
            }
            entry_completed += static_cast<uint64_t>(read);
            SAProgress state{sizeof(SAProgress), index, entry.path.c_str(), entry_completed,
                             entry.uncompressed_size, total_completed + entry_completed, total_size};
            if (archive->cancelled.load() || (progress && progress(context, &state) == 0)) {
                archive->cancelled.store(true);
                result = MZ_INTERNAL_ERROR;
                break;
            }
        }
        int32_t close_result = mz_zip_reader_entry_close(reader);
        std::fclose(file);
        if (result == MZ_OK && close_result != MZ_OK)
            result = close_result;
        if (result != MZ_OK) {
            std::remove(temporary.c_str());
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
            if (archive->cancelled.load())
                return fail(error, SA_ERROR_CANCELLED, result, "Operation was cancelled");
            return fail(error, minizip_error_code(result, !archive->password.empty()), result,
                        "Unable to extract ZIP entry");
        }
        std::error_code ec;
        if (fs::exists(fs::symlink_status(output, ec)))
            fs::remove(output, ec);
        fs::rename(temporary, output, ec);
        if (ec) {
            std::remove(temporary.c_str());
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
            return fail(error, SA_ERROR_IO, ec.value(), "Unable to finish extracted file");
        }
        if (options.preserve_timestamps)
            apply_modification_time(output, entry.modification_time);
        total_completed += entry.uncompressed_size;
        result = mz_zip_reader_goto_next_entry(reader);
    }
    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    if (result != MZ_END_OF_LIST && !archive->entries.empty())
        return fail(error, minizip_error_code(result, !archive->password.empty()), result,
                    "Unable to continue reading ZIP archive");
    if (options.preserve_timestamps) {
        for (auto iterator = archive->entries.rbegin(); iterator != archive->entries.rend(); ++iterator) {
            if (iterator->kind == SA_ENTRY_DIRECTORY)
                apply_modification_time(root / iterator->path, iterator->modification_time);
        }
    }
    return 1;
}

int32_t open_rar(SAArchive *archive, unsigned int mode, RarCallbackContext *callback,
                 HANDLE &handle, SAError *error) {
    std::wstring archive_path = utf8_to_wide(archive->path);
    RAROpenArchiveDataEx open_data{};
    open_data.ArcNameW = archive_path.data();
    open_data.OpenMode = mode;
    open_data.Callback = rar_callback;
    open_data.UserData = reinterpret_cast<LPARAM>(callback);
    handle = RAROpenArchiveEx(&open_data);
    if (!handle)
        return fail(error, unrar_error_code(open_data.OpenResult, !archive->password.empty()),
                    static_cast<int32_t>(open_data.OpenResult), "Unable to open RAR archive");
    return 1;
}

int32_t extract_rar(SAArchive *archive, const fs::path &root,
                    const SAExtractionOptions &options,
                    SAProgressCallback progress, void *context, SAError *error) {
    uint64_t total_size = 0;
    for (const auto &entry : archive->entries)
        total_size += entry.uncompressed_size;
    RarCallbackContext callback{&archive->password, &archive->cancelled};
    callback.progress_callback = progress;
    callback.progress_context = context;
    callback.progress.struct_size = sizeof(SAProgress);
    callback.progress.total_size = total_size;
    HANDLE handle = nullptr;
    if (!open_rar(archive, RAR_OM_EXTRACT, &callback, handle, error))
        return 0;

    int32_t result = ERAR_SUCCESS;
    uint64_t total_completed = 0;
    for (uint64_t index = 0; index < archive->entries.size(); ++index) {
        RARHeaderDataEx header{};
        result = RARReadHeaderEx(handle, &header);
        if (result != ERAR_SUCCESS)
            break;
        const Entry &entry = archive->entries[index];
        fs::path output;
        bool skip = false;
        if (!prepare_output_path(root, entry, options.overwrite_policy, output, skip, error)) {
            RARCloseArchive(handle);
            return 0;
        }
        callback.progress.entry_index = index;
        callback.progress.entry_path = entry.path.c_str();
        callback.progress.entry_completed = 0;
        callback.progress.entry_total = entry.uncompressed_size;
        callback.progress.total_completed = total_completed;
        if (archive->cancelled.load()) {
            RARCloseArchive(handle);
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        }
        if (entry.kind == SA_ENTRY_DIRECTORY) {
            result = RARProcessFileW(handle, RAR_SKIP, nullptr, nullptr);
        } else if (entry.kind != SA_ENTRY_FILE) {
            RARCloseArchive(handle);
            return fail(error, SA_ERROR_UNSAFE_LINK, 0, "Link extraction is disabled");
        } else if (skip) {
            result = RARProcessFileW(handle, RAR_TEST, nullptr, nullptr);
        } else {
            std::string temporary = temporary_path_for(output, index);
            std::wstring temporary_wide = utf8_to_wide(temporary);
            result = RARProcessFileW(handle, RAR_EXTRACT, nullptr, temporary_wide.data());
            if (result == ERAR_SUCCESS) {
                std::error_code ec;
                if (fs::exists(fs::symlink_status(output, ec)))
                    fs::remove(output, ec);
                fs::rename(temporary, output, ec);
                if (ec) {
                    std::remove(temporary.c_str());
                    RARCloseArchive(handle);
                    return fail(error, SA_ERROR_IO, ec.value(), "Unable to finish extracted file");
                }
                if (options.preserve_timestamps)
                    apply_modification_time(output, entry.modification_time);
            } else {
                std::remove(temporary.c_str());
            }
        }
        if (result != ERAR_SUCCESS)
            break;
        total_completed += entry.uncompressed_size;
        callback.progress.total_completed = total_completed;
    }
    RARCloseArchive(handle);
    if (archive->cancelled.load())
        return fail(error, SA_ERROR_CANCELLED, result, "Operation was cancelled");
    if (result != ERAR_SUCCESS)
        return fail(error, unrar_error_code(result, !archive->password.empty()), result,
                    "Unable to extract RAR entry");
    if (options.preserve_timestamps) {
        for (auto iterator = archive->entries.rbegin(); iterator != archive->entries.rend(); ++iterator) {
            if (iterator->kind == SA_ENTRY_DIRECTORY)
                apply_modification_time(root / iterator->path, iterator->modification_time);
        }
    }
    return 1;
}

int32_t extract_one_zip(SAArchive *archive, uint64_t index, const fs::path &root,
                        const SAExtractionOptions &options,
                        SAProgressCallback progress, void *context, SAError *error) {
    const Entry &entry = archive->entries[index];
    if (entry.kind != SA_ENTRY_FILE && entry.kind != SA_ENTRY_DIRECTORY)
        return fail(error, SA_ERROR_UNSAFE_LINK, 0, "Link extraction is disabled");
    fs::path output;
    bool skip = false;
    if (!prepare_output_path(root, entry, options.overwrite_policy, output, skip, error))
        return 0;
    if (entry.kind == SA_ENTRY_DIRECTORY || skip) {
        if (entry.kind == SA_ENTRY_DIRECTORY && options.preserve_timestamps)
            apply_modification_time(output, entry.modification_time);
        return 1;
    }

    void *reader = nullptr;
    if (!open_zip_reader(archive, &reader, error))
        return 0;
    if (!zip_goto_index(reader, index, error)) {
        mz_zip_reader_close(reader);
        mz_zip_reader_delete(&reader);
        return 0;
    }
    std::string temporary = temporary_path_for(output, index);
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) {
        mz_zip_reader_close(reader);
        mz_zip_reader_delete(&reader);
        return fail(error, SA_ERROR_IO, errno, "Unable to create extracted file");
    }
    int32_t result = mz_zip_reader_entry_open(reader);
    uint64_t completed = 0;
    uint8_t buffer[128 * 1024];
    while (result == MZ_OK) {
        int32_t read = mz_zip_reader_entry_read(reader, buffer, sizeof(buffer));
        if (read < 0) {
            result = read;
            break;
        }
        if (read == 0)
            break;
        if (std::fwrite(buffer, 1, static_cast<size_t>(read), file) != static_cast<size_t>(read)) {
            result = MZ_WRITE_ERROR;
            break;
        }
        completed += static_cast<uint64_t>(read);
        SAProgress state{sizeof(SAProgress), index, entry.path.c_str(), completed,
                         entry.uncompressed_size, completed, entry.uncompressed_size};
        if (archive->cancelled.load() || (progress && progress(context, &state) == 0)) {
            archive->cancelled.store(true);
            result = MZ_INTERNAL_ERROR;
            break;
        }
    }
    int32_t close_result = mz_zip_reader_entry_close(reader);
    std::fclose(file);
    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    if (result == MZ_OK && close_result != MZ_OK)
        result = close_result;
    if (result != MZ_OK) {
        std::remove(temporary.c_str());
        if (archive->cancelled.load())
            return fail(error, SA_ERROR_CANCELLED, result, "Operation was cancelled");
        return fail(error, minizip_error_code(result, !archive->password.empty()), result,
                    "Unable to extract ZIP entry");
    }
    std::error_code ec;
    if (fs::exists(fs::symlink_status(output, ec)))
        fs::remove(output, ec);
    fs::rename(temporary, output, ec);
    if (ec) {
        std::remove(temporary.c_str());
        return fail(error, SA_ERROR_IO, ec.value(), "Unable to finish extracted file");
    }
    if (options.preserve_timestamps)
        apply_modification_time(output, entry.modification_time);
    return 1;
}

int32_t extract_one_rar(SAArchive *archive, uint64_t index, const fs::path &root,
                        const SAExtractionOptions &options,
                        SAProgressCallback progress, void *context, SAError *error) {
    RarCallbackContext callback{&archive->password, &archive->cancelled};
    HANDLE handle = nullptr;
    if (!open_rar(archive, RAR_OM_EXTRACT, &callback, handle, error))
        return 0;
    int32_t result = ERAR_SUCCESS;
    fs::path output;
    std::string temporary;
    bool skip = false;
    for (uint64_t current = 0; current <= index; ++current) {
        RARHeaderDataEx header{};
        result = RARReadHeaderEx(handle, &header);
        if (result != ERAR_SUCCESS)
            break;
        if (current < index) {
            result = RARProcessFileW(handle, RAR_SKIP, nullptr, nullptr);
            if (result != ERAR_SUCCESS)
                break;
            continue;
        }
        const Entry &entry = archive->entries[index];
        if (entry.kind != SA_ENTRY_FILE && entry.kind != SA_ENTRY_DIRECTORY) {
            RARCloseArchive(handle);
            return fail(error, SA_ERROR_UNSAFE_LINK, 0, "Link extraction is disabled");
        }
        if (!prepare_output_path(root, entry, options.overwrite_policy, output, skip, error)) {
            RARCloseArchive(handle);
            return 0;
        }
        callback.progress_callback = progress;
        callback.progress_context = context;
        callback.progress = {sizeof(SAProgress), index, entry.path.c_str(), 0,
                             entry.uncompressed_size, 0, entry.uncompressed_size};
        if (entry.kind == SA_ENTRY_DIRECTORY) {
            result = RARProcessFileW(handle, RAR_SKIP, nullptr, nullptr);
        } else if (skip) {
            result = RARProcessFileW(handle, RAR_TEST, nullptr, nullptr);
        } else {
            temporary = temporary_path_for(output, index);
            std::wstring temporary_wide = utf8_to_wide(temporary);
            result = RARProcessFileW(handle, RAR_EXTRACT, nullptr, temporary_wide.data());
        }
    }
    RARCloseArchive(handle);
    if (archive->cancelled.load()) {
        if (!temporary.empty())
            std::remove(temporary.c_str());
        return fail(error, SA_ERROR_CANCELLED, result, "Operation was cancelled");
    }
    if (result != ERAR_SUCCESS) {
        if (!temporary.empty())
            std::remove(temporary.c_str());
        return fail(error, unrar_error_code(result, !archive->password.empty()), result,
                    "Unable to extract RAR entry");
    }
    const Entry &entry = archive->entries[index];
    if (entry.kind == SA_ENTRY_DIRECTORY) {
        if (options.preserve_timestamps)
            apply_modification_time(output, entry.modification_time);
        return 1;
    }
    if (skip)
        return 1;
    std::error_code ec;
    if (fs::exists(fs::symlink_status(output, ec)))
        fs::remove(output, ec);
    fs::rename(temporary, output, ec);
    if (ec) {
        std::remove(temporary.c_str());
        return fail(error, SA_ERROR_IO, ec.value(), "Unable to finish extracted file");
    }
    if (options.preserve_timestamps)
        apply_modification_time(output, entry.modification_time);
    return 1;
}

struct ZipInput {
    fs::path source;
    std::string archive_path;
    bool directory = false;
    uint64_t size = 0;
};

int32_t append_zip_input(const fs::path &source, const std::string &archive_path,
                         std::vector<ZipInput> &items, std::set<std::string> &names,
                         SAError *error) {
    std::error_code ec;
    auto status = fs::symlink_status(source, ec);
    if (ec || !fs::exists(status))
        return fail(error, SA_ERROR_IO, ec.value(), "ZIP input does not exist");
    if (fs::is_symlink(status))
        return fail(error, SA_ERROR_UNSAFE_LINK, 0, "ZIP creation does not follow symbolic links");
    std::string normalized = normalized_entry_path(archive_path);
    if (normalized.empty())
        return fail(error, SA_ERROR_UNSAFE_PATH, 0, "ZIP input has an unsafe archive path");
    bool is_directory = fs::is_directory(status);
    if (is_directory && normalized.back() != '/')
        normalized.push_back('/');
    if (!names.insert(normalized).second)
        return fail(error, SA_ERROR_INVALID_ARGUMENT, 0, "ZIP contains duplicate archive paths");
    uint64_t size = is_directory ? 0 : fs::file_size(source, ec);
    if (ec)
        return fail(error, SA_ERROR_IO, ec.value(), "Unable to inspect ZIP input");
    items.push_back({source, normalized, is_directory, size});
    return 1;
}

int32_t collect_zip_inputs(const char *const *source_paths, const char *const *archive_paths,
                           size_t item_count, const SAZipCreateOptions &options,
                           std::vector<ZipInput> &items, SAError *error) {
    std::set<std::string> names;
    for (size_t index = 0; index < item_count; ++index) {
        if (!source_paths[index] || source_paths[index][0] == 0)
            return fail(error, SA_ERROR_INVALID_ARGUMENT, 0, "ZIP input path is empty");
        fs::path source(source_paths[index]);
        std::string root_name = archive_paths && archive_paths[index] && archive_paths[index][0]
                                    ? archive_paths[index]
                                    : source.filename().string();
        std::error_code ec;
        if (!fs::is_directory(fs::symlink_status(source, ec))) {
            if (!append_zip_input(source, root_name, items, names, error))
                return 0;
            continue;
        }
        std::string directory_prefix = options.include_parent_directory ? root_name : "";
        if (!directory_prefix.empty() &&
            !append_zip_input(source, directory_prefix, items, names, error))
            return 0;
        fs::recursive_directory_iterator iterator(source, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        for (; iterator != end; iterator.increment(ec)) {
            if (ec)
                return fail(error, SA_ERROR_IO, ec.value(), "Unable to enumerate ZIP input directory");
            fs::path relative = fs::relative(iterator->path(), source, ec);
            if (ec)
                return fail(error, SA_ERROR_IO, ec.value(), "Unable to resolve ZIP input path");
            std::string name = directory_prefix.empty()
                                   ? relative.generic_string()
                                   : (fs::path(directory_prefix) / relative).generic_string();
            if (!append_zip_input(iterator->path(), name, items, names, error))
                return 0;
        }
    }
    return 1;
}

bool is_zip_split_volume(const fs::path &candidate, const fs::path &archive_path) {
    if (candidate.stem() != archive_path.stem())
        return false;
    std::string extension = candidate.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (extension.size() < 4 || extension[0] != '.' || extension[1] != 'z')
        return false;
    return std::all_of(extension.begin() + 2, extension.end(), [](unsigned char value) {
        return std::isdigit(value) != 0;
    });
}

std::vector<fs::path> zip_split_volumes(const fs::path &archive_path) {
    std::vector<fs::path> volumes;
    fs::path directory = archive_path.parent_path();
    if (directory.empty())
        directory = fs::current_path();
    std::error_code error;
    fs::directory_iterator iterator(directory, error);
    if (error)
        return volumes;
    for (const auto &item : iterator) {
        if (is_zip_split_volume(item.path(), archive_path))
            volumes.push_back(item.path());
    }
    return volumes;
}

void remove_zip_outputs(const fs::path &archive_path, bool includes_split_volumes) {
    std::error_code error;
    fs::remove(archive_path, error);
    if (!includes_split_volumes)
        return;
    for (const auto &volume : zip_split_volumes(archive_path)) {
        error.clear();
        fs::remove(volume, error);
    }
}

} // namespace

extern "C" {

SAOpenOptions swiftarchive_open_options_default(void) {
    SAOpenOptions options{};
    options.struct_size = sizeof(options);
    options.filename_encoding = SA_FILENAME_ENCODING_AUTO;
    options.maximum_entry_count = 100000;
    options.maximum_entry_size = 20ULL * 1024 * 1024 * 1024;
    options.maximum_total_size = 50ULL * 1024 * 1024 * 1024;
    options.maximum_compression_ratio = 10000.0;
    return options;
}

SAExtractionOptions swiftarchive_extraction_options_default(void) {
    SAExtractionOptions options{};
    options.struct_size = sizeof(options);
    options.overwrite_policy = SA_OVERWRITE_ERROR;
    options.preserve_timestamps = 1;
    return options;
}

SAZipCreateOptions swiftarchive_zip_create_options_default(void) {
    SAZipCreateOptions options{};
    options.struct_size = sizeof(options);
    options.compression_level = MZ_COMPRESS_LEVEL_NORMAL;
    options.use_aes = 1;
    options.include_parent_directory = 1;
    return options;
}

SAArchive *swiftarchive_archive_open(const char *path, SAArchiveFormat format,
                           const SAOpenOptions *options, SAError *error) {
    clear_error(error);
    if (!path || path[0] == 0) {
        fail(error, SA_ERROR_INVALID_ARGUMENT, 0, "Archive path is empty");
        return nullptr;
    }
    auto *archive = new (std::nothrow) SAArchive;
    if (!archive) {
        fail(error, SA_ERROR_INTERNAL, 0, "Unable to allocate archive handle");
        return nullptr;
    }
    archive->path = path;
    archive->limits = options ? *options : swiftarchive_open_options_default();
    if (archive->limits.maximum_entry_count == 0 || archive->limits.maximum_entry_size == 0 ||
        archive->limits.maximum_total_size == 0) {
        delete archive;
        fail(error, SA_ERROR_INVALID_ARGUMENT, 0, "Resource limits must be greater than zero");
        return nullptr;
    }
    if (options && options->password)
        archive->password = options->password;
    archive->encoding = options ? options->filename_encoding : SA_FILENAME_ENCODING_AUTO;

    if (format == SA_ARCHIVE_FORMAT_AUTO) {
        FILE *file = std::fopen(path, "rb");
        if (!file) {
            delete archive;
            fail(error, SA_ERROR_OPEN_FAILED, 0, "Unable to open archive");
            return nullptr;
        }
        uint8_t magic[512]{};
        size_t count = std::fread(magic, 1, sizeof(magic), file);
        std::fclose(file);
        if (count >= 4 && magic[0] == 'P' && magic[1] == 'K')
            format = SA_ARCHIVE_FORMAT_ZIP;
        else if (count >= 7 && std::memcmp(magic, "Rar!\x1a\x07", 6) == 0)
            format = SA_ARCHIVE_FORMAT_RAR;
        else if (count >= 2 && magic[0] == 0x1f && magic[1] == 0x8b)
            format = SA_ARCHIVE_FORMAT_GZIP;
        else if (count >= 262 && std::memcmp(magic + 257, "ustar", 5) == 0)
            format = SA_ARCHIVE_FORMAT_TAR;
        else {
            SAError local_error{};
            SAError *attempt_error = error ? error : &local_error;
            auto try_format = [&](SAArchiveFormat candidate) {
                clear_error(attempt_error);
                archive->entries.clear();
                archive->format = candidate;
                switch (candidate) {
                case SA_ARCHIVE_FORMAT_ZIP:
                    return list_zip(archive, attempt_error);
                case SA_ARCHIVE_FORMAT_RAR:
                    return list_rar(archive, attempt_error);
                case SA_ARCHIVE_FORMAT_TAR:
                    return list_tar(archive, attempt_error);
                case SA_ARCHIVE_FORMAT_GZIP:
                    return list_gzip(archive, attempt_error);
                default:
                    return 0;
                }
            };
            auto is_conclusive_error = [](SAErrorCode code) {
                return code == SA_ERROR_PASSWORD_REQUIRED || code == SA_ERROR_BAD_PASSWORD ||
                       code == SA_ERROR_UNSAFE_PATH || code == SA_ERROR_UNSAFE_LINK ||
                       code == SA_ERROR_RESOURCE_LIMIT || code == SA_ERROR_CANCELLED;
            };

            if (try_format(SA_ARCHIVE_FORMAT_ZIP))
                return archive;
            if (is_conclusive_error(attempt_error->code)) {
                delete archive;
                return nullptr;
            }
            if (try_format(SA_ARCHIVE_FORMAT_RAR))
                return archive;
            if (is_conclusive_error(attempt_error->code)) {
                delete archive;
                return nullptr;
            }
            if (try_format(SA_ARCHIVE_FORMAT_TAR))
                return archive;
            if (is_conclusive_error(attempt_error->code)) {
                delete archive;
                return nullptr;
            }
            if (try_format(SA_ARCHIVE_FORMAT_GZIP))
                return archive;
            if (is_conclusive_error(attempt_error->code)) {
                delete archive;
                return nullptr;
            }

            clear_error(attempt_error);
            archive->entries.clear();
            archive->format = SA_ARCHIVE_FORMAT_AUTO;
            if (list_libarchive(archive, attempt_error))
                return archive;
            if (is_conclusive_error(attempt_error->code)) {
                delete archive;
                return nullptr;
            }

            delete archive;
            return nullptr;
        }
    }
    archive->format = format;
    int32_t success = 0;
    switch (format) {
    case SA_ARCHIVE_FORMAT_ZIP:
        success = list_zip(archive, error);
        break;
    case SA_ARCHIVE_FORMAT_RAR:
        success = list_rar(archive, error);
        break;
    case SA_ARCHIVE_FORMAT_TAR:
        success = list_tar(archive, error);
        break;
    case SA_ARCHIVE_FORMAT_GZIP:
        success = list_gzip(archive, error);
        break;
    case SA_ARCHIVE_FORMAT_7ZIP:
    case SA_ARCHIVE_FORMAT_BZIP2:
    case SA_ARCHIVE_FORMAT_XZ:
    case SA_ARCHIVE_FORMAT_LZMA:
    case SA_ARCHIVE_FORMAT_LZIP:
    case SA_ARCHIVE_FORMAT_COMPRESS:
    case SA_ARCHIVE_FORMAT_ZSTD:
    case SA_ARCHIVE_FORMAT_LZ4:
    case SA_ARCHIVE_FORMAT_CAB:
    case SA_ARCHIVE_FORMAT_CPIO:
    case SA_ARCHIVE_FORMAT_ISO9660:
    case SA_ARCHIVE_FORMAT_LHA:
    case SA_ARCHIVE_FORMAT_AR:
    case SA_ARCHIVE_FORMAT_WARC:
        archive->format = SA_ARCHIVE_FORMAT_AUTO;
        success = list_libarchive(archive, error);
        break;
    default:
        break;
    }
    if (!success && format == SA_ARCHIVE_FORMAT_ZIP) {
        SAErrorCode code = error ? error->code : SA_ERROR_NONE;
        bool conclusive = code == SA_ERROR_PASSWORD_REQUIRED || code == SA_ERROR_BAD_PASSWORD ||
                          code == SA_ERROR_UNSAFE_PATH || code == SA_ERROR_UNSAFE_LINK ||
                          code == SA_ERROR_RESOURCE_LIMIT || code == SA_ERROR_CANCELLED;
        if (!conclusive) {
            clear_error(error);
            archive->entries.clear();
            archive->format = SA_ARCHIVE_FORMAT_AUTO;
            success = list_libarchive(archive, error);
        }
    }
    if (!success) {
        if (!error || error->code == SA_ERROR_NONE)
            fail(error, SA_ERROR_UNSUPPORTED_FORMAT, 0, "Archive format is not supported");
        delete archive;
        return nullptr;
    }
    return archive;
}

void swiftarchive_archive_close(SAArchive *archive) {
    delete archive;
}

SAArchiveFormat swiftarchive_archive_format(const SAArchive *archive) {
    return archive ? archive->format : SA_ARCHIVE_FORMAT_AUTO;
}

uint64_t swiftarchive_archive_entry_count(const SAArchive *archive) {
    return archive ? archive->entries.size() : 0;
}

int32_t swiftarchive_archive_get_entry(const SAArchive *archive, uint64_t index,
                             SAEntryInfo *entry, SAError *error) {
    clear_error(error);
    if (!archive || !entry || index >= archive->entries.size())
        return fail(error, SA_ERROR_ENTRY_NOT_FOUND, 0, "Archive entry was not found");
    const Entry &source = archive->entries[index];
    std::memset(entry, 0, sizeof(*entry));
    entry->struct_size = sizeof(*entry);
    entry->path = source.path.c_str();
    entry->kind = source.kind;
    entry->compressed_size = source.compressed_size;
    entry->uncompressed_size = source.uncompressed_size;
    entry->modification_time = source.modification_time;
    entry->crc32 = source.crc32;
    entry->encrypted = source.encrypted;
    entry->solid = source.solid;
    entry->split_before = source.split_before;
    entry->split_after = source.split_after;
    return 1;
}

int32_t swiftarchive_archive_extract_all(SAArchive *archive, const char *destination,
                               const SAExtractionOptions *options,
                               SAProgressCallback progress, void *context,
                               SAError *error) {
    clear_error(error);
    if (!archive || !destination || destination[0] == 0)
        return fail(error, SA_ERROR_INVALID_ARGUMENT, 0, "Destination path is empty");
    if (!archive->uses_libarchive && archive->format == SA_ARCHIVE_FORMAT_ZIP) {
        for (const auto &entry : archive->entries) {
            if (!require_zip_password(archive, entry, error))
                return 0;
        }
    }
    archive->cancelled.store(false);
    SAExtractionOptions effective = options ? *options : swiftarchive_extraction_options_default();
    fs::path root(destination);
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec)
        return fail(error, SA_ERROR_IO, ec.value(), "Unable to create destination directory");
    if (fs::is_symlink(fs::symlink_status(root, ec)))
        return fail(error, SA_ERROR_UNSAFE_PATH, 0, "Destination is a symbolic link");
    if (archive->uses_libarchive)
        return extract_libarchive(archive, root, effective, progress, context, error);
    if (archive->format == SA_ARCHIVE_FORMAT_ZIP)
        return extract_zip(archive, root, effective, progress, context, error);
    if (archive->format == SA_ARCHIVE_FORMAT_RAR)
        return extract_rar(archive, root, effective, progress, context, error);
    if (archive->format == SA_ARCHIVE_FORMAT_TAR)
        return extract_tar(archive, root, effective, progress, context, error);
    if (archive->format == SA_ARCHIVE_FORMAT_GZIP)
        return extract_gzip(archive, root, effective, progress, context, error);
    return fail(error, SA_ERROR_UNSUPPORTED_FORMAT, 0, "Archive format is not supported");
}

int32_t swiftarchive_archive_extract_entry(SAArchive *archive, uint64_t index,
                                 const char *destination,
                                 const SAExtractionOptions *options,
                                 SAProgressCallback progress, void *context,
                                 SAError *error) {
    clear_error(error);
    if (!archive || index >= archive->entries.size() || !destination || destination[0] == 0)
        return fail(error, SA_ERROR_INVALID_ARGUMENT, 0, "Invalid entry extraction request");
    if (!archive->uses_libarchive && archive->format == SA_ARCHIVE_FORMAT_ZIP &&
        !require_zip_password(archive, archive->entries[index], error))
        return 0;
    archive->cancelled.store(false);
    SAExtractionOptions effective = options ? *options : swiftarchive_extraction_options_default();
    fs::path root(destination);
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec)
        return fail(error, SA_ERROR_IO, ec.value(), "Unable to create destination directory");
    if (fs::is_symlink(fs::symlink_status(root, ec)))
        return fail(error, SA_ERROR_UNSAFE_PATH, 0, "Destination is a symbolic link");
    if (archive->uses_libarchive)
        return extract_one_libarchive(archive, index, root, effective,
                                      progress, context, error);
    if (archive->format == SA_ARCHIVE_FORMAT_ZIP)
        return extract_one_zip(archive, index, root, effective, progress, context, error);
    if (archive->format == SA_ARCHIVE_FORMAT_RAR)
        return extract_one_rar(archive, index, root, effective, progress, context, error);
    if (archive->format == SA_ARCHIVE_FORMAT_TAR)
        return extract_tar_entry(archive, index, root, effective, progress, context,
                                 0, archive->entries[index].uncompressed_size, error);
    if (archive->format == SA_ARCHIVE_FORMAT_GZIP)
        return extract_gzip(archive, root, effective, progress, context, error);
    return fail(error, SA_ERROR_UNSUPPORTED_FORMAT, 0, "Archive format is not supported");
}

int32_t swiftarchive_archive_read_entry(SAArchive *archive, uint64_t index,
                              SADataCallback callback, void *context,
                              SAError *error) {
    clear_error(error);
    if (!archive || !callback || index >= archive->entries.size())
        return fail(error, SA_ERROR_INVALID_ARGUMENT, 0, "Invalid entry read request");
    if (archive->entries[index].kind != SA_ENTRY_FILE)
        return fail(error, SA_ERROR_UNSUPPORTED_FEATURE, 0, "Only regular files can be read");
    if (!archive->uses_libarchive && archive->format == SA_ARCHIVE_FORMAT_ZIP &&
        !require_zip_password(archive, archive->entries[index], error))
        return 0;
    archive->cancelled.store(false);
    if (archive->uses_libarchive)
        return read_libarchive_entry(archive, index, callback, context, error);
    if (archive->format == SA_ARCHIVE_FORMAT_ZIP) {
        void *reader = nullptr;
        if (!open_zip_reader(archive, &reader, error))
            return 0;
        if (!zip_goto_index(reader, index, error)) {
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
            return 0;
        }
        int32_t result = mz_zip_reader_entry_open(reader);
        uint8_t buffer[128 * 1024];
        while (result == MZ_OK && !archive->cancelled.load()) {
            int32_t read = mz_zip_reader_entry_read(reader, buffer, sizeof(buffer));
            if (read < 0) {
                result = read;
                break;
            }
            if (read == 0)
                break;
            if (callback(context, buffer, static_cast<size_t>(read)) == 0) {
                archive->cancelled.store(true);
                break;
            }
        }
        int32_t close_result = mz_zip_reader_entry_close(reader);
        mz_zip_reader_close(reader);
        mz_zip_reader_delete(&reader);
        if (archive->cancelled.load())
            return fail(error, SA_ERROR_CANCELLED, 0, "Operation was cancelled");
        if (result == MZ_OK && close_result != MZ_OK)
            result = close_result;
        if (result != MZ_OK)
            return fail(error, minizip_error_code(result, !archive->password.empty()), result,
                        "Unable to read ZIP entry");
        return 1;
    }

    if (archive->format == SA_ARCHIVE_FORMAT_TAR)
        return stream_tar_entry(archive, index, callback, context, nullptr, nullptr,
                                0, archive->entries[index].uncompressed_size, error);
    if (archive->format == SA_ARCHIVE_FORMAT_GZIP)
        return stream_gzip_entry(archive, index, callback, context, nullptr, nullptr, error);

    RarCallbackContext callback_context{&archive->password, &archive->cancelled};
    callback_context.data_callback = callback;
    callback_context.data_context = context;
    HANDLE handle = nullptr;
    if (!open_rar(archive, RAR_OM_EXTRACT, &callback_context, handle, error))
        return 0;
    int32_t result = ERAR_SUCCESS;
    for (uint64_t current = 0; current <= index; ++current) {
        RARHeaderDataEx header{};
        result = RARReadHeaderEx(handle, &header);
        if (result != ERAR_SUCCESS)
            break;
        callback_context.data_callback = current == index ? callback : nullptr;
        result = RARProcessFileW(handle, current == index ? RAR_TEST : RAR_SKIP, nullptr, nullptr);
        if (result != ERAR_SUCCESS)
            break;
    }
    RARCloseArchive(handle);
    if (archive->cancelled.load())
        return fail(error, SA_ERROR_CANCELLED, result, "Operation was cancelled");
    if (result != ERAR_SUCCESS)
        return fail(error, unrar_error_code(result, !archive->password.empty()), result,
                    "Unable to read RAR entry");
    return 1;
}

void swiftarchive_archive_cancel(SAArchive *archive) {
    if (archive)
        archive->cancelled.store(true);
}

int32_t swiftarchive_zip_create(const char *archive_path, const char *const *source_paths,
                      const char *const *archive_paths, size_t item_count,
                      const SAZipCreateOptions *options,
                      SAProgressCallback progress, void *context, SAError *error) {
    clear_error(error);
    if (!archive_path || archive_path[0] == 0 || !source_paths || item_count == 0)
        return fail(error, SA_ERROR_INVALID_ARGUMENT, 0, "ZIP creation requires an output and input paths");
    SAZipCreateOptions effective = options ? *options : swiftarchive_zip_create_options_default();
    fs::path output_path(archive_path);
    std::error_code output_error;
    if (fs::exists(fs::symlink_status(output_path, output_error)) ||
        (effective.volume_size > 0 && !zip_split_volumes(output_path).empty()))
        return fail(error, SA_ERROR_DESTINATION_EXISTS, 0, "ZIP output already exists");
    if (output_error && output_error != std::errc::no_such_file_or_directory)
        return fail(error, SA_ERROR_IO, output_error.value(), "Unable to inspect ZIP output");
    std::vector<ZipInput> items;
    if (!collect_zip_inputs(source_paths, archive_paths, item_count, effective, items, error))
        return 0;
    uint64_t total_size = 0;
    for (const auto &item : items)
        total_size += item.size;

    void *writer = mz_zip_writer_create();
    if (!writer)
        return fail(error, SA_ERROR_INTERNAL, MZ_MEM_ERROR, "Unable to allocate ZIP writer");
    mz_zip_writer_set_password(writer, effective.password && effective.password[0] ? effective.password : nullptr);
    mz_zip_writer_set_aes(writer, effective.password && effective.password[0] && effective.use_aes);
    mz_zip_writer_set_compress_method(writer, MZ_COMPRESS_METHOD_DEFLATE);
    mz_zip_writer_set_compress_level(writer, effective.compression_level);
    int32_t result = mz_zip_writer_open_file(writer, archive_path,
                                             static_cast<int64_t>(effective.volume_size), 0);
    if (result != MZ_OK) {
        mz_zip_writer_delete(&writer);
        return fail(error, minizip_error_code(result, effective.password && effective.password[0]), result,
                    "Unable to create ZIP archive");
    }

    uint64_t total_completed = 0;
    bool callback_cancelled = false;
    for (uint64_t index = 0; index < items.size() && result == MZ_OK; ++index) {
        const ZipInput &item = items[index];
        mz_zip_file info{};
        info.version_madeby = MZ_VERSION_MADEBY;
        info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        info.filename = item.archive_path.c_str();
        info.uncompressed_size = static_cast<int64_t>(item.size);
        info.flag = MZ_ZIP_FLAG_UTF8;
        info.modified_date = std::time(nullptr);
        struct stat attributes{};
        if (stat(item.source.c_str(), &attributes) == 0) {
            info.modified_date = attributes.st_mtime;
            info.external_fa = static_cast<uint32_t>(attributes.st_mode) << 16;
        }
        if (effective.password && effective.password[0] && effective.use_aes)
            info.aes_version = MZ_AES_VERSION;
        result = mz_zip_writer_entry_open(writer, &info);
        if (result != MZ_OK)
            break;
        if (!item.directory) {
            FILE *source = std::fopen(item.source.c_str(), "rb");
            if (!source) {
                result = MZ_OPEN_ERROR;
            } else {
                uint8_t buffer[128 * 1024];
                uint64_t entry_completed = 0;
                while (result == MZ_OK) {
                    size_t read = std::fread(buffer, 1, sizeof(buffer), source);
                    if (read == 0) {
                        if (std::ferror(source))
                            result = MZ_READ_ERROR;
                        break;
                    }
                    int32_t written = mz_zip_writer_entry_write(writer, buffer, static_cast<int32_t>(read));
                    if (written != static_cast<int32_t>(read)) {
                        result = written < 0 ? written : MZ_WRITE_ERROR;
                        break;
                    }
                    entry_completed += read;
                    SAProgress state{sizeof(SAProgress), index, item.archive_path.c_str(), entry_completed,
                                     item.size, total_completed + entry_completed, total_size};
                    if (progress && progress(context, &state) == 0) {
                        callback_cancelled = true;
                        result = MZ_INTERNAL_ERROR;
                        break;
                    }
                }
                std::fclose(source);
            }
        }
        int32_t close_result = mz_zip_writer_entry_close(writer);
        if (result == MZ_OK && close_result != MZ_OK)
            result = close_result;
        total_completed += item.size;
    }
    int32_t close_result = mz_zip_writer_close(writer);
    mz_zip_writer_delete(&writer);
    if (callback_cancelled) {
        remove_zip_outputs(output_path, effective.volume_size > 0);
        return fail(error, SA_ERROR_CANCELLED, result, "Operation was cancelled");
    }
    if (result == MZ_OK && close_result != MZ_OK)
        result = close_result;
    if (result != MZ_OK) {
        remove_zip_outputs(output_path, effective.volume_size > 0);
        return fail(error, minizip_error_code(result, effective.password && effective.password[0]), result,
                    "Unable to create ZIP archive");
    }
    return 1;
}

const char *swiftarchive_version(void) {
    return "0.2.0";
}

} // extern "C"
