import CSwiftArchiveCore
import Foundation

final class ProgressContext: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false
    let handler: (@Sendable (ArchiveProgress) -> Bool)?

    init(handler: (@Sendable (ArchiveProgress) -> Bool)?) {
        self.handler = handler
    }

    func cancel() {
        lock.lock()
        cancelled = true
        lock.unlock()
    }

    var isCancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return cancelled
    }

    func shouldContinue(_ value: ArchiveProgress) -> Bool {
        lock.lock()
        let wasCancelled = cancelled
        lock.unlock()
        return !wasCancelled && (handler?(value) ?? true)
    }

    func reportCompletion(totalSize: UInt64) {
        _ = shouldContinue(ArchiveProgress(
            entryIndex: 0,
            entryPath: "",
            entryCompleted: totalSize,
            entryTotal: totalSize,
            totalCompleted: totalSize,
            totalSize: totalSize
        ))
    }
}

final class DataContext: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false
    var data = Data()
    let consumer: (@Sendable (Data) -> Bool)?

    init(consumer: (@Sendable (Data) -> Bool)?) {
        self.consumer = consumer
    }

    func cancel() {
        lock.lock()
        cancelled = true
        lock.unlock()
    }

    var isCancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return cancelled
    }

    func consume(_ bytes: UnsafePointer<UInt8>, count: Int) -> Bool {
        lock.lock()
        let wasCancelled = cancelled
        lock.unlock()
        guard !wasCancelled else { return false }
        let chunk = Data(bytes: bytes, count: count)
        if let consumer {
            return consumer(chunk)
        }
        data.append(chunk)
        return true
    }

    func consume(_ chunk: Data) -> Bool {
        guard !chunk.isEmpty else { return true }
        return chunk.withUnsafeBytes { bytes in
            guard let baseAddress = bytes.bindMemory(to: UInt8.self).baseAddress else { return true }
            return consume(baseAddress, count: chunk.count)
        }
    }
}

private let archiveProgressCallback: SAProgressCallback = { context, progress in
    guard let context, let progress else { return 0 }
    let box = Unmanaged<ProgressContext>.fromOpaque(context).takeUnretainedValue()
    let value = ArchiveProgress(
        entryIndex: Int(progress.pointee.entry_index),
        entryPath: progress.pointee.entry_path.map(String.init(cString:)) ?? "",
        entryCompleted: progress.pointee.entry_completed,
        entryTotal: progress.pointee.entry_total,
        totalCompleted: progress.pointee.total_completed,
        totalSize: progress.pointee.total_size
    )
    return box.shouldContinue(value) ? 1 : 0
}

private let archiveDataCallback: SADataCallback = { context, bytes, length in
    guard let context, let bytes else { return 0 }
    let box = Unmanaged<DataContext>.fromOpaque(context).takeUnretainedValue()
    return box.consume(bytes, count: length) ? 1 : 0
}

public final class ArchiveReader: @unchecked Sendable {
    public let url: URL
    public let format: ArchiveFormat
    public let entries: [ArchiveEntry]

    private let handle: OpaquePointer?
    private let sevenZipBackend: SevenZipBackend?
    private let operationLock = NSLock()

    public convenience init(
        url: URL,
        format requestedFormat: ArchiveFormat? = nil,
        options: ArchiveOpenOptions = .init()
    ) throws {
        try self.init(urls: [url], format: requestedFormat, options: options)
    }

    public init(
        urls: [URL],
        format requestedFormat: ArchiveFormat? = nil,
        options: ArchiveOpenOptions = .init()
    ) throws {
        var seenPaths = Set<String>()
        let urls = urls.filter { seenPaths.insert($0.standardizedFileURL.path).inserted }
        guard let url = urls.first else {
            throw ArchiveError(
                code: .invalidArgument,
                backendCode: 0,
                message: "At least one archive URL is required"
            )
        }
        if SevenZipBackend.shouldHandle(urls: urls, requestedFormat: requestedFormat) {
            let backend = try SevenZipBackend(
                urls: urls,
                password: options.password,
                limits: options.limits
            )
            self.url = backend.primaryURL
            format = .sevenZip
            entries = backend.entries
            handle = nil
            sevenZipBackend = backend
            return
        }

        sevenZipBackend = nil
        var error = SAError()
        var nativeOptions = swiftarchive_open_options_default()
        nativeOptions.filename_encoding = SAFilenameEncoding(rawValue: UInt32(options.filenameEncoding.rawValue))
        nativeOptions.maximum_entry_count = options.limits.maximumEntryCount
        nativeOptions.maximum_entry_size = options.limits.maximumEntrySize
        nativeOptions.maximum_total_size = options.limits.maximumTotalSize
        nativeOptions.maximum_compression_ratio = options.limits.maximumCompressionRatio

        let opened: OpaquePointer? = Self.withOptionalCString(options.password) { password in
            nativeOptions.password = password
            return url.path.withCString { path in
                let nativeFormat: SAArchiveFormat
                switch requestedFormat {
                case .zip: nativeFormat = SA_ARCHIVE_FORMAT_ZIP
                case .rar: nativeFormat = SA_ARCHIVE_FORMAT_RAR
                case .tar: nativeFormat = SA_ARCHIVE_FORMAT_TAR
                case .gzip: nativeFormat = SA_ARCHIVE_FORMAT_GZIP
                case .sevenZip: nativeFormat = SA_ARCHIVE_FORMAT_7ZIP
                case .bzip2: nativeFormat = SA_ARCHIVE_FORMAT_BZIP2
                case .xz: nativeFormat = SA_ARCHIVE_FORMAT_XZ
                case .lzma: nativeFormat = SA_ARCHIVE_FORMAT_LZMA
                case .lzip: nativeFormat = SA_ARCHIVE_FORMAT_LZIP
                case .compress: nativeFormat = SA_ARCHIVE_FORMAT_COMPRESS
                case .zstandard: nativeFormat = SA_ARCHIVE_FORMAT_ZSTD
                case .lz4: nativeFormat = SA_ARCHIVE_FORMAT_LZ4
                case .cab: nativeFormat = SA_ARCHIVE_FORMAT_CAB
                case .cpio: nativeFormat = SA_ARCHIVE_FORMAT_CPIO
                case .iso9660: nativeFormat = SA_ARCHIVE_FORMAT_ISO9660
                case .lha: nativeFormat = SA_ARCHIVE_FORMAT_LHA
                case .ar: nativeFormat = SA_ARCHIVE_FORMAT_AR
                case .warc: nativeFormat = SA_ARCHIVE_FORMAT_WARC
                case nil: nativeFormat = SA_ARCHIVE_FORMAT_AUTO
                }
                return swiftarchive_archive_open(path, nativeFormat, &nativeOptions, &error)
            }
        }
        guard let opened else { throw Self.makeError(error) }
        handle = opened
        self.url = url

        switch swiftarchive_archive_format(opened) {
        case SA_ARCHIVE_FORMAT_ZIP:
            format = .zip
        case SA_ARCHIVE_FORMAT_RAR:
            format = .rar
        case SA_ARCHIVE_FORMAT_TAR:
            format = .tar
        case SA_ARCHIVE_FORMAT_GZIP:
            format = .gzip
        case SA_ARCHIVE_FORMAT_7ZIP:
            format = .sevenZip
        case SA_ARCHIVE_FORMAT_BZIP2:
            format = .bzip2
        case SA_ARCHIVE_FORMAT_XZ:
            format = .xz
        case SA_ARCHIVE_FORMAT_LZMA:
            format = .lzma
        case SA_ARCHIVE_FORMAT_LZIP:
            format = .lzip
        case SA_ARCHIVE_FORMAT_COMPRESS:
            format = .compress
        case SA_ARCHIVE_FORMAT_ZSTD:
            format = .zstandard
        case SA_ARCHIVE_FORMAT_LZ4:
            format = .lz4
        case SA_ARCHIVE_FORMAT_CAB:
            format = .cab
        case SA_ARCHIVE_FORMAT_CPIO:
            format = .cpio
        case SA_ARCHIVE_FORMAT_ISO9660:
            format = .iso9660
        case SA_ARCHIVE_FORMAT_LHA:
            format = .lha
        case SA_ARCHIVE_FORMAT_AR:
            format = .ar
        case SA_ARCHIVE_FORMAT_WARC:
            format = .warc
        default:
            swiftarchive_archive_close(opened)
            throw ArchiveError(code: .unsupportedFormat, backendCode: 0, message: "Unsupported archive format")
        }

        var loadedEntries: [ArchiveEntry] = []
        loadedEntries.reserveCapacity(Int(swiftarchive_archive_entry_count(opened)))
        for index in 0..<swiftarchive_archive_entry_count(opened) {
            var info = SAEntryInfo()
            guard swiftarchive_archive_get_entry(opened, index, &info, &error) != 0 else {
                swiftarchive_archive_close(opened)
                throw Self.makeError(error)
            }
            loadedEntries.append(Self.makeEntry(info, index: Int(index)))
        }
        entries = loadedEntries
    }

    deinit {
        if let handle {
            swiftarchive_archive_close(handle)
        }
    }

    public func cancel() {
        if let sevenZipBackend {
            sevenZipBackend.cancel()
        } else if let handle {
            swiftarchive_archive_cancel(handle)
        }
    }

    public func extract(
        to destinationURL: URL,
        options: ArchiveExtractionOptions = .init(),
        progress: (@Sendable (ArchiveProgress) -> Bool)? = nil
    ) async throws {
        let context = ProgressContext(handler: progress)
        try await withTaskCancellationHandler {
            try await Task.detached {
                try self.extractSynchronously(to: destinationURL, options: options, context: context)
            }.value
        } onCancel: {
            context.cancel()
            self.cancel()
        }
    }

    public func extract(
        _ entry: ArchiveEntry,
        to destinationURL: URL,
        options: ArchiveExtractionOptions = .init(),
        progress: (@Sendable (ArchiveProgress) -> Bool)? = nil
    ) async throws {
        let context = ProgressContext(handler: progress)
        try await withTaskCancellationHandler {
            try await Task.detached {
                try self.extractSynchronously(
                    entry: entry,
                    to: destinationURL,
                    options: options,
                    context: context
                )
            }.value
        } onCancel: {
            context.cancel()
            self.cancel()
        }
    }

    public func data(for entry: ArchiveEntry) async throws -> Data {
        let context = DataContext(consumer: nil)
        try await withTaskCancellationHandler {
            try await Task.detached {
                try self.readSynchronously(entry: entry, context: context)
            }.value
        } onCancel: {
            context.cancel()
            self.cancel()
        }
        return context.data
    }

    public func read(
        _ entry: ArchiveEntry,
        consumer: @escaping @Sendable (Data) -> Bool
    ) async throws {
        let context = DataContext(consumer: consumer)
        try await withTaskCancellationHandler {
            try await Task.detached {
                try self.readSynchronously(entry: entry, context: context)
            }.value
        } onCancel: {
            context.cancel()
            self.cancel()
        }
    }

    private func extractSynchronously(
        to destinationURL: URL,
        options: ArchiveExtractionOptions,
        context: ProgressContext
    ) throws {
        operationLock.lock()
        defer { operationLock.unlock() }
        if let sevenZipBackend {
            try sevenZipBackend.extract(to: destinationURL, options: options, context: context)
            return
        }
        guard let handle else {
            throw ArchiveError(code: .internal, backendCode: 0, message: "Archive backend is unavailable")
        }
        var error = SAError()
        var nativeOptions = swiftarchive_extraction_options_default()
        switch options.overwritePolicy {
        case .error: nativeOptions.overwrite_policy = SA_OVERWRITE_ERROR
        case .skip: nativeOptions.overwrite_policy = SA_OVERWRITE_SKIP
        case .replace: nativeOptions.overwrite_policy = SA_OVERWRITE_REPLACE
        }
        nativeOptions.preserve_timestamps = options.preserveTimestamps ? 1 : 0
        let opaque = Unmanaged.passUnretained(context).toOpaque()
        let success = destinationURL.path.withCString { destination in
            swiftarchive_archive_extract_all(
                handle,
                destination,
                &nativeOptions,
                archiveProgressCallback,
                opaque,
                &error
            )
        }
        guard success != 0 else { throw Self.makeError(error) }
    }

    private func readSynchronously(entry: ArchiveEntry, context: DataContext) throws {
        guard entry.index >= 0, entry.index < entries.count, entries[entry.index] == entry else {
            throw ArchiveError(code: .entryNotFound, backendCode: 0, message: "Entry does not belong to this archive")
        }
        operationLock.lock()
        defer { operationLock.unlock() }
        if let sevenZipBackend {
            try sevenZipBackend.read(entry: entry, context: context)
            return
        }
        guard let handle else {
            throw ArchiveError(code: .internal, backendCode: 0, message: "Archive backend is unavailable")
        }
        var error = SAError()
        let opaque = Unmanaged.passUnretained(context).toOpaque()
        let success = swiftarchive_archive_read_entry(
            handle,
            UInt64(entry.index),
            archiveDataCallback,
            opaque,
            &error
        )
        guard success != 0 else { throw Self.makeError(error) }
    }

    private func extractSynchronously(
        entry: ArchiveEntry,
        to destinationURL: URL,
        options: ArchiveExtractionOptions,
        context: ProgressContext
    ) throws {
        guard entry.index >= 0, entry.index < entries.count, entries[entry.index] == entry else {
            throw ArchiveError(code: .entryNotFound, backendCode: 0, message: "Entry does not belong to this archive")
        }
        operationLock.lock()
        defer { operationLock.unlock() }
        if let sevenZipBackend {
            try sevenZipBackend.extract(
                entry: entry,
                to: destinationURL,
                options: options,
                context: context
            )
            return
        }
        guard let handle else {
            throw ArchiveError(code: .internal, backendCode: 0, message: "Archive backend is unavailable")
        }
        var error = SAError()
        var nativeOptions = swiftarchive_extraction_options_default()
        switch options.overwritePolicy {
        case .error: nativeOptions.overwrite_policy = SA_OVERWRITE_ERROR
        case .skip: nativeOptions.overwrite_policy = SA_OVERWRITE_SKIP
        case .replace: nativeOptions.overwrite_policy = SA_OVERWRITE_REPLACE
        }
        nativeOptions.preserve_timestamps = options.preserveTimestamps ? 1 : 0
        let opaque = Unmanaged.passUnretained(context).toOpaque()
        let success = destinationURL.path.withCString { destination in
            swiftarchive_archive_extract_entry(
                handle,
                UInt64(entry.index),
                destination,
                &nativeOptions,
                archiveProgressCallback,
                opaque,
                &error
            )
        }
        guard success != 0 else { throw Self.makeError(error) }
    }

    private static func makeEntry(_ info: SAEntryInfo, index: Int) -> ArchiveEntry {
        let kind: ArchiveEntry.Kind
        switch info.kind {
        case SA_ENTRY_DIRECTORY: kind = .directory
        case SA_ENTRY_SYMBOLIC_LINK: kind = .symbolicLink
        case SA_ENTRY_HARD_LINK: kind = .hardLink
        case SA_ENTRY_OTHER_LINK: kind = .otherLink
        default: kind = .file
        }
        return ArchiveEntry(
            index: index,
            path: info.path.map(String.init(cString:)) ?? "",
            kind: kind,
            compressedSize: info.compressed_size,
            uncompressedSize: info.uncompressed_size,
            modificationDate: info.modification_time > 0
                ? Date(timeIntervalSince1970: TimeInterval(info.modification_time))
                : nil,
            crc32: info.crc32,
            isEncrypted: info.encrypted != 0,
            isSolid: info.solid != 0,
            isSplitBefore: info.split_before != 0,
            isSplitAfter: info.split_after != 0
        )
    }

    static func makeError(_ native: SAError) -> ArchiveError {
        var copy = native
        let message = withUnsafePointer(to: &copy.message) { pointer in
            pointer.withMemoryRebound(to: CChar.self, capacity: 512) { String(cString: $0) }
        }
        return ArchiveError(
            code: ArchiveErrorCode(rawValue: Int32(native.code.rawValue)) ?? .internal,
            backendCode: native.backend_code,
            message: message.isEmpty ? "Archive operation failed" : message
        )
    }

    static func withOptionalCString<Result>(
        _ value: String?,
        body: (UnsafePointer<CChar>?) throws -> Result
    ) rethrows -> Result {
        if let value {
            return try value.withCString(body)
        }
        return try body(nil)
    }
}
