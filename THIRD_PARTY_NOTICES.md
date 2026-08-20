# Third-Party Notices

SwiftArchive contains vendored source code from the following projects.

## minizip-ng 4.2.2

Copyright (C) Nathan Moinvaziri and contributors.

minizip-ng is distributed under the zlib license. The complete notice is kept at `Sources/CMinizipNG/LICENSE.minizip-ng`.

SwiftArchive alters minizip-ng's raw-entry mode so compressed bytes from
methods handled by an external decoder, currently Deflate64, can still pass
through minizip-ng's ZipCrypto and WinZip AES decryption streams. Normal
decoded-entry behavior is unchanged. The alteration is in
`Sources/CMinizipNG/mz_zip.c`.

Upstream: https://github.com/zlib-ng/minizip-ng

## UnRAR 7.2.7

Copyright (C) Alexander Roshal.

UnRAR source code may be used in any software to handle RAR archives without limitations free of charge, but cannot be used to develop RAR (WinRAR) compatible archiver and to re-create RAR compression algorithm, which is proprietary. Distribution of modified UnRAR source code in separate form or as a part of other software is permitted, provided that full text of this paragraph, starting from "UnRAR source code" words, is included in license, or in documentation if license is not available, and in source code comments of resulting package.

The complete UnRAR license is kept at `Sources/CUnRAR/LICENSE.UnRAR`. It is not an OSI-approved open-source license.

SwiftArchive's non-interactive UnRAR callback aborts an unsuccessful automatic
volume change instead of retrying the same inaccessible path indefinitely. The
result is exposed to callers as a missing-volume error so a host application
can request access to the remaining archive parts.

Upstream: https://www.rarlab.com/rar_add.htm

## libarchive 3.8.9

Copyright (c) 2003-2018 the libarchive authors.

libarchive is distributed under a two-clause BSD license, with file-specific
notices described by the upstream distribution. The complete distribution
notice is kept at `Sources/CLibArchive/LICENSE.libarchive`.

SwiftArchive changes libarchive's Apple default archive charset to UTF-8 so
filename decoding does not depend on mutable process-wide C locale state. This
is an altered source version; the change is in
`Sources/CLibArchive/archive_string.c`.

SwiftArchive also removes an unconditional diagnostic from
`Sources/CLibArchive/archive_read_support_format_zip.c`. The upstream code
guards it with `DEBUG`, but Swift package Debug builds define that symbol for
the whole target and would otherwise write archive metadata to stderr.

Upstream: https://github.com/libarchive/libarchive

## XZ Utils / liblzma 5.8.3

Copyright (C) The XZ Utils authors and contributors.

Only liblzma and its required common source are compiled. That code is under
the BSD Zero Clause License (0BSD). Complete notices are kept at
`Sources/CLibLZMA/COPYING` and `Sources/CLibLZMA/COPYING.0BSD`.

Upstream: https://github.com/tukaani-project/xz

## bzip2 / libbzip2 1.0.8

Copyright (C) 1996-2019 Julian R Seward.

libbzip2 is distributed under its permissive BSD-style license. The complete
notice is kept at `Sources/CBZip2/LICENSE.bzip2`.

Upstream: https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz

## Zstandard 1.5.7

Copyright (c) Meta Platforms, Inc. and affiliates.

The Zstandard library is distributed under the three-clause BSD license. The
complete notice is kept at `Sources/CZstd/LICENSE.zstd`.

Two internal headers contain a no-op preprocessor guard around
`ZSTD_STATIC_LINKING_ONLY` to support command-line configuration under Clang
modules. This does not change the library behavior.

Upstream: https://github.com/facebook/zstd

## LZ4 1.10.0

Copyright (c) 2011-2020 Yann Collet.

The LZ4 library is distributed under the two-clause BSD license. The complete
notice is kept at `Sources/CLZ4/LICENSE`.

Upstream: https://github.com/lz4/lz4

## PLzmaSDK 1.6.1 and LZMA SDK 26.01

Copyright (c) 2015-2026 Oleh Kulykov.

PLzmaSDK is distributed under the MIT License. Its bundled LZMA SDK source is
placed in the public domain by Igor Pavlov. The complete PLzmaSDK notice is
kept at `Vendor/PLzmaSDK/LICENSE` and the exact upstream revision is recorded
in `Vendor/PLzmaSDK/UPSTREAM_VERSION`.

SwiftArchive vendors PLzmaSDK commit
`f449bc3e13204b68a7e05fca80ce8c31642085ec` and makes the following changes:

- removes package-level unsafe `-fPIC` and `-fno-rtti` flags so the package can
  be consumed as a dependency by another Swift package;
- exposes archive solid-state and item platform attributes needed for safe
  entry classification;
- reports required and incorrect passwords, unsupported methods, CRC errors,
  unexpected endings, and corrupt data distinctly; and
- converts non-BMP password scalars to UTF-16 surrogate pairs before passing
  them to the LZMA SDK on platforms where `wchar_t` is 32-bit.

Upstream: https://github.com/OlehKulykov/PLzmaSDK

LZMA SDK license information: https://www.7-zip.org/sdk.html

## zlib contrib/infback9

Copyright (C) 1995-2026 Jean-loup Gailly and Mark Adler.

The Deflate64 decoder comes from zlib's `contrib/infback9` directory at commit
`e3dc0a85b7032e98380dec011bc8f2c2ee0d8fca` and is distributed under the zlib
license. The complete notice and upstream revision are kept at
`Sources/CSwiftArchiveCore/Deflate64/LICENSE.zlib` and
`Sources/CSwiftArchiveCore/Deflate64/UPSTREAM_VERSION`.

SwiftArchive marks this as an altered source version: `infback9.c` uses local
`calloc` and `free` callbacks instead of zlib's private, non-public allocation
symbols.

Upstream: https://github.com/madler/zlib/tree/develop/contrib/infback9

## rarfile test fixtures

Selected RAR test fixtures under `Tests/SwiftArchiveTests/Fixtures` come from the rarfile project, copyright (c) 2005-2026 Marko Kreen. They are distributed under the ISC license; the complete notice is kept at `Tests/SwiftArchiveTests/Fixtures/LICENSE.rarfile`.

Upstream: https://github.com/markokr/rarfile

## libarchive ZIPX test fixtures

`zipx-zstd.zipx` and `zipx-xz.zipx` under
`Tests/SwiftArchiveTests/Fixtures` are decoded copies of libarchive's upstream
test fixtures at commit `df40011ec353e38557e1ec5e1d45b4c2d368ad77`:

- `libarchive/test/test_read_format_zip_zstd.zipx.uu`
- `libarchive/test/test_read_format_zip_xz_multi.zipx.uu`

They remain subject to libarchive's distribution notices. The complete notice
is kept at `Sources/CLibArchive/LICENSE.libarchive`; exact source details and
checksums are recorded in `Tests/SwiftArchiveTests/Fixtures/FIXTURES.md`.
