import CSwiftArchiveCore
import Foundation

public enum ZipArchive {
    public static func create(
        at archiveURL: URL,
        inputs: [ZipInput],
        options: ZipCreationOptions = .init(),
        progress: (@Sendable (ArchiveProgress) -> Bool)? = nil
    ) async throws {
        guard !inputs.isEmpty else {
            throw ArchiveError(code: .invalidArgument, backendCode: 0, message: "At least one ZIP input is required")
        }
        let context = ProgressContextForZip(handler: progress)
        try await withTaskCancellationHandler {
            try await Task.detached {
                try createSynchronously(
                    at: archiveURL,
                    inputs: inputs,
                    options: options,
                    context: context
                )
            }.value
        } onCancel: {
            context.cancel()
        }
    }

    private static func createSynchronously(
        at archiveURL: URL,
        inputs: [ZipInput],
        options: ZipCreationOptions,
        context: ProgressContextForZip
    ) throws {
        var error = SAError()
        var nativeOptions = swiftarchive_zip_create_options_default()
        nativeOptions.compression_level = options.compressionLevel
        nativeOptions.use_aes = options.encryption == .aes256 ? 1 : 0
        nativeOptions.include_parent_directory = options.includeParentDirectory ? 1 : 0
        nativeOptions.volume_size = options.volumeSize

        let sourceStorage = inputs.map { strdup($0.sourceURL.path)! }
        let archiveStorage = inputs.map { input -> UnsafeMutablePointer<CChar>? in
            input.pathInArchive.map { strdup($0) }
        }
        defer {
            sourceStorage.forEach { pointer in free(pointer) }
            archiveStorage.forEach { pointer in
                if let pointer { free(pointer) }
            }
        }
        let sourcePointers = sourceStorage.map { UnsafePointer($0) as UnsafePointer<CChar>? }
        let archivePointers: [UnsafePointer<CChar>?] = archiveStorage.map { pointer in
            pointer.map { UnsafePointer($0) }
        }
        let opaque = Unmanaged.passUnretained(context).toOpaque()

        let success: Int32 = ArchiveReader.withOptionalCString(options.password) { password in
            nativeOptions.password = password
            return archiveURL.path.withCString { archivePath in
                sourcePointers.withUnsafeBufferPointer { sources in
                    archivePointers.withUnsafeBufferPointer { paths in
                        swiftarchive_zip_create(
                            archivePath,
                            sources.baseAddress,
                            paths.baseAddress,
                            inputs.count,
                            &nativeOptions,
                            zipProgressCallback,
                            opaque,
                            &error
                        )
                    }
                }
            }
        }
        guard success != 0 else { throw ArchiveReader.makeError(error) }
    }
}

private final class ProgressContextForZip: @unchecked Sendable {
    private let lock = NSLock()
    private var isCancelled = false
    let handler: (@Sendable (ArchiveProgress) -> Bool)?

    init(handler: (@Sendable (ArchiveProgress) -> Bool)?) {
        self.handler = handler
    }

    func cancel() {
        lock.lock()
        isCancelled = true
        lock.unlock()
    }

    func shouldContinue(_ value: ArchiveProgress) -> Bool {
        lock.lock()
        let cancelled = isCancelled
        lock.unlock()
        return !cancelled && (handler?(value) ?? true)
    }
}

private let zipProgressCallback: SAProgressCallback = { context, progress in
    guard let context, let progress else { return 0 }
    let box = Unmanaged<ProgressContextForZip>.fromOpaque(context).takeUnretainedValue()
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
