import Foundation

public enum ArchiveFormat: Sendable, Equatable {
    case zip
    case rar
    case tar
    case gzip
    case sevenZip
    case bzip2
    case xz
    case lzma
    case lzip
    case compress
    case zstandard
    case lz4
    case cab
    case cpio
    case iso9660
    case lha
    case ar
    case warc
}

public enum ArchiveFilenameEncoding: Int32, Sendable, CaseIterable {
    case automatic = 0
    case cp437 = 437
    case cp932 = 932
    case cp936 = 936
    case cp950 = 950
    case utf8 = 65001
}

public struct ArchiveLimits: Sendable, Equatable {
    public var maximumEntryCount: UInt64
    public var maximumEntrySize: UInt64
    public var maximumTotalSize: UInt64
    public var maximumCompressionRatio: Double

    public init(
        maximumEntryCount: UInt64 = 100_000,
        maximumEntrySize: UInt64 = 20 * 1_024 * 1_024 * 1_024,
        maximumTotalSize: UInt64 = 50 * 1_024 * 1_024 * 1_024,
        maximumCompressionRatio: Double = 10_000
    ) {
        self.maximumEntryCount = maximumEntryCount
        self.maximumEntrySize = maximumEntrySize
        self.maximumTotalSize = maximumTotalSize
        self.maximumCompressionRatio = maximumCompressionRatio
    }
}

public struct ArchiveOpenOptions: Sendable, Equatable {
    public var password: String?
    public var filenameEncoding: ArchiveFilenameEncoding
    public var limits: ArchiveLimits

    public init(
        password: String? = nil,
        filenameEncoding: ArchiveFilenameEncoding = .automatic,
        limits: ArchiveLimits = .init()
    ) {
        self.password = password
        self.filenameEncoding = filenameEncoding
        self.limits = limits
    }
}

public enum ArchiveOverwritePolicy: Sendable {
    case error
    case skip
    case replace
}

public struct ArchiveExtractionOptions: Sendable {
    public var overwritePolicy: ArchiveOverwritePolicy
    public var preserveTimestamps: Bool

    public init(
        overwritePolicy: ArchiveOverwritePolicy = .error,
        preserveTimestamps: Bool = true
    ) {
        self.overwritePolicy = overwritePolicy
        self.preserveTimestamps = preserveTimestamps
    }
}

public struct ZipCreationOptions: Sendable, Equatable {
    public var password: String?
    public var compressionLevel: Int16
    public var encryption: ZipEncryption
    public var includeParentDirectory: Bool
    public var volumeSize: UInt64

    public init(
        password: String? = nil,
        compressionLevel: Int16 = 6,
        encryption: ZipEncryption = .aes256,
        includeParentDirectory: Bool = true,
        volumeSize: UInt64 = 0
    ) {
        self.password = password
        self.compressionLevel = min(9, max(0, compressionLevel))
        self.encryption = encryption
        self.includeParentDirectory = includeParentDirectory
        self.volumeSize = volumeSize
    }
}

public enum ZipEncryption: Sendable, Equatable {
    case aes256
    case zipCrypto
}

public struct ArchiveEntry: Sendable, Identifiable, Equatable {
    public enum Kind: Sendable, Equatable {
        case file
        case directory
        case symbolicLink
        case hardLink
        case otherLink
    }

    public let index: Int
    public let path: String
    public let kind: Kind
    public let compressedSize: UInt64
    public let uncompressedSize: UInt64
    public let modificationDate: Date?
    public let crc32: UInt32
    public let isEncrypted: Bool
    public let isSolid: Bool
    public let isSplitBefore: Bool
    public let isSplitAfter: Bool

    public var id: Int { index }
}

public struct ArchiveProgress: Sendable, Equatable {
    public let entryIndex: Int
    public let entryPath: String
    public let entryCompleted: UInt64
    public let entryTotal: UInt64
    public let totalCompleted: UInt64
    public let totalSize: UInt64

    public var fractionCompleted: Double {
        guard totalSize > 0 else { return totalCompleted > 0 ? 1 : 0 }
        return min(1, Double(totalCompleted) / Double(totalSize))
    }
}

public struct ZipInput: Sendable, Equatable {
    public let sourceURL: URL
    public let pathInArchive: String?

    public init(sourceURL: URL, pathInArchive: String? = nil) {
        self.sourceURL = sourceURL
        self.pathInArchive = pathInArchive
    }
}

public enum ArchiveErrorCode: Int32, Sendable {
    case invalidArgument = 1
    case unsupportedFormat = 2
    case openFailed = 3
    case corruptArchive = 4
    case passwordRequired = 5
    case badPassword = 6
    case io = 7
    case entryNotFound = 8
    case destinationExists = 9
    case unsafePath = 10
    case unsafeLink = 11
    case resourceLimit = 12
    case cancelled = 13
    case unsupportedFeature = 14
    case `internal` = 15
    case missingVolume = 16
}

public struct ArchiveError: Error, Sendable, Equatable, LocalizedError {
    public let code: ArchiveErrorCode
    public let backendCode: Int32
    public let message: String

    public var errorDescription: String? { message }
}
