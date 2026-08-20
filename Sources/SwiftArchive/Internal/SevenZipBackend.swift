import Foundation
import PLzmaSDK

final class SevenZipBackend: @unchecked Sendable {
    let primaryURL: URL
    let volumeURLs: [URL]
    let entries: [ArchiveEntry]

    private let decoder: PLzmaSDK.Decoder
    private let nativeItems: [PLzmaSDK.Item]
    private let password: String?
    private let limits: ArchiveLimits
    private let usesVolumeNaming: Bool
    private let progressState: SevenZipProgressState
    private let progressDelegate: SevenZipProgressDelegate

    static func shouldHandle(urls: [URL], requestedFormat: ArchiveFormat?) -> Bool {
        if requestedFormat == .sevenZip { return true }
        if urls.contains(where: { SevenZipVolumeResolver.isSevenZipName($0) }) { return true }
        return urls.contains(where: hasSevenZipSignature)
    }

    init(urls: [URL], password: String?, limits: ArchiveLimits) throws {
        let resolved = try SevenZipVolumeResolver.resolve(urls)
        primaryURL = resolved.primaryURL
        volumeURLs = resolved.urls
        usesVolumeNaming = resolved.usesVolumeNaming
        self.password = password
        self.limits = limits

        do {
            let inputStreams = try resolved.urls.map { url in
                try PLzmaSDK.InStream(path: PLzmaSDK.Path(url.path))
            }
            let input = inputStreams.count == 1
                ? inputStreams[0]
                : try PLzmaSDK.InStream(streams: inputStreams)
            let state = SevenZipProgressState()
            let delegate = SevenZipProgressDelegate(state: state)
            let decoder = try PLzmaSDK.Decoder(
                stream: input,
                fileType: .sevenZ,
                delegate: delegate
            )
            try decoder.setPassword(password)
            guard try decoder.open() else {
                throw ArchiveError(
                    code: .cancelled,
                    backendCode: 0,
                    message: "Opening the 7z archive was cancelled"
                )
            }

            let count = Int(try decoder.count())
            let isSolid = try decoder.isSolid()
            guard UInt64(count) <= limits.maximumEntryCount else {
                throw ArchiveError(
                    code: .resourceLimit,
                    backendCode: 0,
                    message: "Archive contains too many entries"
                )
            }

            var totalSize: UInt64 = 0
            var loadedItems: [PLzmaSDK.Item] = []
            var loadedEntries: [ArchiveEntry] = []
            loadedItems.reserveCapacity(count)
            loadedEntries.reserveCapacity(count)
            for index in 0..<count {
                let item = try decoder.item(at: UInt32(index))
                let normalizedPath = try Self.normalizedEntryPath(try item.path().description)
                guard item.size <= limits.maximumEntrySize else {
                    throw ArchiveError(
                        code: .resourceLimit,
                        backendCode: 0,
                        message: "Archive entry exceeds the configured size limit"
                    )
                }
                let (nextTotal, overflow) = totalSize.addingReportingOverflow(item.size)
                guard !overflow, nextTotal <= limits.maximumTotalSize else {
                    throw ArchiveError(
                        code: .resourceLimit,
                        backendCode: 0,
                        message: "Archive exceeds the configured total size limit"
                    )
                }
                totalSize = nextTotal
                if item.packSize > 0,
                   limits.maximumCompressionRatio > 0,
                   Double(item.size) / Double(item.packSize) > limits.maximumCompressionRatio {
                    throw ArchiveError(
                        code: .resourceLimit,
                        backendCode: 0,
                        message: "Archive entry exceeds the configured compression ratio"
                    )
                }

                let modificationDate = item.modificationDate.timeIntervalSince1970 > 0
                    ? item.modificationDate
                    : nil
                let kind = Self.entryKind(isDirectory: item.isDir, attributes: item.attributes)
                guard kind == .file || kind == .directory else {
                    throw ArchiveError(
                        code: .unsafeLink,
                        backendCode: 0,
                        message: "7z link and special-file entries are not allowed"
                    )
                }
                loadedItems.append(item)
                loadedEntries.append(ArchiveEntry(
                    index: index,
                    path: normalizedPath,
                    kind: kind,
                    compressedSize: item.packSize,
                    uncompressedSize: item.size,
                    modificationDate: modificationDate,
                    crc32: item.crc32,
                    isEncrypted: item.encrypted,
                    isSolid: isSolid,
                    isSplitBefore: resolved.urls.count > 1 && index == 0,
                    isSplitAfter: resolved.urls.count > 1 && index == count - 1
                ))
            }

            let archiveSize = resolved.urls.reduce(into: UInt64(0)) { result, url in
                guard let size = try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize,
                      size > 0 else { return }
                let (next, overflow) = result.addingReportingOverflow(UInt64(size))
                result = overflow ? UInt64.max : next
            }
            if archiveSize > 0,
               limits.maximumCompressionRatio > 0,
               Double(totalSize) / Double(archiveSize) > limits.maximumCompressionRatio {
                throw ArchiveError(
                    code: .resourceLimit,
                    backendCode: 0,
                    message: "Archive exceeds the configured compression ratio"
                )
            }

            self.decoder = decoder
            nativeItems = loadedItems
            entries = loadedEntries
            progressState = state
            progressDelegate = delegate
            state.configure(entries: loadedEntries, totalSize: totalSize)
        } catch {
            throw Self.map(
                error,
                hasPassword: !(password?.isEmpty ?? true),
                encryptedEntry: false,
                usesVolumes: resolved.urls.count > 1 || resolved.usesVolumeNaming
            )
        }
    }

    func cancel() {
        progressState.cancel()
        try? decoder.abort()
    }

    func extract(
        to destinationURL: URL,
        options: ArchiveExtractionOptions,
        context: ProgressContext
    ) throws {
        try extract(
            indexes: Array(entries.indices),
            to: destinationURL,
            options: options,
            context: context
        )
    }

    func extract(
        entry: ArchiveEntry,
        to destinationURL: URL,
        options: ArchiveExtractionOptions,
        context: ProgressContext
    ) throws {
        try validate(entry)
        try extract(
            indexes: [entry.index],
            to: destinationURL,
            options: options,
            context: context
        )
    }

    func read(entry: ArchiveEntry, context: DataContext) throws {
        try validate(entry)
        guard entry.kind == .file else {
            throw ArchiveError(
                code: .invalidArgument,
                backendCode: 0,
                message: "Only regular archive entries can be read"
            )
        }

        let temporaryRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("swiftarchive-sevenzip-\(UUID().uuidString)", isDirectory: true)
        defer { try? FileManager.default.removeItem(at: temporaryRoot) }
        do {
            try extract(
                indexes: [entry.index],
                to: temporaryRoot,
                options: .init(overwritePolicy: .error, preserveTimestamps: false),
                context: nil
            )
            let fileURL = temporaryRoot.appendingPathComponent(entry.path)
            let file = try FileHandle(forReadingFrom: fileURL)
            defer { try? file.close() }
            while true {
                if context.isCancelled {
                    throw ArchiveError(code: .cancelled, backendCode: 0, message: "Archive read was cancelled")
                }
                guard let data = try file.read(upToCount: 128 * 1_024), !data.isEmpty else { break }
                guard context.consume(data) else {
                    throw ArchiveError(code: .cancelled, backendCode: 0, message: "Archive read was cancelled")
                }
            }
        } catch {
            throw Self.map(
                error,
                hasPassword: !(password?.isEmpty ?? true),
                encryptedEntry: entry.isEncrypted,
                usesVolumes: volumeURLs.count > 1 || usesVolumeNaming
            )
        }
    }

    private func extract(
        indexes: [Int],
        to destinationURL: URL,
        options: ArchiveExtractionOptions,
        context: ProgressContext?
    ) throws {
        let selectedEntries = indexes.map { entries[$0] }
        if selectedEntries.contains(where: \.isEncrypted), password?.isEmpty != false {
            throw ArchiveError(
                code: .passwordRequired,
                backendCode: 0,
                message: "A password is required to read this 7z archive"
            )
        }
        if context?.isCancelled == true {
            throw ArchiveError(code: .cancelled, backendCode: 0, message: "Extraction was cancelled")
        }

        let fileManager = FileManager.default
        try Self.ensureSafeDirectory(destinationURL, beneath: nil, fileManager: fileManager)
        var plans = try makePlans(
            indexes: indexes,
            destinationRoot: destinationURL,
            options: options,
            fileManager: fileManager
        )
        defer {
            for plan in plans {
                try? fileManager.removeItem(at: plan.temporaryURL)
            }
        }
        guard !plans.isEmpty else {
            context?.reportCompletion(totalSize: selectedEntries.reduce(0) { $0 + $1.uncompressedSize })
            return
        }

        do {
            let streams = try PLzmaSDK.ItemOutStreamArray(capacity: UInt32(plans.count))
            for index in plans.indices {
                let stream = try PLzmaSDK.OutStream(path: PLzmaSDK.Path(plans[index].temporaryURL.path))
                plans[index].stream = stream
                try streams.add(item: nativeItems[plans[index].entry.index], stream: stream)
            }

            progressState.begin(context)
            defer { progressState.end() }
            let extracted = try decoder.extract(itemsToStreams: streams)
            guard extracted else {
                if context?.isCancelled == true || progressState.wasCancelled {
                    throw ArchiveError(code: .cancelled, backendCode: 0, message: "Extraction was cancelled")
                }
                throw ArchiveError(code: .corruptArchive, backendCode: 0, message: "Could not extract the 7z archive")
            }

            for plan in plans {
                let values = try plan.temporaryURL.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
                guard values.isRegularFile == true else {
                    throw ArchiveError(code: .unsafeLink, backendCode: 0, message: "Archive contains a link entry")
                }
                let actualSize = UInt64(values.fileSize ?? 0)
                guard actualSize == plan.entry.uncompressedSize else {
                    throw ArchiveError(code: .corruptArchive, backendCode: 0, message: "Extracted entry size does not match its archive header")
                }
                guard actualSize <= limits.maximumEntrySize else {
                    throw ArchiveError(code: .resourceLimit, backendCode: 0, message: "Extracted entry exceeds the configured size limit")
                }
            }

            for plan in plans {
                try Self.commit(
                    plan,
                    overwritePolicy: options.overwritePolicy,
                    preserveTimestamp: options.preserveTimestamps,
                    fileManager: fileManager
                )
            }
            context?.reportCompletion(totalSize: selectedEntries.reduce(0) { $0 + $1.uncompressedSize })
        } catch {
            throw Self.map(
                error,
                hasPassword: !(password?.isEmpty ?? true),
                encryptedEntry: selectedEntries.contains(where: \.isEncrypted),
                usesVolumes: volumeURLs.count > 1 || usesVolumeNaming
            )
        }
    }

    private func makePlans(
        indexes: [Int],
        destinationRoot: URL,
        options: ArchiveExtractionOptions,
        fileManager: FileManager
    ) throws -> [SevenZipExtractionPlan] {
        var plans: [SevenZipExtractionPlan] = []
        var planIndexByPath: [String: Int] = [:]
        for index in indexes {
            let entry = entries[index]
            let destination = destinationRoot.appendingPathComponent(entry.path).standardizedFileURL
            guard Self.isContained(destination, in: destinationRoot) else {
                throw ArchiveError(code: .unsafePath, backendCode: 0, message: "Archive entry escapes the destination")
            }
            if entry.kind == .directory {
                try Self.ensureSafeDirectory(destination, beneath: destinationRoot, fileManager: fileManager)
                continue
            }

            let parent = destination.deletingLastPathComponent()
            try Self.ensureSafeDirectory(parent, beneath: destinationRoot, fileManager: fileManager)
            if let existingIndex = planIndexByPath[entry.path] {
                switch options.overwritePolicy {
                case .error:
                    throw ArchiveError(code: .destinationExists, backendCode: 0, message: "Archive contains duplicate output paths")
                case .skip:
                    continue
                case .replace:
                    plans.remove(at: existingIndex)
                    planIndexByPath = Dictionary(uniqueKeysWithValues: plans.enumerated().map { ($0.element.entry.path, $0.offset) })
                }
            }

            if fileManager.fileExists(atPath: destination.path) {
                let values = try destination.resourceValues(forKeys: [.isSymbolicLinkKey, .isDirectoryKey])
                if values.isSymbolicLink == true {
                    throw ArchiveError(code: .unsafeLink, backendCode: 0, message: "Destination contains a symbolic link")
                }
                if values.isDirectory == true {
                    throw ArchiveError(code: .destinationExists, backendCode: 0, message: "A directory exists at the archive output path")
                }
                switch options.overwritePolicy {
                case .error:
                    throw ArchiveError(code: .destinationExists, backendCode: 0, message: "Destination file already exists")
                case .skip:
                    continue
                case .replace:
                    break
                }
            }

            let temporary = parent.appendingPathComponent(".swiftarchive-7z-\(UUID().uuidString).part")
            planIndexByPath[entry.path] = plans.count
            plans.append(SevenZipExtractionPlan(
                entry: entry,
                finalURL: destination,
                temporaryURL: temporary,
                stream: nil
            ))
        }
        return plans
    }

    private func validate(_ entry: ArchiveEntry) throws {
        guard entry.index >= 0,
              entry.index < entries.count,
              entries[entry.index] == entry else {
            throw ArchiveError(code: .entryNotFound, backendCode: 0, message: "Entry does not belong to this archive")
        }
    }

    private static func commit(
        _ plan: SevenZipExtractionPlan,
        overwritePolicy: ArchiveOverwritePolicy,
        preserveTimestamp: Bool,
        fileManager: FileManager
    ) throws {
        if fileManager.fileExists(atPath: plan.finalURL.path) {
            let values = try plan.finalURL.resourceValues(forKeys: [.isSymbolicLinkKey, .isDirectoryKey])
            if values.isSymbolicLink == true {
                throw ArchiveError(code: .unsafeLink, backendCode: 0, message: "Destination contains a symbolic link")
            }
            switch overwritePolicy {
            case .error:
                throw ArchiveError(code: .destinationExists, backendCode: 0, message: "Destination file already exists")
            case .skip:
                try? fileManager.removeItem(at: plan.temporaryURL)
                return
            case .replace:
                guard values.isDirectory != true else {
                    throw ArchiveError(code: .destinationExists, backendCode: 0, message: "A directory exists at the archive output path")
                }
                _ = try fileManager.replaceItemAt(plan.finalURL, withItemAt: plan.temporaryURL)
            }
        } else {
            try fileManager.moveItem(at: plan.temporaryURL, to: plan.finalURL)
        }
        if preserveTimestamp, let date = plan.entry.modificationDate {
            try fileManager.setAttributes([.modificationDate: date], ofItemAtPath: plan.finalURL.path)
        }
    }

    private static func ensureSafeDirectory(
        _ directory: URL,
        beneath root: URL?,
        fileManager: FileManager
    ) throws {
        let directory = directory.standardizedFileURL
        if let root, !isContained(directory, in: root) {
            throw ArchiveError(code: .unsafePath, backendCode: 0, message: "Archive directory escapes the destination")
        }

        var missing: [URL] = []
        var current = directory
        while !fileManager.fileExists(atPath: current.path) {
            missing.append(current)
            let parent = current.deletingLastPathComponent()
            guard parent.path != current.path else {
                throw ArchiveError(code: .io, backendCode: 0, message: "Unable to locate an existing destination parent")
            }
            current = parent
        }
        let ancestorValues = try current.resourceValues(forKeys: [.isSymbolicLinkKey, .isDirectoryKey])
        guard ancestorValues.isSymbolicLink != true, ancestorValues.isDirectory == true else {
            throw ArchiveError(code: .unsafeLink, backendCode: 0, message: "Destination path contains a symbolic link")
        }

        for component in missing.reversed() {
            try fileManager.createDirectory(at: component, withIntermediateDirectories: false)
            let values = try component.resourceValues(forKeys: [.isSymbolicLinkKey, .isDirectoryKey])
            guard values.isSymbolicLink != true, values.isDirectory == true else {
                throw ArchiveError(code: .unsafeLink, backendCode: 0, message: "Destination path contains a symbolic link")
            }
        }
        if missing.isEmpty {
            let values = try directory.resourceValues(forKeys: [.isSymbolicLinkKey, .isDirectoryKey])
            guard values.isSymbolicLink != true, values.isDirectory == true else {
                throw ArchiveError(code: .unsafeLink, backendCode: 0, message: "Destination path is not a safe directory")
            }
        }
    }

    private static func isContained(_ url: URL, in root: URL) -> Bool {
        let rootPath = root.standardizedFileURL.path
        let path = url.standardizedFileURL.path
        return path == rootPath || path.hasPrefix(rootPath.hasSuffix("/") ? rootPath : rootPath + "/")
    }

    private static func normalizedEntryPath(_ input: String) throws -> String {
        guard !input.isEmpty, !input.contains("\0") else {
            throw ArchiveError(code: .unsafePath, backendCode: 0, message: "Archive contains an empty or invalid path")
        }
        let value = input.replacingOccurrences(of: "\\", with: "/")
        guard !value.hasPrefix("/"),
              !(value.count >= 2 && value[value.index(after: value.startIndex)] == ":") else {
            throw ArchiveError(code: .unsafePath, backendCode: 0, message: "Archive contains an absolute path")
        }
        var components: [Substring] = []
        for component in value.split(separator: "/", omittingEmptySubsequences: true) {
            if component == ".." {
                throw ArchiveError(code: .unsafePath, backendCode: 0, message: "Archive path escapes through a parent component")
            }
            if component != "." { components.append(component) }
        }
        guard !components.isEmpty else {
            throw ArchiveError(code: .unsafePath, backendCode: 0, message: "Archive contains an empty path")
        }
        return components.joined(separator: "/")
    }

    private static func entryKind(isDirectory: Bool, attributes: UInt32) -> ArchiveEntry.Kind {
        guard attributes & 0x8000 != 0 else {
            return isDirectory ? .directory : .file
        }
        let fileType = (attributes >> 16) & 0xF000
        switch fileType {
        case 0x4000:
            return .directory
        case 0, 0x8000:
            return .file
        case 0xA000:
            return .symbolicLink
        default:
            return .otherLink
        }
    }

    private static func hasSevenZipSignature(_ url: URL) -> Bool {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return false }
        defer { try? handle.close() }
        guard let bytes = try? handle.read(upToCount: 6) else { return false }
        return bytes == Data([0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C])
    }

    private static func map(
        _ error: Error,
        hasPassword: Bool,
        encryptedEntry: Bool,
        usesVolumes: Bool
    ) -> ArchiveError {
        if let archiveError = error as? ArchiveError { return archiveError }
        guard let exception = error as? PLzmaSDK.Exception else {
            if error is CancellationError {
                return ArchiveError(code: .cancelled, backendCode: 0, message: "Archive operation was cancelled")
            }
            return ArchiveError(code: .io, backendCode: 0, message: error.localizedDescription)
        }

        let detail = "\(exception.what) \(exception.reason)".lowercased()
        if detail.contains("password required") {
            return ArchiveError(code: .passwordRequired, backendCode: Int32(exception.code.rawValue), message: "A password is required to open this 7z archive")
        }
        if detail.contains("incorrect archive password") ||
            (encryptedEntry && (detail.contains("crc") || detail.contains("data is corrupt"))) {
            return ArchiveError(
                code: hasPassword ? .badPassword : .passwordRequired,
                backendCode: Int32(exception.code.rawValue),
                message: hasPassword ? "The 7z archive password is incorrect" : "A password is required to read this 7z archive"
            )
        }
        if detail.contains("unsupported") {
            return ArchiveError(code: .unsupportedFeature, backendCode: Int32(exception.code.rawValue), message: exception.what)
        }
        if usesVolumes && (detail.contains("unexpected") || detail.contains("can't open") || detail.contains("corrupt")) {
            return ArchiveError(code: .missingVolume, backendCode: Int32(exception.code.rawValue), message: "A 7z volume is missing, unreadable, or incomplete")
        }
        switch exception.code {
        case .notEnoughMemory:
            return ArchiveError(code: .resourceLimit, backendCode: Int32(exception.code.rawValue), message: exception.what)
        case .io:
            return ArchiveError(code: .io, backendCode: Int32(exception.code.rawValue), message: exception.what)
        case .invalidArguments:
            return ArchiveError(code: .invalidArgument, backendCode: Int32(exception.code.rawValue), message: exception.what)
        default:
            return ArchiveError(code: .corruptArchive, backendCode: Int32(exception.code.rawValue), message: exception.what)
        }
    }
}

private struct SevenZipExtractionPlan {
    let entry: ArchiveEntry
    let finalURL: URL
    let temporaryURL: URL
    var stream: PLzmaSDK.OutStream?
}

private enum SevenZipVolumeResolver {
    struct Result {
        let primaryURL: URL
        let urls: [URL]
        let usesVolumeNaming: Bool
    }

    private struct Part {
        let baseURL: URL
        let index: Int
        let url: URL

        var identifier: String { baseURL.standardizedFileURL.path.lowercased() }
    }

    static func isSevenZipName(_ url: URL) -> Bool {
        if part(for: url) != nil { return true }
        return ["7z", "cb7"].contains(url.pathExtension.lowercased())
    }

    static func resolve(_ candidates: [URL]) throws -> Result {
        var seen = Set<String>()
        let candidates = candidates.filter { seen.insert($0.standardizedFileURL.path).inserted }
        guard let first = candidates.first else {
            throw ArchiveError(code: .invalidArgument, backendCode: 0, message: "At least one archive URL is required")
        }
        guard let selectedPart = candidates.compactMap(part(for:)).first else {
            return Result(primaryURL: first, urls: [first], usesVolumeNaming: false)
        }

        var partsByIndex: [Int: URL] = [:]
        func add(_ url: URL) throws {
            guard let candidate = part(for: url), candidate.identifier == selectedPart.identifier else { return }
            if let existing = partsByIndex[candidate.index], existing.standardizedFileURL != url.standardizedFileURL {
                throw ArchiveError(code: .missingVolume, backendCode: 0, message: "The 7z volume set contains duplicate part numbers")
            }
            partsByIndex[candidate.index] = url
        }
        for candidate in candidates { try add(candidate) }

        let directory = selectedPart.baseURL.deletingLastPathComponent()
        if let siblings = try? FileManager.default.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: [.isRegularFileKey],
            options: [.skipsHiddenFiles]
        ) {
            for sibling in siblings { try add(sibling) }
        }

        guard let firstPart = partsByIndex[1] else {
            throw ArchiveError(code: .missingVolume, backendCode: 0, message: "The first 7z volume (.001) is missing or inaccessible")
        }
        let indexes = partsByIndex.keys.sorted()
        guard indexes == Array(1...indexes.count) else {
            throw ArchiveError(code: .missingVolume, backendCode: 0, message: "One or more 7z volumes are missing")
        }
        return Result(
            primaryURL: firstPart,
            urls: indexes.compactMap { partsByIndex[$0] },
            usesVolumeNaming: true
        )
    }

    private static func part(for url: URL) -> Part? {
        let suffix = url.pathExtension
        guard suffix.count >= 3,
              suffix.allSatisfy(\.isNumber),
              let index = Int(suffix),
              index > 0 else { return nil }
        let base = url.deletingPathExtension()
        guard ["7z", "cb7"].contains(base.pathExtension.lowercased()) else { return nil }
        return Part(baseURL: base, index: index, url: url)
    }
}

private final class SevenZipProgressState: @unchecked Sendable {
    private let lock = NSLock()
    private var context: ProgressContext?
    private var entriesByPath: [String: ArchiveEntry] = [:]
    private var totalSize: UInt64 = 0
    private var cancelled = false

    var wasCancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return cancelled
    }

    func configure(entries: [ArchiveEntry], totalSize: UInt64) {
        lock.lock()
        entriesByPath = entries.reduce(into: [:]) { $0[$1.path] = $1 }
        self.totalSize = totalSize
        lock.unlock()
    }

    func begin(_ context: ProgressContext?) {
        lock.lock()
        self.context = context
        cancelled = false
        lock.unlock()
    }

    func end() {
        lock.lock()
        context = nil
        lock.unlock()
    }

    func cancel() {
        lock.lock()
        cancelled = true
        context?.cancel()
        lock.unlock()
    }

    func report(decoder: PLzmaSDK.Decoder, path: String, fraction: Double) {
        lock.lock()
        let context = context
        let entry = entriesByPath[path]
        let totalSize = totalSize
        lock.unlock()
        guard let context else { return }
        let fraction = min(1, max(0, fraction))
        let totalCompleted = UInt64(Double(totalSize) * fraction)
        let entryTotal = entry?.uncompressedSize ?? 0
        let progress = ArchiveProgress(
            entryIndex: entry?.index ?? 0,
            entryPath: path,
            entryCompleted: UInt64(Double(entryTotal) * fraction),
            entryTotal: entryTotal,
            totalCompleted: totalCompleted,
            totalSize: totalSize
        )
        guard context.shouldContinue(progress) else {
            cancel()
            try? decoder.abort()
            return
        }
    }
}

private final class SevenZipProgressDelegate: PLzmaSDK.DecoderDelegate {
    private let state: SevenZipProgressState

    init(state: SevenZipProgressState) {
        self.state = state
    }

    func decoder(decoder: PLzmaSDK.Decoder, path: String, progress: Double) {
        state.report(decoder: decoder, path: path, fraction: progress)
    }
}
