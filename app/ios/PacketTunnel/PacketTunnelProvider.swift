
import Foundation
import Network
import NetworkExtension
import UIKit
import UserNotifications
import Vigil

final class PacketTunnelProvider: NEPacketTunnelProvider {
    /// Runs one after the other, never two at the same time.
    private static let queue = DispatchQueue(label: "vigil")
    private let stopped = DispatchSemaphore(value: 0)
    private let lock = NSLock()
    private var run: UInt64 = 0
    private var stopping = false
    private var calls: DispatchSourceTimer?
    private var callers = Set<String>()
    private var chatSession: UInt64 = 0
    private var chatResubmit: TimeInterval = 0
    private var chatSweep: TimeInterval = 0
    private var safetyPeer = ""
    private var chatAround = Set<String>()
    private var chatWaiting = Set<UInt64>()
    private var chatAsking = Set<String>()
    private var chatAccepts = -1
    private var receipts = [(String, Int64, String)]()
    private var locals = [String: String]()
    private var sessions = [String: Int64]()
    private var paths: NWPathMonitor?
    private var probe: WakeProbe?
    private var network: String?
    private var answered: UInt64 = 0

    /// Where the IPv6 traffic we block ends up.
    private static let ipv6Sink = "fd70:6172:6973:6800::1"
    private static let voiceAddress = "169.254.86.1"
    private static let resubmitInterval: TimeInterval = 10
    private static let sweepInterval: TimeInterval = 15

    override func startTunnel(options: [String: NSObject]?,
                              completionHandler: @escaping (Error?) -> Void) {
        ChatVault.callActions()

        guard let proto = protocolConfiguration as? NETunnelProviderProtocol,
              let ref = proto.passwordReference,
              var config = try? TunnelKeychain.load(ref: ref), !config.isEmpty else {
            completionHandler(failure("the tunnel configuration is missing"))
            return
        }

        var out = [CChar](repeating: 0, count: 4096)
        let outlen = out.count

        let ok = config.withUnsafeBytes { raw in
            vigil_config_check(raw.baseAddress?.assumingMemoryBound(to: CChar.self),
                               raw.count, &out, outlen) == 0
        }

        guard ok, let summary = try? JSONSerialization.jsonObject(
            with: Data(String(cString: out).utf8)) as? [String: Any] else {
            TunnelKeychain.wipe(&config)
            completionHandler(failure("the tunnel configuration is invalid"))
            return
        }

        let alone = summary["mode"] as? String == "gospel"

        if !TunnelLink.open(create: true) {
            NSLog("vigil: failed to open the link to the app")
        }

        Self.adopt(proto, summary)

        let attach = ChatVault.defaults.object(forKey: "gospelEnabled") as? Bool ?? true
        var configs = attach ? Self.gospels() : []
        let cathedrals = configs.compactMap { Self.cathedral($0) }
        var voice = Self.joined(configs)

        for idx in configs.indices {
            TunnelKeychain.wipe(&configs[idx])
        }

        attachCompanion()
        ChatVault.safetyApplyAll()

        do {
            try ChatVault.shared.migrateFlock()
        } catch {
            NSLog("vigil: failed to move our chats into their flock: \(error)")
        }

        run = vigil_run_id()
        let number = run

        lock.lock()
        stopping = false
        lock.unlock()

        let network = alone ? Self.settings(alone: summary)
            : settings(summary, voice: cathedrals)

        setTunnelNetworkSettings(network) { error in
            if let error {
                TunnelKeychain.wipe(&config)
                TunnelKeychain.wipe(&voice)
                completionHandler(error)
                return
            }

            let fd = alone ? -1 : vigil_ios_utun_fd()
            guard alone || fd >= 0 else {
                TunnelKeychain.wipe(&config)
                TunnelKeychain.wipe(&voice)
                completionHandler(self.failure("no utun interface was found"))
                return
            }

            let datadir = FileManager.default
                .containerURL(forSecurityApplicationGroupIdentifier: TunnelKeychain.group)?
                .path

            Self.queue.async { [weak self, stopped = self.stopped] in
                let ret = config.withUnsafeBytes { raw in
                    voice.withUnsafeBytes { vraw -> Int32 in
                        let ptr = raw.baseAddress?.assumingMemoryBound(to: CChar.self)
                        let vlen = vraw.count
                        let vptr = vlen > 0
                            ? vraw.baseAddress?.assumingMemoryBound(to: CChar.self) : nil

                        guard let datadir else {
                            return vigil_run(number, fd, ptr, raw.count, vptr, vlen, nil)
                        }

                        return datadir.withCString { dir in
                            vigil_run(number, fd, ptr, raw.count, vptr, vlen, dir)
                        }
                    }
                }

                NSLog("vigil: vigil exited with \(ret)")
                TunnelKeychain.wipe(&config)
                TunnelKeychain.wipe(&voice)
                stopped.signal()

                guard let self else { return }

                self.lock.lock()
                let requested = self.stopping
                self.lock.unlock()

                if !requested {
                    let reason = ret == -1 ? "the tunnel failed to start" : "the tunnel stopped"
                    self.cancelTunnelWithError(self.failure(reason))
                }
            }

            DispatchQueue.main.async {
                self.lock.lock()
                let requested = self.stopping
                self.lock.unlock()

                guard !requested else { return }

                self.watchCalls()
                self.watchNetwork()

                self.probe = WakeProbe.configured()
                self.probe?.start()
            }

            completionHandler(nil)
        }
    }

    override func stopTunnel(with reason: NEProviderStopReason,
                             completionHandler: @escaping () -> Void) {
        guard run != 0 else {
            completionHandler()
            return
        }

        lock.lock()
        stopping = true
        lock.unlock()

        onMain {
            self.calls?.cancel()
            self.calls = nil

            self.paths?.cancel()
            self.paths = nil

            self.probe?.stop()
            self.probe = nil
        }

        vigil_stop(run)
        _ = stopped.wait(timeout: .now() + 5)

        TunnelLink.close()
        completionHandler()
    }

    override func sleep(completionHandler: @escaping () -> Void) {
        NSLog("vigil: the system is going to sleep")
        completionHandler()
    }

    override func wake() {
        NSLog("vigil: the system woke up")
        vigil_poke()
    }

    private func safetyStart(_ peer: String, _ own: String, _ theirs: String) {
        var mine = Self.unhex(own)
        var other = Self.unhex(theirs)
        defer {
            TunnelKeychain.wipe(&mine)
            TunnelKeychain.wipe(&other)
        }

        let len = Int(VIGIL_SAFETY_HALF_LEN)

        guard mine.count == len, other.count == len else { return }

        safetyStop()

        if vigil_safety_start(Flock.number(peer), Flock.peer(peer), mine, other, len) == 0 {
            safetyPeer = peer
        }
    }

    private func safetyStop() {
        if !safetyPeer.isEmpty {
            safetyStore(safetyPeer)
        }

        vigil_safety_stop()
        safetyPeer = ""
    }

    private func safetyStore(_ peer: String) {
        var input = [UInt8](repeating: 0, count: Int(VIGIL_ADDITIONAL_INPUT_LEN))
        defer { TunnelKeychain.wipe(&input) }

        guard vigil_safety_get(Flock.number(peer), Flock.peer(peer), &input, input.count) == 0 else { return }

        do {
            try ChatVault.shared.safetyCode(peer, Data(input))
        } catch {
            NSLog("vigil: failed to store the safety code for \(peer): \(error)")
            return
        }

        ChatVault.code(peer, on: true)
        ChatVault.safetyApply(peer)
    }

    private static func unhex(_ hex: String) -> [UInt8] {
        let chars = Array(hex.utf8)
        var out = [UInt8]()

        guard chars.count % 2 == 0 else { return [] }

        for idx in stride(from: 0, to: chars.count, by: 2) {
            guard let byte = UInt8(String(decoding: chars[idx..<idx + 2], as: UTF8.self), radix: 16) else {
                return []
            }

            out.append(byte)
        }

        return out
    }

    override func handleAppMessage(_ messageData: Data,
                                   completionHandler: ((Data?) -> Void)?) {
        var request = TunnelLink.input(messageData) ?? []
        var reply = request.isEmpty ? [] : answer(String(decoding: request, as: UTF8.self))

        completionHandler?(TunnelLink.output(reply, capacity: Int(VIGIL_LINK_BATCH_MAX)))

        TunnelKeychain.wipe(&request)
        TunnelKeychain.wipe(&reply)
    }

    private func answer(_ message: String) -> [UInt8] {
        var words = message.split(separator: " ")

        if let seq = words.first.flatMap({ UInt64($0) }) {
            words.removeFirst()

            if seq == answered { words.removeAll() }
            answered = seq
        }

        let id = words.count > 1 && Flock.valid(String(words[1])) ? String(words[1]) : ""

        switch words.first {
        case "dial":
            if !id.isEmpty {
                vigil_call_dial(Flock.number(id), Flock.peer(id))
            }
        case "hangup":
            vigil_call_hangup()
        case "done":
            if !id.isEmpty {
                vigil_call_done(Flock.number(id), Flock.peer(id))
            }
        case "chat":
            submitOutbox()
        case "chat-nick":
            if vigil_chat_nick(ChatVault.defaults.string(forKey: "chatNick") ?? "") == -1 {
                NSLog("vigil: our nickname was refused")
            }
        case "chat-avatar":
            submitAvatar()
        case "chat-group-typing":
            if words.count == 3, let topic = UInt64(words[2], radix: 16) {
                let chat = String(words[1])

                if let group = ChatVault.group(chat), let info = try? ChatVault.shared.groupInfo(chat),
                   info["state"] as? String == "open" {
                    var members = ChatVault.roster(ChatVault.members(info))
                    vigil_chat_group_typing(Flock.number(chat), group.owner, group.id, topic, &members)
                } else {
                    vigil_chat_group_typing(0, 0, 0, 0, nil)
                }
            }
        case "chat-typing":
            if words.count == 4, !id.isEmpty, let topic = UInt64(words[3], radix: 16) {
                vigil_chat_typing(Flock.number(id), Flock.peer(id), topic, words[2] == "1" ? 1 : 0)
            }
        case "chat-open":
            if words.count == 3, !id.isEmpty {
                vigil_chat_open(Flock.number(id), Flock.peer(id), words[2] == "1" ? 1 : 0)
            }
        case "chat-answer":
            if words.count == 2, chatAsking.remove(String(words[1])) != nil {
                askingSave()
            }

            admit()
        case "chat-drop":
            if words.count == 3, !id.isEmpty, let ref = UInt64(words[2], radix: 16) {
                drop([(id, Int64(bitPattern: ref))])
            }
        case "chat-safety":
            if words.count == 2, !id.isEmpty {
                ChatVault.safetyApply(id)
            }
        case "chat-safety-start":
            if words.count == 4, !id.isEmpty {
                safetyStart(id, String(words[2]), String(words[3]))
            }
        case "chat-safety-stop":
            safetyStop()
        case "chat-forget":
            if words.count == 2, !id.isEmpty, vigil_chat_forget(Flock.number(id), Flock.peer(id)) == -1 {
                NSLog("vigil: failed to forget the messages for \(id)")
            }
        case "gospel":
            attachGospel()
        case "companion":
            attachCompanion()
        default:
            break
        }

        var buf = [CChar](repeating: 0, count: Int(VIGIL_STATUS_MAX))
        defer { TunnelKeychain.wipe(&buf) }

        let len = vigil_status(&buf, buf.count)

        guard len > 0 else { return Array("{\"running\":false}".utf8) }

        return buf[0..<len].map { UInt8(bitPattern: $0) }
    }

    private func checkChats(_ status: [String: Any]) {
        guard let chat = status["chat"] as? [String: Any] else { return }

        let vault = ChatVault.shared
        let defaults = ChatVault.defaults
        let session = (chat["session"] as? NSNumber)?.uint64Value ?? 0
        var running = [String: Int64]()
        var own = [String: String]()

        for case let gospel as [String: Any] in chat["gospels"] as? [Any] ?? [] {
            guard let flock = gospel["flock"] as? String, let id = gospel["id"] as? String else { continue }

            running[flock] = (gospel["session"] as? NSNumber)?.int64Value ?? 0
            own[flock] = Flock.local(id)
        }

        if own != locals {
            locals = own
            defaults.set(own, forKey: "chatSelves")
        }

        let restarted = Set(running.filter { sessions[$0.key] != $0.value }.keys)

        if session != chatSession || !restarted.isEmpty {
            chatResubmit = 0
            chatAccepts = -1
            chatWaiting.removeAll()

            if session != chatSession {
                chatAsking.removeAll()
                askingSave()
                if vigil_chat_nick(defaults.string(forKey: "chatNick") ?? "") == -1 {
                    NSLog("vigil: our nickname was refused")
                }

                submitAvatar()
                _ = try? vault.expire()
            }

            if submitOutbox(flocks: restarted) {
                chatSession = session
                sessions = running
            }
        } else if chatResubmit != 0, ProcessInfo.processInfo.systemUptime >= chatResubmit {
            chatResubmit = 0
            submitOutbox(only: chatWaiting, flocks: Set(running.keys))
        }

        let pending = receipts
        receipts.removeAll()

        for (peer, ref, kind) in pending {
            receipt(peer, ref: ref, kind: kind)
        }

        let present = around(status)
        let arrived = present.subtracting(chatAround)

        chatAround = present

        do {
            if ProcessInfo.processInfo.systemUptime >= chatSweep {
                chatSweep = ProcessInfo.processInfo.systemUptime + Self.sweepInterval
                try sweep()
                try rejoin(present)
            } else if !arrived.isEmpty {
                try rejoin(arrived)
            }
        } catch {
            NSLog("vigil: failed to sweep the group outbox: \(error)")
        }

        admit()

        var buf = [CChar](repeating: 0, count: Int(VIGIL_CHAT_EVENTS_MAX))
        let len = vigil_chat_events(&buf, buf.count)
        let data = Data(bytes: buf, count: len)

        TunnelKeychain.wipe(&buf)

        guard len > 0,
              let events = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]],
              !events.isEmpty else {
            mirror(status)
            return
        }

        var unseen = ChatVault.shared.unseen()
        var fresh = [String: Int]()

        var acks = [UInt64]()

        for event in events {
            guard let hex = event["peer"] as? String,
                  let peer = UInt8(hex, radix: 16), peer < 0xff,
                  let kind = event["type"] as? String else { continue }

            let flock = Flock.hex(event["flock"] as? String ?? "0")
            let from = Flock.id(flock, hex)
            let chat = (event["group"] as? String)
                .map { ChatVault.groupChat(flock, event["owner"] as? String ?? "", $0) } ?? from

            guard peer > 0 else { continue }

            let token = (event["token"] as? NSNumber)?.uint64Value ?? 0

            do {
                switch kind {
                case "message":
                    let text = event["text"] as? String ?? ""
                    let nick = event["nick"] as? String ?? ""
                    let quote = ChatVault.quote(event["author"] as? String, event["hash"] as? String)

                    guard try admitted(chat, hex, event) else { continue }

                    try vault.deliver(chat: chat, peer: peer, nick: nick, text: text,
                                      time: Int64(Date().timeIntervalSince1970 * 1000), quote: quote,
                                      topic: event["topic"] as? String ?? "")
                    stored(token)

                    if defaults.string(forKey: "chatOpen") != chat,
                       event["mention"] as? Bool == true || ChatVault.muted(chat) == 0 {
                        unseen[chat, default: 0] += 1
                        fresh[chat, default: 0] += 1
                        notifyChat(chat, from, nick, event["signed"] as? String ?? "", text,
                                   topic: event["topic"] as? String ?? "", badge: unseen.values.reduce(0, +))
                    }
                case "topic":
                    guard let topic = event["topic"] as? String else { continue }

                    guard try admitted(chat, hex, event) else { continue }

                    try vault.deliver(chat: chat, peer: peer, nick: event["nick"] as? String ?? "",
                                      text: event["name"] as? String ?? "",
                                      time: Int64(Date().timeIntervalSince1970 * 1000), kind: kind,
                                      topic: topic,
                                      color: Int(event["color"] as? String ?? "", radix: 16) ?? 0)
                    stored(token)
                case "retract":
                    guard let hash = event["hash"] as? String else { continue }

                    guard try admitted(chat, hex, event) else { continue }

                    try vault.deliver(chat: chat, peer: peer, nick: event["nick"] as? String ?? "", text: hash,
                                      time: Int64(Date().timeIntervalSince1970 * 1000), kind: kind,
                                      topic: event["topic"] as? String ?? "")
                    stored(token)
                case "roster", "leave":
                    let changes = try (kind == "roster" ? roster(chat, peer, event) : left(chat, hex)) ?? []

                    stored(token)

                    for (member, text) in changes {
                        guard defaults.string(forKey: "chatOpen") != chat, ChatVault.muted(chat) == 0 else { continue }

                        unseen[chat, default: 0] += 1
                        fresh[chat, default: 0] += 1
                        notifyChat(chat, member, "", "", text, topic: "", badge: unseen.values.reduce(0, +))
                    }
                case "ask":
                    guard !ChatVault.reachable, try !vault.admits().contains(from),
                          try !vault.ignored().contains(from), chatAsking.insert(from).inserted else { continue }

                    askingSave()

                    if defaults.string(forKey: "chatOpen") != from, ChatVault.muted(from) == 0 {
                        unseen[from, default: 0] += 1
                        fresh[from, default: 0] += 1
                        notifyChat(from, from, "", "", "Is trying to reach you, accept or decline under Peers",
                                   topic: "", badge: unseen.values.reduce(0, +))
                    }
                case "safety":
                    safetyStore(from)
                case "ambry":
                    try vault.deliver(chat: chat, peer: peer, nick: "",
                                      text: event["generation"] as? String ?? "",
                                      time: Int64(Date().timeIntervalSince1970 * 1000),
                                      kind: kind)
                case "data":
                    let nick = event["nick"] as? String ?? ""
                    let name = event["name"] as? String ?? "file"
                    let media = event["kind"] as? String ?? "file"
                    let size = (event["size"] as? NSNumber)?.intValue ?? 0

                    guard size > 0, size <= Int(VIGIL_CHAT_DATA_MAX) else { continue }

                    var bytes = [UInt8](repeating: 0, count: size)
                    let taken = vigil_chat_data_take(token, &bytes, bytes.count)

                    guard taken > 0 else { continue }

                    guard try admitted(chat, hex, event) else {
                        TunnelKeychain.wipe(&bytes)
                        continue
                    }

                    try vault.deliver(chat: chat, peer: peer, nick: nick, text: "",
                                      time: Int64(Date().timeIntervalSince1970 * 1000),
                                      data: Data(bytes[0..<taken]), media: media, name: name,
                                      topic: event["topic"] as? String ?? "")
                    stored(token)
                    TunnelKeychain.wipe(&bytes)

                    if defaults.string(forKey: "chatOpen") != chat, ChatVault.muted(chat) == 0 {
                        unseen[chat, default: 0] += 1
                        fresh[chat, default: 0] += 1
                        notifyChat(chat, from, nick, event["signed"] as? String ?? "",
                                   Self.summary(media, name), topic: event["topic"] as? String ?? "",
                                   badge: unseen.values.reduce(0, +))
                    }
                case "name":
                    try vault.peerName(from, nick: event["nick"] as? String ?? "",
                                       signed: event["signed"] as? String ?? "")
                    defaults.set(defaults.integer(forKey: "chatNames") + 1, forKey: "chatNames")
                case "avatar":
                    let size = (event["size"] as? NSNumber)?.intValue ?? 0

                    if token == 0 {
                        try vault.peerAvatar(from, nil)
                    } else {
                        guard size > 0, size <= Int(VIGIL_CHAT_AVATAR_MAX) else { continue }

                        var bytes = [UInt8](repeating: 0, count: size)
                        let taken = vigil_chat_data_take(token, &bytes, bytes.count)

                        guard taken > 0 else { continue }

                        try vault.peerAvatar(from, Data(bytes[0..<taken]))
                        stored(token)
                        TunnelKeychain.wipe(&bytes)
                    }

                    defaults.set(defaults.integer(forKey: "chatAvatars") + 1, forKey: "chatAvatars")
                case "echo":
                    try echo(chat, event)
                    acks.append(token)
                case "desktop":
                    acks.append(desktop(event))
                case "sent", "delivered":
                    receipt(from, ref: (event["ref"] as? NSNumber)?.int64Value ?? 0, kind: kind)
                default:
                    break
                }
            } catch {
                NSLog("vigil: failed to store a chat event: \(error)")
            }
        }

        if !fresh.isEmpty {
            _ = ChatVault.shared.unseen { counts in
                for (chat, count) in fresh {
                    counts[chat, default: 0] += count
                }
            }
        }

        defaults.set(defaults.integer(forKey: "chatVersion") + 1, forKey: "chatVersion")

        mirror(status)

        for token in acks where token != 0 {
            stored(token)
        }
    }

    private func desktop(_ event: [String: Any]) -> UInt64 {
        let token = (event["token"] as? NSNumber)?.uint64Value ?? 0

        if event["what"] as? String == "resync" {
            ChatVault.resyncWanted()
            return 0
        }

        guard event["what"] as? String == "ask", let text = event["ask"] as? String,
              let ask = try? JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any] else {
            return token
        }

        let size = (event["size"] as? NSNumber)?.intValue ?? 0
        var bytes = [UInt8]()

        if size > 0, size <= Int(VIGIL_CHAT_DATA_MAX) {
            bytes = [UInt8](repeating: 0, count: size)
            let taken = vigil_chat_data_take(token, &bytes, bytes.count)

            if taken != size {
                bytes = []
            }
        }

        defer { TunnelKeychain.wipe(&bytes) }

        var answer: [String: Any] = [
            "op": "answer", "id": (ask["id"] as? NSNumber)?.uint64Value ?? 0, "what": ask["what"] as? String ?? "",
        ]

        do {
            try asked(ask, bytes.isEmpty ? nil : Data(bytes))
            _ = submitOutbox()
            answer["ok"] = true
        } catch {
            NSLog("vigil: could not do what our desktop asked: \(error)")
            answer["ok"] = false
            answer["reason"] = "\(error)"
        }

        ChatVault.shared.emit(answer)

        return token
    }

    private func asked(_ ask: [String: Any], _ data: Data?) throws {
        let vault = ChatVault.shared
        let chat = ask["chat"] as? String ?? ""

        switch ask["what"] as? String ?? "" {
        case "topic":
            _ = try vault.topicSet(chat, topic: ask["topic"], name: ask["name"] as? String ?? "",
                                   color: (ask["color"] as? NSNumber)?.intValue ?? 0,
                                   local: ChatVault.group(chat) == nil ? "" : local(chat))
        case "delete":
            if Flock.valid(chat) {
                guard vigil_chat_forget(Flock.number(chat), Flock.peer(chat)) == 0 else {
                    throw ChatVault.Failure(description: "Vigil is busy, try deleting the chat again.")
                }
            } else if ChatVault.group(chat) == nil {
                throw ChatVault.Failure(description: "There is no chat \(chat).")
            }

            drop(try vault.deleteChat(chat, local: local(chat)))
        case "read":
            try vault.read(chat, topic: ask["topic"] as? String ?? "")
            ChatVault.seen(chat)
        case "favorite", "pin":
            guard let ref = (ask["ref"] as? NSNumber)?.int64Value else {
                throw ChatVault.Failure(description: "There is no such message.")
            }

            let on = ask["on"] as? Bool ?? false

            if ask["what"] as? String == "pin" {
                try vault.pin(chat, ref: ref, on: on)
            } else {
                try vault.favorite(chat, ref: ref, on: on)
            }
        case "star":
            ChatVault.star(chat, on: ask["on"] as? Bool ?? false)
        case "compact":
            ChatVault.compact(chat, on: ask["on"] as? Bool ?? false)
        case "mute":
            ChatVault.mute(chat, until: (ask["until"] as? NSNumber)?.intValue ?? 0)
        case "retract":
            let ref = (ask["ref"] as? NSNumber)?.int64Value ?? 0
            let local = ChatVault.group(chat) == nil ? "" : self.local(chat)

            guard let hash = (ask["hash"] as? String).flatMap({ UInt64($0, radix: 16) }) else {
                throw ChatVault.Failure(description: "Open Vigil on your phone to delete that image.")
            }

            drop(try vault.retractHash(chat, ref: ref, hash: hash, topic: ask["topic"] as? String ?? "",
                                       local: local))
        case "group":
            let flock = Flock.hex(ask["flock"] as? String ?? "0")
            let local = locals[flock] ?? ""

            guard !local.isEmpty else {
                throw ChatVault.Failure(description: "Gospel is not running for flock \(flock).")
            }

            _ = try vault.groupCreate(flock: flock, name: ask["name"] as? String ?? "",
                                      members: ChatVault.picked(ask["members"], flock: flock, without: local) ?? [],
                                      local: local)
        case "edit":
            let local = self.local(chat)

            drop(try vault.groupEdit(chat, name: ask["name"] as? String,
                                     members: ChatVault.picked(ask["members"], flock: Flock.of(chat),
                                                               without: local),
                                     local: local))
        case "leave":
            drop(try vault.groupLeave(chat, local: local(chat)))
        case "nick":
            try ChatVault.setNick(ask["nick"] as? String ?? "")
        case "avatar":
            try vault.ownAvatar(try data.map { try Avatar.jpeg($0) })
            submitAvatar()
            ChatVault.defaults.set(ChatVault.defaults.integer(forKey: "chatAvatars") + 1, forKey: "chatAvatars")
        default:
            throw ChatVault.Failure(description: "Vigil does not know how to do that.")
        }

        ChatVault.defaults.set(ChatVault.defaults.integer(forKey: "chatVersion") + 1, forKey: "chatVersion")
    }

    private func local(_ chat: String) -> String {
        locals[Flock.of(chat)] ?? ""
    }

    private func mirror(_ status: [String: Any]) {
        guard let companion = (status["chat"] as? [String: Any])?["companion"] as? [String: Any],
              companion["role"] as? String == "phone" else { return }

        let room = (companion["room"] as? NSNumber)?.intValue ?? 0

        do {
            _ = try ChatVault.shared.mirrorDrain(room: room, { Self.sync($0, $1) == 0 }, {
                vigil_chat_companion_resync($0 ? 1 : 0) == 0
            })
        } catch {
            NSLog("vigil: failed to send our desktop what changed: \(error)")
        }
    }

    private static func sync(_ json: Data, _ data: Data?) -> Int32 {
        return json.withUnsafeBytes { op in
            (data ?? Data()).withUnsafeBytes { bytes in
                vigil_chat_companion_sync(op.baseAddress?.assumingMemoryBound(to: CChar.self), op.count,
                                          data == nil ? nil : bytes.baseAddress, data?.count ?? 0)
            }
        }
    }

    private func submitAvatar() {
        do {
            let avatar = try ChatVault.shared.ownAvatar() ?? Data()

            guard avatar.count <= Int(VIGIL_CHAT_AVATAR_MAX) else {
                NSLog("vigil: our avatar is too large")
                return
            }

            avatar.withUnsafeBytes { vigil_chat_avatar($0.baseAddress, avatar.count) }
        } catch {
            NSLog("vigil: failed to load our avatar: \(error)")
        }
    }

    private func attachGospel() {
        let on = ChatVault.defaults.object(forKey: "gospelEnabled") as? Bool ?? true
        var configs = on ? Self.gospels() : []
        var set = Self.joined(configs)

        for idx in configs.indices {
            TunnelKeychain.wipe(&configs[idx])
        }

        guard !set.isEmpty else {
            if vigil_gospel(nil, 0) == -1 {
                NSLog("vigil: Gospel was not detached, vigil is not running")
            }
            return
        }

        let attached = set.withUnsafeBytes { raw in
            vigil_gospel(raw.baseAddress?.assumingMemoryBound(to: CChar.self), raw.count) == 0
        }

        TunnelKeychain.wipe(&set)

        if !attached {
            NSLog("vigil: Gospel was not attached, vigil is not running")
        }
    }

    private func attachCompanion() {
        var config = TunnelKeychain.load(id: "companion") ?? []

        let ret = config.withUnsafeBytes { raw in
            vigil_chat_companion(raw.baseAddress?.assumingMemoryBound(to: CChar.self), raw.count)
        }

        TunnelKeychain.wipe(&config)

        if ret == -1 {
            NSLog("vigil: the desktop configuration was refused")
        }
    }

    private func echo(_ chat: String, _ event: [String: Any]) throws {
        let what = event["what"] as? String ?? ""
        let ref = (event["ref"] as? NSNumber)?.int64Value ?? 0
        let to = event["members"] as? [String] ?? []
        let topic = event["topic"] as? String ?? ""
        let serial = (event["serial"] as? NSNumber)?.intValue ?? 0
        let time = Int64(Date().timeIntervalSince1970 * 1000)

        if try ChatVault.shared.groupGone(chat) {
            unsent(chat, ref, event, to)
            return
        }

        if what == "text" {
            try ChatVault.shared.echo(chat: chat, ref: ref, text: event["text"] as? String ?? "", time: time,
                                      quote: ChatVault.quote(event["author"] as? String, event["hash"] as? String),
                                      topic: topic, to: to, serial: serial)
            return
        }

        guard what == "data" else { return }

        let token = (event["token"] as? NSNumber)?.uint64Value ?? 0
        let size = (event["size"] as? NSNumber)?.intValue ?? 0

        guard size > 0, size <= Int(VIGIL_CHAT_DATA_MAX) else { return }

        var bytes = [UInt8](repeating: 0, count: size)
        defer { TunnelKeychain.wipe(&bytes) }

        let taken = vigil_chat_data_take(token, &bytes, bytes.count)

        guard taken > 0 else { return }

        try ChatVault.shared.echo(chat: chat, ref: ref, text: "", time: time, quote: nil, topic: topic, to: to,
                                  serial: serial, data: Data(bytes[0..<taken]),
                                  media: event["kind"] as? String ?? "file", name: event["name"] as? String ?? "file")
    }

    private func unsent(_ chat: String, _ ref: Int64, _ event: [String: Any], _ to: [String]) {
        let token = (event["token"] as? NSNumber)?.uint64Value ?? 0
        let size = (event["size"] as? NSNumber)?.intValue ?? 0

        NSLog("vigil: dropped a message from our desktop to \(chat), we are no longer in that group")

        if event["what"] as? String == "data", size > 0, size <= Int(VIGIL_CHAT_DATA_MAX) {
            var bytes = [UInt8](repeating: 0, count: size)

            _ = vigil_chat_data_take(token, &bytes, bytes.count)
            TunnelKeychain.wipe(&bytes)
        }

        drop(to.map { (Flock.id(Flock.of(chat), $0), ref) })

        ChatVault.shared.emit(["op": "answer", "what": "send", "ok": false,
                               "reason": "You are no longer in that group.", "chat": chat, "ref": ref])
    }

    private func submitted(_ ref: UInt64, _ ret: Int32) {
        guard ret == -1 else {
            chatWaiting.remove(ref)
            return
        }

        chatWaiting.insert(ref)

        if chatResubmit == 0 {
            chatResubmit = ProcessInfo.processInfo.systemUptime + Self.resubmitInterval
        }
    }

    private func stored(_ token: UInt64) {
        if vigil_chat_stored(token) == -1 {
            NSLog("vigil: failed to ack \(token), it is handed over again")
        }
    }

    private func receipt(_ peer: String, ref: Int64, kind: String) {
        do {
            try ChatVault.shared.receipt(peer, ref: ref, kind: kind)
        } catch {
            NSLog("vigil: failed to store a receipt, trying again: \(error)")
            receipts.append((peer, ref, kind))
        }
    }

    private func admitted(_ chat: String, _ peer: String, _ event: [String: Any]) throws -> Bool {
        guard ChatVault.group(chat) != nil else { return true }

        let serial = (event["serial"] as? NSNumber)?.uint32Value ?? 0

        switch try ChatVault.shared.groupCheck(chat, peer: peer, serial: serial) {
        case .drop:
            stored((event["token"] as? NSNumber)?.uint64Value ?? 0)
            return false
        case .hold:
            return false
        case .store:
            return true
        }
    }

    private func sweep() throws {
        let dropped = try ChatVault.shared.expire()

        drop(dropped)

        if !dropped.isEmpty {
            ChatVault.defaults.set(ChatVault.defaults.integer(forKey: "chatVersion") + 1, forKey: "chatVersion")
        }
    }

    private func drop(_ dropped: [(String, Int64)]) {
        for (peer, ref) in dropped {
            if Flock.valid(peer),
               vigil_chat_drop(Flock.number(peer), Flock.peer(peer), UInt64(bitPattern: ref)) == -1 {
                NSLog("vigil: failed to drop \(ref) for \(peer)")
            }
        }
    }

    private func around(_ status: [String: Any]) -> Set<String> {
        var around = Set((status["call"] as? [String: Any])?["peers"] as? [String] ?? [])

        for peer in (status["chat"] as? [String: Any])?["peers"] as? [[String: Any]] ?? [] {
            if peer["present"] as? Bool == true || peer["online"] as? Bool == true, let id = peer["id"] as? String {
                around.insert(id)
            }
        }

        return around
    }

    private func rejoin(_ around: Set<String>) throws {
        let vault = ChatVault.shared
        var queued = false

        for (chat, value) in try vault.groups() {
            let local = self.local(chat)

            guard !local.isEmpty, let info = value as? [String: Any], info["owner"] as? String == local else {
                continue
            }

            for member in info["stale"] as? [String] ?? [] where around.contains(Flock.id(Flock.of(chat), member)) {
                try vault.groupStale(chat, peer: member, on: false)

                if let current = try vault.groupInfo(chat) {
                    try vault.rosterQueue(chat, current, local: local, only: [member])
                    try vault.topicQueue(chat, local: local, targets: [member])
                    queued = true
                }
            }
        }

        if queued {
            submitOutbox()
        }
    }

    private func roster(_ chat: String, _ peer: UInt8, _ event: [String: Any]) throws -> [(String, String)]? {
        let vault = ChatVault.shared
        let owner = ChatVault.hex(peer)
        let local = self.local(chat)
        let flock = Flock.of(chat)
        let info = try vault.groupInfo(chat)
        let serial = (event["serial"] as? NSNumber)?.intValue ?? 0
        let time = Int64(Date().timeIntervalSince1970 * 1000)
        let previous = info?["state"] as? String ?? ""

        if let info, serial <= (info["serial"] as? NSNumber)?.intValue ?? 0 { return nil }
        if previous == "left" || previous == "ended" { return nil }

        var changes = [(String, String)]()

        let members = event["members"] as? [String] ?? []
        let before = info.map { ChatVault.members($0) } ?? []
        let name = event["name"] as? String ?? ""
        let state = event["ended"] as? Bool == true ? "ended" : members.contains(local) ? "open" : "removed"

        if info == nil && state != "open" { return nil }

        try vault.groupSave(chat, [
            "owner": owner, "id": String(Flock.local(chat).dropFirst(3)), "name": name, "members": members,
            "serial": serial, "state": state,
        ])

        if info == nil || previous == "removed" {
            if state == "open" {
                try vault.deliver(chat: chat, peer: peer, nick: "", text: name, time: time, kind: "added")
                changes.append((Flock.id(flock, owner), "added you to \(name.isEmpty ? "a group" : name)"))
            }
        } else {
            for member in members where !before.contains(member) && member != local {
                if let id = UInt8(member, radix: 16) {
                    try vault.deliver(chat: chat, peer: id, nick: "", text: "", time: time, kind: "joined")
                    changes.append((Flock.id(flock, member), "joined the group"))
                }
            }

            for member in before where !members.contains(member) && member != local {
                if let id = UInt8(member, radix: 16) {
                    try vault.deliver(chat: chat, peer: id, nick: "", text: "", time: time, kind: "left")
                    changes.append((Flock.id(flock, member), "left the group"))
                }
            }

            if name != info?["name"] as? String ?? "" {
                try vault.deliver(chat: chat, peer: peer, nick: "", text: name, time: time, kind: "renamed")
            }
        }

        if state != "open" {
            try vault.deliver(chat: chat, peer: peer, nick: "", text: "", time: time, kind: state)
            try drop(vault.groupDrop(chat, before + members))
        } else {
            try drop(vault.groupDrop(chat, before.filter { !members.contains($0) }))
        }

        if state == "removed" {
            changes.append((Flock.id(flock, owner), "removed you from the group"))
        }

        ChatVault.defaults.set(ChatVault.defaults.integer(forKey: "chatNames") + 1, forKey: "chatNames")

        return changes
    }

    private func left(_ chat: String, _ member: String) throws -> [(String, String)]? {
        let vault = ChatVault.shared
        let local = self.local(chat)

        guard var info = try vault.groupInfo(chat), info["owner"] as? String == local,
              info["state"] as? String == "open" else { return nil }

        let members = ChatVault.members(info)

        guard members.contains(member), let peer = UInt8(member, radix: 16) else { return nil }

        info["members"] = members.filter { $0 != member }
        info["serial"] = ((info["serial"] as? NSNumber)?.intValue ?? 0) + 1

        try vault.groupSave(chat, info)
        try drop(vault.groupDrop(chat, [member]))
        try vault.deliver(chat: chat, peer: peer, nick: "", text: "",
                          time: Int64(Date().timeIntervalSince1970 * 1000), kind: "left")
        try vault.rosterQueue(chat, info, local: local)

        submitOutbox()
        ChatVault.defaults.set(ChatVault.defaults.integer(forKey: "chatNames") + 1, forKey: "chatNames")

        return [(Flock.id(Flock.of(chat), member), "left the group")]
    }

    private func onMain(_ body: () -> Void) {
        if Thread.isMainThread {
            body()
        } else {
            DispatchQueue.main.sync(execute: body)
        }
    }

    @discardableResult
    private func submitOutbox(only: Set<UInt64>? = nil, flocks: Set<String>? = nil) -> Bool {
        let outbox: [[String: Any]]

        do {
            outbox = try ChatVault.shared.outboxList()
        } catch {
            NSLog("vigil: failed to read the outbox: \(error)")

            if only != nil {
                chatResubmit = ProcessInfo.processInfo.systemUptime + Self.resubmitInterval
            }

            return false
        }

        var refs = Set<UInt64>()

        for entry in outbox {
            guard let hex = entry["peer"] as? String, Flock.valid(hex),
                  let ref = (entry["ref"] as? NSNumber)?.uint64Value else { continue }

            let flock = Flock.number(hex)
            let peer = Flock.peer(hex)
            let chat = entry["chat"] as? String ?? hex
            let group = ChatVault.group(chat)

            refs.insert(ref)

            if let only, !only.contains(ref) { continue }

            if let flocks, !flocks.contains(Flock.of(hex)) { continue }

            if entry["echo"] as? Bool == true { continue }

            let topic = (entry["tp"] as? String).flatMap { UInt64($0, radix: 16) } ?? 0

            if entry["op"] as? String == "roster", let group {
                let name = Array((entry["name"] as? String ?? "").utf8).map { CChar(bitPattern: $0) }
                let members = ChatVault.roster(entry["members"] as? [String] ?? [])
                let serial = UInt32(truncatingIfNeeded: (entry["serial"] as? NSNumber)?.intValue ?? 0)
                let state = entry["ended"] as? Bool == true ? VIGIL_CHAT_GROUP_ENDED : VIGIL_CHAT_GROUP_OPEN

                guard name.count <= Int(VIGIL_CHAT_GROUP_NAME_MAX) else { continue }

                submitted(ref, vigil_chat_group_roster(flock, peer, ref, group.id, serial, Int32(state), members,
                                                       name, name.count))
                continue
            }

            if entry["op"] as? String == "leave", let group {
                submitted(ref, vigil_chat_group_leave(flock, peer, ref, group.id))
                continue
            }

            if entry["op"] as? String == "retract" {
                guard let hash = (entry["hash"] as? String).flatMap({ UInt64($0, radix: 16) }), hash != 0 else {
                    NSLog("vigil: the retract \(ref) for \(chat) has no hash, dropping it")
                    receipt(hex, ref: Int64(bitPattern: ref), kind: "delivered")
                    continue
                }

                if let group {
                    let serial = UInt32(truncatingIfNeeded: (entry["gs"] as? NSNumber)?.intValue ?? 0)

                    submitted(ref, vigil_chat_group_retract(flock, peer, ref, group.owner, group.id, serial, topic,
                                                            hash))
                    continue
                }

                submitted(ref, vigil_chat_retract(flock, peer, ref, topic, hash))
                continue
            }

            if entry["op"] as? String == "topic" {
                guard topic != 0 else { continue }

                let name = Array((entry["name"] as? String ?? "").utf8).map { CChar(bitPattern: $0) }
                let color = UInt32(truncatingIfNeeded: (entry["color"] as? NSNumber)?.intValue ?? 0)

                guard name.count <= Int(VIGIL_CHAT_TOPIC_NAME_MAX) else { continue }

                if let group {
                    let serial = UInt32(truncatingIfNeeded: (entry["gs"] as? NSNumber)?.intValue ?? 0)

                    submitted(ref, vigil_chat_group_topic(flock, peer, ref, group.id, serial, topic, color, name,
                                                          name.count))
                    continue
                }

                submitted(ref, vigil_chat_topic(flock, peer, ref, topic, color, name, name.count))
                continue
            }

            if entry["data"] != nil {
                let data: Data

                do {
                    guard let found = try ChatVault.shared.queued(ref) else {
                        NSLog("vigil: the data of \(ref) for \(chat) is gone")
                        continue
                    }

                    data = found
                } catch {
                    NSLog("vigil: failed to read the data of \(ref) for \(chat): \(error)")
                    continue
                }

                guard !data.isEmpty, data.count <= Int(VIGIL_CHAT_DATA_MAX) else { continue }

                let media = entry["media"] as? String ?? "file"
                let name = entry["name"] as? String ?? "file"

                let serial = UInt32(truncatingIfNeeded: (entry["gs"] as? NSNumber)?.intValue ?? 0)
                let sent = data.withUnsafeBytes { (raw) -> Int32 in
                    if let group {
                        return vigil_chat_group_data_send(flock, peer, ref, group.owner, group.id, serial, topic,
                                                          ChatVault.kind(media), name, raw.baseAddress, data.count)
                    }

                    if topic != 0 {
                        return vigil_chat_topic_data_send(flock, peer, ref, topic, ChatVault.kind(media), name,
                                                          raw.baseAddress, data.count)
                    }

                    return vigil_chat_data_send(flock, peer, ref, ChatVault.kind(media), name, raw.baseAddress,
                                                data.count)
                }

                submitted(ref, sent)
                continue
            }

            guard let text = entry["text"] as? String else { continue }

            let bytes = Array(text.utf8).map { CChar(bitPattern: $0) }

            if let group {
                let quote = ChatVault.quote(entry["qa"] as? String, entry["qh"] as? String)
                var max = quote == nil ? VIGIL_CHAT_GROUP_MAX : VIGIL_CHAT_GROUP_REPLY_MAX

                if topic != 0 {
                    max = VIGIL_CHAT_GROUP_TOPIC_MAX
                }

                guard !bytes.isEmpty, bytes.count <= Int(max) else { continue }

                let serial = UInt32(truncatingIfNeeded: (entry["gs"] as? NSNumber)?.intValue ?? 0)

                submitted(ref, vigil_chat_group_send(flock, peer, ref, group.owner, group.id, serial, topic,
                                                     quote?.peer ?? 0, quote?.digest ?? 0, bytes, bytes.count))
                continue
            }

            if topic != 0 {
                let quote = ChatVault.quote(entry["qa"] as? String, entry["qh"] as? String)

                guard !bytes.isEmpty, bytes.count <= Int(VIGIL_CHAT_TOPIC_MAX) else { continue }

                submitted(ref, vigil_chat_topic_send(flock, peer, ref, topic, quote?.peer ?? 0, quote?.digest ?? 0,
                                                     bytes, bytes.count))
                continue
            }

            if let quote = ChatVault.quote(entry["qa"] as? String, entry["qh"] as? String) {
                guard !bytes.isEmpty, bytes.count <= Int(VIGIL_CHAT_REPLY_MAX) else { continue }

                submitted(ref, vigil_chat_reply(flock, peer, ref, quote.peer, quote.digest, bytes, bytes.count))
                continue
            }

            guard !bytes.isEmpty, bytes.count <= Int(VIGIL_CHAT_MESSAGE_MAX) else { continue }

            submitted(ref, vigil_chat_send(flock, peer, ref, bytes, bytes.count))
        }

        chatWaiting.formIntersection(refs)

        return true
    }

    private static func summary(_ media: String, _ name: String) -> String {
        switch media {
        case "image":
            return "Image"
        case "voice":
            return "Voice message"
        default:
            return "File: \(name)"
        }
    }

    private static func sender(_ peer: String, _ nick: String, _ signed: String) -> String {
        if ChatVault.names()[peer] != nil {
            return ChatVault.name(peer)
        }

        if !signed.isEmpty {
            return signed
        }

        if !nick.isEmpty {
            return "~\(nick)"
        }

        return ChatVault.name(peer)
    }

    private func admit() {
        guard ChatVault.accepts != chatAccepts else { return }

        guard let admits = try? ChatVault.shared.admits(),
              let ignored = try? ChatVault.shared.ignored() else { return }

        chatAccepts = ChatVault.accepts

        let reachable = ChatVault.reachable

        for flock in locals.keys {
            let number = UInt64(flock, radix: 16) ?? 0

            for peer in UInt8(1)..<UInt8(0xff) {
                let id = Flock.id(flock, ChatVault.hex(peer))

                vigil_chat_ignore(number, peer, ignored.contains(id) ? 1 : 0)
                vigil_chat_accept(number, peer, reachable || admits.contains(id) ? 1 : 0)
            }
        }

        let before = chatAsking.count

        chatAsking.subtract(admits.union(ignored))

        if reachable {
            chatAsking.removeAll()
        }

        if chatAsking.count != before {
            askingSave()
        }
    }

    private func askingSave() {
        let defaults = ChatVault.defaults

        defaults.set(chatAsking.sorted(), forKey: "chatAsking")
        defaults.set(defaults.integer(forKey: "chatVersion") + 1, forKey: "chatVersion")
    }

    private func notifyChat(_ chat: String, _ peer: String, _ nick: String, _ signed: String, _ text: String,
                            topic: String, badge: Int) {
        let preview = ChatVault.defaults.object(forKey: "chatPreview") as? Bool ?? false
        let content = UNMutableNotificationContent()
        let body = preview ? text : "New message"
        let sender = Self.sender(peer, nick, signed)

        content.title = ChatVault.group(chat) == nil ? sender : ChatVault.name(chat: chat)
        content.body = ChatVault.group(chat) == nil ? body : "\(sender): \(body)"
        content.sound = .default
        content.badge = NSNumber(value: badge)
        content.threadIdentifier = "chat-\(chat)"
        content.userInfo = ["chat": chat, "topic": topic]

        UNUserNotificationCenter.current().add(
            UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil))
    }

    private func watchNetwork() {
        let monitor = NWPathMonitor()

        monitor.pathUpdateHandler = { [weak self] path in
            guard let self, path.status == .satisfied else { return }

            let network = path.availableInterfaces.first.map { "\($0.name)" }
            if self.network != nil && network != self.network {
                vigil_rebind()
            }

            self.network = network
        }

        monitor.start(queue: .main)
        paths = monitor
    }

    private func watchCalls() {
        let timer = DispatchSource.makeTimerSource(queue: .main)
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler { [weak self] in self?.checkCalls() }
        timer.resume()

        calls = timer
    }

    private func checkCalls() {
        var buf = [CChar](repeating: 0, count: Int(VIGIL_STATUS_MAX))
        let len = vigil_status(&buf, buf.count)

        guard len > 0,
              let status = try? JSONSerialization.jsonObject(
                with: Data(bytes: buf, count: len)) as? [String: Any] else {
            callers.removeAll()
            return
        }

        checkChats(status)

        var voice = [CChar](repeating: 0, count: 4096)
        let voiceLen = vigil_voice_status(&voice, voice.count)

        if voiceLen > 0,
           let state = try? JSONSerialization.jsonObject(with: Data(bytes: voice, count: voiceLen)) as? [String: Any],
           state["relay"] as? Bool == true {
            UNUserNotificationCenter.current().removeDeliveredNotifications(withIdentifiers: callers.map { "call-\($0)" })
            callers.removeAll()
            return
        }

        guard let call = status["call"] as? [String: Any] else {
            callers.removeAll()
            return
        }

        let now = Set(call["incoming"] as? [String] ?? [])

        for peer in now.subtracting(callers) {
            let content = UNMutableNotificationContent()
            content.title = ChatVault.name(peer)
            content.body = "Incoming call"
            content.sound = .defaultRingtone
            content.interruptionLevel = .timeSensitive
            content.categoryIdentifier = ChatVault.callCategory
            content.userInfo = ["call": peer]

            UNUserNotificationCenter.current().add(
                UNNotificationRequest(identifier: "call-\(peer)", content: content, trigger: nil))
        }

        let gone = callers.subtracting(now).map { "call-\($0)" }
        UNUserNotificationCenter.current().removeDeliveredNotifications(withIdentifiers: gone)

        callers = now
    }

    private static func settings(alone summary: [String: Any]) -> NEPacketTunnelNetworkSettings {
        let cathedral = summary["cathedral"] as? String ?? "127.0.0.1"
        let host = String(cathedral.split(separator: ":").first ?? "127.0.0.1")
        let settings = NEPacketTunnelNetworkSettings(tunnelRemoteAddress: resolve(host))

        settings.ipv4Settings = NEIPv4Settings(addresses: [voiceAddress], subnetMasks: ["255.255.255.255"])

        return settings
    }

    /// The caller must wipe the configurations.
    private static func gospels() -> [[UInt8]] {
        let list = ChatVault.defaults.array(forKey: "gospels") as? [[String: Any]] ?? []

        return list.compactMap { ($0["ref"] as? Data).flatMap { try? TunnelKeychain.load(ref: $0) } }
    }

    /// The caller must wipe the set.
    private static func joined(_ configs: [[UInt8]]) -> [UInt8] {
        var out = [UInt8]()

        for config in configs {
            out.append(contentsOf: config)
            out.append(0)
        }

        return out
    }

    private static func cathedral(_ voice: [UInt8]) -> String? {
        guard let cathedral = summary(voice)?["cathedral"] as? String else { return nil }

        return String(cathedral.split(separator: ":").first ?? "")
    }

    private static func summary(_ config: [UInt8]) -> [String: Any]? {
        guard !config.isEmpty else { return nil }

        var out = [CChar](repeating: 0, count: 4096)
        let outlen = out.count

        let ok = config.withUnsafeBytes { raw in
            vigil_config_check(raw.baseAddress?.assumingMemoryBound(to: CChar.self),
                               raw.count, &out, outlen) == 0
        }

        guard ok else { return nil }

        return try? JSONSerialization.jsonObject(with: Data(String(cString: out).utf8)) as? [String: Any]
    }

    private static func adopt(_ proto: NETunnelProviderProtocol, _ summary: [String: Any]) {
        guard ChatVault.defaults.object(forKey: "gospels") == nil else { return }

        let id = proto.providerConfiguration?["id"] as? String ?? ""
        var ref = summary["mode"] as? String == "gospel" ? proto.passwordReference : nil
        var flock = summary["flock"] as? String

        if ref == nil, let voice = proto.providerConfiguration?["voice"] as? Data,
           var config = try? TunnelKeychain.load(ref: voice) {
            ref = voice
            flock = Self.summary(config)?["flock"] as? String
            TunnelKeychain.wipe(&config)
        }

        guard let ref, let flock else { return }

        ChatVault.defaults.set([["id": id, "ref": ref, "flock": Flock.hex(flock)]], forKey: "gospels")
    }

    private func settings(_ summary: [String: Any], voice: [String]) -> NEPacketTunnelNetworkSettings {
        let endpoint = (summary["peer"] ?? summary["cathedral"]) as? String ?? "127.0.0.1"
        let host = String(endpoint.split(separator: ":").first ?? "127.0.0.1")
        let settings = NEPacketTunnelNetworkSettings(tunnelRemoteAddress: Self.resolve(host))

        let (address, mask) = Self.cidr(summary["address"] as? String ?? "0.0.0.0/32")
        let ipv4 = NEIPv4Settings(addresses: [address], subnetMasks: [mask])

        let routeStrings = summary["routes"] as? [String] ?? []
        let fullTunnel = routeStrings.contains { Self.cidr($0).1 == "0.0.0.0" }

        ipv4.includedRoutes = routeStrings.map { route -> NEIPv4Route in
            let (net, m) = Self.cidr(route)
            if m == "0.0.0.0" {
                return NEIPv4Route.default()
            }
            return NEIPv4Route(destinationAddress: net, subnetMask: m)
        }

        if !voice.isEmpty {
            ipv4.excludedRoutes = voice.map {
                NEIPv4Route(destinationAddress: Self.resolve($0), subnetMask: "255.255.255.255")
            }
        }

        settings.ipv4Settings = ipv4
        settings.mtu = NSNumber(value: summary["mtu"] as? Int ?? 1408)

        if let dns = summary["dns"] as? [String], !dns.isEmpty {
            let resolver = NEDNSSettings(servers: dns)
            resolver.matchDomains = [""]
            settings.dnsSettings = resolver
        }

        if fullTunnel && (summary["block_ipv6"] as? Bool ?? true) {
            let ipv6 = NEIPv6Settings(addresses: [Self.ipv6Sink], networkPrefixLengths: [128])
            ipv6.includedRoutes = [NEIPv6Route.default()]
            settings.ipv6Settings = ipv6
        }

        return settings
    }

    /// NEPacketTunnelNetworkSettings wants a literal ip, not a hostname.
    private static func resolve(_ host: String) -> String {
        var hints = addrinfo(ai_flags: 0, ai_family: AF_UNSPEC, ai_socktype: SOCK_DGRAM,
                              ai_protocol: 0, ai_addrlen: 0, ai_canonname: nil,
                              ai_addr: nil, ai_next: nil)
        var result: UnsafeMutablePointer<addrinfo>?

        guard getaddrinfo(host, nil, &hints, &result) == 0, let info = result else {
            return host
        }
        defer { freeaddrinfo(result) }

        var buf = [CChar](repeating: 0, count: Int(NI_MAXHOST))
        guard getnameinfo(info.pointee.ai_addr, info.pointee.ai_addrlen,
                          &buf, socklen_t(buf.count), nil, 0, NI_NUMERICHOST) == 0 else {
            return host
        }

        return String(cString: buf)
    }

    private static func cidr(_ value: String) -> (String, String) {
        let parts = value.split(separator: "/")
        let bits = parts.count > 1 ? Int(parts[1]) ?? 32 : 32
        let mask: UInt32 = bits == 0 ? 0 : ~UInt32(0) << (32 - UInt32(bits))
        let dotted = (0..<4).map { String((mask >> (24 - $0 * 8)) & 0xff) }.joined(separator: ".")

        return (String(parts.first ?? "0.0.0.0"), dotted)
    }

    private func failure(_ message: String) -> NSError {
        NSError(domain: "se.sanctorum.vigil", code: 1,
                userInfo: [NSLocalizedDescriptionKey: message])
    }
}
