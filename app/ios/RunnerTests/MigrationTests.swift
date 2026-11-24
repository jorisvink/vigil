import XCTest
@testable import Runner

protocol Readable {
    func conversations() throws -> [[String: Any]]
    func messages(_ chat: String, epoch: Int64, stamp: Int64) throws -> [String: Any]
    func data(_ chat: String, file: String) throws -> Data?
    func groups() throws -> [String: Any]
    func peerNames() throws -> [String: Any]
    func accepted() throws -> Set<String>
    func ignored() throws -> Set<String>
    func safetyCodes() throws -> [String: Any]
    func state(_ change: (inout [String: Any]) -> Void) -> [String: Any]
    func outboxList() throws -> [[String: Any]]
}

extension LegacyVault: Readable {}
extension ChatVault: Readable {}

final class MigrationTests: XCTestCase {
    private struct Interrupted: Error {}

    private static let flock = "cafeba00"
    private static let image = Data((0..<2500).map { UInt8($0 & 0xff) })
    private static let oldId = "^([0-9a-f]{2}|g[0-9a-f]{18})(-[0-9a-f]{16}\\.dat)?$"
    private static let oldFile = "^([0-9a-f]{2}|g[0-9a-f]{18})(\\.log|-[0-9a-f]{16}\\.dat)$"

    private var roots = [URL]()
    private var services = [String]()
    private var base: URL!
    private var service = ""
    private var saved: [String: Any]?

    override func setUp() {
        super.setUp()
        saved = ChatVault.defaults.persistentDomain(forName: TunnelKeychain.group)
        fresh()
    }

    override func tearDown() {
        for service in services {
            SecItemDelete([
                kSecClass as String: kSecClassGenericPassword,
                kSecAttrService as String: service,
                kSecAttrAccessGroup as String: TunnelKeychain.group,
            ] as CFDictionary)
        }

        for root in roots {
            try? FileManager.default.removeItem(at: root)
        }

        ChatVault.defaults.setPersistentDomain(saved ?? [:], forName: TunnelKeychain.group)
        super.tearDown()
    }

    func testMigration() throws {
        try seed(chats: true)
        let before = try view(old(), map: Self.moved)
        try deliverLocked()

        let app = new()
        try app.prepare()
        try app.merge()

        try check(app, before)
        try checkFiles()
    }

    func testExtensionFirst() throws {
        try seed(chats: true)
        let before = try view(old(), map: Self.moved)
        try deliverLocked()

        let ext = new(owner: false)
        try ext.migrateFlock()

        XCTAssertEqual(marker(), "1")
        XCTAssertTrue(exists("01.log"))
        XCTAssertEqual(try new(owner: false).accepted().sorted(), before["accepted"] as? [String])

        let app = new()
        try app.prepare()
        try app.merge()

        try check(app, before)
        try checkFiles()
    }

    func testLockedWithoutChats() throws {
        try lockedWaits(chats: false)
    }

    func testLockedWithChats() throws {
        try lockedWaits(chats: true)
    }

    func testInterruptedAnywhere() throws {
        var step = 1

        while step < 5000 {
            if step > 1 {
                fresh()
            }

            try seed(chats: true)
            let before = try view(old(), map: Self.moved)
            try deliverLocked()

            var calls = 0
            var hit = ""
            let app = new()

            app.fault = { what in
                calls += 1
                if calls == step {
                    hit = what
                    throw Interrupted()
                }
            }

            do {
                try app.prepare()
            } catch is Interrupted {
            }

            let again = new()
            try again.prepare()
            try again.merge()

            try check(again, before, "interrupted at call \(step), \(hit)")

            if calls < step {
                break
            }

            step += 1
        }

        XCTAssertGreaterThan(step, 10)
    }

    private func lockedWaits(chats: Bool) throws {
        try seed(chats: chats)
        let before = try view(old(), map: Self.moved)
        try deliverLocked()

        let locked = new()
        locked.fault = { what in
            if what == "item inbox.private" {
                throw ChatVault.Locked()
            }
        }

        XCTAssertThrowsError(try locked.migrateFlock())
        XCTAssertNotEqual(marker(), "2")

        for name in ["names/01.box", "names/02.box", "avatars/01.box", "safety/cafeba00-02.box"] {
            XCTAssertTrue(exists(name), name)
        }

        XCTAssertEqual(count("inbox"), 2)

        let app = new()
        try app.prepare()
        try app.merge()

        try check(app, before)
        try checkFiles()
    }

    private func fresh() {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)

        roots.append(root)
        base = root.appendingPathComponent("chats", isDirectory: true)
        service = "se.sanctorum.vigil.test.\(UUID().uuidString)"
        services.append(service)

        ChatVault.defaults.setPersistentDomain([:], forName: TunnelKeychain.group)
        ChatVault.defaults.set([["id": "test", "ref": Data(), "flock": Self.flock]], forKey: "gospels")
    }

    private func old(owner: Bool = true) -> LegacyVault {
        LegacyVault(base: base, owner: owner, service: service)
    }

    private func new(owner: Bool = true) -> ChatVault {
        ChatVault(base: base, owner: owner, service: service)
    }

    private func seed(chats: Bool) throws {
        let vault = old()
        let now = Int64(Date().timeIntervalSince1970 * 1000)

        try vault.prepare()
        try vault.peerName(0x01, nick: "one", signed: "")
        try vault.peerName(0x02, nick: "two", signed: "")
        try vault.peerAvatar(0x01, Self.image)
        try vault.accept("01", true)
        try vault.ignore("04", true)
        try vault.safetyCode(0x02, Data(repeating: 0x5a, count: 32))

        guard chats else { return }

        try vault.deliver(chat: "01", peer: 0x01, nick: "one", text: "hello", time: now - 9000)
        let sent = try vault.send("01", text: "reply", local: "ca")
        try vault.receipt("01", ref: sent, kind: "delivered")
        let topic = try vault.topicSet("01", topic: nil, name: "work", color: 0x336699, local: "ca")
        try vault.deliver(chat: "01", peer: 0x01, nick: "one", text: "in topic", time: now - 8000,
                          topic: topic, color: 0x336699)
        try vault.deliver(chat: "01", peer: 0x01, nick: "one", text: "", time: now - 7000,
                          data: Data(repeating: 0x11, count: 4000), media: "voice", name: "memo.m4a")
        try vault.deliver(chat: "02", peer: 0x02, nick: "two", text: "", time: now - 6000,
                          data: Self.image, media: "image", name: "pic.jpg")
        _ = try vault.sendData("02", media: "file", name: "doc.pdf", data: Data(repeating: 0x25, count: 3000),
                               local: "ca")

        let owned = try vault.groupCreate(name: "mine", members: ["01", "02"], local: "ca")
        _ = try vault.send(owned, text: "to my group", local: "ca")

        let member = LegacyVault.groupChat("01", "00000000000000aa")
        try vault.groupSave(member, [
            "owner": "01", "id": "00000000000000aa", "name": "theirs", "members": ["01", "02", "ca"],
            "serial": 1, "state": "open", "topics": [[String: Any]](),
        ])
        try vault.deliver(chat: member, peer: 0x02, nick: "two", text: "in their group", time: now - 5000)

        _ = vault.state { all in
            all["stars"] = ["01", member]
            all["mutes"] = ["02": true]
            all["compacts"] = ["02"]
            all["order"] = [member, "01", "02", owned]
        }
    }

    private func deliverLocked() throws {
        let ext = old(owner: false)
        let now = Int64(Date().timeIntervalSince1970 * 1000)

        try ext.deliver(chat: "02", peer: 0x02, nick: "two", text: "while locked", time: now)
        try ext.deliver(chat: "01", peer: 0x01, nick: "one", text: "", time: now + 1,
                        data: Self.image, media: "image", name: "locked.jpg")
    }

    private func check(_ app: ChatVault, _ before: [String: Any], _ context: String = "",
                       file: StaticString = #filePath, line: UInt = #line) throws {
        var after = try view(app, map: { $0 })
        var chats = after["chats"] as? [String: Any] ?? [:]

        for (chat, extra) in [("cafeba00-02", "while locked"), ("cafeba00-01", "locked.jpg")] {
            var entry = chats[chat] as? [String: Any] ?? [:]
            var list = entry["messages"] as? [[String: Any]] ?? []
            let found = list.filter { $0["text"] as? String == extra || $0["name"] as? String == extra }

            XCTAssertEqual(found.count, 1, "\(extra) \(context)", file: file, line: line)

            if extra == "locked.jpg" {
                XCTAssertEqual(found.first?["size"] as? Int, Self.image.count, context, file: file, line: line)
            }

            list.removeAll { $0["text"] as? String == extra || $0["name"] as? String == extra }

            if list.isEmpty, (before["chats"] as? [String: Any])?[chat] == nil {
                chats[chat] = nil
                continue
            }

            entry["messages"] = list
            chats[chat] = entry
        }

        after["chats"] = chats

        XCTAssertEqual(try Self.json(after), try Self.json(before), context, file: file, line: line)
        XCTAssertEqual(marker(), "2", context, file: file, line: line)
        XCTAssertEqual(count("inbox"), 0, context, file: file, line: line)
        XCTAssertEqual(try app.peerAvatar("cafeba00-01"), Self.image, context, file: file, line: line)
        XCTAssertNotNil(try app.safetyCode("cafeba00-02"), context, file: file, line: line)
    }

    private func checkFiles(file: StaticString = #filePath, line: UInt = #line) throws {
        let fm = FileManager.default

        for name in try fm.contentsOfDirectory(atPath: base.path) {
            XCTAssertNil(name.range(of: Self.oldFile, options: .regularExpression), name, file: file, line: line)
        }

        for sub in ["names", "avatars", "safety"] {
            for name in try fm.contentsOfDirectory(atPath: base.appendingPathComponent(sub).path) {
                XCTAssertNil(name.range(of: "^[0-9a-f]{2}\\.box$", options: .regularExpression),
                             "\(sub)/\(name)", file: file, line: line)
            }
        }
    }

    private func view(_ vault: Readable, map: (String) -> String) throws -> [String: Any] {
        var chats = [String: Any]()

        for conversation in try vault.conversations() {
            guard let chat = conversation["peer"] as? String else { continue }

            var list = [[String: Any]]()
            let messages = try vault.messages(chat, epoch: 0, stamp: 0)["messages"] as? [Any] ?? []

            for case let record as [String: Any] in messages {
                var entry = [String: Any]()

                for key in ["dir", "text", "media", "name", "state", "kind", "nick", "tp", "topic", "to"] {
                    if let value = record[key] {
                        entry[key] = value
                    }
                }

                if let peer = record["peer"] as? String, !peer.isEmpty {
                    entry["peer"] = map(peer)
                }

                if let file = record["data"] as? String, !file.isEmpty {
                    entry["size"] = try vault.data(chat, file: file)?.count ?? -1
                }

                list.append(entry)
            }

            let topics = (conversation["topics"] as? [[String: Any]] ?? []).compactMap { $0["name"] as? String }

            chats[map(chat)] = ["messages": list, "topics": topics]
        }

        let state = vault.state { _ in }
        var kept = [String: Any]()

        for key in ["stars", "compacts", "order"] {
            kept[key] = (state[key] as? [String] ?? []).map(map)
        }

        kept["mutes"] = Dictionary(uniqueKeysWithValues: (state["mutes"] as? [String: Any] ?? [:])
            .map { (map($0.key), $0.value) })

        let codes = try vault.safetyCodes()
        let outbox = try vault.outboxList().map { entry -> [String: Any] in
            var moved = entry

            for key in ["peer", "chat", "data"] {
                if let value = entry[key] as? String {
                    moved[key] = map(value)
                }
            }

            return moved
        }

        return [
            "chats": chats,
            "groups": Dictionary(uniqueKeysWithValues: try vault.groups().map { (map($0.key), $0.value) }),
            "names": Dictionary(uniqueKeysWithValues: try vault.peerNames().map { (map($0.key), $0.value) }),
            "accepted": try vault.accepted().map(map).sorted(),
            "ignored": try vault.ignored().map(map).sorted(),
            "codes": [
                "on": (codes["on"] as? [String] ?? []).map(map).sorted(),
                "set": (codes["set"] as? [String] ?? []).map(map).sorted(),
            ],
            "state": kept,
            "outbox": outbox,
        ]
    }

    private static func moved(_ id: String) -> String {
        id.range(of: oldId, options: .regularExpression) != nil ? "\(flock)-\(id)" : id
    }

    private static func json(_ value: Any) throws -> String {
        String(decoding: try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys, .prettyPrinted]),
               as: UTF8.self)
    }

    private func marker() -> String {
        (try? String(contentsOf: base.appendingPathComponent("flocked"), encoding: .utf8)) ?? ""
    }

    private func exists(_ name: String) -> Bool {
        FileManager.default.fileExists(atPath: base.appendingPathComponent(name).path)
    }

    private func count(_ sub: String) -> Int {
        (try? FileManager.default.contentsOfDirectory(atPath: base.appendingPathComponent(sub).path))?.count ?? 0
    }
}
