import SwiftArchive
import Foundation
import XCTest

final class SwiftArchiveTests: XCTestCase {
    func testZIPUnicodeRoundTripAndStreaming() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appending(path: "source")
        let nested = source.appending(path: "中文目录")
        try FileManager.default.createDirectory(at: nested, withIntermediateDirectories: true)
        let original = Data("SwiftArchive 中文内容\n".utf8)
        try original.write(to: nested.appending(path: "说明.txt"))

        let archiveURL = root.appending(path: "unicode.zip")
        try await ZipArchive.create(
            at: archiveURL,
            inputs: [.init(sourceURL: source)],
            options: .init(includeParentDirectory: false)
        )

        let archive = try ArchiveReader(url: archiveURL)
        XCTAssertEqual(archive.format, .zip)
        let entry = try XCTUnwrap(archive.entries.first { $0.path == "中文目录/说明.txt" })
        let loaded = try await archive.data(for: entry)
        XCTAssertEqual(loaded, original)

        let collector = DataCollector()
        try await archive.read(entry) { chunk in
            collector.append(chunk)
            return true
        }
        XCTAssertEqual(collector.data, original)

        let destination = root.appending(path: "extracted")
        try await archive.extract(to: destination)
        XCTAssertEqual(try Data(contentsOf: destination.appending(path: entry.path)), original)
    }

    func testAutomaticDetectionUsesContentInsteadOfExtension() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }

        let source = root.appending(path: "payload.txt")
        let original = Data("prefixed archive".utf8)
        try original.write(to: source)
        let plainZIP = root.appending(path: "plain.zip")
        try await ZipArchive.create(at: plainZIP, inputs: [.init(sourceURL: source)])

        var prefixedZIP = Data([0x00, 0x00, 0x00, 0x18])
        prefixedZIP.append(Data("ftypisom".utf8))
        prefixedZIP.append(Data(repeating: 0, count: 12))
        prefixedZIP.append(try Data(contentsOf: plainZIP))
        let disguisedZIP = root.appending(path: "video.rar")
        try prefixedZIP.write(to: disguisedZIP)

        let zipArchive = try ArchiveReader(url: disguisedZIP)
        XCTAssertEqual(zipArchive.format, .zip)
        let zipEntry = try XCTUnwrap(zipArchive.entries.first)
        let extracted = try await zipArchive.data(for: zipEntry)
        XCTAssertEqual(extracted, original)

        let disguisedRAR = root.appending(path: "renamed.zip")
        try FileManager.default.copyItem(at: fixture("rar5-subdirs.rar"), to: disguisedRAR)
        let rarArchive = try ArchiveReader(url: disguisedRAR)
        XCTAssertEqual(rarArchive.format, .rar)
        XCTAssertFalse(rarArchive.entries.isEmpty)
    }

    func testZIPAESAndWrongPassword() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appending(path: "secret.txt")
        let original = Data("encrypted payload".utf8)
        try original.write(to: source)
        let archiveURL = root.appending(path: "secret.zip")
        try await ZipArchive.create(
            at: archiveURL,
            inputs: [.init(sourceURL: source)],
            options: .init(password: "correct-password", encryption: .aes256)
        )

        let passwordless = try ArchiveReader(url: archiveURL)
        XCTAssertTrue(passwordless.entries[0].isEncrypted)
        do {
            _ = try await passwordless.data(for: passwordless.entries[0])
            XCTFail("Expected a password required error")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .passwordRequired)
        }

        let destination = root.appending(path: "passwordless-extraction", directoryHint: .isDirectory)
        do {
            try await passwordless.extract(to: destination)
            XCTFail("Expected a password required error")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .passwordRequired)
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))

        let correct = try ArchiveReader(url: archiveURL, options: .init(password: "correct-password"))
        XCTAssertTrue(correct.entries[0].isEncrypted)
        let decrypted = try await correct.data(for: correct.entries[0])
        XCTAssertEqual(decrypted, original)

        let wrong = try ArchiveReader(url: archiveURL, options: .init(password: "wrong-password"))
        do {
            _ = try await wrong.data(for: wrong.entries[0])
            XCTFail("Expected a bad password error")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .badPassword)
        }
    }

    func testZIPCryptoRoundTrip() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appending(path: "legacy.txt")
        let original = Data("legacy encryption".utf8)
        try original.write(to: source)
        let archiveURL = root.appending(path: "legacy.zip")
        try await ZipArchive.create(
            at: archiveURL,
            inputs: [.init(sourceURL: source)],
            options: .init(password: "password", encryption: .zipCrypto)
        )
        let archive = try ArchiveReader(url: archiveURL, options: .init(password: "password"))
        let decrypted = try await archive.data(for: archive.entries[0])
        XCTAssertEqual(decrypted, original)
    }

    func testLegacyCP936FilenameIsDecodedAutomatically() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let archiveURL = root.appending(path: "legacy-name.zip")
        let gbkName: [UInt8] = [0xd6, 0xd0, 0xce, 0xc4, 0x2e, 0x74, 0x78, 0x74]
        try makeStoredZIP(filename: gbkName).write(to: archiveURL)
        let archive = try ArchiveReader(url: archiveURL)
        XCTAssertEqual(archive.entries.map(\.path), ["中文.txt"])
    }

    func testUnsafeZIPPathIsRejected() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let archiveURL = root.appending(path: "unsafe.zip")
        try makeStoredZIP(filename: Array("../outside.txt".utf8)).write(to: archiveURL)
        XCTAssertThrowsError(try ArchiveReader(url: archiveURL)) { error in
            XCTAssertEqual((error as? ArchiveError)?.code, .unsafePath)
        }
    }

    func testPreexistingDestinationSymlinkIsRejectedBeforeCreatingDirectories() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appending(path: "payload.txt")
        try Data("payload".utf8).write(to: source)
        let archiveURL = root.appending(path: "symlink-destination.zip")
        try await ZipArchive.create(
            at: archiveURL,
            inputs: [.init(sourceURL: source, pathInArchive: "linked/new/payload.txt")]
        )
        let destination = root.appending(path: "destination")
        let outside = root.appending(path: "outside")
        try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: outside, withIntermediateDirectories: true)
        try FileManager.default.createSymbolicLink(
            at: destination.appending(path: "linked"),
            withDestinationURL: outside
        )

        let archive = try ArchiveReader(url: archiveURL)
        do {
            try await archive.extract(to: destination)
            XCTFail("Expected unsafePath")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .unsafePath)
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: outside.appending(path: "new").path))
    }

    func testResourceLimitIsAppliedBeforeExtraction() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appending(path: "large.txt")
        try Data(repeating: 0x41, count: 4096).write(to: source)
        let archiveURL = root.appending(path: "large.zip")
        try await ZipArchive.create(at: archiveURL, inputs: [.init(sourceURL: source)])
        let limits = ArchiveLimits(maximumEntrySize: 1024)
        XCTAssertThrowsError(try ArchiveReader(url: archiveURL, options: .init(limits: limits))) { error in
            XCTAssertEqual((error as? ArchiveError)?.code, .resourceLimit)
        }
    }

    func testExtractionOverwritePolicies() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appending(path: "file.txt")
        try Data("new".utf8).write(to: source)
        let archiveURL = root.appending(path: "overwrite.zip")
        try await ZipArchive.create(at: archiveURL, inputs: [.init(sourceURL: source)])
        let archive = try ArchiveReader(url: archiveURL)
        let destination = root.appending(path: "destination")
        try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
        let output = destination.appending(path: "file.txt")
        try Data("old".utf8).write(to: output)

        do {
            try await archive.extract(to: destination)
            XCTFail("Expected destinationExists")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .destinationExists)
        }
        try await archive.extract(to: destination, options: .init(overwritePolicy: .skip))
        XCTAssertEqual(String(decoding: try Data(contentsOf: output), as: UTF8.self), "old")
        try await archive.extract(to: destination, options: .init(overwritePolicy: .replace))
        XCTAssertEqual(String(decoding: try Data(contentsOf: output), as: UTF8.self), "new")
    }

    func testProgressCanCancelZIPExtraction() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appending(path: "large.bin")
        try Data(repeating: 0x5a, count: 2 * 1024 * 1024).write(to: source)
        let archiveURL = root.appending(path: "cancel.zip")
        try await ZipArchive.create(at: archiveURL, inputs: [.init(sourceURL: source)])
        let archive = try ArchiveReader(url: archiveURL)
        do {
            try await archive.extract(to: root.appending(path: "cancelled")) { _ in false }
            XCTFail("Expected cancellation")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .cancelled)
        }
    }

    func testZIPSingleEntryAndSplitArchive() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        var state: UInt64 = 0x1234_5678_9abc_def0
        var bytes = Data(capacity: 200_000)
        for _ in 0..<200_000 {
            state = state &* 6_364_136_223_846_793_005 &+ 1
            bytes.append(UInt8(truncatingIfNeeded: state >> 32))
        }
        let source = root.appending(path: "random.bin")
        try bytes.write(to: source)
        let archiveURL = root.appending(path: "split.zip")
        try await ZipArchive.create(
            at: archiveURL,
            inputs: [.init(sourceURL: source)],
            options: .init(volumeSize: 32 * 1024)
        )
        XCTAssertTrue(FileManager.default.fileExists(atPath: root.appending(path: "split.z01").path))

        let archive = try ArchiveReader(url: archiveURL)
        let destination = root.appending(path: "one")
        try await archive.extract(archive.entries[0], to: destination)
        XCTAssertEqual(try Data(contentsOf: destination.appending(path: "random.bin")), bytes)
    }

    func testRARUnicodeSubdirectoriesAndExtraction() async throws {
        let archive = try ArchiveReader(url: fixture("rar5-subdirs.rar"))
        XCTAssertEqual(archive.format, .rar)
        let entry = try XCTUnwrap(archive.entries.first { $0.path == "sub/üȵĩöḋè/file.txt" })
        let data = try await archive.data(for: entry)
        XCTAssertEqual(data.count, 5)

        let destination = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: destination) }
        try await archive.extract(to: destination)
        XCTAssertTrue(FileManager.default.fileExists(atPath: destination.appending(path: entry.path).path))
    }

    func testRARSolidEntryStreaming() async throws {
        let archive = try ArchiveReader(url: fixture("rar5-solid.rar"))
        XCTAssertEqual(archive.entries.count, 2)
        XCTAssertTrue(archive.entries[1].isSolid)
        let data = try await archive.data(for: archive.entries[1])
        XCTAssertEqual(data.count, 2048)

        let destination = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: destination) }
        try await archive.extract(archive.entries[1], to: destination)
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.appending(path: "stest1.txt").path))
        XCTAssertEqual(
            try Data(contentsOf: destination.appending(path: "stest2.txt")).count,
            2048
        )
    }

    func testRARPasswordAndEncryptedHeaders() async throws {
        let archive = try ArchiveReader(
            url: fixture("rar5-psw.rar"),
            options: .init(password: "password")
        )
        let decrypted = try await archive.data(for: archive.entries[0])
        XCTAssertEqual(decrypted.count, 2048)

        let wrong = try ArchiveReader(
            url: fixture("rar5-psw.rar"),
            options: .init(password: "wrong")
        )
        do {
            _ = try await wrong.data(for: wrong.entries[0])
            XCTFail("Expected badPassword")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .badPassword)
        }

        XCTAssertThrowsError(try ArchiveReader(url: fixture("rar5-hpsw.rar"))) { error in
            XCTAssertEqual((error as? ArchiveError)?.code, .passwordRequired)
        }
        let encryptedHeaders = try ArchiveReader(
            url: fixture("rar5-hpsw.rar"),
            options: .init(password: "password")
        )
        XCTAssertFalse(encryptedHeaders.entries.isEmpty)
    }

    func testRARMultiVolumeExtraction() async throws {
        let archive = try ArchiveReader(url: fixture("rar5-vols.part1.rar"))
        let destination = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: destination) }
        try await archive.extract(to: destination)
        XCTAssertTrue(FileManager.default.fileExists(atPath: destination.appending(path: "vols/bigfile.txt").path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: destination.appending(path: "vols/smallfile.txt").path))
    }

    func testRARLinksAreRejectedByDefault() throws {
        XCTAssertThrowsError(try ArchiveReader(url: fixture("rar5-symlink-unix.rar"))) { error in
            XCTAssertEqual((error as? ArchiveError)?.code, .unsafeLink)
        }
    }

    private func fixture(_ name: String) -> URL {
        Bundle.module.url(forResource: name, withExtension: nil, subdirectory: "Fixtures")!
    }

    private func temporaryDirectory() throws -> URL {
        let url = FileManager.default.temporaryDirectory
            .appending(path: "swiftarchive-tests-\(UUID().uuidString)", directoryHint: .isDirectory)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    private func makeStoredZIP(filename: [UInt8]) -> Data {
        var data = Data()
        let localOffset = UInt32(0)
        data.appendLE(UInt32(0x04034b50))
        data.appendLE(UInt16(20))
        data.appendLE(UInt16(0))
        data.appendLE(UInt16(0))
        data.appendLE(UInt16(0))
        data.appendLE(UInt16(0))
        data.appendLE(UInt32(0))
        data.appendLE(UInt32(0))
        data.appendLE(UInt32(0))
        data.appendLE(UInt16(filename.count))
        data.appendLE(UInt16(0))
        data.append(contentsOf: filename)

        let centralOffset = UInt32(data.count)
        data.appendLE(UInt32(0x02014b50))
        data.appendLE(UInt16(20))
        data.appendLE(UInt16(20))
        data.appendLE(UInt16(0))
        data.appendLE(UInt16(0))
        data.appendLE(UInt16(0))
        data.appendLE(UInt16(0))
        data.appendLE(UInt32(0))
        data.appendLE(UInt32(0))
        data.appendLE(UInt32(0))
        data.appendLE(UInt16(filename.count))
        data.appendLE(UInt16(0))
        data.appendLE(UInt16(0))
        data.appendLE(UInt16(0))
        data.appendLE(UInt16(0))
        data.appendLE(UInt32(0))
        data.appendLE(localOffset)
        data.append(contentsOf: filename)

        let centralSize = UInt32(data.count) - centralOffset
        data.appendLE(UInt32(0x06054b50))
        data.appendLE(UInt16(0))
        data.appendLE(UInt16(0))
        data.appendLE(UInt16(1))
        data.appendLE(UInt16(1))
        data.appendLE(centralSize)
        data.appendLE(centralOffset)
        data.appendLE(UInt16(0))
        return data
    }
}

private extension Data {
    mutating func appendLE<T: FixedWidthInteger>(_ value: T) {
        var littleEndian = value.littleEndian
        Swift.withUnsafeBytes(of: &littleEndian) { append(contentsOf: $0) }
    }
}

private final class DataCollector: @unchecked Sendable {
    private let lock = NSLock()
    private var storage = Data()

    var data: Data {
        lock.lock()
        defer { lock.unlock() }
        return storage
    }

    func append(_ data: Data) {
        lock.lock()
        storage.append(data)
        lock.unlock()
    }
}
