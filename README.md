# SwiftArchive

SwiftArchive is an independent Swift package for working with common archive
and compression formats through a unified API. Its Swift interface and other language bindings
share the same C/C++ core. It does not depend on command-line tools, private
platform archive APIs, or a specific host application.

## Features

| Format | List | Stream entries | Extract | Create | Encryption | Multi-volume | Solid |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ZIP / ZIPX | Yes | Yes | Yes | Yes (ZIP) | ZipCrypto / WinZip AES | Yes | N/A |
| RAR | Yes | Yes | Yes | No | RAR 3/5 | Yes | Yes |
| TAR | Yes | Yes | Yes | No | N/A | No | N/A |
| GZIP | Yes (single stream) | Yes | Yes | No | N/A | No | N/A |
| 7z | Yes | Yes | Yes | No | 7z AES-256, including encrypted headers | Yes | Yes |
| BZIP2 / XZ / LZMA / LZIP / Zstandard / LZ4 / Unix Compress | Yes (single stream) | Yes | Yes | No | N/A | No | N/A |
| CAB / CPIO / ISO 9660 / LHA-LZH / AR / WARC | Yes | Yes | Yes | No | N/A | No | Format dependent |

ZIP filenames follow the UTF-8 flag when present. Legacy archives support
automatic encoding detection and explicit CP437, CP932, CP936, and CP950
overrides. Newly created ZIP archives always use UTF-8 filenames.
TAR supports POSIX ustar headers, PAX path/size/time metadata, and GNU long
names. GZIP exposes its decompressed stream as one regular-file entry and uses
the original filename from the header when it is safe and valid UTF-8.
Compressed TAR streams are detected by content and exposed as TAR archives.
ZIPX read and extraction support includes Store, Deflate, Deflate64, BZip2,
LZMA, PPMd, Zstandard, and XZ. Deflate64 entries can also use ZipCrypto or
WinZip AES encryption. Less common proprietary ZIP media codecs are reported
as unsupported instead of being treated as corrupt data.

7z archives are decoded by the LZMA SDK through PLzmaSDK. Content encryption,
encrypted headers, Unicode passwords, solid archives, and numbered
`.7z.001` volume sets are supported. Archive creation is intentionally limited
to ZIP; SwiftArchive does not create 7z or RAR archives.

SwiftArchive also provides:

- Format detection from archive contents instead of filename extensions,
  including ZIP files with prepended data
- async/await APIs
- Extraction of one entry or an entire archive
- Progress reporting and cancellation propagated to the active backend
- Configurable overwrite behavior
- Limits for entry count, per-entry size, total size, and compression ratio
- Protection against path traversal, absolute paths, and link entries
- A C-compatible library product named `CSwiftArchiveCore`

The package supports iOS 15, macOS 12, tvOS 15, and visionOS 1 or later.

## Installation

Add SwiftArchive as a dependency in another Swift package:

```swift
dependencies: [
    .package(
        url: "https://github.com/SeedxOS/SwiftArchive.git",
        branch: "main"
    )
]
```

Then add `.product(name: "SwiftArchive", package: "SwiftArchive")` to the
dependencies of the target that uses it.

## Reading and Extracting Archives

```swift
import SwiftArchive

let archive = try ArchiveReader(
    url: archiveURL,
    options: .init(
        password: "password",
        filenameEncoding: .automatic
    )
)

for entry in archive.entries {
    print(entry.path, entry.uncompressedSize)
}

try await archive.extract(
    to: destinationURL,
    options: .init(overwritePolicy: .replace)
) { progress in
    print(progress.fractionCompleted)
    return true
}
```

Pass all explicitly authorized parts when opening a split 7z archive. URLs may
be unordered; SwiftArchive resolves the first volume and verifies that the
numbered set is contiguous:

```swift
let archive = try ArchiveReader(
    urls: selectedVolumeURLs,
    options: .init(password: password)
)
```

When the archive parts are ordinary files in one accessible directory,
opening any `.7z.NNN` part also discovers its siblings. Sandboxed applications
should still pass every security-scoped URL explicitly because directory
enumeration does not grant access to sibling files.

Read one entry without writing it to disk:

```swift
let data = try await archive.data(for: archive.entries[0])

try await archive.read(archive.entries[0]) { chunk in
    consume(chunk)
    return true
}
```

Use `archive.extract(entry, to: destinationURL)` to extract one entry. For a
solid RAR or 7z archive, the backend processes the required solid block
internally but writes only the requested entry to the destination.

## Creating ZIP Archives

```swift
try await ZipArchive.create(
    at: outputURL,
    inputs: [
        ZipInput(sourceURL: folderURL),
        ZipInput(sourceURL: fileURL, pathInArchive: "docs/readme.txt")
    ],
    options: .init(
        password: "password",
        compressionLevel: 6,
        encryption: .aes256,
        includeParentDirectory: true
    )
)
```

Set `volumeSize` to create a multi-volume ZIP archive. A value of `0` creates a
regular single-file archive. SwiftArchive does not create 7z or RAR archives.

## Filename Encodings

`.automatic` first accepts names marked as UTF-8 or names that are valid UTF-8.
It then scores common legacy encodings to select the most likely result.
Automatic detection cannot eliminate every ambiguity in legacy ZIP archives,
so applications should let users retry with `.cp437`, `.cp932`, `.cp936`,
`.cp950`, or `.utf8` when necessary.

## Concurrency and Sandboxing

An `ArchiveReader` serializes its operations. Do not rely on one reader to
process multiple entries concurrently. Progress and data callbacks run on a
background thread, so UI updates must be dispatched to the main actor.

SwiftArchive operates only on URLs supplied by its caller. Security-scoped
resources, iCloud coordination, external folder access, temporary directory
lifetime, and user-facing overwrite confirmation are responsibilities of the
host application. Any access token must remain valid for the entire asynchronous
operation.

## License

Original SwiftArchive source code is available under the MIT License. See
[`LICENSE`](LICENSE).

SwiftArchive contains separately licensed third-party code:

- The ZIP backend is minizip-ng, licensed under the zlib license.
- The RAR backend is the official UnRAR source, distributed under its dedicated
  freeware source license.
- Additional read/extract support uses libarchive 3.8.9. Its optional filters
  are built from vendored XZ Utils/liblzma 5.8.3, bzip2 1.0.8, Zstandard 1.5.7,
  and LZ4 1.10.0 source.
- Encrypted and multi-volume 7z support uses PLzmaSDK 1.6.1 under the MIT
  license and its bundled LZMA SDK 26.01 code, which is in the public domain.
- Deflate64 decoding uses zlib's `contrib/infback9` code under the zlib
  license.
- Selected RAR test fixtures come from rarfile and are licensed under the ISC
  license.

UnRAR may be used free of charge to process RAR archives, but its license
prohibits using the source to develop a RAR-compatible archiver or to recreate
the proprietary RAR compression algorithm. The UnRAR license is not an
OSI-approved open-source license and is not replaced by SwiftArchive's MIT
license. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and the license
files in the vendored source directories for complete terms.

## Verification

Run the test suite on macOS:

```sh
swift test
```

Build for a generic iOS device before release:

```sh
xcodebuild -scheme SwiftArchive \
  -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO build
```

The test suite covers UTF-8 and CP936 filenames, ZipCrypto, WinZip AES, wrong
passwords, path traversal, resource limits, overwrite behavior, cancellation,
multi-volume ZIP, password-protected RAR, encrypted RAR headers, solid archives,
Unicode paths, multi-volume RAR, TAR listing/extraction/link rejection, and GZIP
streaming/footer validation. It also covers 7z and compressed TAR streams using
XZ, bzip2, Zstandard, and LZ4, including Unicode paths, cancellation, unsafe
paths, and resource limits. Dedicated fixtures cover encrypted 7z content and
headers, Unicode 7z passwords, solid and split 7z archives, missing volumes,
missing ZIP and RAR volumes, and ZIPX Deflate64, BZip2, LZMA, PPMd, Zstandard,
and XZ methods.
