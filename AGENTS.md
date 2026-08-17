# SwiftArchive Contributor Guide

## Project Scope

SwiftArchive is an independent Swift package that provides a unified API for
reading archives, extracting archives, and creating ZIP files on Apple
platforms. It does not depend on a specific app and does not implement UI,
file picking, sandbox authorization, or user-facing overwrite confirmation.

The package has three layers:

- `SwiftArchive` provides the type-safe Swift API, async/await integration,
  progress reporting, and cancellation.
- `CSwiftArchiveCore` provides a C-compatible ABI boundary for validation,
  security policy, and backend dispatch. It can also be reused by bindings for
  CPython and other languages.
- `CMinizipNG` and `CUnRAR` contain pinned third-party native backends.

SwiftArchive can read and extract ZIP and RAR archives and can create ZIP
archives. It does not create RAR archives. Add support for new formats in
`CSwiftArchiveCore` without exposing backend-specific types through the public
Swift API. Keep the detailed feature matrix in `README.md` rather than
duplicating it here.

## Security Requirements

- Validate every output path before writing it. Reject absolute paths, drive
  letter paths, NUL bytes, and paths that escape through `..` components.
- Reject symbolic links, hard links, and other redirecting entries by default.
  Any change to this default requires dedicated security tests.
- Enforce entry count, per-entry size, total size, and compression ratio limits
  both before extraction and while data is being written.
- Propagate cancellation from Swift through the C ABI to the active backend.
  Cancelling only the awaiting Swift task is insufficient.
- Never write archive passwords to logs, error messages, or persistent files.

## Third-Party Code and Licensing

- Original SwiftArchive code is licensed under the MIT License. The root
  `LICENSE` file is authoritative.
- Vendored code and test fixtures are excluded from the MIT grant and remain
  under the licenses identified in `THIRD_PARTY_NOTICES.md` and their source
  directories.
- UnRAR permits use and distribution for processing RAR archives, but prohibits
  using its source to develop a RAR-compatible archiver or to recreate the
  proprietary RAR compression algorithm. Do not describe UnRAR as an
  OSI-approved open-source component or remove its required restriction text.
- When updating vendored code, update `THIRD_PARTY_NOTICES.md`, the original
  license file, version information, and relevant tests in the same change.
- Do not reformat vendored source without a functional reason. Document any
  required patch to third-party code in the change description.
- Record the source, version, and license of any new third-party code. Do not add
  dependencies whose terms conflict with the package's current distribution.

## API and Implementation Conventions

- Keep the C ABI limited to fixed-width integers, plain C strings, callbacks,
  and opaque pointers. Do not expose C++ standard library types through it.
- Retain `struct_size` in C structures. Append new fields only at the end so
  callers can negotiate structure versions safely.
- Use value types for public Swift configuration and entry models. Map backend
  failures to `ArchiveError`.
- Do not operate on the same archive handle concurrently. The Swift layer is
  responsible for serializing access.
- Native callbacks may arrive on background threads. Their context must remain
  alive for the complete native operation.
- Use UTF-8 for filenames and public text. Preserve non-ASCII test data needed
  to verify filename encoding behavior.

## Build and Verification

- Run `swift test` before submitting a change.
- Add focused regression tests for behavior changes. Relevant coverage includes
  unencrypted and encrypted ZIP files, non-ASCII filenames, wrong passwords,
  path traversal, resource limits, cancellation, and RAR variants.
- Before a release, build every platform declared in `Package.swift`. Run the
  full functional test suite on macOS and verify at least an arm64 iOS Simulator
  build.
- Measure performance changes in a release build with fixed input archives. Do
  not draw performance conclusions from debug builds.
- Do not commit `.build`, DerivedData, extraction output, credentials, personal
  data, or archive fixtures whose origin and redistribution terms are unknown.

## Change Hygiene

- Keep third-party version upgrades, public API redesigns, and unrelated
  formatting in separate changes.
