# SwiftArchive

SwiftArchive is an independent Swift package for working with ZIP and RAR
archives through a unified API. Its Swift interface and other language bindings
share the same C/C++ core. It does not depend on command-line tools, private
platform archive APIs, or a specific host application.

## Features

| Format | List | Stream entries | Extract | Create | Encryption | Multi-volume | Solid |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ZIP | Yes | Yes | Yes | Yes | ZipCrypto / WinZip AES | Yes | N/A |
| RAR | Yes | Yes | Yes | No | RAR 3/5 | Yes | Yes |

ZIP filenames follow the UTF-8 flag when present. Legacy archives support
automatic encoding detection and explicit CP437, CP932, CP936, and CP950
overrides. Newly created ZIP archives always use UTF-8 filenames.

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

Read one entry without writing it to disk:

```swift
let data = try await archive.data(for: archive.entries[0])

try await archive.read(archive.entries[0]) { chunk in
    consume(chunk)
    return true
}
```

Use `archive.extract(entry, to: destinationURL)` to extract one entry. For a
solid RAR archive, the backend processes preceding data internally but writes
only the requested entry to the destination.

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
regular single-file archive. SwiftArchive does not create RAR archives.

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

Build for an arm64 iOS Simulator before release:

```sh
xcodebuild -scheme SwiftArchive \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro' build
```

The test suite covers UTF-8 and CP936 filenames, ZipCrypto, WinZip AES, wrong
passwords, path traversal, resource limits, overwrite behavior, cancellation,
multi-volume ZIP, password-protected RAR, encrypted RAR headers, solid archives,
Unicode paths, and multi-volume RAR.
