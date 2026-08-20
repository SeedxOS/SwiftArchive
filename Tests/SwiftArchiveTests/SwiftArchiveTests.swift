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

    func testZIPCreationRejectsExistingOutputsAndCleansCancelledVolumes() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appending(path: "random.bin")
        var state: UInt64 = 0x1020_3040_5060_7080
        var bytes = Data(capacity: 512 * 1_024)
        for _ in 0..<(512 * 1_024) {
            state = state &* 6_364_136_223_846_793_005 &+ 1
            bytes.append(UInt8(truncatingIfNeeded: state >> 32))
        }
        try bytes.write(to: source)

        let existingURL = root.appending(path: "existing.zip")
        let original = Data("keep existing archive".utf8)
        try original.write(to: existingURL)
        do {
            try await ZipArchive.create(at: existingURL, inputs: [.init(sourceURL: source)])
            XCTFail("Expected destinationExists")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .destinationExists)
        }
        XCTAssertEqual(try Data(contentsOf: existingURL), original)

        let reservedURL = root.appending(path: "reserved.zip")
        let reservedVolume = root.appending(path: "reserved.z01")
        try original.write(to: reservedVolume)
        do {
            try await ZipArchive.create(
                at: reservedURL,
                inputs: [.init(sourceURL: source)],
                options: .init(volumeSize: 32 * 1_024)
            )
            XCTFail("Expected destinationExists")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .destinationExists)
        }
        XCTAssertEqual(try Data(contentsOf: reservedVolume), original)

        let cancelledURL = root.appending(path: "cancelled.zip")
        do {
            try await ZipArchive.create(
                at: cancelledURL,
                inputs: [.init(sourceURL: source)],
                options: .init(volumeSize: 32 * 1_024)
            ) { _ in false }
            XCTFail("Expected cancelled")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .cancelled)
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: cancelledURL.path))
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appending(path: "cancelled.z01").path))
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

    func testTARListingStreamingAndExtraction() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let archiveURL = root.appending(path: "sample.tar")
        let payload = Data("TAR 中文内容\n".utf8)
        try makeTAR(entries: [
            ("folder", Data(), UInt8(ascii: "5")),
            ("folder/说明.txt", payload, UInt8(ascii: "0")),
        ]).write(to: archiveURL)

        let archive = try ArchiveReader(url: archiveURL)
        XCTAssertEqual(archive.format, .tar)
        XCTAssertEqual(archive.entries.count, 2)
        let entry = try XCTUnwrap(archive.entries.first { $0.path == "folder/说明.txt" })
        XCTAssertEqual(entry.kind, .file)
        XCTAssertEqual(entry.uncompressedSize, UInt64(payload.count))
        let streamedPayload = try await archive.data(for: entry)
        XCTAssertEqual(streamedPayload, payload)

        let destination = root.appending(path: "extracted")
        try await archive.extract(to: destination)
        XCTAssertEqual(try Data(contentsOf: destination.appending(path: entry.path)), payload)
    }

    func testTARGNULongNameAndPAXPath() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let payload = Data("extended TAR metadata".utf8)

        let gnuPath = String(repeating: "very-long-directory/", count: 7) + "gnu-file.txt"
        let gnuURL = root.appending(path: "gnu-long-name.tar")
        try makeTAR(entries: [
            ("././@LongLink", Data((gnuPath + "\0").utf8), UInt8(ascii: "L")),
            ("placeholder", payload, UInt8(ascii: "0")),
        ]).write(to: gnuURL)
        let gnuArchive = try ArchiveReader(url: gnuURL)
        XCTAssertEqual(gnuArchive.entries.map(\.path), [gnuPath])
        let gnuData = try await gnuArchive.data(for: gnuArchive.entries[0])
        XCTAssertEqual(gnuData, payload)

        let paxPath = String(repeating: "目录/", count: 30) + "说明.txt"
        let paxURL = root.appending(path: "pax-path.tar")
        let paxMetadata = makePAXRecord(key: "path", value: paxPath)
            + makePAXRecord(key: "mtime", value: "1700000123.75")
        try makeTAR(entries: [
            ("PaxHeaders/file", Data(paxMetadata.utf8), UInt8(ascii: "x")),
            ("placeholder", payload, UInt8(ascii: "0")),
        ]).write(to: paxURL)
        let paxArchive = try ArchiveReader(url: paxURL)
        let paxEntry = try XCTUnwrap(paxArchive.entries.first)
        XCTAssertEqual(paxEntry.path, paxPath)
        XCTAssertEqual(Int(try XCTUnwrap(paxEntry.modificationDate).timeIntervalSince1970), 1_700_000_123)
        let paxData = try await paxArchive.data(for: paxEntry)
        XCTAssertEqual(paxData, payload)
    }

    func testTARUnsafePathsAndLinksAreRejected() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let unsafeURL = root.appending(path: "unsafe.tar")
        try makeTAR(entries: [("../outside.txt", Data("bad".utf8), UInt8(ascii: "0"))])
            .write(to: unsafeURL)
        XCTAssertThrowsError(try ArchiveReader(url: unsafeURL)) { error in
            XCTAssertEqual((error as? ArchiveError)?.code, .unsafePath)
        }

        let linkURL = root.appending(path: "link.tar")
        try makeTAR(entries: [("link", Data(), UInt8(ascii: "2"))]).write(to: linkURL)
        XCTAssertThrowsError(try ArchiveReader(url: linkURL)) { error in
            XCTAssertEqual((error as? ArchiveError)?.code, .unsafeLink)
        }
    }

    func testTARProgressCancellationRemovesPartialOutput() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let archiveURL = root.appending(path: "cancel.tar")
        try makeTAR(entries: [
            ("large.bin", Data(repeating: 0x5a, count: 512 * 1024), UInt8(ascii: "0")),
        ]).write(to: archiveURL)
        let archive = try ArchiveReader(url: archiveURL)
        let destination = root.appending(path: "cancelled")
        do {
            try await archive.extract(to: destination) { _ in false }
            XCTFail("Expected cancellation")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .cancelled)
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.appending(path: "large.bin").path))
    }

    func testGZIPStreamingExtractionAndResourceLimit() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let archiveURL = root.appending(path: "payload.txt.gz")
        let gzip = try XCTUnwrap(Data(base64Encoded: "H4sIAAAAAAAAA3OP8gxQKEiszMlPTOECAOZRTLUNAAAA"))
        try gzip.write(to: archiveURL)

        let archive = try ArchiveReader(url: archiveURL)
        XCTAssertEqual(archive.format, .gzip)
        XCTAssertEqual(archive.entries.map(\.path), ["payload.txt"])
        let expected = Data("GZIP payload\n".utf8)
        let streamedPayload = try await archive.data(for: archive.entries[0])
        XCTAssertEqual(streamedPayload, expected)

        let destination = root.appending(path: "extracted")
        try await archive.extract(to: destination)
        XCTAssertEqual(try Data(contentsOf: destination.appending(path: "payload.txt")), expected)

        let limits = ArchiveLimits(maximumEntrySize: 8)
        XCTAssertThrowsError(try ArchiveReader(url: archiveURL, options: .init(limits: limits))) { error in
            XCTAssertEqual((error as? ArchiveError)?.code, .resourceLimit)
        }
    }

    func testGZIPCorruptionAndForgedFooterAreRejectedWhileStreaming() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let original = try XCTUnwrap(Data(base64Encoded: "H4sIAAAAAAAAA3OP8gxQKEiszMlPTOECAOZRTLUNAAAA"))

        var corrupt = original
        corrupt[corrupt.count - 8] ^= 0xff
        let corruptURL = root.appending(path: "corrupt.gz")
        try corrupt.write(to: corruptURL)
        let corruptArchive = try ArchiveReader(url: corruptURL)
        do {
            _ = try await corruptArchive.data(for: corruptArchive.entries[0])
            XCTFail("Expected corruptArchive")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .corruptArchive)
        }

        let largeGZIP = try XCTUnwrap(Data(base64Encoded: "H4sIAAAAAAAAA+3BMQEAAADCoPVP7W0HoAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA3gAi6g7iAAAEAA=="))
        var forgedSize = largeGZIP
        forgedSize.replaceSubrange((forgedSize.count - 4)..<forgedSize.count, with: [1, 0, 0, 0])
        let forgedURL = root.appending(path: "forged.gz")
        try forgedSize.write(to: forgedURL)
        let limits = ArchiveLimits(
            maximumEntrySize: 1_024,
            maximumTotalSize: 1_024
        )
        let forgedArchive = try ArchiveReader(url: forgedURL, options: .init(limits: limits))
        do {
            _ = try await forgedArchive.data(for: forgedArchive.entries[0])
            XCTFail("Expected resourceLimit")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .resourceLimit)
        }
    }

    func testGZIPOriginalFilenameAndTARPayloadRoundTrip() async throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let document = Data("nested archive document".utf8)
        let tar = makeTAR(entries: [
            ("Documents/readme.txt", document, UInt8(ascii: "0")),
        ])
        let archiveURL = root.appending(path: "download.gz")
        try makeGZIP(payload: tar, originalFilename: "bundle.tar").write(to: archiveURL)

        let gzipArchive = try ArchiveReader(url: archiveURL)
        XCTAssertEqual(gzipArchive.format, .gzip)
        XCTAssertEqual(gzipArchive.entries.map(\.path), ["bundle.tar"])
        let streamedTAR = try await gzipArchive.data(for: gzipArchive.entries[0])
        XCTAssertEqual(streamedTAR, tar)

        let tarURL = root.appending(path: "bundle.tar")
        try streamedTAR.write(to: tarURL)
        let tarArchive = try ArchiveReader(url: tarURL, format: .tar)
        XCTAssertEqual(tarArchive.entries.map(\.path), ["Documents/readme.txt"])
        let nestedDocument = try await tarArchive.data(for: tarArchive.entries[0])
        XCTAssertEqual(nestedDocument, document)

        let tgzURL = root.appending(path: "fallback.tgz")
        try makeGZIP(payload: tar).write(to: tgzURL)
        XCTAssertEqual(try ArchiveReader(url: tgzURL).entries.map(\.path), ["fallback.tar"])
    }

    func testEmptyTARIsValid() throws {
        let root = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let archiveURL = root.appending(path: "empty.tar")
        try Data(repeating: 0, count: 1_024).write(to: archiveURL)
        let archive = try ArchiveReader(url: archiveURL)
        XCTAssertEqual(archive.format, .tar)
        XCTAssertTrue(archive.entries.isEmpty)
    }

    func testLibarchive7ZipListingStreamingAndExtraction() async throws {
        let archive = try ArchiveReader(url: fixture("libarchive-standard.7z"))
        XCTAssertEqual(archive.format, .sevenZip)
        let entry = try XCTUnwrap(archive.entries.first { $0.path == "目录/内容.txt" })
        let streamed = try await archive.data(for: entry)
        XCTAssertEqual(streamed, Data("libarchive fixture payload\n".utf8))

        let destination = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: destination) }
        try await archive.extract(to: destination)
        XCTAssertEqual(
            try Data(contentsOf: destination.appending(path: "目录/内容.txt")),
            Data("libarchive fixture payload\n".utf8)
        )
    }

    func testLibarchiveEncrypted7ZipPasswords() async throws {
        XCTAssertThrowsError(try ArchiveReader(url: fixture("libarchive-encrypted.7z"))) { error in
            XCTAssertEqual((error as? ArchiveError)?.code, .unsupportedFeature)
        }
        XCTAssertThrowsError(
            try ArchiveReader(
                url: fixture("libarchive-encrypted.7z"),
                options: .init(password: "incorrect")
            )
        ) { error in
            XCTAssertEqual((error as? ArchiveError)?.code, .unsupportedFeature)
        }
        XCTAssertThrowsError(
            try ArchiveReader(
                url: fixture("libarchive-encrypted.7z"),
                options: .init(password: "open-sesame")
            )
        ) { error in
            XCTAssertEqual((error as? ArchiveError)?.code, .unsupportedFeature)
        }
    }

    func testLibarchiveCompressedTARFilters() async throws {
        for name in [
            "libarchive-bundle.tar.xz",
            "libarchive-bundle.tar.bz2",
            "libarchive-bundle.tar.zst",
            "libarchive-bundle.tar.lz4",
        ] {
            let archive = try ArchiveReader(url: fixture(name))
            XCTAssertEqual(archive.format, .tar, name)
            let entry = try XCTUnwrap(
                archive.entries.first { $0.path == "目录/内容.txt" },
                name
            )
            let streamed = try await archive.data(for: entry)
            XCTAssertEqual(streamed, Data("libarchive fixture payload\n".utf8), name)
        }
    }

    func testLibarchiveSecurityLimitsAndCancellation() async throws {
        XCTAssertThrowsError(
            try ArchiveReader(url: fixture("libarchive-unsafe.tar.xz"))
        ) { error in
            XCTAssertEqual((error as? ArchiveError)?.code, .unsafePath)
        }

        XCTAssertThrowsError(
            try ArchiveReader(
                url: fixture("libarchive-standard.7z"),
                options: .init(
                    limits: .init(
                        maximumEntryCount: 100,
                        maximumEntrySize: 4,
                        maximumTotalSize: 100,
                        maximumCompressionRatio: 1_000
                    )
                )
            )
        ) { error in
            XCTAssertEqual((error as? ArchiveError)?.code, .resourceLimit)
        }

        let archive = try ArchiveReader(url: fixture("libarchive-standard.7z"))
        let entry = try XCTUnwrap(archive.entries.first { $0.path == "目录/内容.txt" })
        do {
            try await archive.read(entry) { _ in false }
            XCTFail("Expected cancellation")
        } catch let error as ArchiveError {
            XCTAssertEqual(error.code, .cancelled)
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

    private func makeTAR(entries: [(path: String, data: Data, type: UInt8)]) -> Data {
        var archive = Data()
        for entry in entries {
            var header = Data(repeating: 0, count: 512)
            header.replaceSubrange(0..<min(entry.path.utf8.count, 100), with: entry.path.utf8.prefix(100))
            writeTAROctal(0o644, to: &header, offset: 100, length: 8)
            writeTAROctal(0, to: &header, offset: 108, length: 8)
            writeTAROctal(0, to: &header, offset: 116, length: 8)
            writeTAROctal(UInt64(entry.data.count), to: &header, offset: 124, length: 12)
            writeTAROctal(1_700_000_000, to: &header, offset: 136, length: 12)
            header.replaceSubrange(148..<156, with: Data(repeating: 0x20, count: 8))
            header[156] = entry.type
            header.replaceSubrange(257..<263, with: Data("ustar\0".utf8))
            header.replaceSubrange(263..<265, with: Data("00".utf8))
            let checksum = header.reduce(UInt64(0)) { $0 + UInt64($1) }
            let checksumString = String(format: "%06llo\0 ", checksum)
            header.replaceSubrange(148..<156, with: checksumString.utf8)
            archive.append(header)
            archive.append(entry.data)
            let padding = (512 - entry.data.count % 512) % 512
            archive.append(Data(repeating: 0, count: padding))
        }
        archive.append(Data(repeating: 0, count: 1024))
        return archive
    }

    private func makePAXRecord(key: String, value: String) -> String {
        let body = " \(key)=\(value)\n"
        var length = body.utf8.count + 1
        while true {
            let candidate = "\(length)\(body)"
            let actualLength = candidate.utf8.count
            if actualLength == length { return candidate }
            length = actualLength
        }
    }

    private func makeGZIP(payload: Data, originalFilename: String? = nil) -> Data {
        var archive = Data([0x1f, 0x8b, 0x08, originalFilename == nil ? 0 : 0x08])
        archive.append(contentsOf: [0, 0, 0, 0, 0, 3])
        if let originalFilename {
            archive.append(contentsOf: originalFilename.utf8)
            archive.append(0)
        }

        if payload.isEmpty {
            archive.append(contentsOf: [1, 0, 0, 0xff, 0xff])
        } else {
            var offset = 0
            while offset < payload.count {
                let count = min(65_535, payload.count - offset)
                let isFinal = offset + count == payload.count
                archive.append(isFinal ? 1 : 0)
                archive.appendLE(UInt16(count))
                archive.appendLE(~UInt16(count))
                archive.append(payload[offset..<(offset + count)])
                offset += count
            }
        }
        archive.appendLE(crc32(payload))
        archive.appendLE(UInt32(truncatingIfNeeded: payload.count))
        return archive
    }

    private func crc32(_ data: Data) -> UInt32 {
        var crc = UInt32.max
        for byte in data {
            crc ^= UInt32(byte)
            for _ in 0..<8 {
                crc = (crc >> 1) ^ ((crc & 1) == 0 ? 0 : 0xedb8_8320)
            }
        }
        return ~crc
    }

    private func writeTAROctal(_ value: UInt64, to data: inout Data, offset: Int, length: Int) {
        let digits = String(value, radix: 8)
        let padded = String(repeating: "0", count: max(0, length - digits.count - 1)) + digits + "\0"
        data.replaceSubrange(offset..<(offset + length), with: padded.utf8.prefix(length))
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
