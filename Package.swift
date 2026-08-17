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
            dependencies: ["CMinizipNG", "CUnRAR"],
            path: "Sources/CSwiftArchiveCore",
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("../CMinizipNG"),
                .define("_UNIX"),
            ]
        ),
        .target(
            name: "SwiftArchive",
            dependencies: ["CSwiftArchiveCore"],
            path: "Sources/SwiftArchive"
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
