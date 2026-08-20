# Third-Party Notices

SwiftArchive contains vendored source code from the following projects.

## minizip-ng 4.2.2

Copyright (C) Nathan Moinvaziri and contributors.

minizip-ng is distributed under the zlib license. The complete notice is kept at `Sources/CMinizipNG/LICENSE.minizip-ng`.

Upstream: https://github.com/zlib-ng/minizip-ng

## UnRAR 7.2.7

Copyright (C) Alexander Roshal.

UnRAR source code may be used in any software to handle RAR archives without limitations free of charge, but cannot be used to develop RAR (WinRAR) compatible archiver and to re-create RAR compression algorithm, which is proprietary. Distribution of modified UnRAR source code in separate form or as a part of other software is permitted, provided that full text of this paragraph, starting from "UnRAR source code" words, is included in license, or in documentation if license is not available, and in source code comments of resulting package.

The complete UnRAR license is kept at `Sources/CUnRAR/LICENSE.UnRAR`. It is not an OSI-approved open-source license.

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

## rarfile test fixtures

Selected RAR test fixtures under `Tests/SwiftArchiveTests/Fixtures` come from the rarfile project, copyright (c) 2005-2026 Marko Kreen. They are distributed under the ISC license; the complete notice is kept at `Tests/SwiftArchiveTests/Fixtures/LICENSE.rarfile`.

Upstream: https://github.com/markokr/rarfile
