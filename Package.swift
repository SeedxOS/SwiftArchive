// swift-tools-version: 6.0

import PackageDescription

let unrarSources = [
    "rar.cpp", "strlist.cpp", "strfn.cpp", "pathfn.cpp", "smallfn.cpp",
    "global.cpp", "file.cpp", "filefn.cpp", "filcreat.cpp", "archive.cpp",
    "arcread.cpp", "unicode.cpp", "system.cpp", "crypt.cpp", "crc.cpp",
    "rawread.cpp", "encname.cpp", "resource.cpp", "match.cpp", "timefn.cpp",
    "rdwrfn.cpp", "consio.cpp", "options.cpp", "errhnd.cpp", "rarvm.cpp",
    "secpassword.cpp", "rijndael.cpp", "getbits.cpp", "sha1.cpp", "sha256.cpp",
    "blake2s.cpp", "hash.cpp", "extinfo.cpp", "extract.cpp", "volume.cpp",
    "list.cpp", "find.cpp", "unpack.cpp", "headers.cpp", "threadpool.cpp",
    "rs16.cpp", "cmddata.cpp", "ui.cpp", "largepage.cpp", "filestr.cpp",
    "scantree.cpp", "dll.cpp", "qopen.cpp",
]

let liblzmaSources = [
    "src/common/tuklib_physmem.c",
    "src/liblzma/check/check.c", "src/liblzma/check/crc32_fast.c",
    "src/liblzma/check/crc64_fast.c", "src/liblzma/check/sha256.c",
    "src/liblzma/common/alone_decoder.c", "src/liblzma/common/auto_decoder.c",
    "src/liblzma/common/block_buffer_decoder.c", "src/liblzma/common/block_decoder.c",
    "src/liblzma/common/block_header_decoder.c", "src/liblzma/common/block_util.c",
    "src/liblzma/common/common.c", "src/liblzma/common/easy_decoder_memusage.c",
    "src/liblzma/common/easy_preset.c", "src/liblzma/common/file_info.c",
    "src/liblzma/common/filter_buffer_decoder.c", "src/liblzma/common/filter_common.c",
    "src/liblzma/common/filter_decoder.c", "src/liblzma/common/filter_flags_decoder.c",
    "src/liblzma/common/hardware_physmem.c", "src/liblzma/common/index.c",
    "src/liblzma/common/index_decoder.c", "src/liblzma/common/index_hash.c",
    "src/liblzma/common/lzip_decoder.c", "src/liblzma/common/stream_buffer_decoder.c",
    "src/liblzma/common/stream_decoder.c", "src/liblzma/common/stream_flags_common.c",
    "src/liblzma/common/stream_flags_decoder.c", "src/liblzma/common/string_conversion.c",
    "src/liblzma/common/vli_decoder.c", "src/liblzma/common/vli_size.c",
    "src/liblzma/delta/delta_common.c", "src/liblzma/delta/delta_decoder.c",
    "src/liblzma/lz/lz_decoder.c", "src/liblzma/lzma/lzma2_decoder.c",
    "src/liblzma/lzma/lzma_decoder.c", "src/liblzma/lzma/lzma_encoder_presets.c",
    "src/liblzma/simple/arm.c", "src/liblzma/simple/arm64.c",
    "src/liblzma/simple/armthumb.c", "src/liblzma/simple/ia64.c",
    "src/liblzma/simple/powerpc.c", "src/liblzma/simple/riscv.c",
    "src/liblzma/simple/simple_coder.c", "src/liblzma/simple/simple_decoder.c",
    "src/liblzma/simple/sparc.c", "src/liblzma/simple/x86.c",
]

let libarchiveSources = [
    "archive_acl.c", "archive_check_magic.c", "archive_cryptor.c",
    "archive_digest.c", "archive_entry.c", "archive_entry_copy_stat.c",
    "archive_entry_link_resolver.c", "archive_entry_sparse.c", "archive_entry_stat.c",
    "archive_entry_strmode.c", "archive_entry_xattr.c", "archive_hmac.c",
    "archive_options.c", "archive_pack_dev.c", "archive_parse_date.c",
    "archive_pathmatch.c", "archive_ppmd7.c", "archive_ppmd8.c",
    "archive_random.c", "archive_rb.c", "archive_read.c",
    "archive_read_add_passphrase.c", "archive_read_open_fd.c",
    "archive_read_open_file.c", "archive_read_open_filename.c",
    "archive_read_open_memory.c", "archive_read_set_options.c",
    "archive_read_support_filter_bzip2.c",
    "archive_read_support_filter_compress.c", "archive_read_support_filter_gzip.c",
    "archive_read_support_filter_lz4.c", "archive_read_support_filter_none.c",
    "archive_read_support_filter_rpm.c", "archive_read_support_filter_uu.c",
    "archive_read_support_filter_xz.c", "archive_read_support_filter_zstd.c",
    "archive_read_support_format_7zip.c", "archive_read_support_format_ar.c",
    "archive_read_support_format_cab.c", "archive_read_support_format_cpio.c",
    "archive_read_support_format_empty.c", "archive_read_support_format_iso9660.c",
    "archive_read_support_format_lha.c", "archive_read_support_format_raw.c",
    "archive_read_support_format_tar.c", "archive_read_support_format_warc.c",
    "archive_read_support_format_zip.c", "archive_string.c",
    "archive_string_sprintf.c", "archive_time.c", "archive_util.c",
    "archive_version_details.c", "archive_virtual.c", "xxhash.c",
]

let package = Package(
    name: "SwiftArchive",
    platforms: [
        .iOS(.v15),
        .macOS(.v12),
        .tvOS(.v15),
        .visionOS(.v1),
    ],
    products: [
        .library(name: "SwiftArchive", targets: ["SwiftArchive"]),
        .library(name: "CSwiftArchiveCore", targets: ["CSwiftArchiveCore"]),
    ],
    targets: [
        .target(
            name: "CLibLZMA",
            path: "Sources/CLibLZMA",
            sources: liblzmaSources,
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("src/liblzma/api"),
                .headerSearchPath("src/common"),
                .headerSearchPath("src/liblzma/check"),
                .headerSearchPath("src/liblzma/common"),
                .headerSearchPath("src/liblzma/delta"),
                .headerSearchPath("src/liblzma/lz"),
                .headerSearchPath("src/liblzma/lzma"),
                .headerSearchPath("src/liblzma/rangecoder"),
                .headerSearchPath("src/liblzma/simple"),
                .define("HAVE_CHECK_CRC32"),
                .define("HAVE_CHECK_CRC64"),
                .define("HAVE_CHECK_SHA256"),
                .define("HAVE_DECODERS"),
                .define("HAVE_DECODER_ARM"),
                .define("HAVE_DECODER_ARM64"),
                .define("HAVE_DECODER_ARMTHUMB"),
                .define("HAVE_DECODER_DELTA"),
                .define("HAVE_DECODER_IA64"),
                .define("HAVE_DECODER_LZMA1"),
                .define("HAVE_DECODER_LZMA2"),
                .define("HAVE_DECODER_POWERPC"),
                .define("HAVE_DECODER_RISCV"),
                .define("HAVE_DECODER_SPARC"),
                .define("HAVE_DECODER_X86"),
                .define("HAVE_INTTYPES_H"),
                .define("HAVE_LZIP_DECODER"),
                .define("HAVE_STDBOOL_H"),
                .define("HAVE_STDINT_H"),
                .define("HAVE__BOOL"),
                .define("TUKLIB_SYMBOL_PREFIX", to: "lzma_"),
            ]
        ),
        .target(
            name: "CBZip2",
            path: "Sources/CBZip2",
            sources: [
                "blocksort.c", "bzlib.c", "compress.c", "crctable.c",
                "decompress.c", "huffman.c", "randtable.c",
            ],
            publicHeadersPath: "include"
        ),
        .target(
            name: "CZstd",
            path: "Sources/CZstd",
            sources: [
                "common/debug.c", "common/entropy_common.c", "common/error_private.c",
                "common/fse_decompress.c", "common/xxhash.c", "common/zstd_common.c",
                "decompress/huf_decompress.c", "decompress/zstd_ddict.c",
                "decompress/zstd_decompress.c", "decompress/zstd_decompress_block.c",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("common"),
                .headerSearchPath("decompress"),
                .define("XXH_NAMESPACE", to: "ZSTD_"),
                .define("ZSTD_DISABLE_ASM"),
                .define("ZSTD_STATIC_LINKING_ONLY"),
            ]
        ),
        .target(
            name: "CLZ4",
            path: "Sources/CLZ4",
            sources: ["lz4.c", "lz4frame.c", "lz4hc.c", "xxhash.c"],
            publicHeadersPath: "include"
        ),
        .target(
            name: "CLibArchive",
            dependencies: ["CLibLZMA", "CBZip2", "CZstd", "CLZ4"],
            path: "Sources/CLibArchive",
            sources: libarchiveSources,
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("."),
                .define("HAVE_CONFIG_H"),
            ],
            linkerSettings: [
                .linkedLibrary("z"),
                .linkedFramework("CoreFoundation"),
                .linkedFramework("Security"),
            ]
        ),
        .target(
            name: "CMinizipNG",
            path: "Sources/CMinizipNG",
            sources: [
                "mz_crypt.c", "mz_crypt_apple.c", "mz_os.c", "mz_os_posix.c",
                "mz_strm.c", "mz_strm_buf.c", "mz_strm_mem.c",
                "mz_strm_os_posix.c", "mz_strm_pkcrypt.c", "mz_strm_split.c",
                "mz_strm_wzaes.c", "mz_strm_zlib.c", "mz_zip.c", "mz_zip_rw.c",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("."),
                .define("HAVE_ARC4RANDOM_BUF"),
                .define("HAVE_ICONV"),
                .define("HAVE_INTTYPES_H"),
                .define("HAVE_PKCRYPT"),
                .define("HAVE_STDINT_H"),
                .define("HAVE_WZAES"),
                .define("HAVE_ZLIB"),
                .define("ZLIB_COMPAT"),
                .define("_DARWIN_C_SOURCE"),
            ],
            linkerSettings: [
                .linkedLibrary("z"),
                .linkedLibrary("iconv"),
                .linkedFramework("CoreFoundation"),
                .linkedFramework("Security"),
            ]
        ),
        .target(
            name: "CUnRAR",
            path: "Sources/CUnRAR",
            sources: unrarSources,
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("."),
                .define("RARDLL"),
                .define("RAR_SMP"),
                .define("_FILE_OFFSET_BITS", to: "64"),
                .define("_LARGEFILE_SOURCE"),
            ]
        ),
        .target(
            name: "CSwiftArchiveCore",
            dependencies: ["CMinizipNG", "CUnRAR", "CLibArchive"],
            path: "Sources/CSwiftArchiveCore",
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("../CMinizipNG"),
                .define("_UNIX"),
            ],
            linkerSettings: [
                .linkedLibrary("z"),
            ]
        ),
        .target(
            name: "SwiftArchive",
            dependencies: ["CSwiftArchiveCore"],
            path: "Sources/SwiftArchive",
            resources: [.copy("Resources/PrivacyInfo.xcprivacy")]
        ),
        .testTarget(
            name: "SwiftArchiveTests",
            dependencies: ["SwiftArchive"],
            path: "Tests/SwiftArchiveTests",
            resources: [.copy("Fixtures")]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
