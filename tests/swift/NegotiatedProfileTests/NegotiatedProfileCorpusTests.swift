//
//  Negotiated-profile substrate: the SWIFT READER.
//
//  Unlike the C++ reader, which reuses the C parser object, this one is an
//  INDEPENDENT implementation of the same grammar and the same FNV-1a 64
//  digest. Three readers agreeing on the count and digest is the point; a
//  shared implementation in all three would prove only that the file exists.
//
//  It reads the ONE authoritative corpus at tests/vectors/, resolved from this
//  source file's own path -- NOT from a SwiftPM bundle resource, which cannot
//  reach it (see corpusURL() for the verified reason).
//
import XCTest
import Foundation

final class NegotiatedProfileCorpusTests: XCTestCase {

    /// The declared facts, independently spelled on this side.
    private let declaredCount = 79
    private let declaredDigest: UInt64 = 0xaf21_b873_0806_8ae6

    /// The declared per-record byte cap, part of the grammar. The C reader
    /// enforces the same number as NP_CORPUS_MAX_BYTES.
    private let maxRecordBytes = 64

    // MARK: FNV-1a 64, independently implemented

    private func fnv1a64<S: Sequence>(_ bytes: S) -> UInt64
    where S.Element == UInt8 {
        var h: UInt64 = 14695981039346656037
        for b in bytes {
            h ^= UInt64(b)
            h = h &* 1099511628211
        }
        return h
    }

    func testFNVKnownAnswers() {
        XCTAssertEqual(fnv1a64([UInt8]()), 14695981039346656037,
                       "the empty input must be the FNV offset basis")
        XCTAssertEqual(fnv1a64(Array("abc".utf8)), 0xe71f_a219_0541_574b)
        XCTAssertNotEqual(fnv1a64(Array("ab".utf8)), fnv1a64(Array("abc".utf8)),
                          "a truncation must change the digest")
        XCTAssertNotEqual(fnv1a64(Array("abd".utf8)), fnv1a64(Array("abc".utf8)))
    }

    // MARK: the grammar, independently implemented

    struct Record: Equatable {
        let transport: String
        let media: String
        let property: String
        let value: UInt64
        let bytes: [UInt8]
    }

    enum CorpusError: Error, Equatable {
        case reason(String)
    }

    private static let transports: Set<String> = ["d16", "d18"]
    private static let medias: Set<String> = ["loc01"]
    private static let properties: Set<String> = [
        "timestamp", "type_delta",
        "even_t2", "even_t4", "even_t6",   // the Type is in the TOKEN
        "odd_prop", "odd_hdr", "after_odd", "desync_2prop",
    ]

    /// Canonical decimal: digits only, no leading zero unless the field is "0".
    private func canonicalU64(_ s: Substring) throws -> UInt64 {
        guard !s.isEmpty, s.count <= 20 else { throw CorpusError.reason("value length") }
        if s.first == "0" && s.count != 1 { throw CorpusError.reason("leading zero") }
        var v: UInt64 = 0
        for ch in s.utf8 {
            guard ch >= 0x30, ch <= 0x39 else { throw CorpusError.reason("non-digit") }
            let (m, o1) = v.multipliedReportingOverflow(by: 10)
            guard !o1 else { throw CorpusError.reason("overflow") }
            let (a, o2) = m.addingReportingOverflow(UInt64(ch - 0x30))
            guard !o2 else { throw CorpusError.reason("overflow") }
            v = a
        }
        return v
    }

    /// Canonical hex: lowercase only, even length, at least one byte.
    private func canonicalHex(_ s: Substring) throws -> [UInt8] {
        guard !s.isEmpty, s.count % 2 == 0 else { throw CorpusError.reason("hex length") }
        guard s.count / 2 <= maxRecordBytes else {
            throw CorpusError.reason("record exceeds the declared byte cap")
        }
        var out: [UInt8] = []
        out.reserveCapacity(s.count / 2)
        var hi: UInt8? = nil
        for ch in s.utf8 {
            let nib: UInt8
            switch ch {
            case 0x30...0x39: nib = ch - 0x30
            case 0x61...0x66: nib = ch - 0x61 + 10
            default: throw CorpusError.reason("non-canonical hex")  // uppercase included
            }
            if let h = hi { out.append((h << 4) | nib); hi = nil } else { hi = nib }
        }
        return out
    }

    /// Exactly `want` single-space-separated non-empty fields, no tabs.
    private func fields(_ line: Substring, _ want: Int) throws -> [Substring] {
        if line.contains("\t") { throw CorpusError.reason("tab") }
        let parts = line.split(separator: " ", omittingEmptySubsequences: false)
        guard parts.count == want else { throw CorpusError.reason("arity") }
        for p in parts where p.isEmpty { throw CorpusError.reason("empty field") }
        return parts
    }

    func parse(_ data: Data) throws -> (records: [Record], digest: UInt64) {
        let digest = fnv1a64(data)
        guard let last = data.last, last == 0x0a else {
            throw CorpusError.reason("no final newline")
        }
        guard let text = String(data: data, encoding: .utf8) else {
            throw CorpusError.reason("not utf-8")
        }
        // dropLast() removes the empty trailing element the final newline makes,
        // so a missing terminator cannot masquerade as a blank line.
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
                        .dropLast()
        guard lines.count >= 3 else { throw CorpusError.reason("too few lines") }
        guard lines[lines.startIndex] == "np-corpus 1" else {
            throw CorpusError.reason("magic")
        }
        let head = try fields(lines[lines.startIndex + 1], 2)
        guard head[0] == "count" else { throw CorpusError.reason("count keyword") }
        let declared = try canonicalU64(head[1])
        guard declared > 0, declared <= 128 else { throw CorpusError.reason("count range") }
        guard lines.count == Int(declared) + 3 else {
            throw CorpusError.reason("line count disagrees with count")
        }
        guard lines[lines.startIndex + lines.count - 1] == "end" else {
            throw CorpusError.reason("terminator")
        }

        var recs: [Record] = []
        for i in 0..<Int(declared) {
            let f = try fields(lines[lines.startIndex + 2 + i], 5)
            let t = String(f[0]), m = String(f[1]), p = String(f[2])
            guard Self.transports.contains(t) else { throw CorpusError.reason("transport") }
            guard Self.medias.contains(m) else { throw CorpusError.reason("media") }
            guard Self.properties.contains(p) else { throw CorpusError.reason("property") }
            recs.append(Record(transport: t, media: m, property: p,
                               value: try canonicalU64(f[3]),
                               bytes: try canonicalHex(f[4])))
        }
        // the semantic key is unique
        var seen = Set<String>()
        for r in recs {
            let key = "\(r.transport)|\(r.media)|\(r.property)|\(r.value)"
            guard seen.insert(key).inserted else {
                throw CorpusError.reason("duplicate semantic key")
            }
        }
        return (recs, digest)
    }

    // MARK: the checked-in corpus

    /// The single authoritative corpus, resolved from this SOURCE FILE's path
    /// rather than the working directory, so the test is location-independent
    /// and no copy of the corpus exists anywhere.
    ///
    /// A SwiftPM bundle resource cannot reach it: a resource path outside the
    /// target directory is refused, and a symlink placed inside the target is
    /// copied verbatim into the bundle as a dangling relative link (verified).
    private func corpusURL() -> URL {
        // .../tests/swift/NegotiatedProfileTests/<this file>
        var u = URL(fileURLWithPath: #filePath)
        u.deleteLastPathComponent()            // NegotiatedProfileTests
        u.deleteLastPathComponent()            // swift
        u.deleteLastPathComponent()            // tests
        return u.appendingPathComponent("vectors/negotiated_profile.vectors")
    }

    private func corpusData() throws -> Data {
        let url = corpusURL()
        guard FileManager.default.fileExists(atPath: url.path) else {
            XCTFail("corpus not found at \(url.path)")
            throw CorpusError.reason("missing corpus")
        }
        return try Data(contentsOf: url)
    }

    func testCorpusPathIsTheAuthoritativeOne() {
        XCTAssertTrue(corpusURL().path.hasSuffix(
            "tests/vectors/negotiated_profile.vectors"),
            "the Swift reader must read the one authoritative corpus")
    }

    func testCheckedInCorpusCountAndDigest() throws {
        let (recs, digest) = try parse(try corpusData())
        XCTAssertEqual(recs.count, declaredCount)
        XCTAssertEqual(digest, declaredDigest,
                       "the Swift reader must agree with the C and C++ readers")
    }

    func testRejectsTruncation() throws {
        var data = try corpusData()
        data.removeLast()                     // drop the final newline
        XCTAssertThrowsError(try parse(data))
    }

    func testRejectsGrammarViolations() {
        func rejects(_ s: String, _ what: String) {
            XCTAssertThrowsError(try parse(Data(s.utf8)), what)
        }
        rejects("np-corpus 1\ncount 1\nd16 loc01 timestamp 64 4040\nend\nx\n",
                "trailing garbage")
        rejects("np-corpus 1\ncount 1\nd16 loc01 timestamp 64 4040\n"
                + "d18 loc01 timestamp 64 40\nend\n", "trailing record")
        rejects("np-corpus 1\ncount 2\nd16 loc01 timestamp 64 4040\nend\n",
                "count long")
        rejects("np-corpus 1\ncount 1\nd16 loc01 timestamp 64 4A40\nend\n",
                "uppercase hex")
        rejects("np-corpus 1\ncount 1\nd16 loc01 timestamp 64 404\nend\n",
                "odd hex")
        rejects("np-corpus 1\ncount 1\nd16 loc01 timestamp 064 4040\nend\n",
                "leading zero")
        rejects("np-corpus 1\ncount 1\nd17 loc01 timestamp 64 4040\nend\n",
                "unknown transport")
        rejects("np-corpus 1\ncount 1\nd16 loc01 nope 64 4040\nend\n",
                "unknown property")
        rejects("np-corpus 1\ncount 2\nd16 loc01 timestamp 64 4040\n"
                + "d16 loc01 timestamp 64 4040\nend\n", "duplicate key")
        rejects("np-corpus 1\ncount 1\nd16  loc01 timestamp 64 4040\nend\n",
                "double space")
        rejects("np-corpus 1\ncount 1\nd16 loc01 timestamp 64 4040\nend",
                "no final newline")
        rejects("", "empty")
    }

    /// The declared per-record byte cap, at its exact boundary -- the same
    /// 64/65 pair the C reader probes.
    func testRecordByteCapBoundary() throws {
        func corpus(withBytes n: Int) -> Data {
            let hex = String(repeating: "a0", count: n)
            return Data("np-corpus 1\ncount 1\nd16 loc01 timestamp 1 \(hex)\nend\n".utf8)
        }
        XCTAssertNoThrow(try parse(corpus(withBytes: maxRecordBytes)),
                         "\(maxRecordBytes) bytes must be accepted")
        XCTAssertThrowsError(try parse(corpus(withBytes: maxRecordBytes + 1)),
                            "\(maxRecordBytes + 1) bytes must be refused")
    }

    /// A field containing an embedded NUL must be REFUSED, not truncated to a
    /// legal token. This is the case where a C reader using strcmp on a copied
    /// span would disagree with this reader about the same bytes.
    func testRejectsEmbeddedNul() {
        func withNul(_ prefix: String, _ suffix: String) -> Data {
            var d = Data(prefix.utf8)
            d.append(0x00)
            d.append(contentsOf: Array(suffix.utf8))
            return d
        }
        XCTAssertThrowsError(try parse(withNul(
            "np-corpus 1\ncount 1\nd16", "x loc01 timestamp 1 40\nend\n")))
        XCTAssertThrowsError(try parse(withNul(
            "np-corpus 1\ncount 1\nd16 loc01", "x timestamp 1 40\nend\n")))
        XCTAssertThrowsError(try parse(withNul(
            "np-corpus 1\ncount 1\nd16 loc01 timestamp", "x 1 40\nend\n")))
    }
}
