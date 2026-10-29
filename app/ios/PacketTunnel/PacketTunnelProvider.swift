
import Foundation
import Network
import NetworkExtension
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
    private var chatWaiting = Set<UInt64>()
    private var receipts = [(String, Int64, String, Int, Int)]()
    private var paths: NWPathMonitor?
    private var probe: WakeProbe?
    private var network: String?
    private var gospel: Data?
    private var answered: UInt64 = 0

    /// Where the IPv6 traffic we block ends up.
    private static let ipv6Sink = "fd70:6172:6973:6800::1"
    private static let voiceAddress = "169.254.86.1"
    private static let resubmitInterval: TimeInterval = 10

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

        gospel = proto.providerConfiguration?["voice"] as? Data

        var voice = gospel.flatMap { try? TunnelKeychain.load(ref: $0) } ?? []
        let attach = ChatVault.defaults.object(forKey: "gospelEnabled") as? Bool ?? true

        run = vigil_run_id()
        let number = run

        lock.lock()
        stopping = false
        lock.unlock()

        let network = alone ? Self.settings(alone: summary)
            : settings(summary, voice: Self.cathedral(voice))

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
                config.withUnsafeBytes { raw in
                    voice.withUnsafeBytes { vraw in
                        let ptr = raw.baseAddress?.assumingMemoryBound(to: CChar.self)
                        let vlen = attach ? vraw.count : 0
                        let vptr = vlen > 0
                            ? vraw.baseAddress?.assumingMemoryBound(to: CChar.self) : nil

                        if let datadir {
                            _ = datadir.withCString { dir in
                                vigil_run(number, fd, ptr, raw.count, vptr, vlen, dir)
                            }
                        } else {
                            _ = vigil_run(number, fd, ptr, raw.count, vptr, vlen, nil)
                        }
                    }
                }

                TunnelKeychain.wipe(&config)
                TunnelKeychain.wipe(&voice)
                stopped.signal()

                guard let self else { return }

                self.lock.lock()
                let requested = self.stopping
                self.lock.unlock()

                if !requested {
                    self.cancelTunnelWithError(self.failure("the tunnel stopped"))
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

        switch words.first {
        case "dial":
            if words.count == 2, let peer = UInt8(words[1], radix: 16), peer > 0, peer < 0xff {
                vigil_call_dial(peer)
            }
        case "hangup":
            vigil_call_hangup()
        case "done":
            if words.count == 2, let peer = UInt8(words[1], radix: 16), peer > 0, peer < 0xff {
                vigil_call_done(peer)
            }
        case "chat":
            submitOutbox()
        case "chat-nick":
            _ = vigil_chat_nick(ChatVault.defaults.string(forKey: "chatNick") ?? "")
        case "chat-open", "chat-typing":
            if words.count == 3, let peer = UInt8(words[1], radix: 16), peer > 0, peer < 0xff {
                let on: Int32 = words[2] == "1" ? 1 : 0
                if words.first == "chat-open" {
                    vigil_chat_open(peer, on)
                } else {
                    vigil_chat_typing(peer, on)
                }
            }
        case "chat-forget":
            if words.count == 2, let peer = UInt8(words[1], radix: 16), peer > 0, peer < 0xff {
                vigil_chat_forget(peer)
            }
        case "gospel":
            attachGospel()
        case "chat-group":
            vigil_chat_group(UInt16(truncatingIfNeeded: ChatVault.defaults.integer(forKey: "chatGroup")))
        case "chat-group-typing":
            if words.count == 2 {
                vigil_chat_group_typing(words[1] == "1" ? 1 : 0)
            }
        case "chat-group-data":
            if words.count == 6, let ref = UInt64(words[1]),
               let data = try? ChatVault.shared.data(String(words[3]), file: String(words[2])),
               !data.isEmpty, data.count <= Int(VIGIL_CHAT_DATA_MAX),
               let name = Data(base64Encoded: String(words[5])).flatMap({ String(data: $0, encoding: .utf8) }) {
                _ = data.withUnsafeBytes {
                    vigil_chat_group_data_send(ref, ChatVault.kind(String(words[4])), name,
                                               $0.baseAddress, data.count)
                }
            }
        case "chat-group-send":
            if words.count == 3, let ref = UInt64(words[1]),
               let text = Data(base64Encoded: String(words[2])) {
                let bytes = [UInt8](text).map { CChar(bitPattern: $0) }
                if !bytes.isEmpty, bytes.count <= Int(VIGIL_CHAT_MESSAGE_MAX) {
                    _ = vigil_chat_group_send(ref, bytes, bytes.count)
                }
            }
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

        if session != chatSession {
            chatResubmit = 0
            chatWaiting.removeAll()
            _ = vigil_chat_nick(defaults.string(forKey: "chatNick") ?? "")
            vigil_chat_group(UInt16(truncatingIfNeeded: defaults.integer(forKey: "chatGroup")))
            if submitOutbox() { chatSession = session }
        } else if chatResubmit != 0, ProcessInfo.processInfo.systemUptime >= chatResubmit {
            chatResubmit = 0
            submitOutbox(only: chatWaiting)
        }

        let pending = receipts
        receipts.removeAll()

        for (chat, ref, kind, acked, waiting) in pending {
            receipt(chat, ref: ref, kind: kind, acked: acked, pending: waiting)
        }

        var buf = [CChar](repeating: 0, count: Int(VIGIL_CHAT_EVENTS_MAX))
        let len = vigil_chat_events(&buf, buf.count)
        let data = Data(bytes: buf, count: len)

        TunnelKeychain.wipe(&buf)

        guard len > 0,
              let events = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]],
              !events.isEmpty else { return }

        var unseen = defaults.dictionary(forKey: "chatUnseen") as? [String: Int] ?? [:]

        for event in events {
            guard let hex = event["peer"] as? String,
                  let peer = UInt8(hex, radix: 16), peer < 0xff,
                  let kind = event["type"] as? String else { continue }

            let group = (event["group"] as? String).flatMap { UInt16($0, radix: 16) }
            let chat = group.map { ChatVault.groupChat($0) } ?? hex

            guard group != nil || peer > 0 else { continue }

            do {
                switch kind {
                case "message":
                    let text = event["text"] as? String ?? ""
                    let nick = event["nick"] as? String ?? ""

                    try vault.deliver(chat: chat, peer: peer, nick: nick, text: text,
                                      time: Int64(Date().timeIntervalSince1970 * 1000))
                    vigil_chat_stored((event["token"] as? NSNumber)?.uint64Value ?? 0)

                    if defaults.string(forKey: "chatOpen") != chat {
                        unseen[chat, default: 0] += 1
                        notifyChat(chat, peer, nick, text, badge: unseen.values.reduce(0, +))
                    }
                case "joined", "left":
                    try vault.deliver(chat: chat, peer: peer, nick: event["nick"] as? String ?? "",
                                      text: "", time: Int64(Date().timeIntervalSince1970 * 1000),
                                      kind: kind)
                case "data":
                    let nick = event["nick"] as? String ?? ""
                    let name = event["name"] as? String ?? "file"
                    let media = event["kind"] as? String ?? "file"
                    let token = (event["token"] as? NSNumber)?.uint64Value ?? 0
                    let size = (event["size"] as? NSNumber)?.intValue ?? 0

                    guard size > 0, size <= Int(VIGIL_CHAT_DATA_MAX) else { continue }

                    var bytes = [UInt8](repeating: 0, count: size)
                    let taken = vigil_chat_data_take(token, &bytes, bytes.count)

                    guard taken > 0 else { continue }

                    try vault.deliver(chat: chat, peer: peer, nick: nick, text: "",
                                      time: Int64(Date().timeIntervalSince1970 * 1000),
                                      data: Data(bytes[0..<taken]), media: media, name: name)
                    vigil_chat_stored(token)
                    TunnelKeychain.wipe(&bytes)

                    if defaults.string(forKey: "chatOpen") != chat {
                        unseen[chat, default: 0] += 1
                        notifyChat(chat, peer, nick, Self.summary(media, name),
                                   badge: unseen.values.reduce(0, +))
                    }
                case "sent", "delivered":
                    receipt(chat, ref: (event["ref"] as? NSNumber)?.int64Value ?? 0, kind: kind)
                case "acks":
                    receipt(chat, ref: (event["ref"] as? NSNumber)?.int64Value ?? 0, kind: kind,
                            acked: (event["acked"] as? NSNumber)?.intValue ?? 0,
                            pending: (event["pending"] as? NSNumber)?.intValue ?? 0)
                default:
                    break
                }
            } catch {
                NSLog("vigil: failed to store a chat event: \(error)")
            }
        }

        defaults.set(unseen, forKey: "chatUnseen")
        defaults.set(defaults.integer(forKey: "chatVersion") + 1, forKey: "chatVersion")
    }

    private func attachGospel() {
        let on = ChatVault.defaults.object(forKey: "gospelEnabled") as? Bool ?? true

        guard on, var config = gospel.flatMap({ try? TunnelKeychain.load(ref: $0) }) else {
            _ = vigil_gospel(nil, 0)
            return
        }

        config.withUnsafeBytes { raw in
            _ = vigil_gospel(raw.baseAddress?.assumingMemoryBound(to: CChar.self), raw.count)
        }

        TunnelKeychain.wipe(&config)
    }

    private func receipt(_ chat: String, ref: Int64, kind: String, acked: Int = 0, pending: Int = 0) {
        do {
            try ChatVault.shared.receipt(chat, ref: ref, kind: kind, acked: acked, pending: pending)
        } catch {
            NSLog("vigil: failed to store a receipt, trying again: \(error)")
            receipts.append((chat, ref, kind, acked, pending))
        }
    }

    private func onMain(_ body: () -> Void) {
        if Thread.isMainThread {
            body()
        } else {
            DispatchQueue.main.sync(execute: body)
        }
    }

    @discardableResult
    private func submitOutbox(only: Set<UInt64>? = nil) -> Bool {
        guard let outbox = try? ChatVault.shared.outboxList() else {
            if only != nil {
                chatResubmit = ProcessInfo.processInfo.systemUptime + Self.resubmitInterval
            }
            return false
        }

        var refs = Set<UInt64>()

        for entry in outbox {
            guard let chat = entry["peer"] as? String,
                  let peer = UInt8(chat, radix: 16), peer > 0, peer < 0xff,
                  let ref = (entry["ref"] as? NSNumber)?.uint64Value else { continue }

            refs.insert(ref)

            if let only, !only.contains(ref) { continue }

            if let file = entry["data"] as? String {
                guard let data = try? ChatVault.shared.data(chat, file: file) else { return false }
                guard !data.isEmpty, data.count <= Int(VIGIL_CHAT_DATA_MAX) else { continue }

                let media = entry["media"] as? String ?? "file"
                let name = entry["name"] as? String ?? "file"

                let sent = data.withUnsafeBytes {
                    vigil_chat_data_send(peer, ref, ChatVault.kind(media), name, $0.baseAddress, data.count)
                }

                if sent == -1 {
                    chatWaiting.insert(ref)
                    if chatResubmit == 0 {
                        chatResubmit = ProcessInfo.processInfo.systemUptime + Self.resubmitInterval
                    }
                } else {
                    chatWaiting.remove(ref)
                }
                continue
            }

            guard let text = entry["text"] as? String else { continue }

            let bytes = Array(text.utf8).map { CChar(bitPattern: $0) }
            guard !bytes.isEmpty, bytes.count <= Int(VIGIL_CHAT_MESSAGE_MAX) else { continue }

            _ = vigil_chat_send(peer, ref, bytes, bytes.count)
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

    private func notifyChat(_ chat: String, _ peer: UInt8, _ nick: String, _ text: String, badge: Int) {
        let preview = ChatVault.defaults.object(forKey: "chatPreview") as? Bool ?? false
        let content = UNMutableNotificationContent()
        let body = preview ? text : "New message"
        let named = ChatVault.names()[ChatVault.hex(peer)] != nil
        let sender = named || nick.isEmpty ? ChatVault.name(peer) : "~\(nick)"

        content.title = ChatVault.group(chat) == nil ? sender : ChatVault.name(chat: chat)
        content.body = ChatVault.group(chat) == nil ? body : "\(sender): \(body)"
        content.sound = .default
        content.badge = NSNumber(value: badge)
        content.threadIdentifier = "chat-\(chat)"
        content.userInfo = ["chat": chat]

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

        guard let call = status["call"] as? [String: Any] else {
            callers.removeAll()
            return
        }

        let now = Set(call["incoming"] as? [String] ?? [])

        for peer in now.subtracting(callers) {
            let content = UNMutableNotificationContent()
            content.title = UInt8(peer, radix: 16).map { ChatVault.name($0) } ?? "Peer \(peer)"
            content.body = "Incoming call"
            content.sound = .default
            content.interruptionLevel = .timeSensitive
            content.categoryIdentifier = ChatVault.callCategory
            content.userInfo = ["call": peer]

            if #available(iOS 15.2, *) {
                content.sound = .defaultRingtone
            }

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

    private static func cathedral(_ voice: [UInt8]) -> String? {
        guard !voice.isEmpty else { return nil }

        var out = [CChar](repeating: 0, count: 4096)
        let outlen = out.count

        let ok = voice.withUnsafeBytes { raw in
            vigil_config_check(raw.baseAddress?.assumingMemoryBound(to: CChar.self),
                               raw.count, &out, outlen) == 0
        }

        guard ok, let summary = try? JSONSerialization.jsonObject(
            with: Data(String(cString: out).utf8)) as? [String: Any],
              let cathedral = summary["cathedral"] as? String else { return nil }

        return String(cathedral.split(separator: ":").first ?? "")
    }

    private func settings(_ summary: [String: Any], voice: String?) -> NEPacketTunnelNetworkSettings {
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

        if let voice {
            ipv4.excludedRoutes = [
                NEIPv4Route(destinationAddress: Self.resolve(voice), subnetMask: "255.255.255.255"),
            ]
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
