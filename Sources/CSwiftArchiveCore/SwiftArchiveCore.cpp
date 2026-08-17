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
#include "mz.h"
#include "mz_os.h"
#include "mz_strm.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
    if (input.empty() || input.find('\0') != std::string::npos)
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
        uint8_t magic[8]{};
        size_t count = std::fread(magic, 1, sizeof(magic), file);
        std::fclose(file);
        if (count >= 4 && magic[0] == 'P' && magic[1] == 'K')
            format = SA_ARCHIVE_FORMAT_ZIP;
        else if (count >= 7 && std::memcmp(magic, "Rar!\x1a\x07", 6) == 0)
            format = SA_ARCHIVE_FORMAT_RAR;
        else {
            SAError local_error{};
            SAError *attempt_error = error ? error : &local_error;
            auto try_format = [&](SAArchiveFormat candidate) {
                clear_error(attempt_error);
                archive->entries.clear();
                archive->format = candidate;
                return candidate == SA_ARCHIVE_FORMAT_ZIP ? list_zip(archive, attempt_error)
                                                          : list_rar(archive, attempt_error);
            };
            auto is_conclusive_error = [](SAErrorCode code) {
                return code != SA_ERROR_UNSUPPORTED_FORMAT && code != SA_ERROR_OPEN_FAILED &&
                       code != SA_ERROR_CORRUPT_ARCHIVE;
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

            delete archive;
            fail(error, SA_ERROR_UNSUPPORTED_FORMAT, 0, "Archive format is not supported");
            return nullptr;
        }
    }
    archive->format = format;
    int32_t success = format == SA_ARCHIVE_FORMAT_ZIP ? list_zip(archive, error)
                                                       : format == SA_ARCHIVE_FORMAT_RAR ? list_rar(archive, error)
                                                                                         : 0;
    if (!success) {
        if (format != SA_ARCHIVE_FORMAT_ZIP && format != SA_ARCHIVE_FORMAT_RAR)
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
    if (archive->format == SA_ARCHIVE_FORMAT_ZIP) {
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
    if (archive->format == SA_ARCHIVE_FORMAT_ZIP)
        return extract_zip(archive, root, effective, progress, context, error);
    return extract_rar(archive, root, effective, progress, context, error);
}

int32_t swiftarchive_archive_extract_entry(SAArchive *archive, uint64_t index,
                                 const char *destination,
                                 const SAExtractionOptions *options,
                                 SAProgressCallback progress, void *context,
                                 SAError *error) {
    clear_error(error);
    if (!archive || index >= archive->entries.size() || !destination || destination[0] == 0)
        return fail(error, SA_ERROR_INVALID_ARGUMENT, 0, "Invalid entry extraction request");
    if (archive->format == SA_ARCHIVE_FORMAT_ZIP &&
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
    if (archive->format == SA_ARCHIVE_FORMAT_ZIP)
        return extract_one_zip(archive, index, root, effective, progress, context, error);
    return extract_one_rar(archive, index, root, effective, progress, context, error);
}

int32_t swiftarchive_archive_read_entry(SAArchive *archive, uint64_t index,
                              SADataCallback callback, void *context,
                              SAError *error) {
    clear_error(error);
    if (!archive || !callback || index >= archive->entries.size())
        return fail(error, SA_ERROR_INVALID_ARGUMENT, 0, "Invalid entry read request");
    if (archive->entries[index].kind != SA_ENTRY_FILE)
        return fail(error, SA_ERROR_UNSUPPORTED_FEATURE, 0, "Only regular files can be read");
    if (archive->format == SA_ARCHIVE_FORMAT_ZIP &&
        !require_zip_password(archive, archive->entries[index], error))
        return 0;
    archive->cancelled.store(false);
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
    if (result == MZ_INTERNAL_ERROR) {
        std::remove(archive_path);
        return fail(error, SA_ERROR_CANCELLED, result, "Operation was cancelled");
    }
    if (result == MZ_OK && close_result != MZ_OK)
        result = close_result;
    if (result != MZ_OK) {
        std::remove(archive_path);
        return fail(error, minizip_error_code(result, effective.password && effective.password[0]), result,
                    "Unable to create ZIP archive");
    }
    return 1;
}

const char *swiftarchive_version(void) {
    return "0.1.0";
}

} // extern "C"
