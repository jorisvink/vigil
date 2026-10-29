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

    private struct Conversation {
        var key: SymmetricKey
        var count: UInt64
        var read: UInt64
        var flags: UInt64
    }

    static let shared = ChatVault()

    static let defaults = UserDefaults(suiteName: TunnelKeychain.group) ?? .standard

    private static let service = "se.sanctorum.vigil.chat"
    private static let keyAad = Data("VIGIL.CHAT.KEY".utf8)
    private static let recordAad = Data("VIGIL.CHAT.V1".utf8)
    private static let inboxAad = Data("VIGIL.CHAT.INBOX".utf8)
    private static let outboxAad = Data("VIGIL.CHAT.OUTBOX".utf8)
    private static let receiptsAad = Data("VIGIL.CHAT.RECEIPTS".utf8)
    private static let dataAad = Data("VIGIL.CHAT.DATA".utf8)
    private static let truncated: UInt64 = 1

    private let dir: URL?
    private let inbox: URL?
    private var open = [String: Conversation]()
    private var refs: Int64 = 0
    private let owner = Bundle.main.bundleURL.pathExtension != "appex"

    var hardware: Bool { SecureEnclave.isAvailable }

    private init() {
        let base = FileManager.default
            .containerURL(forSecurityApplicationGroupIdentifier: TunnelKeychain.group)?
            .appendingPathComponent("chats", isDirectory: true)

        dir = base
        inbox = base?.appendingPathComponent("inbox", isDirectory: true)

        for case var url? in [dir, inbox] {
            try? FileManager.default.createDirectory(
                at: url, withIntermediateDirectories: true,
                attributes: [.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication])

            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try? url.setResourceValues(values)
        }
    }

    func prepare() throws {
        _ = try agreementPublic()
        _ = try outboxKey()
    }

    func lock() {
        open.removeAll()
    }

    func ref() -> Int64 {
        refs = max(refs + 1, Int64(Date().timeIntervalSince1970 * 1000) * 1000)
        return refs
    }

    func deliver(chat: String, peer: UInt8, nick: String, text: String, time: Int64,
                 kind: String = "", data: Data? = nil, media: String = "",
                 name: String = "") throws {
        var message: [String: Any] = [
            "chat": chat, "peer": Self.hex(peer), "nick": nick, "text": text, "time": time,
            "kind": kind,
        ]

        if let data {
            message["data"] = data.base64EncodedString()
            message["media"] = media
            message["name"] = name
        }

        let plain = try JSONSerialization.data(withJSONObject: message)

        let sealed = try seal(plain, to: agreementPublic(), info: Self.inboxAad)
        try write(try folder(inbox).appendingPathComponent(
            String(format: "%016llx-%016llx.box", UInt64(time), UInt64.random(in: 0...UInt64.max))),
            sealed)
    }

    func merge() throws {
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
                wipe(file)
                continue
            }

            guard let message = try JSONSerialization.jsonObject(with: plain) as? [String: Any],
                  let peer = message["peer"] as? String,
                  let chat = Self.chat(message["chat"] as? String ?? peer),
                  let text = message["text"] as? String else {
                wipe(file)
                continue
            }

            let kind = message["kind"] as? String ?? ""
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

            try append(chat, record)

            wipe(file)
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

            wipe(receipts)
        }
    }

    func conversations() throws -> [[String: Any]] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: folder(dir).path)) ?? []
        var out = [[String: Any]]()

        for name in names where name.hasSuffix(".log") {
            guard let id = Self.chat(String(name.dropLast(4))) else { continue }

            let chat = try messages(id)
            let list = chat["messages"] as? [[String: Any]] ?? []

            out.append([
                "peer": id,
                "unread": chat["unread"] ?? 0,
                "last": list.last.map { $0 as Any } ?? NSNull(),
            ])
        }

        return out
    }

    func messages(_ chat: String) throws -> [String: Any] {
        guard let conversation = try conversation(chat, create: false) else {
            return ["messages": [], "unread": 0]
        }

        var messages = [[String: Any]]()
        var index = [Int64: Int]()
        var damaged = false
        var unread = 0

        try records(chat, conversation) { seq, record in
            guard let record else {
                damaged = true
                return
            }

            switch record["t"] as? String {
            case "msg":
                guard let ref = (record["ref"] as? NSNumber)?.int64Value else { return }
                let incoming = record["dir"] as? String == "in"

                if incoming && seq >= conversation.read { unread += 1 }

                index[ref] = messages.count
                messages.append([
                    "ref": ref,
                    "dir": record["dir"] ?? "in",
                    "text": record["text"] ?? "",
                    "time": record["time"] ?? 0,
                    "nick": record["nick"] ?? "",
                    "peer": record["peer"] ?? "",
                    "data": record["data"] ?? "",
                    "media": record["media"] ?? "",
                    "name": record["name"] ?? "",
                    "state": incoming ? "received" : "queued",
                ])
            case "member":
                messages.append([
                    "ref": record["ref"] ?? 0,
                    "dir": "in",
                    "kind": record["kind"] ?? "",
                    "text": "",
                    "time": record["time"] ?? 0,
                    "nick": record["nick"] ?? "",
                    "peer": record["peer"] ?? "",
                    "state": "received",
                ])
            case "sent":
                if let ref = (record["ref"] as? NSNumber)?.int64Value, let at = index[ref],
                   messages[at]["state"] as? String == "queued" {
                    messages[at]["state"] = "sent"
                }
            case "delivered":
                if let ref = (record["ref"] as? NSNumber)?.int64Value, let at = index[ref] {
                    messages[at]["state"] = "delivered"
                }
            case "failed":
                if let ref = (record["ref"] as? NSNumber)?.int64Value, let at = index[ref],
                   messages[at]["state"] as? String == "queued" {
                    messages[at]["state"] = "failed"
                }
            case "acks":
                if let ref = (record["ref"] as? NSNumber)?.int64Value, let at = index[ref] {
                    let pending = (record["pending"] as? NSNumber)?.intValue ?? 0

                    messages[at]["acked"] = (record["acked"] as? NSNumber)?.intValue ?? 0
                    messages[at]["pending"] = pending
                    messages[at]["state"] = pending == 0 ? "delivered" : "sent"
                }
            default:
                break
            }
        }

        var out: [String: Any] = ["messages": messages, "unread": unread]

        if damaged {
            out["warning"] = "Part of this chat could not be read, it was damaged or tampered with."
        } else if conversation.flags & Self.truncated != 0 {
            out["warning"] = "Messages were removed from the end of this chat outside of Vigil."
        }

        return out
    }

    func send(_ chat: String, text: String) throws -> Int64 {
        let ref = ref()

        try append(chat, [
            "t": "msg", "dir": "out", "ref": ref, "text": text,
            "time": Int64(Date().timeIntervalSince1970 * 1000),
        ])

        guard Self.group(chat) == nil else { return ref }

        try exclusive {
            var list = try outboxList()
            list.append(["peer": chat, "ref": ref, "text": text])
            try outboxSave(list)
        }

        return ref
    }

    func sendData(_ chat: String, media: String, name: String, data: Data) throws -> (Int64, String) {
        let ref = ref()
        let file = try dataWrite(chat, ref: ref, data)

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

    func data(_ chat: String, file: String) throws -> Data? {
        guard Self.data(file, of: chat) else {
            throw Failure(description: "There is no data \(file).")
        }

        guard let conversation = try conversation(chat, create: false) else { return nil }

        let path = try folder(dir).appendingPathComponent(file)
        guard let sealed = try? Data(contentsOf: path) else { return nil }

        return try openBox(sealed, key: conversation.key, aad: Self.dataAad(chat, file))
    }

    func outboxList() throws -> [[String: Any]] {
        let file = try folder(dir).appendingPathComponent("outbox.box")
        guard FileManager.default.fileExists(atPath: file.path) else { return [] }

        let plain = try openBox(Data(contentsOf: file), key: outboxKey(), aad: Self.outboxAad)
        return try JSONSerialization.jsonObject(with: plain) as? [[String: Any]] ?? []
    }

    func receipt(_ chat: String, ref: Int64, kind: String, acked: Int = 0, pending: Int = 0) throws {
        try exclusive { try addReceipt(chat, ref: ref, kind: kind, acked: acked, pending: pending) }
    }

    private func addReceipt(_ chat: String, ref: Int64, kind: String, acked: Int,
                            pending: Int) throws {
        if kind == "delivered" {
            let list = try outboxList()
            let kept = list.filter { ($0["ref"] as? NSNumber)?.int64Value != ref }
            if kept.count != list.count { try outboxSave(kept) }
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
        guard var conversation = try conversation(chat, create: false) else { return }

        conversation.read = conversation.count
        open[chat] = conversation
        try save(chat, conversation)
    }

    func delete(_ chat: String) throws {
        open[chat] = nil
        remove("key.\(chat)")
        wipe(try folder(dir).appendingPathComponent("\(chat).log"))

        let names = (try? FileManager.default.contentsOfDirectory(atPath: try folder(dir).path)) ?? []

        for name in names where Self.data(name, of: chat) {
            wipe(try folder(dir).appendingPathComponent(name))
        }

        try exclusive {
            let list = try outboxList()
            let kept = list.filter { $0["peer"] as? String != chat }
            if kept.count != list.count { try outboxSave(kept) }
        }
    }

    private func dataWrite(_ chat: String, ref: Int64, _ data: Data) throws -> String {
        guard let conversation = try conversation(chat, create: true) else {
            throw Failure(description: "There is no chat \(chat).")
        }

        let file = String(format: "%@-%016llx.dat", chat, UInt64(bitPattern: ref))
        let sealed = try sealBox(data, key: conversation.key, aad: Self.dataAad(chat, file))

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

        conversation.count += 1
        open[chat] = conversation
        try save(chat, conversation)
    }

    private func records(_ chat: String, _ conversation: Conversation,
                         _ each: (UInt64, [String: Any]?) throws -> Void) throws {
        let file = try folder(dir).appendingPathComponent("\(chat).log")
        guard let data = try? Data(contentsOf: file) else { return }

        let bytes = [UInt8](data)
        var offset = 0
        var seq: UInt64 = 0

        while offset + 4 <= bytes.count {
            let len = Int(bytes[offset]) << 24 | Int(bytes[offset + 1]) << 16 |
                Int(bytes[offset + 2]) << 8 | Int(bytes[offset + 3])
            guard len >= 28, offset + 4 + len <= bytes.count else { break }

            let sealed = Data(bytes[(offset + 4)..<(offset + 4 + len)])

            guard let plain = try? openBox(sealed, key: conversation.key,
                                           aad: Self.aad(chat, seq: seq)) else {
                try each(seq, nil)
                break
            }

            try each(seq, try JSONSerialization.jsonObject(with: plain) as? [String: Any])

            offset += 4 + len
            seq += 1
        }
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

        guard let wrapped = try item("key.\(chat)") else {
            guard create else { return nil }

            let conversation = Conversation(key: SymmetricKey(size: .bits256), count: 0, read: 0, flags: 0)
            wipe(try folder(dir).appendingPathComponent("\(chat).log"))
            try save(chat, conversation)
            open[chat] = conversation

            return conversation
        }

        let plain = [UInt8](try unseal(wrapped, info: Self.keyAad + Self.label(chat)))
        guard plain.count == 56 else { throw Failure(description: "invalid conversation key") }

        var conversation = Conversation(
            key: SymmetricKey(data: plain[0..<32]),
            count: Self.number(plain, 32), read: Self.number(plain, 40), flags: Self.number(plain, 48))

        let count = try framed(chat)

        if count != conversation.count {
            if count < conversation.count { conversation.flags |= Self.truncated }
            conversation.count = count
            try save(chat, conversation)
        }

        open[chat] = conversation

        return conversation
    }

    private func save(_ chat: String, _ conversation: Conversation) throws {
        var plain = conversation.key.withUnsafeBytes { Data($0) }
        plain.append(contentsOf: Self.bytes(conversation.count))
        plain.append(contentsOf: Self.bytes(conversation.read))
        plain.append(contentsOf: Self.bytes(conversation.flags))

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

    private func remove(_ account: String) {
        SecItemDelete(query(account) as CFDictionary)
    }

    private func folder(_ url: URL?) throws -> URL {
        guard let url else { throw Failure(description: "no app group container") }
        return url
    }

    private func write(_ file: URL, _ data: Data) throws {
        try data.write(to: file, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
    }

    private func wipe(_ file: URL) {
        guard let size = (try? FileManager.default.attributesOfItem(atPath: file.path))?[.size]
                as? NSNumber else { return }

        if let handle = try? FileHandle(forWritingTo: file) {
            try? handle.write(contentsOf: Data(count: size.intValue))
            try? handle.synchronize()
            try? handle.close()
        }

        try? FileManager.default.removeItem(at: file)
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

    static let callCategory = "call"

    static func callActions() {
        let answer = UNNotificationAction(identifier: "answer", title: "Answer", options: [.foreground])
        let decline = UNNotificationAction(identifier: "decline", title: "Decline", options: [.destructive])

        UNUserNotificationCenter.current().setNotificationCategories([
            UNNotificationCategory(identifier: callCategory, actions: [answer, decline], intentIdentifiers: []),
        ])
    }

    private static let namesKey = "peerNames"
}
