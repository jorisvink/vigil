import CryptoKit
import Foundation
import Security
import UserNotifications
import Vigil

final class ChatVault {
    struct Locked: Error, CustomStringConvertible {
        var description: String { "Unlock your device to read your chats." }
    }

    struct Failure: Error, CustomStringConvertible {
        let description: String
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
        var count: UInt64
        var read: UInt64
        var flags: UInt64
        var unread: Int64 = -1
        var last: Data?
        var ambry: UInt32 = 0
    }

    private final class History {
        let chat: String
        let epoch: Int64
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

        init(chat: String, epoch: Int64) {
            self.chat = chat
            self.epoch = epoch
        }
    }

    static let shared = ChatVault()
    static let selfAuthor = "00"

    static let defaults = UserDefaults(suiteName: TunnelKeychain.group) ?? .standard

    private static let service = "se.sanctorum.vigil.chat"
    private static let keyAad = Data("VIGIL.CHAT.KEY".utf8)
    private static let metaAad = Data("VIGIL.CHAT.META".utf8)
    private static let recordAad = Data("VIGIL.CHAT.V1".utf8)
    private static let inboxAad = Data("VIGIL.CHAT.INBOX".utf8)
    private static let outboxAad = Data("VIGIL.CHAT.OUTBOX".utf8)
    private static let queuedAad = Data("VIGIL.CHAT.QUEUED".utf8)
    private static let receiptsAad = Data("VIGIL.CHAT.RECEIPTS".utf8)
    private static let dataAad = Data("VIGIL.CHAT.DATA".utf8)
    private static let avatarAad = Data("VIGIL.CHAT.AVATAR".utf8)
    private static let nameAad = Data("VIGIL.CHAT.NAME".utf8)
    private static let truncated: UInt64 = 1

    private let dir: URL?
    private let inbox: URL?
    private let avatars: URL?
    private let names: URL?
    private var open = [String: Conversation]()
    private var keys = [String: SymmetricKey]()
    private var focused: String?
    private var awake = false
    private var depth = 0
    private var history: History?
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
        avatars = base?.appendingPathComponent("avatars", isDirectory: true)
        names = base?.appendingPathComponent("names", isDirectory: true)

        for case var url? in [dir, inbox, avatars, names] {
            try? FileManager.default.createDirectory(
                at: url, withIntermediateDirectories: true,
                attributes: [.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication])

            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try? url.setResourceValues(values)
        }
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        mutex.lock()
        depth += 1

        defer {
            depth -= 1

            if depth == 0 {
                settle()
            }

            mutex.unlock()
        }

        return try body()
    }

    func prepare() throws {
        try locked {
            _ = try agreementPublic()
            _ = try outboxKey()
            try exclusive { try settleQueued() }
        }
    }

    func wake() {
        locked {
            awake = true
        }
    }

    func lock() {
        locked {
            awake = false
            focused = nil
        }
    }

    func focus(_ chat: String, on: Bool) {
        locked {
            if on && awake {
                focused = chat
            } else if focused == chat {
                focused = nil
            }
        }
    }

    private func settle() {
        if !awake {
            open.removeAll()
        }

        keys = keys.filter { $0.key == focused }

        if history?.chat != focused {
            history = nil
        }
    }

    func ref() -> Int64 {
        return locked {
            refs = max(refs + 1, Int64(Date().timeIntervalSince1970 * 1000) * 1000)
            return refs
        }
    }

    func deliver(chat: String, peer: UInt8, nick: String, text: String, time: Int64,
                 kind: String = "", data: Data? = nil, media: String = "",
                 name: String = "", quote: Quote? = nil) throws {
        try locked {
            var message: [String: Any] = [
                "chat": chat, "peer": Self.hex(peer), "nick": nick, "text": text, "time": time,
                "kind": kind,
            ]

            if let data {
                message["data"] = data.base64EncodedString()
                message["media"] = media
                message["name"] = name
            }

            if let quote {
                message["qa"] = quote.author
                message["qh"] = quote.hash
            }

            let plain = try JSONSerialization.data(withJSONObject: message)

            let sealed = try seal(plain, to: agreementPublic(), info: Self.inboxAad)
            try write(try folder(inbox).appendingPathComponent(
                String(format: "%016llx-%016llx.box", UInt64(time), UInt64.random(in: 0...UInt64.max))),
                sealed)
        }
    }

    func merge() throws {
        try locked {
            let inbox = try folder(inbox)
            let pending = (try? FileManager.default.contentsOfDirectory(atPath: inbox.path)) ?? []

            for name in pending.filter({ $0.hasSuffix(".box") }).sorted() {
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

                guard let message = try JSONSerialization.jsonObject(with: plain) as? [String: Any],
                      let peer = message["peer"] as? String,
                      let chat = Self.chat(message["chat"] as? String ?? peer),
                      let text = message["text"] as? String else {
                    try wipe(file)
                    continue
                }

                let kind = message["kind"] as? String ?? ""

                if kind == "ambry", try !rotated(chat, text) {
                    try wipe(file)
                    continue
                }

                let ref = ref()
                var record: [String: Any] = [
                    "t": kind.isEmpty ? "msg" : "member", "kind": kind, "dir": "in", "ref": ref,
                    "peer": peer, "text": text, "nick": message["nick"] as? String ?? "",
                    "time": (message["time"] as? NSNumber)?.int64Value ?? 0,
                ]

                if let data = (message["data"] as? String).flatMap({ Data(base64Encoded: $0) }) {
                    record["data"] = try dataWrite(chat, ref: ref, data)
                    record["media"] = message["media"] as? String ?? "file"
                    record["name"] = message["name"] as? String ?? "file"
                }

                if let quote = Self.quote(message["qa"] as? String, message["qh"] as? String) {
                    record["qa"] = quote.author
                    record["qh"] = quote.hash
                }

                try append(chat, record)

                try wipe(file)
            }

            let receipts = try folder(dir).appendingPathComponent("receipts.box")

            try exclusive {
                guard FileManager.default.fileExists(atPath: receipts.path) else { return }

                let list = try JSONSerialization.jsonObject(
                    with: openBox(Data(contentsOf: receipts), key: outboxKey(), aad: Self.receiptsAad))
                    as? [[String: Any]] ?? []

                for receipt in list {
                    guard let chat = (receipt["peer"] as? String).flatMap({ Self.chat($0) }),
                          let kind = receipt["t"] as? String,
                          let ref = (receipt["ref"] as? NSNumber)?.int64Value,
                          try item("key.\(chat)") != nil else { continue }

                    try append(chat, [
                        "t": kind, "ref": ref,
                        "acked": receipt["acked"] ?? 0, "pending": receipt["pending"] ?? 0,
                    ])
                }

                try wipe(receipts)
            }
        }
    }

    private func rotated(_ chat: String, _ text: String) throws -> Bool {
        guard let generation = UInt32(text, radix: 16),
              var conversation = try conversation(chat, create: false),
              conversation.ambry != generation else { return false }

        let known = conversation.ambry != 0

        conversation.ambry = generation
        open[chat] = conversation
        try save(chat, conversation)

        return known
    }

    func conversations() throws -> [[String: Any]] {
        return try locked {
            let names = (try? FileManager.default.contentsOfDirectory(atPath: folder(dir).path)) ?? []
            var out = [[String: Any]]()

            for name in names where name.hasSuffix(".log") {
                guard let id = Self.chat(String(name.dropLast(4))) else { continue }
                guard var conversation = try conversation(id, create: false) else { continue }

                if conversation.unread < 0 {
                    let full = History(chat: id, epoch: 0)

                    do {
                        try load(id, conversation, full)
                        try summarize(id, &conversation, full)
                    } catch CryptoKitError.authenticationFailure {
                        conversation.unread = 0
                    }
                }

                var last: Any = NSNull()

                if let data = conversation.last, let message = try? JSONSerialization.jsonObject(with: data) {
                    last = message
                }

                out.append(["peer": id, "unread": conversation.unread, "last": last])
            }

            return out
        }
    }

    func messages(_ chat: String, epoch: Int64 = 0, stamp: Int64 = 0) throws -> [String: Any] {
        return try locked {
            guard var conversation = try conversation(chat, create: false) else {
                return ["messages": [], "unread": 0]
            }

            let current = try cached(chat)
            let fresh = current.offset == 0

            try load(chat, conversation, current)

            if fresh, conversation.unread < 0 {
                try summarize(chat, &conversation, current)
            }

            var out: [String: Any] = [
                "unread": conversation.unread, "epoch": current.epoch, "stamp": current.stamp,
            ]

            if epoch == current.epoch {
                out["messages"] = current.messages.indices
                    .filter { current.stamps[$0] > stamp }
                    .map { current.messages[$0] }
                out["delta"] = true
            } else {
                out["messages"] = current.messages
            }

            if current.damaged {
                out["warning"] = "Part of this chat could not be read, it was damaged or tampered with."
            } else if conversation.flags & Self.truncated != 0 {
                out["warning"] = "Messages were removed from the end of this chat outside of Vigil."
            }

            return out
        }
    }

    func send(_ chat: String, text: String, quote: Quote? = nil) throws -> Int64 {
        return try locked {
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

            try append(chat, record)

            guard Self.group(chat) == nil else { return ref }

            try exclusive {
                var list = try outboxList()
                list.append(entry)
                try outboxSave(list)
            }

            return ref
        }
    }

    func quoted(_ chat: String, ref: Int64) throws -> Quote {
        return try locked {
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
        conversation.unread = full.unread
        conversation.last = try full.messages.last.map { try JSONSerialization.data(withJSONObject: $0) }
        open[chat] = conversation
        try save(chat, conversation)
    }

    private func load(_ chat: String, _ conversation: Conversation, _ current: History) throws {
        current.stamp += 1

        let file = try folder(dir).appendingPathComponent("\(chat).log")
        guard let handle = try? FileHandle(forReadingFrom: file) else { return }
        defer { try? handle.close() }

        try handle.seek(toOffset: current.offset)
        guard let data = try handle.readToEnd() else { return }
        guard let key = try key(chat) else { return }

        let bytes = [UInt8](data)
        var offset = 0

        while offset + 4 <= bytes.count {
            let len = Int(bytes[offset]) << 24 | Int(bytes[offset + 1]) << 16 |
                Int(bytes[offset + 2]) << 8 | Int(bytes[offset + 3])
            guard len >= 28, offset + 4 + len <= bytes.count else { break }

            let sealed = Data(bytes[(offset + 4)..<(offset + 4 + len)])

            guard let plain = try? openBox(sealed, key: key,
                                           aad: Self.aad(chat, seq: current.seq)) else {
                current.damaged = true
                break
            }

            if let record = try JSONSerialization.jsonObject(with: plain) as? [String: Any] {
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

            if message["dir"] as? String == "in", current.seq >= read {
                current.unread += 1
            }

            current.index[ref] = current.messages.count
            current.messages.append(message)
            current.stamps.append(current.stamp)

            if message["qh"] != nil {
                Self.quote(current, current.messages.count - 1)
            }
        case "member":
            current.messages.append(Self.message(record))
            current.stamps.append(current.stamp)
        case "sent":
            change(current, ref) {
                if $0["state"] as? String == "queued" {
                    $0["state"] = "sent"
                }
            }
        case "delivered":
            change(current, ref) { $0["state"] = "delivered" }
        case "failed":
            change(current, ref) {
                if $0["state"] as? String == "queued" {
                    $0["state"] = "failed"
                }
            }
        case "acks":
            let pending = (record["pending"] as? NSNumber)?.intValue ?? 0

            change(current, ref) {
                $0["acked"] = (record["acked"] as? NSNumber)?.intValue ?? 0
                $0["pending"] = pending
                $0["state"] = pending == 0 ? "delivered" : "sent"
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

        return message
    }

    private static func quote(_ current: History, _ at: Int) {
        guard let wanted = Self.quote(current.messages[at]["qa"] as? String,
                                      current.messages[at]["qh"] as? String) else { return }

        current.messages[at]["quoted"] = true

        for other in stride(from: at - 1, through: 0, by: -1) {
            guard current.messages[other]["kind"] == nil else { continue }
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

    func sendData(_ chat: String, media: String, name: String, data: Data) throws -> (Int64, String) {
        try locked {
            let ref = ref()
            let file = try dataWrite(chat, ref: ref, data)

            try queue(UInt64(bitPattern: ref), data)

            try append(chat, [
                "t": "msg", "dir": "out", "ref": ref, "text": "", "data": file, "media": media,
                "name": name, "time": Int64(Date().timeIntervalSince1970 * 1000),
            ])

            guard Self.group(chat) == nil else { return (ref, file) }

            try exclusive {
                var list = try outboxList()
                list.append(["peer": chat, "ref": ref, "data": file, "media": media, "name": name])
                try outboxSave(list)
            }

            return (ref, file)
        }
    }

    func data(_ chat: String, file: String) throws -> Data? {
        return try locked {
            guard Self.data(file, of: chat) else {
                throw Failure(description: "There is no data \(file).")
            }

            guard let key = try key(chat) else { return nil }

            let path = try folder(dir).appendingPathComponent(file)
            guard let sealed = try? Data(contentsOf: path) else { return nil }

            return try openBox(sealed, key: key, aad: Self.dataAad(chat, file))
        }
    }

    func queued(_ ref: UInt64) throws -> Data? {
        return try locked {
            let name = Self.queuedName(ref)
            let path = try folder(dir).appendingPathComponent(name)
            guard let sealed = try? Data(contentsOf: path) else { return nil }

            return try openBox(sealed, key: outboxKey(), aad: Self.queuedAad + Data(name.utf8))
        }
    }

    func unqueue(_ ref: UInt64) throws {
        try locked {
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
            guard let chat = entry["peer"] as? String, let file = entry["data"] as? String,
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
        return try locked {
            let file = try folder(dir).appendingPathComponent("avatar.box")
            guard FileManager.default.fileExists(atPath: file.path) else { return nil }

            return try openBox(Data(contentsOf: file), key: outboxKey(), aad: Self.avatarAad)
        }
    }

    func ownAvatar(_ data: Data?) throws {
        try locked {
            let file = try folder(dir).appendingPathComponent("avatar.box")

            guard let data else {
                try wipe(file)
                return
            }

            try write(file, sealBox(data, key: outboxKey(), aad: Self.avatarAad))
        }
    }

    func peerAvatar(_ peer: UInt8) throws -> Data? {
        return try locked {
            let file = try folder(avatars).appendingPathComponent("\(Self.hex(peer)).box")
            guard let sealed = try? Data(contentsOf: file) else { return nil }

            do {
                return try unseal(sealed, info: Self.avatarAad + [peer])
            } catch let error as Locked {
                throw error
            } catch is CryptoKitError {
                try wipe(file)
                return nil
            }
        }
    }

    func peerAvatar(_ peer: UInt8, _ data: Data?) throws {
        try locked {
            let file = try folder(avatars).appendingPathComponent("\(Self.hex(peer)).box")

            guard let data else {
                try wipe(file)
                return
            }

            try write(file, seal(data, to: agreementPublic(), info: Self.avatarAad + [peer]))
        }
    }

    func peerName(_ peer: UInt8, nick: String, signed: String) throws {
        try locked {
            let file = try folder(names).appendingPathComponent("\(Self.hex(peer)).box")
            let plain = try JSONSerialization.data(withJSONObject: ["nick": nick, "signed": signed])

            try write(file, seal(plain, to: agreementPublic(), info: Self.nameAad + [peer]))
        }
    }

    func peerNames() throws -> [String: Any] {
        return try locked {
            let names = try folder(names)
            let files = (try? FileManager.default.contentsOfDirectory(atPath: names.path)) ?? []
            var out = [String: Any]()

            for name in files where name.count == 6 && name.hasSuffix(".box") {
                let hex = String(name.prefix(2))
                guard let peer = UInt8(hex, radix: 16) else { continue }

                let file = names.appendingPathComponent(name)
                let plain: Data

                do {
                    plain = try unseal(Data(contentsOf: file), info: Self.nameAad + [peer])
                } catch let error as Locked {
                    throw error
                } catch is CryptoKitError {
                    try wipe(file)
                    continue
                }

                out[hex] = try JSONSerialization.jsonObject(with: plain)
            }

            return out
        }
    }

    func outboxList() throws -> [[String: Any]] {
        return try locked {
            let file = try folder(dir).appendingPathComponent("outbox.box")
            guard FileManager.default.fileExists(atPath: file.path) else { return [] }

            let plain = try openBox(Data(contentsOf: file), key: outboxKey(), aad: Self.outboxAad)
            return try JSONSerialization.jsonObject(with: plain) as? [[String: Any]] ?? []
        }
    }

    func receipt(_ chat: String, ref: Int64, kind: String, acked: Int = 0, pending: Int = 0) throws {
        try locked {
            try exclusive { try addReceipt(chat, ref: ref, kind: kind, acked: acked, pending: pending) }
        }
    }

    private func addReceipt(_ chat: String, ref: Int64, kind: String, acked: Int,
                            pending: Int) throws {
        if kind == "delivered" {
            let list = try outboxList()
            let kept = list.filter { ($0["ref"] as? NSNumber)?.int64Value != ref }

            if kept.count != list.count {
                try outboxSave(kept)
                try? unqueue(UInt64(bitPattern: ref))
            }
        }

        let file = try folder(dir).appendingPathComponent("receipts.box")
        var list = [[String: Any]]()

        if FileManager.default.fileExists(atPath: file.path) {
            list = try JSONSerialization.jsonObject(
                with: openBox(Data(contentsOf: file), key: outboxKey(), aad: Self.receiptsAad))
                as? [[String: Any]] ?? []
        }

        list.append(["peer": chat, "ref": ref, "t": kind, "acked": acked, "pending": pending])
        try write(file, sealBox(JSONSerialization.data(withJSONObject: list),
                                key: outboxKey(), aad: Self.receiptsAad))
    }

    func read(_ chat: String) throws {
        try locked {
            guard var conversation = try conversation(chat, create: false) else { return }

            conversation.read = conversation.count
            conversation.unread = 0
            open[chat] = conversation
            try save(chat, conversation)
        }
    }

    func favorite(_ chat: String, ref: Int64, on: Bool) throws {
        try locked {
            guard try conversation(chat, create: false) != nil else { return }

            try append(chat, ["t": "fav", "ref": ref, "on": on])
        }
    }

    func pin(_ chat: String, ref: Int64, on: Bool) throws {
        try locked {
            guard try conversation(chat, create: false) != nil else { return }

            try append(chat, ["t": "pin", "ref": ref, "on": on])
        }
    }

    func delete(_ chat: String) throws {
        try locked {
            open[chat] = nil
            keys[chat] = nil

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

            try exclusive {
                let list = try outboxList()
                let kept = list.filter { $0["peer"] as? String != chat }

                if kept.count != list.count { try outboxSave(kept) }

                for entry in list where entry["peer"] as? String == chat {
                    if let ref = (entry["ref"] as? NSNumber)?.uint64Value {
                        try? unqueue(ref)
                    }
                }
            }
        }
    }

    private func dataWrite(_ chat: String, ref: Int64, _ data: Data) throws -> String {
        guard try conversation(chat, create: true) != nil, let key = try key(chat) else {
            throw Failure(description: "There is no chat \(chat).")
        }

        let file = String(format: "%@-%016llx.dat", chat, UInt64(bitPattern: ref))
        let sealed = try sealBox(data, key: key, aad: Self.dataAad(chat, file))

        try write(try folder(dir).appendingPathComponent(file), sealed)

        return file
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

    private func exclusive(_ body: () throws -> Void) throws {
        let path = try folder(dir).appendingPathComponent("lock").path
        let fd = Darwin.open(path, O_CREAT | O_RDWR, 0o600)
        guard fd >= 0 else { throw Failure(description: "failed to open the chat lock") }
        defer { Darwin.close(fd) }

        guard flock(fd, LOCK_EX) == 0 else { throw Failure(description: "failed to lock the chats") }
        defer { flock(fd, LOCK_UN) }

        try body()
    }

    private func outboxSave(_ list: [[String: Any]]) throws {
        try write(try folder(dir).appendingPathComponent("outbox.box"),
                  sealBox(JSONSerialization.data(withJSONObject: list),
                          key: outboxKey(), aad: Self.outboxAad))
    }

    private func append(_ chat: String, _ record: [String: Any]) throws {
        guard var conversation = try conversation(chat, create: true), let key = try key(chat) else {
            throw Failure(description: "no conversation")
        }

        let plain = try JSONSerialization.data(withJSONObject: record)
        let sealed = try sealBox(plain, key: key,
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

        let type = record["t"] as? String

        if type == "msg" || type == "member" {
            conversation.last = try JSONSerialization.data(withJSONObject: Self.message(record))
        }

        if type == "msg", record["dir"] as? String == "in", conversation.unread >= 0 {
            conversation.unread += 1
        }

        conversation.count += 1
        open[chat] = conversation
        try save(chat, conversation)
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
        if let conversation = open[chat] { return conversation }

        guard try item("key.\(chat)") != nil else {
            guard create else { return nil }

            let key = SymmetricKey(size: .bits256)
            let conversation = Conversation(count: 0, read: 0, flags: 0, unread: 0)

            try wipe(try folder(dir).appendingPathComponent("\(chat).log"))
            try save(chat, conversation)
            try saveKey(chat, key)

            keys[chat] = key
            open[chat] = conversation

            return conversation
        }

        var conversation = try stored(chat) ?? unsplit(chat)
        let count = try framed(chat)

        if count != conversation.count {
            if count < conversation.count { conversation.flags |= Self.truncated }
            conversation.count = count
            conversation.unread = -1
            conversation.last = nil
            try save(chat, conversation)
        }

        open[chat] = conversation

        return conversation
    }

    private func recovered(_ chat: String) throws -> Conversation {
        let count = try framed(chat)
        let conversation = Conversation(count: count, read: count, flags: Self.truncated)

        try save(chat, conversation)

        return conversation
    }

    private func stored(_ chat: String) throws -> Conversation? {
        guard let wrapped = try item("meta.\(chat)") else { return nil }

        do {
            var plain = try unseal(wrapped, info: Self.metaAad + Self.label(chat))
            defer { plain.resetBytes(in: plain.indices) }

            return try Self.parsed(plain)
        } catch CryptoKitError.authenticationFailure {
            return try recovered(chat)
        }
    }

    private func unsplit(_ chat: String) throws -> Conversation {
        guard let wrapped = try item("key.\(chat)") else {
            throw Failure(description: "invalid conversation key")
        }

        var plain = try unseal(wrapped, info: Self.keyAad + Self.label(chat))
        defer { plain.resetBytes(in: plain.indices) }

        guard plain.count >= 32 else { throw Failure(description: "invalid conversation key") }

        if plain.count == 32 {
            return try recovered(chat)
        }

        let conversation = try Self.parsed(plain.dropFirst(32))

        try save(chat, conversation)
        try saveKey(chat, SymmetricKey(data: plain.prefix(32)))

        return conversation
    }

    private static func parsed(_ data: Data) throws -> Conversation {
        var plain = [UInt8](data)
        defer { TunnelKeychain.wipe(&plain) }

        guard plain.count >= 24 else { throw Failure(description: "invalid conversation key") }

        var conversation = Conversation(count: number(plain, 0), read: number(plain, 8),
                                        flags: number(plain, 16))

        if plain.count >= 36 {
            let size = Int(plain[32]) << 24 | Int(plain[33]) << 16 | Int(plain[34]) << 8 | Int(plain[35])

            guard plain.count == 36 + size || plain.count == 40 + size else {
                throw Failure(description: "invalid conversation key")
            }

            conversation.unread = Int64(bitPattern: number(plain, 24))

            if size > 0 {
                conversation.last = Data(plain[36..<(36 + size)])
            }

            if plain.count == 40 + size {
                conversation.ambry = UInt32(plain[36 + size]) << 24 | UInt32(plain[37 + size]) << 16 |
                    UInt32(plain[38 + size]) << 8 | UInt32(plain[39 + size])
            }
        }

        return conversation
    }

    private func key(_ chat: String) throws -> SymmetricKey? {
        if let key = keys[chat] { return key }

        guard let wrapped = try item("key.\(chat)") else { return nil }

        var plain = try unseal(wrapped, info: Self.keyAad + Self.label(chat))
        defer { plain.resetBytes(in: plain.indices) }

        guard plain.count >= 32 else { throw Failure(description: "invalid conversation key") }

        let key = SymmetricKey(data: plain.prefix(32))
        keys[chat] = key

        return key
    }

    private func saveKey(_ chat: String, _ key: SymmetricKey) throws {
        var plain = key.withUnsafeBytes { Data($0) }
        defer { plain.resetBytes(in: plain.indices) }

        try store("key.\(chat)",
                  seal(plain, to: agreementPublic(), info: Self.keyAad + Self.label(chat)),
                  unlocked: true)
    }

    private func save(_ chat: String, _ conversation: Conversation) throws {
        var plain = Data()
        defer { plain.resetBytes(in: plain.indices) }

        plain.append(contentsOf: Self.bytes(conversation.count))
        plain.append(contentsOf: Self.bytes(conversation.read))
        plain.append(contentsOf: Self.bytes(conversation.flags))
        plain.append(contentsOf: Self.bytes(UInt64(bitPattern: conversation.unread)))
        plain.append(contentsOf: Self.bytes(UInt32(conversation.last?.count ?? 0)))
        plain.append(conversation.last ?? Data())
        plain.append(contentsOf: Self.bytes(conversation.ambry))

        try store("meta.\(chat)",
                  seal(plain, to: agreementPublic(), info: Self.metaAad + Self.label(chat)),
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
        guard bytes.count > 3, bytes[0] == 1 else { throw Failure(description: "unknown sealed data") }

        let len = Int(bytes[1]) << 8 | Int(bytes[2])
        guard bytes.count > 3 + len else { throw Failure(description: "short sealed data") }

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

    private static func aad(_ chat: String, seq: UInt64) -> Data {
        var aad = recordAad
        aad.append(label(chat))
        aad.append(contentsOf: bytes(seq))
        return aad
    }

    private static func label(_ chat: String) -> Data {
        if group(chat) != nil { return Data(chat.utf8) }
        return Data([UInt8(chat, radix: 16) ?? 0])
    }

    private static func bytes<T: FixedWidthInteger>(_ value: T) -> [UInt8] {
        withUnsafeBytes(of: value.bigEndian) { Array($0) }
    }

    private static func number(_ bytes: [UInt8], _ offset: Int) -> UInt64 {
        bytes[offset..<(offset + 8)].reduce(0) { $0 << 8 | UInt64($1) }
    }

    static func hex(_ peer: UInt8) -> String { String(format: "%02x", peer) }

    static func chat(_ id: String) -> String? {
        if group(id) != nil { return id }
        guard id.count == 2, id == id.lowercased(), let peer = UInt8(id, radix: 16),
              peer > 0, peer < 0xff else { return nil }
        return id
    }

    static func group(_ chat: String) -> UInt16? {
        guard chat.count == 5, chat.hasPrefix("g"), chat == chat.lowercased() else { return nil }
        return UInt16(chat.dropFirst(), radix: 16)
    }

    static func groupChat(_ group: UInt16) -> String { String(format: "g%04x", group) }

    static func name(_ peer: UInt8) -> String { names()[hex(peer)] ?? "Peer \(hex(peer))" }

    static func name(chat: String) -> String {
        guard let group = group(chat) else { return names()[chat] ?? "Peer \(chat)" }
        return names()[chat] ?? String(format: "Group %04x", group)
    }

    static func names() -> [String: String] {
        if let names = defaults.dictionary(forKey: namesKey) as? [String: String] { return names }
        return UserDefaults.standard.dictionary(forKey: namesKey) as? [String: String] ?? [:]
    }

    static func save(_ names: [String: Any]) -> [String: String] {
        var clean = [String: String]()

        for (key, value) in names {
            guard group(key) != nil || (key.count == 2 && UInt8(key, radix: 16) != nil &&
                                        key == key.lowercased()),
                  let value = value as? String else { continue }

            let name = String(value.trimmingCharacters(in: .whitespacesAndNewlines).prefix(32))
            if !name.isEmpty { clean[key] = name }
        }

        defaults.set(clean, forKey: namesKey)
        UserDefaults.standard.removeObject(forKey: namesKey)

        return clean
    }

    static func mute(_ chat: String, until: Int) {
        var mutes = defaults.dictionary(forKey: mutesKey) as? [String: Int] ?? [:]
        mutes[chat] = until == 0 ? nil : until
        defaults.set(mutes, forKey: mutesKey)
    }

    static func muted(_ chat: String) -> Int {
        let mutes = defaults.dictionary(forKey: mutesKey) as? [String: Int] ?? [:]
        let until = mutes[chat] ?? 0

        if until > 0 && until <= Int(Date().timeIntervalSince1970 * 1000) {
            return 0
        }

        return until
    }

    static let callCategory = "call"

    static func callActions() {
        let answer = UNNotificationAction(identifier: "answer", title: "Answer", options: [.foreground])
        let decline = UNNotificationAction(identifier: "decline", title: "Decline", options: [.destructive])

        UNUserNotificationCenter.current().setNotificationCategories([
            UNNotificationCategory(identifier: callCategory, actions: [answer, decline], intentIdentifiers: []),
        ])
    }

    private static let namesKey = "peerNames"
    private static let mutesKey = "chatMutes"
}
