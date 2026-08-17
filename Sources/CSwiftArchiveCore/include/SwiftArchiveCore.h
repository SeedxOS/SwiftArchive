#ifndef SWIFT_ARCHIVE_CORE_H
#define SWIFT_ARCHIVE_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SAArchive SAArchive;

typedef enum SAArchiveFormat {
    SA_ARCHIVE_FORMAT_AUTO = 0,
    SA_ARCHIVE_FORMAT_ZIP = 1,
    SA_ARCHIVE_FORMAT_RAR = 2,
} SAArchiveFormat;

typedef enum SAFilenameEncoding {
    SA_FILENAME_ENCODING_AUTO = 0,
    SA_FILENAME_ENCODING_UTF8 = 65001,
    SA_FILENAME_ENCODING_CP437 = 437,
    SA_FILENAME_ENCODING_CP932 = 932,
    SA_FILENAME_ENCODING_CP936 = 936,
    SA_FILENAME_ENCODING_CP950 = 950,
} SAFilenameEncoding;

typedef enum SAOverwritePolicy {
    SA_OVERWRITE_ERROR = 0,
    SA_OVERWRITE_SKIP = 1,
    SA_OVERWRITE_REPLACE = 2,
} SAOverwritePolicy;

typedef enum SAErrorCode {
    SA_ERROR_NONE = 0,
    SA_ERROR_INVALID_ARGUMENT = 1,
    SA_ERROR_UNSUPPORTED_FORMAT = 2,
    SA_ERROR_OPEN_FAILED = 3,
    SA_ERROR_CORRUPT_ARCHIVE = 4,
    SA_ERROR_PASSWORD_REQUIRED = 5,
    SA_ERROR_BAD_PASSWORD = 6,
    SA_ERROR_IO = 7,
    SA_ERROR_ENTRY_NOT_FOUND = 8,
    SA_ERROR_DESTINATION_EXISTS = 9,
    SA_ERROR_UNSAFE_PATH = 10,
    SA_ERROR_UNSAFE_LINK = 11,
    SA_ERROR_RESOURCE_LIMIT = 12,
    SA_ERROR_CANCELLED = 13,
    SA_ERROR_UNSUPPORTED_FEATURE = 14,
    SA_ERROR_INTERNAL = 15,
} SAErrorCode;

typedef enum SAEntryKind {
    SA_ENTRY_FILE = 0,
    SA_ENTRY_DIRECTORY = 1,
    SA_ENTRY_SYMBOLIC_LINK = 2,
    SA_ENTRY_HARD_LINK = 3,
    SA_ENTRY_OTHER_LINK = 4,
} SAEntryKind;

typedef struct SAError {
    uint32_t struct_size;
    SAErrorCode code;
    int32_t backend_code;
    char message[512];
} SAError;

typedef struct SAOpenOptions {
    uint32_t struct_size;
    const char *password;
    SAFilenameEncoding filename_encoding;
    uint64_t maximum_entry_count;
    uint64_t maximum_entry_size;
    uint64_t maximum_total_size;
    double maximum_compression_ratio;
} SAOpenOptions;

typedef struct SAExtractionOptions {
    uint32_t struct_size;
    SAOverwritePolicy overwrite_policy;
    uint8_t preserve_timestamps;
} SAExtractionOptions;

typedef struct SAZipCreateOptions {
    uint32_t struct_size;
    const char *password;
    int16_t compression_level;
    uint8_t use_aes;
    uint8_t include_parent_directory;
    uint64_t volume_size;
} SAZipCreateOptions;

typedef struct SAEntryInfo {
    uint32_t struct_size;
    const char *path;
    SAEntryKind kind;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    int64_t modification_time;
    uint32_t crc32;
    uint8_t encrypted;
    uint8_t solid;
    uint8_t split_before;
    uint8_t split_after;
} SAEntryInfo;

typedef struct SAProgress {
    uint32_t struct_size;
    uint64_t entry_index;
    const char *entry_path;
    uint64_t entry_completed;
    uint64_t entry_total;
    uint64_t total_completed;
    uint64_t total_size;
} SAProgress;

typedef int32_t (*SAProgressCallback)(void *context, const SAProgress *progress);
typedef int32_t (*SADataCallback)(void *context, const uint8_t *bytes, size_t length);

SAOpenOptions swiftarchive_open_options_default(void);
SAExtractionOptions swiftarchive_extraction_options_default(void);
SAZipCreateOptions swiftarchive_zip_create_options_default(void);

SAArchive *swiftarchive_archive_open(const char *path, SAArchiveFormat format,
                           const SAOpenOptions *options, SAError *error);
void swiftarchive_archive_close(SAArchive *archive);
SAArchiveFormat swiftarchive_archive_format(const SAArchive *archive);
uint64_t swiftarchive_archive_entry_count(const SAArchive *archive);
int32_t swiftarchive_archive_get_entry(const SAArchive *archive, uint64_t index,
                             SAEntryInfo *entry, SAError *error);
int32_t swiftarchive_archive_extract_all(SAArchive *archive, const char *destination,
                               const SAExtractionOptions *options,
                               SAProgressCallback progress, void *context,
                               SAError *error);
int32_t swiftarchive_archive_extract_entry(SAArchive *archive, uint64_t index,
                                 const char *destination,
                                 const SAExtractionOptions *options,
                                 SAProgressCallback progress, void *context,
                                 SAError *error);
int32_t swiftarchive_archive_read_entry(SAArchive *archive, uint64_t index,
                              SADataCallback callback, void *context,
                              SAError *error);
void swiftarchive_archive_cancel(SAArchive *archive);

int32_t swiftarchive_zip_create(const char *archive_path,
                      const char *const *source_paths,
                      const char *const *archive_paths,
                      size_t item_count,
                      const SAZipCreateOptions *options,
                      SAProgressCallback progress, void *context,
                      SAError *error);

const char *swiftarchive_version(void);

#ifdef __cplusplus
}
#endif

#endif
