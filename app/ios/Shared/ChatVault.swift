import CryptoKit
import Foundation
import Security
import UserNotifications
import Vigil

enum Flock {
    static func hex(_ flock: UInt64) -> String { String(flock, radix: 16) }

    static func hex(_ flock: String) -> String { hex(UInt64(flock, radix: 16) ?? 0) }

    static func id(_ flock: UInt64, _ peer: UInt8) -> String { "\(hex(flock))-\(ChatVault.hex(peer))" }

    static func id(_ flock: String, _ peer: String) -> String { "\(flock)-\(peer)" }

    static func of(_ id: String) -> String { String(id.prefix { $0 != "-" }) }

    static func number(_ id: String) -> UInt64 { UInt64(of(id), radix: 16) ?? 0 }

    static func local(_ id: String) -> String {
        guard let dash = id.firstIndex(of: "-") else { return id }

        return String(id[id.index(after: dash)...])
    }

    static func peer(_ id: String) -> UInt8 { UInt8(local(id), radix: 16) ?? 0 }

    static func valid(_ id: String) -> Bool {
        guard id.range(of: "^[0-9a-f]{1,16}-[0-9a-f]{2}$", options: .regularExpression) != nil else {
            return false
        }

        return peer(id) > 0 && peer(id) < 0xff
    }
}

final class ChatVault {
    struct Locked: Error, CustomStringConvertible {
        var description: String { "Unlock your device to read your chats." }
    }

    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    struct Lost {
        var chats = Set<String>()
        var files = [String: String]()
        var state = false

        var isEmpty: Bool { chats.isEmpty && files.isEmpty && !state }
    }

    struct Quote {
        let author: String
        let hash: String

        var peer: UInt8 {
            UInt8(author, radix: 16) ?? 0
        }

        var digest: UInt64 {
            UInt64(hash, radix: 16) ?? 0
        }
    }

    private struct Conversation {
        var key: SymmetricKey
        var count: UInt64
        var read: UInt64
        var flags: UInt64
        var unread: Int64 = -1
        var last: Data?
        var ambry: UInt32 = 0
        var topics: Data?
    }

    private final class History {
        let chat: String
        let epoch: Int64
        var raw: [[String: Any]]?
        var offset: UInt64 = 0
        var seq: UInt64 = 0
        var stamp: Int64 = 0
        var pinned: Int64 = 0
        var unread: Int64 = 0
        var damaged = false
        var messages = [[String: Any]]()
        var stamps = [Int64]()
        var index = [Int64: Int]()
        var digests = [Int: String]()
        var topics = [[String: Any]]()
        var unreads = [String: Int64]()
        var reads = [String: UInt64]()

        init(chat: String, epoch: Int64) {
            self.chat = chat
            self.epoch = epoch
        }
    }

    static let shared = ChatVault()
    static let selfAuthor = "00"
    static let companionRef = Int64(1) << 62

    static let defaults = UserDefaults(suiteName: TunnelKeychain.group) ?? .standard

    private static let service = "se.sanctorum.vigil.chat"
    private static let keyAad = Data("VIGIL.CHAT.KEY".utf8)
    private static let metaAad = Data("VIGIL.CHAT.META".utf8)
    private static let recordAad = Data("VIGIL.CHAT.V1".utf8)
    private static let inboxAad = Data("VIGIL.CHAT.INBOX".utf8)
    private static let outboxAad = Data("VIGIL.CHAT.OUTBOX".utf8)
    private static let queuedAad = Data("VIGIL.CHAT.QUEUED".utf8)
    private static let receiptsAad = Data("VIGIL.CHAT.RECEIPTS".utf8)
    private static let groupsAad = Data("VIGIL.CHAT.GROUPS".utf8)
    private static let acceptedAad = Data("VIGIL.CHAT.ACCEPTED".utf8)
    private static let ignoredAad = Data("VIGIL.CHAT.IGNORED".utf8)
    private static let groupExpire: Int64 = 3 * 24 * 60 * 60 * 1000
    private static let dataAad = Data("VIGIL.CHAT.DATA".utf8)
    private static let avatarAad = Data("VIGIL.CHAT.AVATAR".utf8)
    private static let nameAad = Data("VIGIL.CHAT.NAME".utf8)
    private static let mirrorAad = Data("VIGIL.CHAT.MIRROR".utf8)
    private static let ambryAad = Data("VIGIL.CHAT.AMBRY".utf8)
    private static let lostAad = Data("VIGIL.CHAT.LOST".utf8)
    private static let safetyAad = Data("VIGIL.CHAT.SAFETY".utf8)
    private static let wrapped = 12 + 32 + 16
    private static let creating: Set<String> = ["msg", "member", "topic"]
    private static let batchMax = 12 * 1024
    private static let truncated: UInt64 = 1

    private let dir: URL?
    private let inbox: URL?
    private let mirror: URL?
    private let avatars: URL?
    private let names: URL?
    private let safety: URL?
    private var open = [String: Conversation]()
    private var history: History?
    private var holding = 0
    private var refs: Int64 = 0
    private let mutex = NSRecursiveLock()
    private let owner = Bundle.main.bundleURL.pathExtension != "appex"

    var hardware: Bool { SecureEnclave.isAvailable }

    private init() {
        let base = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: TunnelKeychain.group)?
            .appendingPathComponent("chats", isDirectory: true)

        dir = base
        inbox = base?.appendingPathComponent("inbox", isDirectory: true)
        mirror = base?.appendingPathComponent("mirror", isDirectory: true)
        avatars = base?.appendingPathComponent("avatars", isDirectory: true)
        names = base?.appendingPathComponent("names", isDirectory: true)
        safety = base?.appendingPathComponent("safety", isDirectory: true)

        for case var url? in [dir, inbox, mirror, avatars, names, safety] {
            try? FileManager.default.createDirectory(
                at: url, withIntermediateDirectories: true,
                attributes: [.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication])

            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try? url.setResourceValues(values)
        }
    }

    func prepare() throws {
        try mutex.withLock {
            _ = try agreementPublic()
            _ = try outboxKey()
            try exclusive { try settleQueued() }
            try dropOldGroups()
            try migrateFlock()
        }
    }

    private func dropOldGroups() throws {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: try folder(dir).path)) ?? []

        for name in names where name.range(of: "^g[0-9a-f]{4}(\\.log|-[0-9a-f]{16}\\.dat)$",
                                           options: .regularExpression) != nil {
            let chat = String(name.prefix(5))

            try remove("meta.\(chat)")
            try remove("key.\(chat)")
            try wipe(try folder(dir).appendingPathComponent(name))
        }
    }

    func lock() {
        mutex.withLock {
            open.removeAll()
            history = nil
        }
    }

    func ref() -> Int64 {
        return mutex.withLock {
            refs = max(refs + 1, Int64(Date().timeIntervalSince1970 * 1000) * 1000)
            return refs
        }
    }

    func deliver(chat: String, peer: UInt8, nick: String, text: String, time: Int64,
                 kind: String = "", data: Data? = nil, media: String = "",
                 name: String = "", quote: Quote? = nil, topic: String = "",
                 color: Int = 0) throws {
        try mutex.withLock {
            if kind == "ambry", try !rotated(chat, text) {
                return
            }

            var message: [String: Any] = [
                "chat": chat, "peer": Self.hex(peer), "nick": nick, "text": text, "time": time,
                "kind": kind,
            ]

            if data != nil {
                message["media"] = media
                message["name"] = name
            }

            if let quote {
                message["qa"] = quote.author
                message["qh"] = quote.hash
            }

            if !topic.isEmpty {
                message["tp"] = topic
                message["color"] = color
            }

            try commit(chat, record(message, data: data), data: data)
        }
    }

    func echo(chat: String, ref: Int64, text: String, time: Int64, quote: Quote?, topic: String,
              to: [String], serial: Int, data: Data? = nil, media: String = "", name: String = "") throws {
        try mutex.withLock {
            var message: [String: Any] = [
                "chat": chat, "peer": Self.selfAuthor, "dir": "out", "ref": ref, "text": text, "time": time,
            ]
            var entry: [String: Any] = ["peer": chat, "ref": ref, "text": text]

            if data != nil {
                message["media"] = media
                message["name"] = name
                entry["echo"] = true
            }

            if let quote {
                message["qa"] = quote.author
                message["qh"] = quote.hash
                entry["qa"] = quote.author
                entry["qh"] = quote.hash
            }

            if !topic.isEmpty {
                message["tp"] = topic
                entry["tp"] = topic
            }

            let group = Self.group(chat) != nil

            if group {
                message["to"] = to
            }

            try commit(chat, record(message, data: data), data: data)

            try exclusive {
                var list = try outboxList()

                if !group {
                    list.append(entry)
                }

                for member in to where group {
                    var copy = entry
                    copy["peer"] = Flock.id(Flock.of(chat), member)
                    copy["chat"] = chat
                    copy["gs"] = serial
                    copy["at"] = time
                    list.append(copy)
                }

                try outboxSave(list)
            }

            if !group {
                try accept(chat, true)
            }
        }
    }

    private func record(_ message: [String: Any], data: Data?) throws -> [String: Any] {
        let chat = message["chat"] as? String ?? message["peer"] as? String ?? ""
        let kind = message["kind"] as? String ?? ""
        let out = message["dir"] as? String == "out"
        let ref = out ? ((message["ref"] as? NSNumber)?.int64Value ?? self.ref()) : self.ref()
        let peer = message["peer"] as? String ?? ""
        let text = message["text"] as? String ?? ""
        let time = (message["time"] as? NSNumber)?.int64Value ?? 0

        if kind == "retract" {
            var record: [String: Any] = ["t": "retract", "ref": ref, "peer": peer, "hash": text]

            if let topic = message["tp"] as? String {
                record["tp"] = topic
            }

            return record
        }

        if kind == "topic" {
            return [
                "t": "topic", "ref": ref, "dir": out ? "out" : "in", "tp": message["tp"] as? String ?? "",
                "name": text, "color": (message["color"] as? NSNumber)?.intValue ?? 0, "time": time,
            ]
        }

        var record: [String: Any] = [
            "t": kind.isEmpty ? "msg" : "member", "kind": kind, "dir": out ? "out" : "in", "ref": ref,
            "peer": peer, "text": text, "nick": message["nick"] as? String ?? "", "time": time,
        ]

        if let quote = Self.quote(message["qa"] as? String, message["qh"] as? String) {
            record["qa"] = quote.author
            record["qh"] = quote.hash
        }

        if let topic = message["tp"] as? String {
            record["tp"] = topic
        }

        if let to = message["to"] as? [String] {
            record["to"] = to
        }

        if let data {
            Self.described(&record, chat, media: message["media"] as? String ?? "file",
                           name: message["name"] as? String ?? "file", data: data)
        }

        return record
    }

    private static func described(_ record: inout [String: Any], _ chat: String, media: String, name: String,
                                  data: Data) {
        let ref = (record["ref"] as? NSNumber)?.int64Value ?? 0

        record["data"] = String(format: "%@-%016llx.dat", chat, UInt64(bitPattern: ref))
        record["media"] = media
        record["name"] = name
        record["size"] = data.count

        if media == "image" {
            record["ch"] = String(format: "%016llx", contentHash(data))
        }
    }

    private func commit(_ chat: String, _ record: [String: Any], data: Data? = nil) throws {
        let ref = (record["ref"] as? NSNumber)?.int64Value ?? 0
        let mirrored = ref & Self.companionRef == 0 ? data : nil

        try enqueue(["op": "rec", "chat": chat, "rec": record], data: data, mirrored: mirrored)
    }

    private func enqueue(_ op: [String: Any], data: Data? = nil, mirrored: Data? = nil) throws {
        var entry = op

        if let data {
            entry["data"] = data.base64EncodedString()
        }

        try sequenced {
            try inboxWrite(entry)
            try mirrorWrite(op, mirrored)
        }

        guard holding == 0 else { return }

        do {
            try merge()
        } catch is Locked {
            return
        }
    }

    func emit(_ op: [String: Any], _ data: Data? = nil) {
        emit { (op, data) }
    }

    private func emit(_ make: () -> ([String: Any], Data?)) {
        do {
            try sequenced {
                let (op, data) = make()

                try mirrorWrite(op, data)
            }
        } catch {
            NSLog("vigil: could not queue a change for our desktop, starting a full resync: \(error)")
            Self.resyncWanted()
        }
    }

    private func inboxWrite(_ entry: [String: Any]) throws {
        let plain = try JSONSerialization.data(withJSONObject: entry)
        let inbox = try folder(inbox)

        let sealed = try seal(plain, to: agreementPublic(), info: Self.inboxAad)
        try write(inbox.appendingPathComponent(String(format: "%016llx.box", try Self.next(inbox))), sealed)
    }

    private func mirrorWrite(_ op: [String: Any], _ data: Data?) throws {
        guard Self.linked else { return }

        let json = try JSONSerialization.data(withJSONObject: op)
        let mirror = try folder(mirror)
        var plain = Data(Self.bytes(UInt32(json.count)))

        plain.append(json)

        if let data {
            plain.append(data)
        }

        let name = String(format: "%016llx.box", try Self.next(mirror))

        try write(mirror.appendingPathComponent(name), sealBox(plain, key: outboxKey(), aad: Self.mirrorAad))
    }

    private static var linked: Bool {
        guard var config = TunnelKeychain.load(id: "companion") else { return false }

        TunnelKeychain.wipe(&config)

        return true
    }

    private static func next(_ folder: URL) throws -> UInt64 {
        let names = try FileManager.default.contentsOfDirectory(atPath: folder.path)

        return (names.compactMap { UInt64($0.dropLast(4), radix: 16) }.max() ?? 0) + 1
    }

    func mirrorNext() throws -> (name: String, json: Data, data: Data?)? {
        return try mutex.withLock {
            let mirror = try folder(mirror)
            let names = try FileManager.default.contentsOfDirectory(atPath: mirror.path)

            guard let name = names.filter({ $0.hasSuffix(".box") }).sorted().first else { return nil }

            let plain = try openBox(Data(contentsOf: mirror.appendingPathComponent(name)), key: outboxKey(),
                                    aad: Self.mirrorAad)
            let bytes = [UInt8](plain)

            guard bytes.count >= 4 else { return (name, Data(), nil) }

            let len = Int(bytes[0]) << 24 | Int(bytes[1]) << 16 | Int(bytes[2]) << 8 | Int(bytes[3])

            guard 4 + len <= bytes.count else { return (name, Data(), nil) }

            let data = 4 + len < bytes.count ? Data(bytes[(4 + len)...]) : nil

            return (name, Data(bytes[4..<(4 + len)]), data)
        }
    }

    func mirrorDone(_ name: String) throws {
        try mutex.withLock {
            try wipe(try folder(mirror).appendingPathComponent(name))
        }
    }

    func mirrorDrain(room: Int, _ send: (Data, Data?) -> Bool, _ mark: (Bool) -> Bool) throws -> Int {
        return try mutex.withLock {
            var room = room

            while let next = try mirrorNext() {
                let op = (try? JSONSerialization.jsonObject(with: next.json)) as? [String: Any] ?? [:]
                let size = next.json.count + (next.data?.count ?? 0)
                let kind = op["op"] as? String ?? ""

                if kind == "reset" || kind == "synced" {
                    guard mark(kind == "synced") else { return room }
                } else if !op.isEmpty {
                    if try Self.skipped(op, lostLoad()) || size > room ||
                        next.json.count > Int(VIGIL_COMPANION_JSON_MAX) {
                        try lose(op)
                    } else if !send(next.json, next.data) {
                        return room
                    } else {
                        room -= size
                        try sent(op)
                    }
                }

                try mirrorDone(next.name)
            }

            return room
        }
    }

    func lostSet() -> Lost {
        return mutex.withLock { (try? lostLoad()) ?? Lost() }
    }

    private static func opChat(_ op: [String: Any]) -> String {
        if op["op"] as? String == "batch" {
            return ((op["ops"] as? [[String: Any]])?.first?["chat"] as? String) ?? ""
        }

        return op["chat"] as? String ?? ""
    }

    private static func skipped(_ op: [String: Any], _ lost: Lost) -> Bool {
        guard ["rec", "read", "delete", "batch"].contains(op["op"] as? String ?? "") else { return false }

        return lost.chats.contains(opChat(op))
    }

    private func lose(_ op: [String: Any]) throws {
        try exclusive {
            var lost = try lostLoad()
            let chat = Self.opChat(op)

            switch op["op"] as? String ?? "" {
            case "rec", "read", "delete", "batch", "chat":
                lost.chats.insert(chat)

                if let file = (op["rec"] as? [String: Any])?["data"] as? String, !file.isEmpty {
                    lost.files[file] = chat
                }
            case "data":
                if let file = op["file"] as? String {
                    lost.files[file] = chat
                }
            case "group", "name", "avatar", "avatars", "prefs":
                lost.state = true
            default:
                return
            }

            try lostSave(lost)
        }
    }

    private func sent(_ op: [String: Any]) throws {
        switch op["op"] as? String ?? "" {
        case "chat":
            try lostDone(Self.opChat(op))
        case "data":
            try lostDone(op["file"] as? String ?? "")
        case "avatars":
            try lostChange { lost in
                guard lost.state else { return false }

                lost.state = false
                return true
            }
        default:
            return
        }
    }

    private func lostDone(_ name: String) throws {
        try lostChange { $0.chats.remove(name) != nil || $0.files.removeValue(forKey: name) != nil }
    }

    private func lostChange(_ change: (inout Lost) -> Bool) throws {
        try exclusive {
            var lost = try lostLoad()

            guard change(&lost) else { return }

            try lostSave(lost)
        }
    }

    private func lostLoad() throws -> Lost {
        let file = try folder(dir).appendingPathComponent("lost.box")

        guard FileManager.default.fileExists(atPath: file.path) else { return Lost() }

        let json = try JSONSerialization.jsonObject(
            with: openBox(Data(contentsOf: file), key: outboxKey(), aad: Self.lostAad)) as? [String: Any] ?? [:]
        var lost = Lost()

        lost.chats = Set(json["chats"] as? [String] ?? [])
        lost.files = json["files"] as? [String: String] ?? [:]
        lost.state = json["state"] as? Bool ?? false

        return lost
    }

    private func lostSave(_ lost: Lost) throws {
        let json: [String: Any] = ["chats": Array(lost.chats), "files": lost.files, "state": lost.state]

        try write(try folder(dir).appendingPathComponent("lost.box"),
                  sealBox(JSONSerialization.data(withJSONObject: json), key: outboxKey(), aad: Self.lostAad))
    }

    func merge() throws {
        // Only our app writes the logs of our chats, two processes must never append to one.
        guard owner else { return }

        try mutex.withLock {
            try migrateFlock()

            let inbox = try folder(inbox)
            let pending = ((try? FileManager.default.contentsOfDirectory(atPath: inbox.path)) ?? [])
                .filter { $0.hasSuffix(".box") }
                .sorted { ($0.contains("-") ? 0 : 1, $0) < ($1.contains("-") ? 0 : 1, $1) }

            for name in pending {
                let file = inbox.appendingPathComponent(name)
                let plain: Data

                do {
                    plain = try unseal(Data(contentsOf: file), info: Self.inboxAad)
                } catch let error as Locked {
                    throw error
                } catch is CryptoKitError {
                    try wipe(file)
                    continue
                }

                if let entry = (try? JSONSerialization.jsonObject(with: plain)) as? [String: Any] {
                    if entry["op"] != nil {
                        try applyOp(entry)
                    } else {
                        try legacy(entry)
                    }
                }

                try wipe(file)
            }

            let receipts = try folder(dir).appendingPathComponent("receipts.box")

            try exclusive {
                guard FileManager.default.fileExists(atPath: receipts.path) else { return }

                let list = try JSONSerialization.jsonObject(
                    with: openBox(Data(contentsOf: receipts), key: outboxKey(), aad: Self.receiptsAad))
                    as? [[String: Any]] ?? []

                for receipt in list {
                    guard let chat = receipt["peer"] as? String, let kind = receipt["t"] as? String,
                          let ref = (receipt["ref"] as? NSNumber)?.int64Value else { continue }

                    var record: [String: Any] = ["t": kind, "ref": ref]

                    if let member = receipt["member"] as? String {
                        record["member"] = member
                    }

                    try applyOp(["op": "rec", "chat": chat, "rec": record])
                }

                try wipe(receipts)
            }
        }
    }

    private func legacy(_ message: [String: Any]) throws {
        guard let peer = message["peer"] as? String,
              let chat = Self.chat(message["chat"] as? String ?? peer),
              let text = message["text"] as? String else { return }

        if message["kind"] as? String == "ambry", try !rotated(chat, text) {
            return
        }

        let data = (message["data"] as? String).flatMap { Data(base64Encoded: $0) }
        var op: [String: Any] = ["op": "rec", "chat": chat, "rec": try record(message, data: data)]

        if let encoded = message["data"] as? String {
            op["data"] = encoded
        }

        try applyOp(op)
    }

    private func applyOp(_ op: [String: Any]) throws {
        guard let chat = (op["chat"] as? String).flatMap({ Self.chat($0) }) else { return }

        switch op["op"] as? String {
        case "rec":
            guard let record = op["rec"] as? [String: Any], let type = record["t"] as? String else { return }

            if !Self.creating.contains(type), try item("key.\(chat)") == nil {
                return
            }

            if let data = (op["data"] as? String).flatMap({ Data(base64Encoded: $0) }),
               let file = record["data"] as? String {
                try dataWrite(chat, file: file, data)
            }

            try append(chat, record)

            if type == "retract" {
                try retracted(chat, record)
            } else if type == "topic", record["name"] as? String ?? "" == "" {
                try topicWipe(chat, topic: record["tp"] as? String ?? "")
            }
        case "delete":
            try wipeChat(chat)
        case "read":
            try readMeta(chat, topic: op["topic"] as? String ?? "")
        default:
            break
        }
    }

    private func rotated(_ chat: String, _ generation: String) throws -> Bool {
        var known = false

        try exclusive {
            let file = try folder(dir).appendingPathComponent("ambry.box")
            var all = [String: Any]()

            if FileManager.default.fileExists(atPath: file.path) {
                all = try JSONSerialization.jsonObject(
                    with: openBox(Data(contentsOf: file), key: outboxKey(), aad: Self.ambryAad)) as? [String: Any] ?? [:]
            }

            let before = all[chat] as? String ?? ""

            guard before != generation else { return }

            all[chat] = generation
            try write(file, sealBox(JSONSerialization.data(withJSONObject: all), key: outboxKey(), aad: Self.ambryAad))
            known = !before.isEmpty
        }

        return known
    }

    private final class Resync {
        let full: Bool
        var asked = 0
        var chats: [String]?
        var chat = 0
        var file = 0
        var files = [(String, String)]()

        init(full: Bool) {
            self.full = full
        }

        var what: String {
            if full {
                return "full resync of our desktop"
            }

            return "repair of what our desktop missed"
        }
    }

    private var resync: Resync?
    private static let resyncQueue = 8 * 1024 * 1024

    func resyncStep() {
        guard owner, Self.linked else { return }

        do {
            while try mutex.withLock({ try resyncRun() }) {}
        } catch is Locked {
            return
        } catch {
            NSLog("vigil: resync of our desktop failed, trying again: \(error)")
        }
    }

    private func resyncRun() throws -> Bool {
        let asked = state()["resync"] as? Int ?? 0

        if resync == nil, asked > 0 {
            resync = Resync(full: true)
            resync?.asked = asked
        }

        if resync == nil, mirrorSize() == 0, !lostSet().isEmpty {
            resync = Resync(full: false)
        }

        guard let job = resync, mirrorSize() < Self.resyncQueue else { return false }

        guard let chats = job.chats else {
            guard try resyncHead(job) else { return false }

            NSLog("vigil: \(job.what) started, \(job.chats?.count ?? 0) chats")
            return true
        }

        if job.chat < chats.count {
            var files = [String]()

            guard try dumpChat(chats[job.chat], files: &files) else { return false }

            if job.full {
                job.files += files.map { (chats[job.chat], $0) }
            }

            job.chat += 1
            NSLog("vigil: \(job.what): queued chat \(job.chat) of \(chats.count)")
            return true
        }

        if job.file < job.files.count {
            let (chat, file) = job.files[job.file]

            _ = try dumpData(chat, file: file)
            job.file += 1
            NSLog("vigil: \(job.what): queued file \(job.file) of \(job.files.count)")
            return true
        }

        emit(["op": "synced"])

        if job.full {
            _ = state { all in
                if all["resync"] as? Int ?? 0 == job.asked {
                    all["resync"] = 0
                }
            }
        }

        resync = nil
        NSLog("vigil: \(job.what) is queued for our desktop")

        return false
    }

    private func resyncHead(_ job: Resync) throws -> Bool {
        var lost = lostSet()

        if job.full || lost.state {
            try merge()

            var copied: Head?

            // Nothing may reach our desktop between our copy of the head and its ops.
            try sequenced {
                let pending = (try? FileManager.default.contentsOfDirectory(atPath: folder(inbox).path)) ?? []

                guard !pending.contains(where: { $0.hasSuffix(".box") }) else { return }

                let copy = try dumpHead()

                if job.full {
                    try mirrorWrite(["op": "reset"], nil)
                }

                for (op, data) in copy.ops {
                    try mirrorWrite(op, data)
                }

                copied = copy
            }

            guard let head = copied else { return false }

            if job.full {
                job.chats = head.chats
                return true
            }

            lost = lostSet()
        }

        job.chats = Array(lost.chats)
        job.files = lost.files.map { ($0.value, $0.key) }

        return true
    }

    func state(_ change: (inout [String: Any]) -> Void = { _ in }) -> [String: Any] {
        var all = [String: Any]()

        do {
            try exclusive(lock: "state") {
                let file = try folder(dir).appendingPathComponent("state.json")
                let stored = try? Data(contentsOf: file)

                if let stored {
                    all = (try? JSONSerialization.jsonObject(with: stored)) as? [String: Any] ?? [:]
                } else {
                    all = Self.migrate()
                }

                let before = all as NSDictionary

                change(&all)

                if stored == nil || !before.isEqual(to: all) {
                    try write(file, JSONSerialization.data(withJSONObject: all))
                }
            }
        } catch {
            NSLog("vigil: could not keep the shared chat state: \(error)")
        }

        return all
    }

    private static func migrate() -> [String: Any] {
        let keys = ["unseen": "chatUnseen", "names": namesKey, "mutes": mutesKey, "stars": starsKey,
                    "compacts": compactsKey, "order": orderKey]
        var all = [String: Any]()

        for (key, old) in keys {
            all[key] = defaults.object(forKey: old) ?? UserDefaults.standard.object(forKey: old)
            defaults.removeObject(forKey: old)
            UserDefaults.standard.removeObject(forKey: old)
        }

        all["resync"] = defaults.bool(forKey: "chatResync") ? 1 : 0
        defaults.removeObject(forKey: "chatResync")

        return all
    }

    func unseen(_ change: (inout [String: Int]) -> Void = { _ in }) -> [String: Int] {
        let all = state { all in
            var counts = all["unseen"] as? [String: Int] ?? [:]

            change(&counts)
            all["unseen"] = counts
        }

        return all["unseen"] as? [String: Int] ?? [:]
    }

    static func resyncWanted() {
        _ = shared.state { $0["resync"] = ($0["resync"] as? Int ?? 0) + 1 }
    }

    struct Head {
        var ops = [([String: Any], Data?)]()
        var chats = [String]()
    }

    private func dumpHead() throws -> Head {
        return try mutex.withLock {
            var head = Head()
            let files = (try? FileManager.default.contentsOfDirectory(atPath: folder(avatars).path)) ?? []
            let faces = files.filter { $0.hasSuffix(".box") }.map { String($0.dropLast(4)) }.filter { Flock.valid($0) }

            head.ops.append((["op": "avatars", "peers": faces], nil))
            head.ops.append((Self.prefs(), nil))

            for (chat, info) in try groups() {
                head.ops.append((["op": "group", "chat": chat, "info": info], nil))
            }

            for (peer, name) in try peerNames() {
                if var entry = name as? [String: Any] {
                    entry["op"] = "name"
                    entry["peer"] = peer
                    head.ops.append((entry, nil))
                }
            }

            for peer in faces {
                guard let data = try peerAvatar(peer) else { continue }

                head.ops.append((["op": "avatar", "peer": peer], data))
            }

            let names = (try? FileManager.default.contentsOfDirectory(atPath: folder(dir).path)) ?? []

            head.chats = names.filter { $0.hasSuffix(".log") }.compactMap { Self.chat(String($0.dropLast(4))) }

            return head
        }
    }

    func dumpChat(_ chat: String, files: inout [String]) throws -> Bool {
        return try mutex.withLock {
            try merge()

            var done = false

            try sequenced {
                let pending = (try? FileManager.default.contentsOfDirectory(atPath: folder(inbox).path)) ?? []

                guard !pending.contains(where: { $0.hasSuffix(".box") }) else { return }

                guard let conversation = try conversation(chat, create: false) else {
                    try mirrorWrite(["op": "chat", "chat": chat], nil)
                    try mirrorWrite(["op": "delete", "chat": chat], nil)
                    done = true
                    return
                }

                let full = History(chat: chat, epoch: 0)
                let reads = Self.topicMeta(conversation).mapValues {
                    ($0["read"] as? NSNumber)?.uint64Value ?? conversation.read
                }
                var ops = [[String: Any]]()

                full.raw = []
                try load(chat, conversation, full)

                for (seq, record) in (full.raw ?? []).enumerated() {
                    ops += Self.dumpReads(chat, seq: UInt64(seq), read: conversation.read, reads: reads)
                    ops.append(["op": "rec", "chat": chat, "rec": try dumpRecord(chat, record, &files)])
                }

                ops += Self.dumpReads(chat, seq: UInt64(full.raw?.count ?? 0), read: conversation.read, reads: reads)

                try mirrorWrite(["op": "chat", "chat": chat], nil)

                var batch = [[String: Any]]()
                var size = 0

                for op in ops {
                    let len = try JSONSerialization.data(withJSONObject: op).count

                    if !batch.isEmpty && size + len > Self.batchMax {
                        try mirrorWrite(["op": "batch", "ops": batch], nil)
                        batch.removeAll()
                        size = 0
                    }

                    batch.append(op)
                    size += len
                }

                if !batch.isEmpty {
                    try mirrorWrite(["op": "batch", "ops": batch], nil)
                }

                done = true
            }

            return done
        }
    }

    func dumpData(_ chat: String, file: String) throws -> Int {
        return try mutex.withLock {
            guard Self.data(file, of: chat), let data = try data(chat, file: file) else {
                try lostDone(file)
                return 0
            }

            emit(["op": "data", "chat": chat, "file": file], data)

            return data.count
        }
    }

    func mirrorSize() -> Int {
        guard let mirror = try? folder(mirror) else { return 0 }

        let names = (try? FileManager.default.contentsOfDirectory(atPath: mirror.path)) ?? []

        return names.filter { $0.hasSuffix(".box") }.reduce(0) {
            $0 + (((try? FileManager.default.attributesOfItem(
                atPath: mirror.appendingPathComponent($1).path))?[.size] as? NSNumber)?.intValue ?? 0)
        }
    }

    private static func dumpReads(_ chat: String, seq: UInt64, read: UInt64,
                                  reads: [String: UInt64]) -> [[String: Any]] {
        var ops = [[String: Any]]()

        if seq == read {
            ops.append(["op": "read", "chat": chat])
        }

        for (topic, at) in reads where at == seq {
            ops.append(["op": "read", "chat": chat, "topic": topic])
        }

        return ops
    }

    private func dumpRecord(_ chat: String, _ record: [String: Any], _ files: inout [String]) throws -> [String: Any] {
        guard let file = record["data"] as? String, !file.isEmpty,
              FileManager.default.fileExists(atPath: try folder(dir).appendingPathComponent(file).path) else {
            return record
        }

        var out = record

        files.append(file)

        if record["media"] as? String == "image", record["ch"] == nil, let hash = try contentHash(chat, file: file) {
            out["ch"] = String(format: "%016llx", hash)
        }

        return out
    }

    func conversations() throws -> [[String: Any]] {
        return try mutex.withLock {
            let names = (try? FileManager.default.contentsOfDirectory(atPath: folder(dir).path)) ?? []
            var out = [[String: Any]]()

            for name in names where name.hasSuffix(".log") {
                guard let id = Self.chat(String(name.dropLast(4))) else { continue }
                guard var conversation = try conversation(id, create: false) else { continue }

                if conversation.unread < 0 || conversation.topics == nil {
                    let full = History(chat: id, epoch: 0)

                    try load(id, conversation, full)
                    try summarize(id, &conversation, full)
                }

                var last: Any = NSNull()
                var topics: Any = [Any]()

                if let data = conversation.last,
                   let message = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                    last = Self.shown(id, message)
                }

                if let data = conversation.topics,
                   let list = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] {
                    topics = list.map { topic -> [String: Any] in
                        var entry = topic

                        if let message = topic["last"] as? [String: Any] {
                            entry["last"] = Self.shown(id, message)
                        }

                        return entry
                    }
                }

                var entry: [String: Any] = [
                    "peer": id, "unread": conversation.unread, "last": last, "topics": topics,
                ]

                if Self.group(id) != nil {
                    entry["group"] = try groupInfo(id) ?? NSNull()
                }

                out.append(entry)
            }

            return out
        }
    }

    func messages(_ chat: String, epoch: Int64 = 0, stamp: Int64 = 0) throws -> [String: Any] {
        return try mutex.withLock {
            guard var conversation = try conversation(chat, create: false) else {
                return ["messages": [], "unread": 0]
            }

            var current = try cached(chat)
            var fresh = current.offset == 0

            try load(chat, conversation, current)

            if current.damaged, owner, try repair(chat), let repaired = try self.conversation(chat, create: false) {
                conversation = repaired
                current = try cached(chat)
                fresh = true
                try load(chat, conversation, current)
            }

            if fresh, conversation.unread < 0 || conversation.topics == nil {
                try summarize(chat, &conversation, current)
            }

            var out: [String: Any] = [
                "unread": conversation.unread, "epoch": current.epoch, "stamp": current.stamp,
            ]

            if epoch == current.epoch {
                out["messages"] = try current.messages.indices
                    .filter { current.stamps[$0] > stamp }
                    .map { Self.shown(chat, try gone(current.messages[$0])) }
                out["delta"] = true
            } else {
                out["messages"] = try current.messages.map { Self.shown(chat, try gone($0)) }
            }

            let meta = Self.topicMeta(conversation)

            out["topics"] = current.topics.map { topic -> [String: Any] in
                var entry = topic
                let id = topic["id"] as? String ?? ""

                entry["unread"] = (meta[id]?["unread"] as? NSNumber)?.int64Value ?? 0

                return entry
            }

            if Self.group(chat) != nil {
                out["group"] = try groupInfo(chat) ?? NSNull()
            }

            if current.damaged {
                out["warning"] = "Part of this chat could not be read, it was damaged or tampered with."
            } else if conversation.flags & Self.truncated != 0 {
                out["warning"] = "Messages were removed from the end of this chat outside of Vigil."
            }

            return out
        }
    }

    func send(_ chat: String, text: String, quote: Quote? = nil, topic: String = "",
              local: String = "") throws -> Int64 {
        return try mutex.withLock {
            let ref = ref()
            var record: [String: Any] = [
                "t": "msg", "dir": "out", "ref": ref, "text": text,
                "time": Int64(Date().timeIntervalSince1970 * 1000),
            ]
            var entry: [String: Any] = ["peer": chat, "ref": ref, "text": text]

            if let quote {
                record["qa"] = quote.author
                record["qh"] = quote.hash
                entry["qa"] = quote.author
                entry["qh"] = quote.hash
            }

            if !topic.isEmpty {
                record["tp"] = topic
                entry["tp"] = topic
            }

            try queueOut(chat, record, entry, local)

            return ref
        }
    }

    private func queueOut(_ chat: String, _ record: [String: Any], _ entry: [String: Any],
                          _ local: String, data: Data? = nil) throws {
        guard Self.group(chat) != nil else {
            try commit(chat, record, data: data)
            try exclusive {
                var list = try outboxList()
                list.append(entry)
                try outboxSave(list)
            }
            try accept(chat, true)
            return
        }

        guard let info = try groupInfo(chat) else {
            throw Failure(description: "There is no group \(chat).")
        }

        guard info["state"] as? String == "open" else {
            throw Failure(description: "This group is closed.")
        }

        let to = Self.members(info).filter { $0 != local }
        var stored = record

        stored["to"] = to
        try commit(chat, stored, data: data)

        try exclusive {
            var list = try outboxList()

            list += Self.address(entry, chat, info, local)
            try outboxSave(list)
        }
    }

    private static func address(_ entry: [String: Any], _ chat: String, _ info: [String: Any],
                                _ local: String) -> [[String: Any]] {
        members(info).filter { $0 != local }.map {
            var copy = entry
            copy["peer"] = Flock.id(Flock.of(chat), $0)
            copy["chat"] = chat
            copy["gs"] = info["serial"] ?? 0
            copy["at"] = now()
            return copy
        }
    }

    func groups() throws -> [String: Any] {
        return try mutex.withLock {
            let file = try folder(dir).appendingPathComponent("groups.box")
            guard FileManager.default.fileExists(atPath: file.path) else { return [:] }

            let plain = try openBox(Data(contentsOf: file), key: outboxKey(), aad: Self.groupsAad)
            return try JSONSerialization.jsonObject(with: plain) as? [String: Any] ?? [:]
        }
    }

    func groupInfo(_ chat: String) throws -> [String: Any]? {
        return try groups()[chat] as? [String: Any]
    }

    static var accepts: Int {
        return defaults.integer(forKey: "chatAccepts")
    }

    func accepted() throws -> Set<String> {
        return try mutex.withLock {
            let file = try folder(dir).appendingPathComponent("accepted.box")
            guard FileManager.default.fileExists(atPath: file.path) else {
                return try acceptedSave(acceptedFirst())
            }

            let plain = try openBox(Data(contentsOf: file), key: outboxKey(), aad: Self.acceptedAad)
            return Set(try JSONSerialization.jsonObject(with: plain) as? [String] ?? [])
        }
    }

    func accept(_ peer: String, _ on: Bool) throws {
        try mutex.withLock {
            try exclusive {
                var all = try accepted()
                let changed = on ? all.insert(peer).inserted : all.remove(peer) != nil

                if changed {
                    _ = try acceptedSave(all)
                }
            }
        }
    }

    static var reachable: Bool {
        return defaults.object(forKey: "chatReachable") as? Bool ?? true
    }

    func reach(_ on: Bool) throws {
        try mutex.withLock {
            if !on && Self.reachable {
                try exclusive {
                    _ = try acceptedSave(try accepted().union(try acceptedFirst()))
                }
            }

            Self.defaults.set(on, forKey: "chatReachable")
            Self.defaults.set(Self.accepts + 1, forKey: "chatAccepts")
        }
    }

    func admits() throws -> Set<String> {
        return try mutex.withLock {
            var out = try accepted()

            for case let (chat, info as [String: Any]) in try groups() {
                let flock = Flock.of(chat)

                if let owner = info["owner"] as? String {
                    out.insert(Flock.id(flock, owner))
                }

                out.formUnion(Self.members(info).map { Flock.id(flock, $0) })
            }

            for entry in try outboxList() {
                if let peer = entry["peer"] as? String {
                    out.insert(peer)
                }
            }

            return out.subtracting(try ignored())
        }
    }

    func ignored() throws -> Set<String> {
        return try mutex.withLock {
            let file = try folder(dir).appendingPathComponent("ignored.box")
            guard FileManager.default.fileExists(atPath: file.path) else { return [] }

            let plain = try openBox(Data(contentsOf: file), key: outboxKey(), aad: Self.ignoredAad)
            return Set(try JSONSerialization.jsonObject(with: plain) as? [String] ?? [])
        }
    }

    func ignore(_ peer: String, _ on: Bool) throws {
        if on {
            try accept(peer, false)
        }

        try mutex.withLock {
            try exclusive {
                var all = try ignored()

                if on {
                    all.insert(peer)
                } else {
                    all.remove(peer)
                }

                try write(try folder(dir).appendingPathComponent("ignored.box"),
                          sealBox(JSONSerialization.data(withJSONObject: all.sorted()),
                                  key: outboxKey(), aad: Self.ignoredAad))

                Self.defaults.set(Self.accepts + 1, forKey: "chatAccepts")
            }
        }
    }

    private func acceptedFirst() throws -> Set<String> {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder(dir).path)) ?? []
        var out = Set<String>()

        for name in names where name.hasSuffix(".log") {
            let id = String(name.dropLast(4))

            if Flock.valid(id) {
                out.insert(id)
            }
        }

        for entry in try outboxList() where entry["chat"] == nil {
            if let peer = entry["peer"] as? String {
                out.insert(peer)
            }
        }

        return out
    }

    private func acceptedSave(_ peers: Set<String>) throws -> Set<String> {
        try write(try folder(dir).appendingPathComponent("accepted.box"),
                  sealBox(JSONSerialization.data(withJSONObject: peers.sorted()),
                          key: outboxKey(), aad: Self.acceptedAad))

        Self.defaults.set(Self.accepts + 1, forKey: "chatAccepts")
        return peers
    }

    func groupGone(_ chat: String) throws -> Bool {
        guard Self.group(chat) != nil else { return false }

        return try groupInfo(chat)?["state"] as? String != "open"
    }

    func groupSave(_ chat: String, _ info: [String: Any]?) throws {
        try mutex.withLock {
            try exclusive {
                var all = try groups()

                all[chat] = info
                try write(try folder(dir).appendingPathComponent("groups.box"),
                          sealBox(JSONSerialization.data(withJSONObject: all),
                                  key: outboxKey(), aad: Self.groupsAad))
                Self.defaults.set(Self.accepts + 1, forKey: "chatAccepts")

                emit(["op": "group", "chat": chat, "info": info ?? NSNull()])
            }
        }
    }

    enum Admit {
        case store
        case drop
        case hold
    }

    func groupCheck(_ chat: String, peer: String, serial: UInt32) throws -> Admit {
        return try mutex.withLock {
            guard let info = try groupInfo(chat) else { return .hold }
            guard info["state"] as? String == "open" else { return .drop }
            guard serial <= (info["serial"] as? NSNumber)?.uint32Value ?? 0 else { return .hold }

            if peer == info["owner"] as? String || Self.members(info).contains(peer) {
                return .store
            }

            return .drop
        }
    }

    func groupDrop(_ chat: String, _ members: [String]) throws -> [(String, Int64)] {
        return try mutex.withLock {
            var dropped = [(String, Int64)]()
            let peers = members.map { Flock.id(Flock.of(chat), $0) }

            try exclusive {
                let list = try outboxList()
                var kept = [[String: Any]]()

                for entry in list {
                    let peer = entry["peer"] as? String ?? ""

                    guard entry["chat"] as? String == chat, peers.contains(peer) else {
                        kept.append(entry)
                        continue
                    }

                    dropped.append((peer, (entry["ref"] as? NSNumber)?.int64Value ?? 0))
                }

                guard !dropped.isEmpty else { return }

                try outboxSave(kept)
                try unqueueUnused(kept, list)
            }

            return dropped
        }
    }

    func rosterQueue(_ chat: String, _ info: [String: Any], local: String,
                     also: [String] = [], only: [String]? = nil) throws {
        try mutex.withLock {
            let targets = Set(Set(only ?? (Self.members(info) + also)).subtracting([local])
                .map { Flock.id(Flock.of(chat), $0) })

            try exclusive {
                var list = try outboxList().filter {
                    !($0["chat"] as? String == chat && $0["op"] as? String == "roster" &&
                      targets.contains($0["peer"] as? String ?? ""))
                }

                for peer in targets.sorted() {
                    list.append([
                        "peer": peer, "chat": chat, "ref": ref(), "op": "roster",
                        "serial": info["serial"] ?? 0, "members": info["members"] ?? [String](),
                        "name": info["name"] ?? "", "ended": info["state"] as? String == "ended",
                        "at": Self.now(),
                    ])
                }

                try outboxSave(list)
            }
        }
    }

    func leaveQueue(_ chat: String, owner: String) throws {
        try mutex.withLock {
            try exclusive {
                var list = try outboxList()
                list.append([
                    "peer": Flock.id(Flock.of(chat), owner), "chat": chat, "ref": ref(), "op": "leave",
                    "at": Self.now(),
                ])
                try outboxSave(list)
            }
        }
    }

    static func picked(_ value: Any?, flock: String, without local: String) -> [String]? {
        guard let list = value as? [String] else { return nil }

        var out = [String]()

        for id in list where Flock.valid(id) && Flock.of(id) == flock {
            let peer = Flock.local(id)

            if peer != local && !out.contains(peer) {
                out.append(peer)
            }
        }

        return out
    }

    func groupCreate(flock: String, name: String, members: [String], local: String) throws -> String {
        let name = name.trimmingCharacters(in: .whitespacesAndNewlines)

        guard name.utf8.count <= Int(VIGIL_CHAT_GROUP_NAME_MAX) else {
            throw Failure(description: "A group name is at most \(VIGIL_CHAT_GROUP_NAME_MAX) bytes.")
        }

        guard !members.isEmpty, let me = UInt8(local, radix: 16) else {
            throw Failure(description: "Pick at least one member.")
        }

        let id = String(format: "%016llx", UInt64.random(in: 1...UInt64.max))
        let chat = Self.groupChat(flock, local, id)
        let info: [String: Any] = [
            "owner": local, "id": id, "name": name, "members": members + [local], "serial": 1,
            "state": "open", "topics": [[String: Any]](),
        ]

        try groupSave(chat, info)
        try deliver(chat: chat, peer: me, nick: "", text: name, time: Self.now(), kind: "created")
        try merge()
        try rosterQueue(chat, info, local: local)

        return chat
    }

    func groupEdit(_ chat: String, name: String?, members picked: [String]?,
                   local: String) throws -> [(String, Int64)] {
        guard var info = try groupInfo(chat), let me = UInt8(local, radix: 16) else {
            throw Failure(description: "There is no group \(chat).")
        }

        guard info["owner"] as? String == local else {
            throw Failure(description: "Only the owner can change a group.")
        }

        guard info["state"] as? String == "open" else {
            throw Failure(description: "This group is closed.")
        }

        let time = Self.now()
        let before = Self.members(info)
        let members = picked.map { $0 + [local] } ?? before

        guard members.count > 1 else {
            throw Failure(description: "A group needs at least one other member.")
        }

        if let name = name?.trimmingCharacters(in: .whitespacesAndNewlines) {
            guard name.utf8.count <= Int(VIGIL_CHAT_GROUP_NAME_MAX) else {
                throw Failure(description: "A group name is at most \(VIGIL_CHAT_GROUP_NAME_MAX) bytes.")
            }

            if name != info["name"] as? String ?? "" {
                try deliver(chat: chat, peer: me, nick: "", text: name, time: time, kind: "renamed")
            }

            info["name"] = name
        }

        let gone = before.filter { !members.contains($0) }

        for member in members where !before.contains(member) {
            if let id = UInt8(member, radix: 16) {
                try deliver(chat: chat, peer: id, nick: "", text: "", time: time, kind: "joined")
            }
        }

        for member in gone {
            if let id = UInt8(member, radix: 16) {
                try deliver(chat: chat, peer: id, nick: "", text: "", time: time, kind: "left")
            }
        }

        info["members"] = members
        info["serial"] = ((info["serial"] as? NSNumber)?.intValue ?? 0) + 1
        info["topics"] = try topicsOf(chat, info)

        try groupSave(chat, info)
        let dropped = try groupDrop(chat, gone)
        try merge()
        try rosterQueue(chat, info, local: local, also: gone)
        try topicQueue(chat, local: local, targets: members.filter { !before.contains($0) })

        return dropped
    }

    func deleteChat(_ chat: String, local: String) throws -> [(String, Int64)] {
        var dropped = [(String, Int64)]()

        if Self.group(chat) != nil {
            dropped = try groupLeave(chat, local: local)
        }

        try delete(chat)
        Self.mute(chat, until: 0)
        Self.unlist(chat)
        Self.seen(chat)

        return dropped
    }

    static func setNick(_ nick: String) throws {
        guard nick.withCString({ vigil_chat_nick($0) }) == 0 else {
            throw Failure(description: "A name is 2 to 16 letters or digits.")
        }

        defaults.set(nick, forKey: "chatNick")
    }

    func groupLeave(_ chat: String, local: String) throws -> [(String, Int64)] {
        guard var info = try groupInfo(chat), info["state"] as? String == "open",
              let owner = info["owner"] as? String, let me = UInt8(local, radix: 16) else { return [] }

        let time = Self.now()
        let dropped = try groupDrop(chat, Self.members(info) + [owner])

        if owner == local {
            info["state"] = "ended"
            info["serial"] = ((info["serial"] as? NSNumber)?.intValue ?? 0) + 1
            try groupSave(chat, info)
            try deliver(chat: chat, peer: me, nick: "", text: "", time: time, kind: "ended")
            try rosterQueue(chat, info, local: local)
        } else {
            info["state"] = "left"
            try groupSave(chat, info)
            try deliver(chat: chat, peer: me, nick: "", text: "", time: time, kind: "parted")
            try leaveQueue(chat, owner: owner)
        }

        try merge()

        return dropped
    }

    private func unqueueUnused(_ kept: [[String: Any]], _ list: [[String: Any]]) throws {
        let used = Set(kept.compactMap { ($0["ref"] as? NSNumber)?.uint64Value })

        for entry in list where entry["data"] != nil {
            if let ref = (entry["ref"] as? NSNumber)?.uint64Value, !used.contains(ref) {
                try? unqueue(ref)
            }
        }
    }

    static func members(_ info: [String: Any]) -> [String] {
        info["members"] as? [String] ?? []
    }

    static func roster(_ members: [String]) -> [UInt8] {
        var bits = [UInt8](repeating: 0, count: Int(VIGIL_CHAT_GROUP_ROSTER))

        for member in members {
            guard let peer = UInt8(member, radix: 16) else { continue }
            bits[Int(peer) / 8] |= 1 << (peer % 8)
        }

        return bits
    }

    func quoted(_ chat: String, ref: Int64) throws -> Quote {
        return try mutex.withLock {
            guard let conversation = try conversation(chat, create: false) else {
                throw Failure(description: "That message is gone.")
            }

            let current = try cached(chat)

            try load(chat, conversation, current)

            guard let at = current.index[ref] else {
                throw Failure(description: "That message is gone.")
            }

            let message = current.messages[at]

            if message["dir"] as? String == "out" {
                return Quote(author: Self.selfAuthor, hash: Self.digest(message))
            }

            return Quote(author: message["peer"] as? String ?? "", hash: Self.digest(message))
        }
    }

    static func quote(_ author: String?, _ hash: String?) -> Quote? {
        guard let author, let hash, !hash.isEmpty else { return nil }

        return Quote(author: author, hash: hash)
    }

    private func cached(_ chat: String) throws -> History {
        let path = try folder(dir).appendingPathComponent("\(chat).log").path
        let size = (try? FileManager.default.attributesOfItem(atPath: path))?[.size] as? UInt64 ?? 0

        if let current = history, current.chat == chat, size >= current.offset {
            return current
        }

        let current = History(chat: chat, epoch: Int64.random(in: 1...Int64.max))
        history = current

        return current
    }

    private func summarize(_ chat: String, _ conversation: inout Conversation, _ full: History) throws {
        var topics = [[String: Any]]()

        for topic in full.topics {
            var entry = topic
            let id = topic["id"] as? String ?? ""

            entry["read"] = full.reads[id] ?? conversation.read
            entry["unread"] = full.unreads[id] ?? 0

            if let last = full.messages.last(where: { $0["tp"] as? String == id }) {
                entry["last"] = last
            }

            topics.append(entry)
        }

        conversation.unread = full.unread
        conversation.last = try full.messages.last(where: { $0["tp"] == nil })
            .map { try JSONSerialization.data(withJSONObject: $0) }
        conversation.topics = try JSONSerialization.data(withJSONObject: topics)
        open[chat] = conversation
        try save(chat, conversation)
    }

    private static func topicMeta(_ conversation: Conversation) -> [String: [String: Any]] {
        guard let data = conversation.topics,
              let list = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return [:] }

        var out = [String: [String: Any]]()

        for entry in list {
            if let id = entry["id"] as? String {
                out[id] = entry
            }
        }

        return out
    }

    private static func topicsUpdate(_ topics: Data, _ record: [String: Any], count: UInt64) throws -> Data {
        var list = try JSONSerialization.jsonObject(with: topics) as? [[String: Any]] ?? []
        let topic = record["tp"] as? String ?? ""
        let found = list.first { $0["id"] as? String == topic }

        if record["t"] as? String != "topic" {
            if let at = list.firstIndex(where: { $0["id"] as? String == topic }) {
                list[at]["last"] = Self.message(record)

                if record["dir"] as? String == "in" {
                    list[at]["unread"] = ((list[at]["unread"] as? NSNumber)?.int64Value ?? 0) + 1
                }
            }

            return try JSONSerialization.data(withJSONObject: list)
        }

        list.removeAll { $0["id"] as? String == topic }

        if let name = record["name"] as? String, !name.isEmpty {
            var entry: [String: Any] = [
                "id": topic, "name": name, "color": (record["color"] as? NSNumber)?.intValue ?? 0,
                "read": (found?["read"] as? NSNumber)?.uint64Value ?? count,
                "unread": (found?["unread"] as? NSNumber)?.int64Value ?? 0,
            ]

            if let last = found?["last"] {
                entry["last"] = last
            }

            list.append(entry)
        }

        return try JSONSerialization.data(withJSONObject: list)
    }

    private func load(_ chat: String, _ conversation: Conversation, _ current: History) throws {
        current.stamp += 1
        current.reads = Self.topicMeta(conversation).mapValues {
            ($0["read"] as? NSNumber)?.uint64Value ?? conversation.read
        }

        let file = try folder(dir).appendingPathComponent("\(chat).log")
        guard let handle = try? FileHandle(forReadingFrom: file) else { return }
        defer { try? handle.close() }

        try handle.seek(toOffset: current.offset)
        guard let data = try handle.readToEnd() else { return }

        let bytes = [UInt8](data)
        var offset = 0

        while offset + 4 <= bytes.count {
            let len = Int(bytes[offset]) << 24 | Int(bytes[offset + 1]) << 16 |
                Int(bytes[offset + 2]) << 8 | Int(bytes[offset + 3])
            guard len >= 28, offset + 4 + len <= bytes.count else { break }

            let sealed = Data(bytes[(offset + 4)..<(offset + 4 + len)])

            guard let plain = try? openBox(sealed, key: conversation.key,
                                           aad: Self.aad(chat, seq: current.seq)) else {
                current.damaged = true
                break
            }

            if let record = try JSONSerialization.jsonObject(with: plain) as? [String: Any] {
                current.raw?.append(record)
                Self.apply(current, record, read: conversation.read)
            }

            offset += 4 + len
            current.offset += UInt64(4 + len)
            current.seq += 1
        }
    }

    private static func apply(_ current: History, _ record: [String: Any], read: UInt64) {
        let ref = (record["ref"] as? NSNumber)?.int64Value ?? 0

        switch record["t"] as? String {
        case "msg":
            let message = Self.message(record)

            if message["dir"] as? String == "in" {
                if let topic = message["tp"] as? String {
                    if current.seq >= current.reads[topic] ?? read {
                        current.unreads[topic, default: 0] += 1
                    }
                } else if current.seq >= read {
                    current.unread += 1
                }
            }

            current.index[ref] = current.messages.count
            current.messages.append(message)
            current.stamps.append(current.stamp)

            if message["qh"] != nil {
                Self.quote(current, current.messages.count - 1)
            }
        case "topic":
            let topic = record["tp"] as? String ?? ""
            let name = record["name"] as? String ?? ""

            current.topics.removeAll { $0["id"] as? String == topic }

            if !name.isEmpty {
                current.topics.append([
                    "id": topic, "name": name, "color": (record["color"] as? NSNumber)?.intValue ?? 0,
                ])
            }
        case "member":
            current.messages.append(Self.message(record))
            current.stamps.append(current.stamp)

            if record["kind"] as? String == "left" {
                Self.gone(current, record["peer"] as? String ?? "")
            }
        case "sent":
            change(current, ref) {
                if $0["state"] as? String == "queued" {
                    $0["state"] = "sent"
                }
            }
        case "expired":
            change(current, ref) { _ = Self.unaddress(&$0, record["member"] as? String ?? "") }
        case "delivered":
            change(current, ref) {
                guard let member = record["member"] as? String else {
                    $0["state"] = "delivered"
                    return
                }

                var got = $0["got"] as? [String] ?? []

                if !got.contains(member) {
                    got.append(member)
                }

                $0["got"] = got
                Self.tally(&$0)
            }
        case "failed":
            change(current, ref) {
                if $0["state"] as? String == "queued" {
                    $0["state"] = "failed"
                }
            }
        case "fav":
            change(current, ref) { $0["fav"] = record["on"] as? Bool ?? false }
        case "pin":
            let previous = current.pinned

            if record["on"] as? Bool == true {
                current.pinned = ref
            } else if current.pinned == ref {
                current.pinned = 0
            }

            if previous != current.pinned {
                change(current, previous) { $0["pinned"] = nil }
                change(current, current.pinned) { $0["pinned"] = true }
            }
        default:
            break
        }
    }

    private static func gone(_ current: History, _ member: String) {
        for at in current.messages.indices {
            if Self.unaddress(&current.messages[at], member) {
                current.stamps[at] = current.stamp
            }
        }
    }

    private static func unaddress(_ message: inout [String: Any], _ member: String) -> Bool {
        guard var to = message["to"] as? [String] else { return false }

        let got = message["got"] as? [String] ?? []

        guard !got.contains(member), to.contains(member) else { return false }

        to.removeAll { $0 == member }
        message["to"] = to
        Self.tally(&message)

        return true
    }

    private static func tally(_ message: inout [String: Any]) {
        let to = (message["to"] as? [String])?.count ?? 0
        let acked = (message["got"] as? [String])?.count ?? 0

        message["acked"] = acked
        message["pending"] = max(to - acked, 0)

        if acked > 0, to - acked <= 0 {
            message["state"] = "delivered"
        } else if acked > 0 {
            message["state"] = "sent"
        } else if to == 0 {
            message["state"] = "failed"
        }
    }

    private static func change(_ current: History, _ ref: Int64, _ edit: (inout [String: Any]) -> Void) {
        guard let at = current.index[ref] else { return }

        edit(&current.messages[at])
        current.stamps[at] = current.stamp
    }

    private static func message(_ record: [String: Any]) -> [String: Any] {
        if record["t"] as? String == "member" {
            return [
                "ref": record["ref"] ?? 0,
                "dir": "in",
                "kind": record["kind"] ?? "",
                "text": record["text"] ?? "",
                "time": record["time"] ?? 0,
                "nick": record["nick"] ?? "",
                "peer": record["peer"] ?? "",
                "state": "received",
            ]
        }

        let incoming = record["dir"] as? String == "in"
        var message: [String: Any] = [
            "ref": record["ref"] ?? 0,
            "dir": record["dir"] ?? "in",
            "text": record["text"] ?? "",
            "time": record["time"] ?? 0,
            "nick": record["nick"] ?? "",
            "peer": record["peer"] ?? "",
            "data": record["data"] ?? "",
            "media": record["media"] ?? "",
            "name": record["name"] ?? "",
            "state": incoming ? "received" : "queued",
        ]

        if let found = Self.quote(record["qa"] as? String, record["qh"] as? String) {
            message["qa"] = found.author
            message["qh"] = found.hash
        }

        if let topic = record["tp"] as? String {
            message["tp"] = topic
        }

        if let hash = record["ch"] as? String {
            message["ch"] = hash
        }

        if let to = record["to"] as? [String] {
            message["to"] = to
            Self.tally(&message)
        }

        return message
    }

    private static func quote(_ current: History, _ at: Int) {
        guard let wanted = Self.quote(current.messages[at]["qa"] as? String,
                                      current.messages[at]["qh"] as? String) else { return }

        current.messages[at]["quoted"] = true

        for other in stride(from: at - 1, through: 0, by: -1) {
            guard current.messages[other]["kind"] == nil else { continue }
            guard current.messages[other]["tp"] as? String == current.messages[at]["tp"] as? String else {
                continue
            }
            guard wrote(current.messages[other], wanted.author) else { continue }

            if current.digests[other] == nil {
                current.digests[other] = digest(current.messages[other])
            }

            if current.digests[other] == wanted.hash {
                current.messages[at]["reply"] = current.messages[other]["ref"]
                break
            }
        }
    }

    private static func wrote(_ message: [String: Any], _ author: String) -> Bool {
        if author == selfAuthor {
            return message["dir"] as? String == "out"
        }

        return message["dir"] as? String == "in" && message["peer"] as? String == author
    }

    private static func digest(_ message: [String: Any]) -> String {
        var content = message["text"] as? String ?? ""

        if !(message["data"] as? String ?? "").isEmpty {
            content = message["name"] as? String ?? ""
        }

        let media = message["media"] as? String ?? ""
        let sum = SHA256.hash(data: Data((media + "\n" + content).utf8))

        return sum.prefix(8).map { String(format: "%02x", $0) }.joined()
    }

    func sendData(_ chat: String, media: String, name: String, data: Data,
                  topic: String = "", local: String = "") throws -> (Int64, String) {
        try mutex.withLock {
            let ref = ref()
            var record: [String: Any] = [
                "t": "msg", "dir": "out", "ref": ref, "text": "", "time": Int64(Date().timeIntervalSince1970 * 1000),
            ]

            Self.described(&record, chat, media: media, name: name, data: data)

            let file = record["data"] as? String ?? ""
            var entry: [String: Any] = ["peer": chat, "ref": ref, "data": file, "media": media, "name": name]

            if !topic.isEmpty {
                record["tp"] = topic
                entry["tp"] = topic
            }

            try queue(UInt64(bitPattern: ref), data)
            try queueOut(chat, record, entry, local, data: data)

            return (ref, file)
        }
    }

    static func topicId(_ value: Any?) throws -> String {
        guard let topic = value as? String, !topic.isEmpty else { return "" }

        guard topic.count == 16, let id = UInt64(topic, radix: 16), id != 0 else {
            throw Failure(description: "There is no topic \(topic).")
        }

        return topic
    }

    func topicSet(_ chat: String, topic value: Any?, name: String, color: Int, local: String) throws -> String {
        let name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        var topic = try Self.topicId(value)

        guard name.utf8.count <= Int(VIGIL_CHAT_TOPIC_NAME_MAX) else {
            throw Failure(description: "A topic name is at most \(VIGIL_CHAT_TOPIC_NAME_MAX) bytes.")
        }

        if topic.isEmpty {
            topic = String(format: "%016llx", UInt64.random(in: 1...UInt64.max))
        }

        _ = try self.topic(chat, topic: topic, name: name, color: color & 0xffffff, local: local)

        return topic
    }

    func topic(_ chat: String, topic: String, name: String, color: Int, local: String = "") throws -> Int64 {
        try mutex.withLock {
            let ref = ref()
            let entry: [String: Any] = [
                "peer": chat, "ref": ref, "op": "topic", "tp": topic, "name": name, "color": color,
            ]
            var info: [String: Any]?

            if Self.group(chat) != nil {
                info = try groupInfo(chat)

                guard let info else {
                    throw Failure(description: "There is no group \(chat).")
                }

                guard info["owner"] as? String == local else {
                    throw Failure(description: "Only the owner can change the topics of a group.")
                }

                guard info["state"] as? String == "open" else {
                    throw Failure(description: "This group is closed.")
                }
            }

            try commit(chat, [
                "t": "topic", "ref": ref, "dir": "out", "tp": topic, "name": name, "color": color,
                "time": Int64(Date().timeIntervalSince1970 * 1000),
            ])

            if var updated = info {
                var topics = try topicsOf(chat, updated).filter { $0["id"] as? String != topic }

                if !name.isEmpty {
                    topics.append(["id": topic, "name": name, "color": color])
                }

                updated["topics"] = topics
                try groupSave(chat, updated)
            }

            try exclusive {
                var list = try outboxList()

                if let info {
                    list += Self.address(entry, chat, info, local)
                } else {
                    list.append(entry)
                }

                try outboxSave(list)
            }

            if info == nil {
                try accept(chat, true)
            }

            return ref
        }
    }

    func topicsOf(_ chat: String, _ info: [String: Any]) throws -> [[String: Any]] {
        if let topics = info["topics"] as? [[String: Any]] {
            return topics
        }

        let topics = try messages(chat)["topics"] as? [[String: Any]] ?? []

        return topics.map {
            ["id": $0["id"] ?? "", "name": $0["name"] ?? "", "color": $0["color"] ?? 0]
        }
    }

    func topicsFill(_ flock: String, local: String) throws {
        try mutex.withLock {
            for (chat, value) in try groups() where Flock.of(chat) == flock {
                guard var info = value as? [String: Any], info["owner"] as? String == local,
                      info["topics"] == nil else { continue }

                info["topics"] = try topicsOf(chat, info)
                try groupSave(chat, info)
            }
        }
    }

    func topicQueue(_ chat: String, local: String, targets: [String]) throws {
        try mutex.withLock {
            guard let info = try groupInfo(chat), let topics = info["topics"] as? [[String: Any]],
                  !topics.isEmpty else { return }

            try exclusive {
                var list = try outboxList()

                for peer in targets where peer != local {
                    for topic in topics {
                        list.append([
                            "peer": Flock.id(Flock.of(chat), peer), "chat": chat, "ref": ref(), "op": "topic",
                            "tp": topic["id"] ?? "", "name": topic["name"] ?? "", "color": topic["color"] ?? 0,
                            "gs": info["serial"] ?? 0, "at": Self.now(),
                        ])
                    }
                }

                try outboxSave(list)
            }
        }
    }

    private func topicWipe(_ chat: String, topic: String) throws {
        guard let conversation = try conversation(chat, create: false) else { return }

        let full = History(chat: chat, epoch: 0)

        try load(chat, conversation, full)

        for message in full.messages {
            guard message["tp"] as? String == topic else { continue }
            guard let data = message["data"] as? String, !data.isEmpty else { continue }

            try wipe(try folder(dir).appendingPathComponent(data))
        }
    }

    func retract(_ chat: String, ref: Int64, local: String = "") throws -> [(String, Int64)] {
        try mutex.withLock {
            guard let conversation = try conversation(chat, create: false) else {
                throw Failure(description: "That image is gone.")
            }

            let current = try cached(chat)

            try load(chat, conversation, current)

            guard let at = current.index[ref] else {
                throw Failure(description: "That image is gone.")
            }

            let message = current.messages[at]

            guard message["dir"] as? String == "out", message["media"] as? String == "image",
                  let file = message["data"] as? String, !file.isEmpty else {
                throw Failure(description: "Only your own images can be deleted.")
            }

            guard let hash = try (message["ch"] as? String).flatMap({ UInt64($0, radix: 16) }) ??
                    contentHash(chat, file: file) else {
                throw Failure(description: "That image was already removed.")
            }

            return try retractHash(chat, ref: ref, hash: hash, topic: message["tp"] as? String ?? "", local: local)
        }
    }

    func retractHash(_ chat: String, ref: Int64, hash: UInt64, topic: String,
                     local: String = "") throws -> [(String, Int64)] {
        try mutex.withLock {
            var dropped = [(String, Int64)]()
            var record: [String: Any] = [
                "t": "retract", "ref": self.ref(), "peer": Self.selfAuthor,
                "hash": String(format: "%016llx", hash), "target": ref,
            ]

            if !topic.isEmpty {
                record["tp"] = topic
            }

            try commit(chat, record)
            try wipe(try folder(dir).appendingPathComponent(Self.queuedName(UInt64(bitPattern: ref))))

            var entry: [String: Any] = [
                "peer": chat, "ref": self.ref(), "op": "retract", "hash": String(format: "%016llx", hash),
            ]

            if !topic.isEmpty {
                entry["tp"] = topic
            }

            let info = try Self.group(chat) == nil ? nil : groupInfo(chat)

            try exclusive {
                var list = [[String: Any]]()

                for queued in try outboxList() {
                    let peer = queued["peer"] as? String ?? ""

                    if (queued["ref"] as? NSNumber)?.int64Value == ref,
                       queued["chat"] as? String ?? peer == chat {
                        dropped.append((peer, ref))
                        continue
                    }

                    list.append(queued)
                }

                if Self.group(chat) == nil {
                    list.append(entry)
                } else if let info, info["state"] as? String == "open" {
                    list += Self.address(entry, chat, info, local)
                }

                try outboxSave(list)
            }

            return dropped
        }
    }

    private static func shown(_ chat: String, _ message: [String: Any]) -> [String: Any] {
        guard let peer = message["peer"] as? String, peer.count == 2, peer != selfAuthor,
              UInt8(peer, radix: 16) != nil else { return message }

        var out = message
        out["peer"] = Flock.id(Flock.of(chat), peer)
        return out
    }

    private func gone(_ message: [String: Any]) throws -> [String: Any] {
        guard let file = message["data"] as? String, !file.isEmpty else { return message }

        let path = try folder(dir).appendingPathComponent(file).path

        guard !FileManager.default.fileExists(atPath: path) else { return message }

        var marked = message
        marked["gone"] = true

        return marked
    }

    private func retracted(_ chat: String, _ record: [String: Any]) throws {
        guard let conversation = try conversation(chat, create: false) else { return }

        let full = History(chat: chat, epoch: 0)

        try load(chat, conversation, full)

        guard let at = try retractTarget(chat, full.messages, record),
              let file = full.messages[at]["data"] as? String else { return }

        try wipe(try folder(dir).appendingPathComponent(file))
        history = nil
    }

    private func retractTarget(_ chat: String, _ messages: [[String: Any]], _ record: [String: Any]) throws -> Int? {
        if let target = (record["target"] as? NSNumber)?.int64Value {
            return messages.lastIndex {
                ($0["ref"] as? NSNumber)?.int64Value == target && $0["dir"] as? String == "out"
            }
        }

        guard let want = (record["hash"] as? String).flatMap({ UInt64($0, radix: 16) }) else { return nil }

        for at in messages.indices.reversed() {
            let message = messages[at]

            guard message["media"] as? String == "image",
                  let file = message["data"] as? String, !file.isEmpty,
                  message["tp"] as? String ?? "" == record["tp"] as? String ?? "",
                  Self.wrote(message, record["peer"] as? String ?? "") else { continue }

            let have = try (message["ch"] as? String).flatMap { UInt64($0, radix: 16) } ?? contentHash(chat, file: file)

            if have == want {
                return at
            }
        }

        return nil
    }

    private func contentHash(_ chat: String, file: String) throws -> UInt64? {
        guard Self.data(file, of: chat),
              let conversation = try conversation(chat, create: false),
              let sealed = try? Data(contentsOf: try folder(dir).appendingPathComponent(file)) else { return nil }

        return Self.contentHash(try openBox(sealed, key: conversation.key, aad: Self.dataAad(chat, file)))
    }

    private static func contentHash(_ data: Data) -> UInt64 {
        let hash = SHA256.hash(data: data).prefix(8).reduce(UInt64(0)) { $0 << 8 | UInt64($1) }

        if hash == 0 {
            return 1
        }

        return hash
    }

    func data(_ chat: String, file: String) throws -> Data? {
        return try mutex.withLock {
            guard Self.data(file, of: chat) else {
                throw Failure(description: "There is no data \(file).")
            }

            guard let conversation = try conversation(chat, create: false) else { return nil }

            let path = try folder(dir).appendingPathComponent(file)
            guard let sealed = try? Data(contentsOf: path) else { return nil }

            return try openBox(sealed, key: conversation.key, aad: Self.dataAad(chat, file))
        }
    }

    func queued(_ ref: UInt64) throws -> Data? {
        return try mutex.withLock {
            let name = Self.queuedName(ref)
            let path = try folder(dir).appendingPathComponent(name)
            guard let sealed = try? Data(contentsOf: path) else { return nil }

            return try openBox(sealed, key: outboxKey(), aad: Self.queuedAad + Data(name.utf8))
        }
    }

    func unqueue(_ ref: UInt64) throws {
        try mutex.withLock {
            try wipe(try folder(dir).appendingPathComponent(Self.queuedName(ref)))
        }
    }

    private func queue(_ ref: UInt64, _ data: Data) throws {
        let name = Self.queuedName(ref)
        let sealed = try sealBox(data, key: outboxKey(), aad: Self.queuedAad + Data(name.utf8))

        try write(try folder(dir).appendingPathComponent(name), sealed)
    }

    private static func queuedName(_ ref: UInt64) -> String {
        String(format: "outbox-%016llx.dat", ref)
    }

    private func settleQueued() throws {
        var refs = Set<UInt64>()

        for entry in try outboxList() {
            guard let chat = entry["chat"] as? String ?? entry["peer"] as? String,
                  let file = entry["data"] as? String,
                  let ref = (entry["ref"] as? NSNumber)?.uint64Value else { continue }

            refs.insert(ref)

            if (try? queued(ref)) == nil, let bytes = try? self.data(chat, file: file) {
                try queue(ref, bytes)
            }
        }

        let names = (try? FileManager.default.contentsOfDirectory(atPath: try folder(dir).path)) ?? []

        for name in names where name.hasPrefix("outbox-") && name.hasSuffix(".dat") {
            if let ref = UInt64(name.dropFirst(7).dropLast(4), radix: 16), refs.contains(ref) {
                continue
            }

            try wipe(try folder(dir).appendingPathComponent(name))
        }
    }

    func ownAvatar() throws -> Data? {
        return try mutex.withLock {
            let file = try folder(dir).appendingPathComponent("avatar.box")
            guard FileManager.default.fileExists(atPath: file.path) else { return nil }

            return try openBox(Data(contentsOf: file), key: outboxKey(), aad: Self.avatarAad)
        }
    }

    func ownAvatar(_ data: Data?) throws {
        try mutex.withLock {
            let file = try folder(dir).appendingPathComponent("avatar.box")

            guard let data else {
                try wipe(file)
                return
            }

            try write(file, sealBox(data, key: outboxKey(), aad: Self.avatarAad))
        }
    }

    func peerAvatar(_ peer: String) throws -> Data? {
        return try mutex.withLock {
            let file = try folder(avatars).appendingPathComponent(Self.box(peer))
            guard let sealed = try? Data(contentsOf: file) else { return nil }

            do {
                return try unseal(sealed, info: Self.avatarAad + Data(peer.utf8))
            } catch let error as Locked {
                throw error
            } catch is CryptoKitError {
                try wipe(file)
                return nil
            }
        }
    }

    func peerAvatar(_ peer: String, _ data: Data?) throws {
        try mutex.withLock {
            let file = try folder(avatars).appendingPathComponent(Self.box(peer))

            if let data {
                try write(file, seal(data, to: agreementPublic(), info: Self.avatarAad + Data(peer.utf8)))
            } else {
                try wipe(file)
            }

            emit(["op": "avatar", "peer": peer], data)
        }
    }

    func safetyTime(_ peer: String) throws -> Int64 {
        return try mutex.withLock {
            let file = try folder(safety).appendingPathComponent(Self.box(peer))
            let attributes = try? FileManager.default.attributesOfItem(atPath: file.path)

            guard let date = attributes?[.modificationDate] as? Date else { return 0 }

            return Int64(date.timeIntervalSince1970 * 1000)
        }
    }

    func safetyCode(_ peer: String) throws -> Data? {
        return try mutex.withLock {
            let file = try folder(safety).appendingPathComponent(Self.box(peer))
            guard let sealed = try? Data(contentsOf: file), sealed.count > Self.wrapped else { return nil }

            let aad = Self.safetyAad + Data(peer.utf8)

            do {
                var raw = try openBox(Data(sealed.prefix(Self.wrapped)), key: outboxKey(), aad: aad)
                defer { raw.resetBytes(in: 0..<raw.count) }

                return try openBox(Data(sealed.dropFirst(Self.wrapped)), key: SymmetricKey(data: raw), aad: aad)
            } catch is CryptoKitError {
                try wipe(file)
                return nil
            }
        }
    }

    func safetyCode(_ peer: String, _ plain: Data?) throws {
        try mutex.withLock {
            let file = try folder(safety).appendingPathComponent(Self.box(peer))

            guard let plain else {
                try wipe(file)
                return
            }

            let aad = Self.safetyAad + Data(peer.utf8)
            let key = SymmetricKey(size: .bits256)
            var raw = key.withUnsafeBytes { Data($0) }
            defer { raw.resetBytes(in: 0..<raw.count) }

            try write(file, sealBox(raw, key: outboxKey(), aad: aad) + sealBox(plain, key: key, aad: aad))
        }
    }

    func safetyCodes() throws -> [String: Any] {
        return try mutex.withLock {
            let files = (try? FileManager.default.contentsOfDirectory(atPath: folder(safety).path)) ?? []
            var out = [String]()

            for name in files where name.hasSuffix(".box") {
                let peer = String(name.dropLast(4))
                guard Flock.valid(peer), var record = try safetyCode(peer) else { continue }

                if record.count == Int(VIGIL_ADDITIONAL_INPUT_LEN) {
                    out.append(peer)
                }

                record.resetBytes(in: 0..<record.count)
            }

            return ["on": Self.codes(), "set": out]
        }
    }

    static func codes() -> [String] {
        shared.state()["codes"] as? [String] ?? []
    }

    static func code(_ peer: String, on: Bool) {
        _ = shared.state { all in
            var list = all["codes"] as? [String] ?? []

            list.removeAll { $0 == peer }

            if on {
                list.append(peer)
            }

            all["codes"] = list
        }
    }

    static func safetyApply(_ peer: String) {
        let flock = Flock.number(peer)
        let id = Flock.peer(peer)

        guard codes().contains(peer),
              var record = try? shared.safetyCode(peer),
              record.count == Int(VIGIL_ADDITIONAL_INPUT_LEN) else {
            vigil_additional_input(flock, id, nil, 0)
            return
        }

        record.withUnsafeBytes {
            vigil_additional_input(flock, id, $0.baseAddress, Int(VIGIL_ADDITIONAL_INPUT_LEN))
        }
        record.resetBytes(in: 0..<record.count)
    }

    static func safetyApplyAll() {
        guard let folder = shared.safety,
              let files = try? FileManager.default.contentsOfDirectory(atPath: folder.path) else {
            return
        }

        for name in files where name.hasSuffix(".box") {
            let peer = String(name.dropLast(4))

            if Flock.valid(peer) {
                safetyApply(peer)
            }
        }
    }

    func peerName(_ peer: String, nick: String, signed: String) throws {
        try mutex.withLock {
            let file = try folder(names).appendingPathComponent(Self.box(peer))
            let plain = try JSONSerialization.data(withJSONObject: ["nick": nick, "signed": signed])

            try write(file, seal(plain, to: agreementPublic(), info: Self.nameAad + Data(peer.utf8)))
            emit(["op": "name", "peer": peer, "nick": nick, "signed": signed])
        }
    }

    func peerNames() throws -> [String: Any] {
        return try mutex.withLock {
            let names = try folder(names)
            let files = (try? FileManager.default.contentsOfDirectory(atPath: names.path)) ?? []
            var out = [String: Any]()

            for name in files where name.hasSuffix(".box") {
                let hex = String(name.dropLast(4))
                guard Flock.valid(hex) else { continue }

                let file = names.appendingPathComponent(name)
                let plain: Data

                do {
                    plain = try unseal(Data(contentsOf: file), info: Self.nameAad + Data(hex.utf8))
                } catch let error as Locked {
                    throw error
                } catch is CryptoKitError {
                    try wipe(file)
                    continue
                }

                guard let entry = try? JSONSerialization.jsonObject(with: plain) else {
                    try wipe(file)
                    continue
                }

                out[hex] = entry
            }

            return out
        }
    }

    func outboxList() throws -> [[String: Any]] {
        return try mutex.withLock {
            let file = try folder(dir).appendingPathComponent("outbox.box")
            guard FileManager.default.fileExists(atPath: file.path) else { return [] }

            let plain = try openBox(Data(contentsOf: file), key: outboxKey(), aad: Self.outboxAad)
            return try JSONSerialization.jsonObject(with: plain) as? [[String: Any]] ?? []
        }
    }

    func receipt(_ peer: String, ref: Int64, kind: String) throws {
        try mutex.withLock {
            try exclusive { try addReceipt(peer, ref: ref, kind: kind) }
        }
    }

    private func addReceipt(_ peer: String, ref: Int64, kind: String) throws {
        let outbox = try outboxList()
        let match = { (entry: [String: Any]) -> Bool in
            entry["peer"] as? String == peer && (entry["ref"] as? NSNumber)?.int64Value == ref
        }
        let chat = outbox.first(where: match).flatMap { $0["chat"] as? String } ?? peer

        if kind == "delivered" {
            let kept = outbox.filter { !match($0) }

            if kept.count != outbox.count {
                try outboxSave(kept)
                try unqueueUnused(kept, outbox)
            }
        }

        try receiptWrite(chat, ref: ref, kind: kind, member: chat != peer ? Flock.local(peer) : nil)
    }

    private func receiptWrite(_ chat: String, ref: Int64, kind: String, member: String?) throws {
        var record: [String: Any] = ["t": kind, "ref": ref]

        if let member {
            record["member"] = member
        }

        try commit(chat, record)
    }

    func expire() throws -> [(String, Int64)] {
        return try mutex.withLock {
            var dropped = [(String, Int64)]()
            var stale = [(String, String)]()

            try exclusive {
                let now = Self.now()
                var changed = false
                var kept = [[String: Any]]()

                for var entry in try outboxList() {
                    guard let chat = entry["chat"] as? String else {
                        kept.append(entry)
                        continue
                    }

                    if entry["at"] == nil {
                        entry["at"] = now
                        changed = true
                    }

                    let at = (entry["at"] as? NSNumber)?.int64Value ?? now

                    if now - at < Self.groupExpire {
                        kept.append(entry)
                        continue
                    }

                    let peer = entry["peer"] as? String ?? ""
                    let ref = (entry["ref"] as? NSNumber)?.int64Value ?? 0

                    dropped.append((peer, ref))
                    changed = true

                    switch entry["op"] as? String {
                    case "roster", "topic":
                        stale.append((chat, Flock.local(peer)))
                    case nil:
                        try receiptWrite(chat, ref: ref, kind: "expired", member: Flock.local(peer))
                    default:
                        break
                    }
                }

                if changed {
                    try outboxSave(kept)
                }
            }

            for (chat, peer) in stale {
                try groupStale(chat, peer: peer, on: true)
            }

            return dropped
        }
    }

    func groupStale(_ chat: String, peer: String, on: Bool) throws {
        try mutex.withLock {
            guard var info = try groupInfo(chat) else { return }

            var stale = (info["stale"] as? [String] ?? []).filter { $0 != peer }

            if on {
                stale.append(peer)
            }

            info["stale"] = stale
            try groupSave(chat, info)
        }
    }

    static func now() -> Int64 {
        Int64(Date().timeIntervalSince1970 * 1000)
    }

    func read(_ chat: String, topic: String = "") throws {
        try mutex.withLock {
            var op: [String: Any] = ["op": "read", "chat": chat]

            if !topic.isEmpty {
                op["topic"] = topic
            }

            try enqueue(op)
        }
    }

    private func readMeta(_ chat: String, topic: String) throws {
        guard var conversation = try conversation(chat, create: false) else { return }

        if topic.isEmpty {
            conversation.read = conversation.count
            conversation.unread = 0
            open[chat] = conversation
            try save(chat, conversation)
            return
        }

        if conversation.topics == nil {
            let full = History(chat: chat, epoch: 0)

            try load(chat, conversation, full)
            try summarize(chat, &conversation, full)
        }

        guard let data = conversation.topics,
              var list = try JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return }

        for at in list.indices where list[at]["id"] as? String == topic {
            list[at]["read"] = conversation.count
            list[at]["unread"] = 0
        }

        conversation.topics = try JSONSerialization.data(withJSONObject: list)
        open[chat] = conversation
        try save(chat, conversation)
    }

    func favorite(_ chat: String, ref: Int64, on: Bool) throws {
        try mutex.withLock {
            try commit(chat, ["t": "fav", "ref": ref, "on": on])
        }
    }

    func pin(_ chat: String, ref: Int64, on: Bool) throws {
        try mutex.withLock {
            try commit(chat, ["t": "pin", "ref": ref, "on": on])
        }
    }

    func delete(_ chat: String) throws {
        try mutex.withLock {
            try enqueue(["op": "delete", "chat": chat])

            try exclusive {
                let list = try outboxList()

                // The members still have to hear that we ended or left the group.
                let kept = list.filter {
                    ["roster", "leave"].contains($0["op"] as? String ?? "") ||
                        ($0["peer"] as? String != chat && $0["chat"] as? String != chat)
                }

                if kept.count != list.count { try outboxSave(kept) }

                try unqueueUnused(kept, list)
            }

            if Self.group(chat) != nil {
                try groupSave(chat, nil)
            } else {
                try accept(chat, false)
            }
        }
    }

    private func wipeChat(_ chat: String) throws {
        open[chat] = nil

        if history?.chat == chat {
            history = nil
        }

        try remove("meta.\(chat)")
        try remove("key.\(chat)")
        try wipe(try folder(dir).appendingPathComponent("\(chat).log"))

        let names = (try? FileManager.default.contentsOfDirectory(atPath: try folder(dir).path)) ?? []

        for name in names where Self.data(name, of: chat) {
            try wipe(try folder(dir).appendingPathComponent(name))
        }
    }

    private func dataWrite(_ chat: String, file: String, _ data: Data) throws {
        guard Self.data(file, of: chat), let conversation = try conversation(chat, create: true) else {
            throw Failure(description: "There is no data \(file).")
        }

        let sealed = try sealBox(data, key: conversation.key, aad: Self.dataAad(chat, file))

        try write(try folder(dir).appendingPathComponent(file), sealed)
    }

    private static func dataAad(_ chat: String, _ file: String) -> Data {
        var aad = dataAad
        aad.append(label(chat))
        aad.append(Data(file.utf8))
        return aad
    }

    static func data(_ file: String, of chat: String) -> Bool {
        guard file.hasPrefix("\(chat)-"), file.hasSuffix(".dat") else { return false }

        let ref = file.dropFirst(chat.count + 1).dropLast(4)
        return ref.count == 16 && UInt64(ref, radix: 16) != nil && self.chat(chat) != nil
    }

    static func kind(_ media: String) -> Int32 {
        switch media {
        case "image":
            return Int32(VIGIL_CHAT_DATA_IMAGE)
        case "voice":
            return Int32(VIGIL_CHAT_DATA_VOICE)
        default:
            return Int32(VIGIL_CHAT_DATA_FILE)
        }
    }

    private func sequenced(_ body: () throws -> Void) throws {
        try exclusive(lock: "order", body)
    }

    private func exclusive(lock: String = "lock", _ body: () throws -> Void) throws {
        let path = try folder(dir).appendingPathComponent(lock).path
        let fd = Darwin.open(path, O_CREAT | O_RDWR, 0o600)
        guard fd >= 0 else { throw Failure(description: "failed to open the chat lock") }
        defer { Darwin.close(fd) }

        guard flock(fd, LOCK_EX) == 0 else { throw Failure(description: "failed to lock the chats") }
        defer { flock(fd, LOCK_UN) }

        if lock == "lock" {
            holding += 1
        }

        defer {
            if lock == "lock" {
                holding -= 1
            }
        }

        try body()
    }

    private func outboxSave(_ list: [[String: Any]]) throws {
        try write(try folder(dir).appendingPathComponent("outbox.box"),
                  sealBox(JSONSerialization.data(withJSONObject: list),
                          key: outboxKey(), aad: Self.outboxAad))
        Self.defaults.set(Self.accepts + 1, forKey: "chatAccepts")
    }

    private func append(_ chat: String, _ record: [String: Any]) throws {
        guard var conversation = try conversation(chat, create: true) else {
            throw Failure(description: "no conversation")
        }

        let plain = try JSONSerialization.data(withJSONObject: record)
        let sealed = try sealBox(plain, key: conversation.key,
                                 aad: Self.aad(chat, seq: conversation.count))

        var frame = Data()
        frame.append(contentsOf: Self.bytes(UInt32(sealed.count)))
        frame.append(sealed)

        let file = try folder(dir).appendingPathComponent("\(chat).log")

        if !FileManager.default.fileExists(atPath: file.path) {
            FileManager.default.createFile(atPath: file.path, contents: nil)
        }

        let handle = try FileHandle(forWritingTo: file)
        defer { try? handle.close() }

        try handle.seekToEnd()
        try handle.write(contentsOf: frame)
        try handle.synchronize()

        let type = record["t"] as? String ?? ""

        if record["tp"] != nil, Self.creating.contains(type) {
            if let topics = conversation.topics {
                conversation.topics = try Self.topicsUpdate(topics, record, count: conversation.count)
            }
        } else if type == "msg" || type == "member" {
            conversation.last = try JSONSerialization.data(withJSONObject: Self.message(record))
        }

        if type == "msg", record["tp"] == nil, record["dir"] as? String == "in", conversation.unread >= 0 {
            conversation.unread += 1
        }

        conversation.count += 1
        open[chat] = conversation
        try save(chat, conversation)
    }

    private func repair(_ chat: String) throws -> Bool {
        guard var conversation = try conversation(chat, create: false) else { return false }

        let file = try folder(dir).appendingPathComponent("\(chat).log")
        let bytes = [UInt8](try Data(contentsOf: file))
        var boxes = [Data]()
        var offset = 0

        while offset + 4 <= bytes.count {
            let len = Int(bytes[offset]) << 24 | Int(bytes[offset + 1]) << 16 |
                Int(bytes[offset + 2]) << 8 | Int(bytes[offset + 3])
            guard len >= 28, offset + 4 + len <= bytes.count else { break }

            boxes.append(Data(bytes[(offset + 4)..<(offset + 4 + len)]))
            offset += 4 + len
        }

        var kept = [Data]()

        for (at, box) in boxes.enumerated() {
            var plain = try? openBox(box, key: conversation.key, aad: Self.aad(chat, seq: UInt64(at)))

            for seq in 0..<boxes.count where plain == nil && seq != at {
                plain = try? openBox(box, key: conversation.key, aad: Self.aad(chat, seq: UInt64(seq)))
            }

            guard let plain else { continue }

            if Self.unique(plain), kept.contains(plain) {
                continue
            }

            kept.append(plain)
        }

        var log = Data()

        for (seq, plain) in kept.enumerated() {
            let sealed = try sealBox(plain, key: conversation.key, aad: Self.aad(chat, seq: UInt64(seq)))

            log.append(contentsOf: Self.bytes(UInt32(sealed.count)))
            log.append(sealed)
        }

        try write(file, log)

        conversation.count = UInt64(kept.count)
        conversation.flags &= ~Self.truncated
        conversation.unread = -1
        conversation.last = nil
        conversation.topics = nil
        open[chat] = conversation
        history = nil
        try save(chat, conversation)

        NSLog("vigil: repaired the log of \(chat), kept \(kept.count) of \(boxes.count) records")

        return true
    }

    private static func unique(_ plain: Data) -> Bool {
        guard let record = (try? JSONSerialization.jsonObject(with: plain)) as? [String: Any] else { return false }

        return ["msg", "member", "topic", "retract"].contains(record["t"] as? String ?? "")
    }

    private func framed(_ chat: String) throws -> UInt64 {
        let file = try folder(dir).appendingPathComponent("\(chat).log")
        guard let data = try? Data(contentsOf: file) else { return 0 }

        let bytes = [UInt8](data)
        var offset = 0
        var count: UInt64 = 0

        while offset + 4 <= bytes.count {
            let len = Int(bytes[offset]) << 24 | Int(bytes[offset + 1]) << 16 |
                Int(bytes[offset + 2]) << 8 | Int(bytes[offset + 3])
            guard len >= 28, offset + 4 + len <= bytes.count else { break }

            offset += 4 + len
            count += 1
        }

        return count
    }

    private func conversation(_ chat: String, create: Bool) throws -> Conversation? {
        // Our network extension never opens the history of our chats, only our app does.
        guard owner else { throw Failure(description: "Open Vigil on your phone to do that.") }

        if let conversation = open[chat] { return conversation }

        guard let wrapped = try item("key.\(chat)") else {
            guard create else { return nil }

            let conversation = Conversation(key: SymmetricKey(size: .bits256), count: 0, read: 0, flags: 0,
                                            unread: 0)
            try wipe(try folder(dir).appendingPathComponent("\(chat).log"))
            try save(chat, conversation)
            open[chat] = conversation

            return conversation
        }

        var plain = [UInt8](try unseal(wrapped, info: Self.keyAad + Self.label(chat)))
        var merged = false

        if plain.count == 32, let sealed = try item("meta.\(chat)") {
            let tail = [UInt8](try unseal(sealed, info: Self.metaAad + Self.label(chat)))

            plain.append(contentsOf: tail)
            merged = true
        }

        var conversation = try Self.decode(plain)
        let count = try framed(chat)

        if count != conversation.count {
            if count < conversation.count { conversation.flags |= Self.truncated }
            conversation.count = count
            conversation.unread = -1
            conversation.last = nil
            conversation.topics = nil
            try save(chat, conversation)
        }

        if merged {
            try save(chat, conversation)
            try remove("meta.\(chat)")
        }

        open[chat] = conversation

        return conversation
    }

    private static func decode(_ plain: [UInt8]) throws -> Conversation {
        guard plain.count >= 56 else { throw Failure(description: "invalid conversation key") }

        var conversation = Conversation(
            key: SymmetricKey(data: plain[0..<32]),
            count: number(plain, 32), read: number(plain, 40), flags: number(plain, 48))

        if plain.count >= 68 {
            let size = Int(plain[64]) << 24 | Int(plain[65]) << 16 | Int(plain[66]) << 8 | Int(plain[67])

            guard plain.count == 68 + size || plain.count == 72 + size || plain.count >= 76 + size else {
                throw Failure(description: "invalid conversation key")
            }

            conversation.unread = Int64(bitPattern: Self.number(plain, 56))

            if size > 0 {
                conversation.last = Data(plain[68..<(68 + size)])
            }

            if plain.count >= 72 + size {
                conversation.ambry = UInt32(plain[68 + size]) << 24 | UInt32(plain[69 + size]) << 16 |
                    UInt32(plain[70 + size]) << 8 | UInt32(plain[71 + size])
            }

            if plain.count >= 76 + size {
                let topics = Int(plain[72 + size]) << 24 | Int(plain[73 + size]) << 16 |
                    Int(plain[74 + size]) << 8 | Int(plain[75 + size])

                guard plain.count == 76 + size + topics else {
                    throw Failure(description: "invalid conversation key")
                }

                if topics > 0 {
                    conversation.topics = Data(plain[(76 + size)..<(76 + size + topics)])
                }
            }
        }

        return conversation
    }

    private func save(_ chat: String, _ conversation: Conversation) throws {
        var plain = conversation.key.withUnsafeBytes { Data($0) }
        plain.append(contentsOf: Self.bytes(conversation.count))
        plain.append(contentsOf: Self.bytes(conversation.read))
        plain.append(contentsOf: Self.bytes(conversation.flags))
        plain.append(contentsOf: Self.bytes(UInt64(bitPattern: conversation.unread)))
        plain.append(contentsOf: Self.bytes(UInt32(conversation.last?.count ?? 0)))
        plain.append(conversation.last ?? Data())
        plain.append(contentsOf: Self.bytes(conversation.ambry))
        plain.append(contentsOf: Self.bytes(UInt32(conversation.topics?.count ?? 0)))
        plain.append(conversation.topics ?? Data())

        try store("key.\(chat)",
                  seal(plain, to: agreementPublic(), info: Self.keyAad + Self.label(chat)),
                  unlocked: true)
    }

    private func seal(_ plain: Data, to recipient: P256.KeyAgreement.PublicKey,
                      info: Data) throws -> Data {
        let ephemeral = P256.KeyAgreement.PrivateKey()
        let encoded = ephemeral.publicKey.x963Representation
        let shared = try ephemeral.sharedSecretFromKeyAgreement(with: recipient)
        let key = shared.hkdfDerivedSymmetricKey(
            using: SHA256.self, salt: encoded + recipient.x963Representation,
            sharedInfo: info, outputByteCount: 32)

        var out = Data([1, UInt8(encoded.count >> 8), UInt8(encoded.count & 0xff)])
        out.append(encoded)
        out.append(try sealBox(plain, key: key, aad: info))

        return out
    }

    private func unseal(_ data: Data, info: Data) throws -> Data {
        let bytes = [UInt8](data)
        guard bytes.count > 3, bytes[0] == 1 else { throw CryptoKitError.incorrectParameterSize }

        let len = Int(bytes[1]) << 8 | Int(bytes[2])
        guard bytes.count > 3 + len else { throw CryptoKitError.incorrectParameterSize }

        let encoded = Data(bytes[3..<(3 + len)])
        let ephemeral = try P256.KeyAgreement.PublicKey(x963Representation: encoded)
        let recipient = try agreementPublic()
        let key = try agree(ephemeral).hkdfDerivedSymmetricKey(
            using: SHA256.self, salt: encoded + recipient.x963Representation,
            sharedInfo: info, outputByteCount: 32)

        return try openBox(Data(bytes[(3 + len)...]), key: key, aad: info)
    }

    private func sealBox(_ plain: Data, key: SymmetricKey, aad: Data) throws -> Data {
        guard let combined = try AES.GCM.seal(plain, using: key, authenticating: aad).combined else {
            throw Failure(description: "failed to seal")
        }

        return combined
    }

    private func openBox(_ sealed: Data, key: SymmetricKey, aad: Data) throws -> Data {
        try AES.GCM.open(AES.GCM.SealedBox(combined: sealed), using: key, authenticating: aad)
    }

    private func agreementPublic() throws -> P256.KeyAgreement.PublicKey {
        if let raw = try item("inbox.public") {
            return try P256.KeyAgreement.PublicKey(x963Representation: raw)
        }

        guard owner else { throw Failure(description: "the chat keys are not set up yet") }

        let secret: Data
        let recipient: P256.KeyAgreement.PublicKey

        if SecureEnclave.isAvailable {
            var error: Unmanaged<CFError>?
            guard let access = SecAccessControlCreateWithFlags(
                nil, kSecAttrAccessibleWhenUnlockedThisDeviceOnly, .privateKeyUsage, &error) else {
                throw Failure(description: "no access control for the inbox key")
            }

            let key = try SecureEnclave.P256.KeyAgreement.PrivateKey(accessControl: access)
            secret = key.dataRepresentation
            recipient = key.publicKey
        } else {
            let key = P256.KeyAgreement.PrivateKey()
            secret = key.rawRepresentation
            recipient = key.publicKey
        }

        try store("inbox.private", secret, unlocked: true)
        try store("inbox.public", recipient.x963Representation, unlocked: false)

        return recipient
    }

    private func agree(_ ephemeral: P256.KeyAgreement.PublicKey) throws -> SharedSecret {
        guard let raw = try item("inbox.private") else { throw Failure(description: "no inbox key") }

        if SecureEnclave.isAvailable {
            return try SecureEnclave.P256.KeyAgreement.PrivateKey(dataRepresentation: raw)
                .sharedSecretFromKeyAgreement(with: ephemeral)
        }

        return try P256.KeyAgreement.PrivateKey(rawRepresentation: raw)
            .sharedSecretFromKeyAgreement(with: ephemeral)
    }

    private func outboxKey() throws -> SymmetricKey {
        if let raw = try item("outbox") { return SymmetricKey(data: raw) }

        guard owner else { throw Failure(description: "the chat keys are not set up yet") }

        let key = SymmetricKey(size: .bits256)
        try store("outbox", key.withUnsafeBytes { Data($0) }, unlocked: false)

        return key
    }

    private func query(_ account: String) -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: Self.service,
            kSecAttrAccount as String: account,
            kSecAttrAccessGroup as String: TunnelKeychain.group,
        ]
    }

    private func item(_ account: String) throws -> Data? {
        var search = query(account)
        search[kSecReturnData as String] = true
        search[kSecMatchLimit as String] = kSecMatchLimitOne

        var out: CFTypeRef?
        let status = SecItemCopyMatching(search as CFDictionary, &out)

        switch status {
        case errSecSuccess:
            return out as? Data
        case errSecItemNotFound:
            return nil
        case errSecInteractionNotAllowed:
            throw Locked()
        default:
            throw Failure(description: "keychain error \(status)")
        }
    }

    private func store(_ account: String, _ data: Data, unlocked: Bool) throws {
        var status = SecItemUpdate(query(account) as CFDictionary,
                                   [kSecValueData as String: data] as CFDictionary)

        if status == errSecItemNotFound {
            var add = query(account)
            add[kSecValueData as String] = data
            add[kSecAttrAccessible as String] = unlocked
                ? kSecAttrAccessibleWhenUnlockedThisDeviceOnly
                : kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            status = SecItemAdd(add as CFDictionary, nil)
        }

        if status == errSecInteractionNotAllowed { throw Locked() }
        guard status == errSecSuccess else { throw Failure(description: "keychain error \(status)") }
    }

    private func remove(_ account: String) throws {
        let status = SecItemDelete(query(account) as CFDictionary)

        guard status == errSecSuccess || status == errSecItemNotFound else {
            throw Failure(description: "keychain error \(status)")
        }
    }

    private func folder(_ url: URL?) throws -> URL {
        guard let url else { throw Failure(description: "no app group container") }
        return url
    }

    private func write(_ file: URL, _ data: Data) throws {
        try data.write(to: file, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
    }

    private func wipe(_ file: URL) throws {
        guard FileManager.default.fileExists(atPath: file.path) else { return }

        let size = try FileManager.default.attributesOfItem(atPath: file.path)[.size] as? NSNumber ?? 0
        let handle = try FileHandle(forWritingTo: file)

        try handle.write(contentsOf: Data(count: size.intValue))
        try handle.synchronize()
        try handle.close()

        try FileManager.default.removeItem(at: file)
    }

    static func legacyFlock() -> String? {
        let gospels = defaults.array(forKey: "gospels") as? [[String: Any]] ?? []

        return gospels.first?["flock"] as? String
    }

    func migrateFlock() throws {
        try mutex.withLock {
            let marker = try folder(dir).appendingPathComponent("flocked")
            let step = (try? String(contentsOf: marker, encoding: .utf8)) ?? ""

            guard step != Self.migrated else { return }
            guard let flock = Self.legacyFlock() else { return }

            if step.isEmpty {
                try exclusive { try migrateLists(flock) }
                try migrateBoxes(flock)
                try write(marker, Data(Self.boxed.utf8))
            }

            guard owner else { return }

            try migrateInbox(flock)

            let names = (try? FileManager.default.contentsOfDirectory(atPath: folder(dir).path)) ?? []

            for name in names where name.hasSuffix(".log") {
                let old = String(name.dropLast(4))

                guard old.range(of: Self.oldChat, options: .regularExpression) != nil else { continue }

                do {
                    try migrateChat(old, "\(flock)-\(old)", flock)
                } catch is CryptoKitError {
                    continue
                }
            }

            try migratePeers(folder(self.names), Self.nameAad, flock)
            try migratePeers(folder(avatars), Self.avatarAad, flock)
            try write(marker, Data(Self.migrated.utf8))
        }
    }

    private func migrateLists(_ flock: String) throws {
        let list = try outboxList()

        if !list.isEmpty {
            try outboxSave(list.map { entry in
                var moved = entry

                for key in ["peer", "chat", "data"] {
                    if let value = entry[key] as? String {
                        moved[key] = Self.moved(flock, value)
                    }
                }

                return moved
            })
        }

        let dir = try folder(self.dir)

        for (name, aad) in [("groups.box", Self.groupsAad), ("ambry.box", Self.ambryAad)] {
            let file = dir.appendingPathComponent(name)
            guard FileManager.default.fileExists(atPath: file.path) else { continue }

            let old = try JSONSerialization.jsonObject(
                with: openBox(Data(contentsOf: file), key: outboxKey(), aad: aad)) as? [String: Any] ?? [:]
            let moved = Dictionary(uniqueKeysWithValues: old.map { (Self.moved(flock, $0.key), $0.value) })

            try write(file, sealBox(JSONSerialization.data(withJSONObject: moved), key: outboxKey(), aad: aad))
        }

        for (name, aad) in [("accepted.box", Self.acceptedAad), ("ignored.box", Self.ignoredAad)] {
            let file = dir.appendingPathComponent(name)
            guard FileManager.default.fileExists(atPath: file.path) else { continue }

            let old = try JSONSerialization.jsonObject(
                with: openBox(Data(contentsOf: file), key: outboxKey(), aad: aad)) as? [String] ?? []

            try write(file, sealBox(JSONSerialization.data(withJSONObject: old.map { Self.moved(flock, $0) }),
                                    key: outboxKey(), aad: aad))
        }

        let receipts = dir.appendingPathComponent("receipts.box")

        if FileManager.default.fileExists(atPath: receipts.path) {
            let old = try JSONSerialization.jsonObject(
                with: openBox(Data(contentsOf: receipts), key: outboxKey(), aad: Self.receiptsAad))
                as? [[String: Any]] ?? []
            let moved = old.map { receipt -> [String: Any] in
                var entry = receipt
                entry["peer"] = Self.moved(flock, receipt["peer"] as? String ?? "")
                return entry
            }

            try write(receipts, sealBox(JSONSerialization.data(withJSONObject: moved), key: outboxKey(),
                                        aad: Self.receiptsAad))
        }

        Self.defaults.set(Self.accepts + 1, forKey: "chatAccepts")
    }

    private func migrateBoxes(_ flock: String) throws {
        try lostChange { lost in
            lost.chats = Set(lost.chats.map { Self.moved(flock, $0) })
            lost.files = Dictionary(uniqueKeysWithValues: lost.files.map {
                (Self.moved(flock, $0.key), Self.moved(flock, $0.value))
            })
            return true
        }

        let safety = try folder(self.safety)
        let files = (try? FileManager.default.contentsOfDirectory(atPath: safety.path)) ?? []

        for name in files where name.count == 6 && name.hasSuffix(".box") {
            guard let peer = UInt8(name.prefix(2), radix: 16) else { continue }

            let file = safety.appendingPathComponent(name)
            let sealed = try Data(contentsOf: file)
            let aad = Self.safetyAad + [peer]

            guard sealed.count > Self.wrapped else {
                try wipe(file)
                continue
            }

            var raw = try openBox(Data(sealed.prefix(Self.wrapped)), key: outboxKey(), aad: aad)
            defer { raw.resetBytes(in: 0..<raw.count) }

            var plain = try openBox(Data(sealed.dropFirst(Self.wrapped)), key: SymmetricKey(data: raw), aad: aad)
            defer { plain.resetBytes(in: 0..<plain.count) }

            try safetyCode("\(flock)-\(name.prefix(2))", plain)
            try wipe(file)
        }

        _ = state { all in
            for key in ["names", "mutes"] {
                if let old = all[key] as? [String: Any] {
                    all[key] = Dictionary(uniqueKeysWithValues: old.map { (Self.moved(flock, $0.key), $0.value) })
                }
            }

            for key in ["stars", "compacts", "order", "codes"] {
                if let old = all[key] as? [String] {
                    all[key] = old.map { Self.moved(flock, $0) }
                }
            }

            if let old = all["unseen"] as? [String: Any] {
                all["unseen"] = Dictionary(uniqueKeysWithValues: old.map { (Self.moved(flock, $0.key), $0.value) })
            }
        }
    }

    private static func moved(_ flock: String, _ id: String) -> String {
        id.range(of: oldId, options: .regularExpression) != nil ? "\(flock)-\(id)" : id
    }

    private func migrateInbox(_ flock: String) throws {
        let inbox = try folder(inbox)
        let names = (try? FileManager.default.contentsOfDirectory(atPath: inbox.path)) ?? []

        for name in names where name.hasSuffix(".box") {
            let file = inbox.appendingPathComponent(name)

            guard let plain = try? unseal(Data(contentsOf: file), info: Self.inboxAad),
                  var entry = (try? JSONSerialization.jsonObject(with: plain)) as? [String: Any] else { continue }

            let chat = entry["chat"] as? String ?? entry["peer"] as? String ?? ""

            guard chat.range(of: Self.oldChat, options: .regularExpression) != nil else { continue }

            entry["chat"] = "\(flock)-\(chat)"

            if var rec = entry["rec"] as? [String: Any], let data = rec["data"] as? String {
                rec["data"] = "\(flock)-\(data)"
                entry["rec"] = rec
            }

            try write(file, seal(JSONSerialization.data(withJSONObject: entry), to: agreementPublic(),
                                 info: Self.inboxAad))
        }
    }

    private func migrateChat(_ old: String, _ new: String, _ flock: String) throws {
        let label = old.count == 2 ? Data([UInt8(old, radix: 16) ?? 0]) : Data(old.utf8)
        let dir = try folder(self.dir)

        guard let wrapped = try item("key.\(old)") else { return }

        var plain = [UInt8](try unseal(wrapped, info: Self.keyAad + label))

        if plain.count == 32, let sealed = try item("meta.\(old)") {
            plain.append(contentsOf: [UInt8](try unseal(sealed, info: Self.metaAad + label)))
        }

        var conversation = try Self.decode(plain)

        plain.withUnsafeMutableBufferPointer { $0.update(repeating: 0) }

        if let last = conversation.last, let text = String(data: last, encoding: .utf8) {
            conversation.last = Data(text.replacingOccurrences(of: "\"\(old)-", with: "\"\(new)-").utf8)
        }

        if let topics = conversation.topics, let text = String(data: topics, encoding: .utf8) {
            conversation.topics = Data(text.replacingOccurrences(of: "\"\(old)-", with: "\"\(new)-").utf8)
        }

        let files = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        let data = files.filter { $0.range(of: "^\(old)-[0-9a-f]{16}\\.dat$", options: .regularExpression) != nil }

        for name in data {
            let moved = new + name.dropFirst(old.count)
            var aad = Self.dataAad
            aad.append(label)
            aad.append(Data(name.utf8))

            var content = try openBox(Data(contentsOf: dir.appendingPathComponent(name)),
                                      key: conversation.key, aad: aad)
            defer { content.resetBytes(in: 0..<content.count) }

            try write(dir.appendingPathComponent(moved),
                      sealBox(content, key: conversation.key, aad: Self.dataAad(new, moved)))
        }

        let log = dir.appendingPathComponent("\(old).log")
        let bytes = [UInt8]((try? Data(contentsOf: log)) ?? Data())
        var out = Data()
        var offset = 0
        var seq: UInt64 = 0

        while offset + 4 <= bytes.count {
            let len = Int(bytes[offset]) << 24 | Int(bytes[offset + 1]) << 16 |
                Int(bytes[offset + 2]) << 8 | Int(bytes[offset + 3])
            guard len >= 28, offset + 4 + len <= bytes.count else { break }

            var aad = Self.recordAad
            aad.append(label)
            aad.append(contentsOf: Self.bytes(seq))

            guard let record = try? openBox(Data(bytes[(offset + 4)..<(offset + 4 + len)]),
                                            key: conversation.key, aad: aad),
                  var json = (try? JSONSerialization.jsonObject(with: record)) as? [String: Any] else { break }

            if let file = json["data"] as? String, file.hasPrefix("\(old)-") {
                json["data"] = new + file.dropFirst(old.count)
            }

            let sealed = try sealBox(JSONSerialization.data(withJSONObject: json), key: conversation.key,
                                     aad: Self.aad(new, seq: seq))

            out.append(contentsOf: Self.bytes(UInt32(sealed.count)))
            out.append(sealed)

            offset += 4 + len
            seq += 1
        }

        conversation.count = seq

        try write(dir.appendingPathComponent("\(new).log"), out)
        try save(new, conversation)

        try remove("meta.\(old)")
        try remove("key.\(old)")
        try wipe(log)

        for name in data {
            try wipe(dir.appendingPathComponent(name))
        }
    }

    private func migratePeers(_ folder: URL, _ prefix: Data, _ flock: String) throws {
        let files = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []

        for name in files where name.count == 6 && name.hasSuffix(".box") {
            guard let peer = UInt8(name.prefix(2), radix: 16) else { continue }

            let file = folder.appendingPathComponent(name)

            guard let plain = try? unseal(Data(contentsOf: file), info: prefix + [peer]) else {
                try wipe(file)
                continue
            }

            let id = "\(flock)-\(name.prefix(2))"

            try write(folder.appendingPathComponent("\(id).box"),
                      seal(plain, to: agreementPublic(), info: prefix + Data(id.utf8)))
            try wipe(file)
        }
    }

    private static func aad(_ chat: String, seq: UInt64) -> Data {
        var aad = recordAad
        aad.append(label(chat))
        aad.append(contentsOf: bytes(seq))
        return aad
    }

    private static func label(_ chat: String) -> Data {
        Data(chat.utf8)
    }

    private static func bytes<T: FixedWidthInteger>(_ value: T) -> [UInt8] {
        withUnsafeBytes(of: value.bigEndian) { Array($0) }
    }

    private static func number(_ bytes: [UInt8], _ offset: Int) -> UInt64 {
        bytes[offset..<(offset + 8)].reduce(0) { $0 << 8 | UInt64($1) }
    }

    static func hex(_ peer: UInt8) -> String { String(format: "%02x", peer) }

    private static func box(_ peer: String) -> String { "\(peer).box" }

    static func chat(_ id: String) -> String? {
        if group(id) != nil { return id }
        return Flock.valid(id) ? id : nil
    }

    static func group(_ chat: String) -> (flock: String, owner: UInt8, id: UInt64)? {
        guard chat.range(of: "^[0-9a-f]{1,16}-g[0-9a-f]{18}$", options: .regularExpression) != nil else {
            return nil
        }

        let local = Flock.local(chat)

        guard let owner = UInt8(local.dropFirst().prefix(2), radix: 16),
              let id = UInt64(local.dropFirst(3), radix: 16) else { return nil }
        return (Flock.of(chat), owner, id)
    }

    static func groupChat(_ flock: String, _ owner: String, _ group: String) -> String {
        "\(flock)-g\(owner)\(group)"
    }

    static func name(_ peer: String) -> String { names()[peer] ?? peer }

    static func name(chat: String) -> String {
        guard group(chat) != nil else { return names()[chat] ?? chat }

        let name = (try? shared.groupInfo(chat))?["name"] as? String ?? ""

        return name.isEmpty ? "Group" : name
    }

    static func names() -> [String: String] {
        let all = shared.state()["names"] as? [String: String] ?? [:]

        return all.filter { Flock.valid($0.key) }
    }

    static func save(_ names: [String: Any]) -> [String: String] {
        var clean = [String: String]()

        for (key, value) in names {
            guard Flock.valid(key), let value = value as? String else { continue }

            let name = String(value.trimmingCharacters(in: .whitespacesAndNewlines).prefix(32))
            if !name.isEmpty { clean[key] = name }
        }

        _ = shared.state { $0["names"] = clean }
        shared.emit { (prefs(), nil) }

        return clean
    }

    static func mute(_ chat: String, until: Int) {
        _ = shared.state { all in
            var mutes = all["mutes"] as? [String: Int] ?? [:]
            mutes[chat] = until == 0 ? nil : until
            all["mutes"] = mutes
        }

        shared.emit { (prefs(), nil) }
    }

    static func muted(_ chat: String) -> Int {
        let mutes = shared.state()["mutes"] as? [String: Int] ?? [:]
        let until = mutes[chat] ?? 0

        if until > 0 && until <= Int(Date().timeIntervalSince1970 * 1000) {
            return 0
        }

        return until
    }

    static func star(_ chat: String, on: Bool) {
        mark("stars", chat, on: on)
    }

    static func compact(_ chat: String, on: Bool) {
        mark("compacts", chat, on: on)
    }

    private static func mark(_ key: String, _ chat: String, on: Bool) {
        _ = shared.state { all in
            var list = all[key] as? [String] ?? []

            list.removeAll { $0 == chat }

            if on {
                list.append(chat)
            }

            all[key] = list
        }

        shared.emit { (prefs(), nil) }
    }

    static func order(_ chats: [String]) {
        _ = shared.state { $0["order"] = chats }
        shared.emit { (prefs(), nil) }
    }

    static func prefs() -> [String: Any] {
        let all = shared.state()
        let mark = { (list: [String]) in Dictionary(list.map { ($0, true) }) { a, _ in a } }

        return [
            "op": "prefs",
            "stars": mark(all["stars"] as? [String] ?? []),
            "compacts": mark(all["compacts"] as? [String] ?? []),
            "mutes": all["mutes"] as? [String: Int] ?? [:],
            "order": all["order"] as? [String] ?? [],
            "names": names(),
        ]
    }

    static func seen(_ chat: String) {
        let unseen = shared.unseen { $0[chat] = nil }
        let center = UNUserNotificationCenter.current()
        center.setBadgeCount(unseen.values.reduce(0, +))
        center.getDeliveredNotifications { delivered in
            center.removeDeliveredNotifications(
                withIdentifiers: delivered
                    .filter { $0.request.content.threadIdentifier == "chat-\(chat)" }
                    .map { $0.request.identifier })
        }
    }

    static func unlist(_ chat: String) {
        _ = shared.state { all in
            for key in ["stars", "compacts", "order"] {
                all[key] = (all[key] as? [String] ?? []).filter { $0 != chat }
            }
        }

        shared.emit { (prefs(), nil) }
    }

    static func arrange(_ chats: [[String: Any]]) -> [[String: Any]] {
        let all = shared.state()
        let stars = all["stars"] as? [String] ?? []
        let compacts = all["compacts"] as? [String] ?? []
        let order = all["order"] as? [String] ?? []

        return chats.map { chat in
            let id = chat["peer"] as? String ?? ""
            var entry = chat

            entry["star"] = stars.contains(id)
            entry["compact"] = compacts.contains(id)
            entry["order"] = order.firstIndex(of: id) ?? -1

            return entry
        }
    }

    static let callCategory = "call"

    static func callActions() {
        let answer = UNNotificationAction(identifier: "answer", title: "Answer", options: [.foreground])
        let decline = UNNotificationAction(identifier: "decline", title: "Decline", options: [.destructive])

        UNUserNotificationCenter.current().setNotificationCategories([
            UNNotificationCategory(identifier: callCategory, actions: [answer, decline], intentIdentifiers: []),
        ])
    }

    private static let oldChat = "^([0-9a-f]{2}|g[0-9a-f]{18})$"
    private static let oldId = "^([0-9a-f]{2}|g[0-9a-f]{18})(-[0-9a-f]{16}\\.dat)?$"
    private static let boxed = "1"
    private static let migrated = "2"
    private static let namesKey = "peerNames"
    private static let mutesKey = "chatMutes"
    private static let starsKey = "chatStars"
    private static let orderKey = "chatOrder"
    private static let compactsKey = "chatCompacts"
}
