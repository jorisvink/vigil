// Answers the user interface on the se.sanctorum.vigil/core channel.
//
// Tunnels live in the shared keychain, their summaries in our user
// defaults. The packet tunnel provider gets the keychain reference
// when we connect and runs the core.

import AVFoundation
import Flutter
import Foundation
import ImageIO
import NetworkExtension
import Photos
import QuickLook
import UniformTypeIdentifiers
import UIKit
import UserNotifications
import Vigil

final class VigilPlugin: NSObject, FlutterPlugin, UIDocumentPickerDelegate,
                         QLPreviewControllerDataSource, QLPreviewControllerDelegate {
    private static let channel = "se.sanctorum.vigil/core"
    private static let provider = (Bundle.main.bundleIdentifier ?? "") + ".PacketTunnel"
    private static let tunnelsKey = "tunnels"
    private static let companionKey = "companion"

    static var requested: String?
    static var requestedTopic: String?

    private var version = 0
    private var avatars = 0
    private var renamed = 0
    private let reader = DispatchQueue(label: "se.sanctorum.vigil.chats")
    private let remote = DispatchQueue(label: "se.sanctorum.vigil.remote")
    private let resyncWork = DispatchQueue(label: "se.sanctorum.vigil.resync")
    private var resyncing = false
    private var safetyHalf: [UInt8]?
    private var safetyFor = ""
    private var opened: String?
    private var last: String?
    private var silent: TimeInterval?
    private var linking = false
    private var badged: Int?
    private var loaded: NETunnelProviderManager?

    private static let silence: TimeInterval = 10
    private var picking: (String, String, FlutterResult)?
    private var saving: (URL, FlutterResult)?
    private var preview: OpenItem?

    static func register(with registrar: FlutterPluginRegistrar) {
        let channel = FlutterMethodChannel(name: channel, binaryMessenger: registrar.messenger())
        let plugin = VigilPlugin()

        registrar.addMethodCallDelegate(plugin, channel: channel)

        VigilVideo.shared.registry = registrar.textures()
        VigilScanner.shared.registry = registrar.textures()
        VigilVideo.install()

        for name in [UIApplication.didEnterBackgroundNotification,
                     UIApplication.protectedDataWillBecomeUnavailableNotification] {
            NotificationCenter.default.addObserver(
                forName: name, object: nil, queue: .main
            ) { [weak plugin] _ in plugin?.background() }
        }

        for name in [Notification.Name.NEVPNConfigurationChange, .NEVPNStatusDidChange] {
            NotificationCenter.default.addObserver(
                forName: name, object: nil, queue: .main
            ) { [weak plugin] _ in plugin?.loaded = nil }
        }

        NotificationCenter.default.addObserver(
            forName: UIScene.willDeactivateNotification, object: nil, queue: .main
        ) { note in cover(note.object as? UIWindowScene, on: true) }

        NotificationCenter.default.addObserver(
            forName: UIScene.didActivateNotification, object: nil, queue: .main
        ) { note in cover(note.object as? UIWindowScene, on: false) }

        try? FileManager.default.removeItem(at: openFolder)
        plugin.gospels()
        try? ChatVault.shared.prepare()

        TunnelLink.open(create: false)
    }

    private static var openFolder: URL {
        FileManager.default.temporaryDirectory.appendingPathComponent("open")
    }

    private static func cover(_ scene: UIWindowScene?, on: Bool) {
        for window in scene?.windows ?? [] {
            let cover = window.viewWithTag(coverTag)

            if !on {
                cover?.removeFromSuperview()
                continue
            }

            guard cover == nil else { continue }

            let view = UIView(frame: window.bounds)
            view.tag = coverTag
            view.backgroundColor = UIColor(red: 0x15 / 255, green: 0x12 / 255, blue: 0x0f / 255, alpha: 1)
            view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            window.addSubview(view)
        }
    }

    private static let coverTag = 0x76676c

    private func background() {
        if var data = VigilVoice.shared.finish() {
            data.resetBytes(in: 0..<data.count)
        }

        VigilVoice.shared.stop()
        VigilScanner.shared.stop()
        ChatVault.shared.lock()

        if let chat = opened {
            if ChatVault.group(chat) != nil {
                VigilCall.shared.send?("chat-group-typing - 0")
            } else {
                VigilCall.shared.send?("chat-typing \(chat) 0 0")
            }

            opened = nil
        }

        ChatVault.defaults.removeObject(forKey: "chatOpen")
    }

    func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "list":
            result(json(tunnels().map { ["id": $0.id, "summary": $0.summary] }))
        case "check":
            var config = Self.bytes(call.arguments)
            defer { TunnelKeychain.wipe(&config) }

            guard Handoff.ours(config) else {
                result(check(config).1)
                return
            }

            let ok = Handoff.held(config) { held in
                result(check(held).1)
            }

            if !ok {
                result(json(["error": Handoff.gone]))
            }
        case "import":
            var config = Self.bytes(call.arguments)
            defer { TunnelKeychain.wipe(&config) }

            guard Handoff.ours(config) else {
                importTunnel(config, result: result)
                return
            }

            let ok = Handoff.held(config) { held in
                importTunnel(held, result: result)
            }

            if ok {
                Handoff.clear()
            } else {
                result(FlutterError(code: "invalid", message: Handoff.gone, details: nil))
            }
        case "handoff":
            result(FlutterStandardTypedData(bytes: Data(Handoff.token())))
        case "edit":
            let args = call.arguments as? [String: Any] ?? [:]
            edit(args["id"] as? String ?? "", edits: args["edits"] as? String ?? "", result: result)
        case "remove":
            remove(call.arguments as? String ?? "", result: result)
        case "companion":
            result(UserDefaults.standard.string(forKey: Self.companionKey) ?? "")
        case "connect":
            connect(call.arguments as? String ?? "", result: result)
        case "disconnect":
            disconnect(result: result)
        case "gospel":
            gospel(call.arguments as? Bool ?? true, result: result)
        case "status":
            status(result: result)
        case "call":
            let args = call.arguments as? [String: Any]
            let peer = args?["peer"] as? String ?? call.arguments as? String ?? ""
            let video = args?["video"] as? Bool ?? false

            withCamera(video, result: result) { granted in
                VigilCall.shared.dial(peer, video: granted)
            }
        case "answer":
            withCamera(call.arguments as? Bool ?? false, result: result) { granted in
                VigilCall.shared.answer(video: granted)
            }
        case "camera":
            let on = call.arguments as? Bool ?? false

            withCamera(on, result: result) { granted in
                VigilCall.shared.camera(granted)
            }
        case "flip":
            VigilVideo.shared.flip()
            result("")
        case "videoTextures":
            do {
                result(json(try VigilVideo.shared.textures()))
            } catch {
                result(FlutterError(code: "video", message: "There is no video.", details: nil))
            }
        case "videoState":
            result(json(VigilVideo.shared.state()))
        case "videoRelease":
            VigilVideo.shared.release()
            result("")
        case "scanStart":
            VigilScanner.shared.start(result: result)
        case "scanState":
            result(json(VigilScanner.shared.state()))
        case "scanStop":
            VigilScanner.shared.stop()
            result("")
        case "scanImage":
            do {
                result(try VigilScanner.shared.image(call.arguments as? String ?? ""))
            } catch {
                result(FlutterError(code: "invalid", message: "That image cannot be read.", details: nil))
            }
        case "decline", "hangup":
            VigilCall.shared.hangup()
            result("")
        case "mute":
            VigilCall.shared.mute(call.arguments as? Bool ?? false)
            result("")
        case "speaker":
            VigilCall.shared.speaker(call.arguments as? Bool ?? false)
            result("")
        case "names":
            if let raw = call.arguments as? String,
               let names = try? JSONSerialization.jsonObject(with: Data(raw.utf8)) as? [String: Any] {
                result(json(ChatVault.save(names)))
            } else {
                result(json(ChatVault.names()))
            }
        case "chatData":
            let args = call.arguments as? [String: Any] ?? [:]

            result(Self.reply {
                try ChatVault.shared.data(args["peer"] as? String ?? "", file: args["data"] as? String ?? "")
                    .map { FlutterStandardTypedData(bytes: $0) }
            })
        case "chatAvatar":
            let hex = call.arguments as? String ?? ""

            result(Self.reply {
                var data: Data?

                if hex == ChatVault.selfAuthor {
                    data = try ChatVault.shared.ownAvatar()
                } else if Flock.valid(hex) {
                    data = try ChatVault.shared.peerAvatar(hex)
                }

                return data.map { FlutterStandardTypedData(bytes: $0) }
            })
        case "chatGroups":
            result(Self.reply { json(try ChatVault.shared.groups()) })
        case "chatNames":
            result(Self.reply { json(try ChatVault.shared.peerNames()) })
        case "chatAvatarSet":
            result(Self.reply {
                let image = try (call.arguments as? FlutterStandardTypedData).map { try Avatar.jpeg($0.data) }

                try ChatVault.shared.ownAvatar(image)
                VigilCall.shared.send?("chat-avatar")
                avatars += 1
                return ""
            })
        case "chatDataOpen":
            let args = call.arguments as? [String: Any] ?? [:]

            result(Self.reply {
                try open(args["peer"] as? String ?? "", file: args["data"] as? String ?? "",
                         name: args["name"] as? String ?? "file")
                return ""
            })
        case "chatDataSave":
            let args = call.arguments as? [String: Any] ?? [:]

            do {
                if args["media"] as? String == "image" {
                    try savePhoto(args["peer"] as? String ?? "", file: args["data"] as? String ?? "",
                                  result: result)
                } else {
                    try save(args["peer"] as? String ?? "", file: args["data"] as? String ?? "",
                             name: args["name"] as? String ?? "file", result: result)
                }
            } catch let error as ChatVault.Locked {
                result(FlutterError(code: "locked", message: error.description, details: nil))
            } catch let error as ChatVault.Failure {
                result(FlutterError(code: "invalid", message: error.description, details: nil))
            } catch {
                result(FlutterError(code: "failed", message: "\(error)", details: nil))
            }
        case "chatFileSend":
            let args = call.arguments as? [String: Any] ?? [:]
            let hex = args["peer"] as? String ?? ""

            guard ChatVault.chat(hex) != nil, picking == nil, saving == nil, let top = Self.top(),
                  let topic = try? ChatVault.topicId(args["topic"]) else {
                result(FlutterError(code: "invalid", message: "Cannot pick a file now.", details: nil))
                return
            }

            let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.item], asCopy: true)
            picker.delegate = self
            picking = (hex, topic, result)
            top.present(picker, animated: true)
        case "voiceRecord", "voiceSend", "voiceCancel", "voicePlay", "voiceStop", "voiceSeek", "voiceSpeed",
             "voicePosition":
            voice(call, result: result)
        case "clipboardImage":
            result(UIPasteboard.general.hasImages ? "true" : "false")
        case "clipboardImageData":
            do {
                result(FlutterStandardTypedData(bytes: try Self.pasted()))
            } catch let error as ChatVault.Failure {
                result(FlutterError(code: "invalid", message: error.description, details: nil))
            } catch {
                result(FlutterError(code: "failed", message: "\(error)", details: nil))
            }
        case "ignored":
            result(Self.reply { json(try ChatVault.shared.ignored().sorted()) })
        case "reachable":
            result(Self.reply {
                if let on = call.arguments as? Bool {
                    try ChatVault.shared.reach(on)
                    VigilCall.shared.send?("chat-answer -")
                    version += 1
                }

                return ChatVault.reachable ? "true" : "false"
            })
        case "chats", "chat":
            reader.async {
                let reply = Self.reply { try self.chat(call) }
                DispatchQueue.main.async { result(reply) }
            }
        case "chatSend", "chatImageSend", "chatRead", "chatOpen", "chatTyping",
             "chatImageData", "chatDelete", "chatSettings", "chatFavorite",
             "chatPin", "chatMute", "chatTopic", "groupCreate", "groupEdit", "groupLeave",
             "chatStar", "chatCompact", "chatOrder", "chatRetract", "chatAccept", "chatIgnore":
            result(Self.reply { try chat(call) })
        case "groupVoiceOn":
            result(Self.reply {
                try GroupVoice.shared.on(call.arguments as? String ?? "")
                return ""
            })
        case "groupVoiceOff":
            result(Self.reply {
                try GroupVoice.shared.off()
                return ""
            })
        case "groupVoiceMute":
            result(Self.reply {
                try GroupVoice.shared.mute(call.arguments as? Bool == true)
                return ""
            })
        case "groupVoiceEarpiece":
            result(Self.reply {
                try GroupVoice.shared.earpiece(call.arguments as? Bool == true)
                return ""
            })
        case "groupVoiceKeys":
            result("")
        case "groupVoiceReady":
            result(vigil_channel_ready() == 1 ? "true" : "false")
        case "groupVoiceTalk":
            groupVoiceTalk(call.arguments as? Bool == true, result: result)
        case "safetyCode":
            result(Self.reply { try safetyCode(call) })
        case "safetyBegin":
            result(Self.reply { try safetyBegin(call) })
        case "safetyWords":
            result(Self.reply { try safetyWords(call) })
        case "safetyStop":
            safetyStop()
            result("")
        case "safetyCodes":
            result(Self.reply { try json(ChatVault.shared.safetyCodes()) })
        case "flag":
            let args = call.arguments as? [String: Any] ?? [:]
            let key = "ui.\(args["key"] as? String ?? "")"

            if let value = args["value"] as? Bool {
                UserDefaults.standard.set(value, forKey: key)
            }

            result(UserDefaults.standard.bool(forKey: key) ? "true" : "false")
        case "eraseAll":
            erase(result: result)
        case "remoteWordList":
            remote.async {
                var words: [String] = []
                var idx: UInt16 = 0

                while let word = vigil_remote_word(idx) {
                    words.append(String(cString: word))
                    idx += 1
                }

                DispatchQueue.main.async { result(words.joined(separator: " ")) }
            }
        case "remoteWords":
            var words = Self.bytes(call.arguments)
            words.append(0)
            defer { TunnelKeychain.wipe(&words) }

            var out = [CChar](repeating: 0, count: 256)
            let outlen = out.count

            let ok = words.withUnsafeBytes { raw in
                vigil_remote_words(raw.baseAddress?.assumingMemoryBound(to: CChar.self),
                                   &out, outlen) == 0
            }

            if ok {
                result(String(cString: out))
            } else {
                result("")
            }
        case "remoteStart":
            let args = call.arguments as? [String: Any] ?? [:]
            let id = args["id"] as? String ?? ""
            let secret = args["secret"] as? String ?? ""

            remote.async {
                let ok = vigil_remote_start(id, secret) == 0

                DispatchQueue.main.async {
                    if ok {
                        result("")
                    } else {
                        result(FlutterError(code: "invalid",
                                            message: "The tunnel to Covenant could not be set up.",
                                            details: nil))
                    }
                }
            }
        case "remoteRun":
            remote.async {
                let state = Int(vigil_remote_run())
                DispatchQueue.main.async { result(state) }
            }
        case "remoteConfig":
            var out = [CChar](repeating: 0, count: 4096)
            let outlen = out.count
            defer {
                for idx in out.indices {
                    out[idx] = 0
                }
            }

            if vigil_remote_config(&out, outlen) == 0 {
                var bytes = out.prefix(while: { $0 != 0 }).map { UInt8(bitPattern: $0) }
                defer { TunnelKeychain.wipe(&bytes) }
                result(FlutterStandardTypedData(bytes: Data(bytes)))
            } else {
                result(FlutterStandardTypedData(bytes: Data()))
            }
        case "remoteStop":
            remote.async {
                vigil_remote_stop()
                DispatchQueue.main.async { result("") }
            }
        case "version":
            result(String(cString: vigil_version()))
        case "kyrkaVersion":
            result(String(cString: kyrka_version()))
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    private struct Stored {
        let id: String
        let summary: [String: Any]
        let ref: Data
    }

    private func tunnels() -> [Stored] {
        let list = UserDefaults.standard.array(forKey: Self.tunnelsKey) as? [[String: Any]] ?? []

        return list.compactMap { entry in
            guard let id = entry["id"] as? String,
                  let summary = entry["summary"] as? String,
                  let ref = entry["ref"] as? Data,
                  var parsed = try? JSONSerialization.jsonObject(with: Data(summary.utf8))
                    as? [String: Any] else { return nil }
            if parsed["mode"] as? String == "voice" { parsed["mode"] = "gospel" }
            return Stored(id: id, summary: parsed, ref: ref)
        }
    }

    private func voiceTunnel() -> Stored? {
        let first = gospels().first?["id"] as? String

        return tunnels().first { $0.id == first }
    }

    @discardableResult
    private func gospels() -> [[String: Any]] {
        let before = ChatVault.defaults.array(forKey: "gospels") as? [[String: Any]] ?? []
        let current = tunnels().filter { $0.summary["mode"] as? String == "gospel" }
        let order = before.compactMap { $0["id"] as? String }
        let kept = order.compactMap { id in current.first { $0.id == id } }
        let added = current.reversed().filter { !order.contains($0.id) }
        let list: [[String: Any]] = (kept + added).map {
            ["id": $0.id, "ref": $0.ref, "flock": Flock.hex($0.summary["cathedral_flock"] as? String ?? "0")]
        }

        if !NSArray(array: list).isEqual(to: before) {
            ChatVault.defaults.set(list, forKey: "gospels")
        }

        return list
    }

    private func gospelChanged() {
        gospels()

        manager { manager in
            guard let manager, let session = manager.connection as? NETunnelProviderSession,
                  [.connected, .connecting, .reasserting].contains(session.status) else { return }

            let id = self.tunnelId(manager)

            if self.tunnels().contains(where: { $0.id == id }) {
                TunnelLink.request(session, "gospel") { _ in }
                return
            }

            guard let voice = self.voiceTunnel() else { return }

            self.connect(voice.id) { _ in }
        }
    }

    private func save(_ list: [[String: Any]]) {
        UserDefaults.standard.set(list, forKey: Self.tunnelsKey)
    }

    /// The caller must wipe the returned configuration.
    private static func bytes(_ arguments: Any?) -> [UInt8] {
        guard let typed = arguments as? FlutterStandardTypedData else { return [] }
        return [UInt8](typed.data)
    }

    private func check(_ config: [UInt8]) -> (Bool, String) {
        guard !config.isEmpty else { return (false, "{\"error\":\"no data\"}") }

        var out = [CChar](repeating: 0, count: 4096)
        let outlen = out.count

        let ok = config.withUnsafeBytes { raw in
            vigil_config_check(raw.baseAddress?.assumingMemoryBound(to: CChar.self),
                               raw.count, &out, outlen) == 0
        }

        return (ok, String(cString: out))
    }

    private func importTunnel(_ config: [UInt8], result: @escaping FlutterResult) {
        let (ok, summary) = check(config)

        guard ok else {
            let error = (try? JSONSerialization.jsonObject(with: Data(summary.utf8))
                as? [String: Any])?["error"] as? String
            result(FlutterError(code: "invalid", message: error ?? "invalid configuration", details: nil))
            return
        }

        let mode = (try? JSONSerialization.jsonObject(with: Data(summary.utf8))
            as? [String: Any])?["mode"] as? String

        let flock = Flock.hex((try? JSONSerialization.jsonObject(with: Data(summary.utf8))
            as? [String: Any])?["cathedral_flock"] as? String ?? "0")
        let present = gospels()

        if mode == "gospel" && present.contains(where: { $0["flock"] as? String == flock }) {
            result(FlutterError(code: "invalid",
                                message: "There is already a Gospel configuration for flock \(flock), " +
                                    "remove it first.",
                                details: nil))
            return
        }

        if mode == "gospel" && present.count >= Int(VIGIL_GOSPEL_MAX) {
            result(FlutterError(code: "invalid",
                                message: "There can be at most \(VIGIL_GOSPEL_MAX) Gospel configurations.",
                                details: nil))
            return
        }

        if mode == "companion" {
            importCompanion(config, summary: summary, result: result)
            return
        }

        let id = UUID().uuidString.lowercased()

        do {
            let ref = try TunnelKeychain.store(id: id, config: config)
            var list = UserDefaults.standard.array(forKey: Self.tunnelsKey) as? [[String: Any]] ?? []
            list.insert(["id": id, "summary": summary, "ref": ref], at: 0)
            save(list)

            if mode == "gospel" {
                gospelChanged()
            }

            let parsed = (try? JSONSerialization.jsonObject(with: Data(summary.utf8))) ?? [:]
            result(json(["id": id, "summary": parsed]))
        } catch {
            result(FlutterError(code: "failed", message: "\(error)", details: nil))
        }
    }

    private func edit(_ id: String, edits: String, result: @escaping FlutterResult) {
        guard let tunnel = tunnels().first(where: { $0.id == id }) else {
            result(FlutterError(code: "failed", message: "unknown tunnel", details: nil))
            return
        }

        var out = [CChar](repeating: 0, count: Int(VIGIL_CONFIG_MAX) + 1)
        var err = [CChar](repeating: 0, count: 128)
        defer { TunnelKeychain.wipe(&out) }

        var config: [UInt8]

        do {
            config = try TunnelKeychain.load(ref: tunnel.ref)
        } catch {
            result(FlutterError(code: "failed", message: "\(error)", details: nil))
            return
        }

        defer { TunnelKeychain.wipe(&config) }

        let outlen = out.count
        let errlen = err.count

        let ok = !config.isEmpty && config.withUnsafeBytes { raw in
            edits.withCString { ed in
                vigil_config_edit(raw.baseAddress?.assumingMemoryBound(to: CChar.self),
                                  raw.count, ed, strlen(ed),
                                  &out, outlen, &err, errlen) == 0
            }
        }

        guard ok else {
            let reason = err[0] == 0 ? "invalid configuration" : String(cString: err)
            result(FlutterError(code: "invalid", message: reason, details: nil))
            return
        }

        var edited = out.prefix(while: { $0 != 0 }).map { UInt8(bitPattern: $0) }
        defer { TunnelKeychain.wipe(&edited) }

        let (valid, summary) = check(edited)

        guard valid else {
            result(FlutterError(code: "invalid", message: "invalid configuration", details: nil))
            return
        }

        do {
            try TunnelKeychain.update(id: id, config: edited)
        } catch {
            result(FlutterError(code: "failed", message: "\(error)", details: nil))
            return
        }

        let list = (UserDefaults.standard.array(forKey: Self.tunnelsKey) as? [[String: Any]] ?? [])
            .map { entry -> [String: Any] in
                guard (entry["id"] as? String) == id else { return entry }
                var entry = entry
                entry["summary"] = summary
                return entry
            }
        save(list)
        gospels()

        let parsed = (try? JSONSerialization.jsonObject(with: Data(summary.utf8))) ?? [:]
        result(json(["id": id, "summary": parsed]))
    }

    private func importCompanion(_ config: [UInt8], summary: String, result: @escaping FlutterResult) {
        let parsed = (try? JSONSerialization.jsonObject(with: Data(summary.utf8)) as? [String: Any]) ?? [:]

        guard parsed["spi"] as? String == "0102" else {
            result(FlutterError(code: "invalid", message: "This configuration is for your desktop, open it there.",
                                details: nil))
            return
        }

        do {
            _ = TunnelKeychain.remove(id: "companion")
            _ = try TunnelKeychain.store(id: "companion", config: config)
            UserDefaults.standard.set(summary, forKey: Self.companionKey)
        } catch {
            result(FlutterError(code: "failed", message: "\(error)", details: nil))
            return
        }

        companionChanged()
        result(json(["id": "companion", "summary": parsed]))
    }

    private func companionChanged() {
        manager { manager in
            guard let session = manager?.connection as? NETunnelProviderSession,
                  [.connected, .connecting, .reasserting].contains(session.status) else { return }

            TunnelLink.request(session, "companion") { _ in }
        }
    }

    private func remove(_ id: String, result: @escaping FlutterResult) {
        if id == "companion" {
            _ = TunnelKeychain.remove(id: id)
            UserDefaults.standard.removeObject(forKey: Self.companionKey)
            companionChanged()
            result("")
            return
        }

        guard TunnelKeychain.remove(id: id) else {
            result(FlutterError(code: "failed", message: "The configuration could not be removed.", details: nil))
            return
        }

        let gospel = gospels().contains { $0["id"] as? String == id }
        let list = (UserDefaults.standard.array(forKey: Self.tunnelsKey) as? [[String: Any]] ?? [])
            .filter { ($0["id"] as? String) != id }
        save(list)

        manager { manager in
            guard let manager, self.tunnelId(manager) == id else {
                if gospel {
                    self.gospelChanged()
                }

                result("")
                return
            }

            if gospel, let voice = self.voiceTunnel() {
                self.gospels()
                self.connect(voice.id, result: result)
                return
            }

            self.gospels()

            manager.removeFromPreferences { error in
                DispatchQueue.main.async { self.loaded = nil }

                if let error {
                    result(FlutterError(code: "failed", message: "\(error.localizedDescription)", details: nil))
                } else {
                    result("")
                }
            }
        }
    }

    static func send(_ message: String, done: @escaping () -> Void) {
        NETunnelProviderManager.loadAllFromPreferences { managers, _ in
            let finish = { DispatchQueue.main.async(execute: done) }

            guard let session = managers?.first?.connection as? NETunnelProviderSession else {
                finish()
                return
            }

            TunnelLink.request(session, message) { _ in finish() }
        }
    }

    private func manager(_ done: @escaping (NETunnelProviderManager?) -> Void) {
        NETunnelProviderManager.loadAllFromPreferences { managers, _ in
            DispatchQueue.main.async { done(managers?.first) }
        }
    }

    private func current(_ done: @escaping (NETunnelProviderManager?) -> Void) {
        if let loaded {
            done(loaded)
            return
        }

        manager { manager in
            self.loaded = manager
            done(manager)
        }
    }

    private func tunnelId(_ manager: NETunnelProviderManager) -> String? {
        let proto = manager.protocolConfiguration as? NETunnelProviderProtocol
        return proto?.providerConfiguration?["id"] as? String
    }

    private func connect(_ id: String, result: @escaping FlutterResult) {
        guard let tunnel = tunnels().first(where: { $0.id == id }) else {
            result(FlutterError(code: "failed", message: "unknown tunnel", details: nil))
            return
        }

        if voiceTunnel() != nil {
            AVAudioSession.sharedInstance().requestRecordPermission { _ in }
            UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge]) { _, _ in }
        }

        manager { existing in
            let manager = existing ?? NETunnelProviderManager()
            let proto = NETunnelProviderProtocol()

            proto.providerBundleIdentifier = Self.provider
            proto.serverAddress = (tunnel.summary["peer"] ?? tunnel.summary["cathedral"]) as? String ?? "sanctum"
            proto.passwordReference = tunnel.ref
            proto.providerConfiguration = ["id": id]

            manager.localizedDescription = "Vigil: \(tunnel.summary["instance"] as? String ?? id)"
            manager.protocolConfiguration = proto
            manager.isEnabled = true
            manager.onDemandRules = Self.onDemandRules
            manager.isOnDemandEnabled = true

            manager.saveToPreferences { error in
                if let error {
                    DispatchQueue.main.async {
                        result(FlutterError(code: "failed", message: error.localizedDescription, details: nil))
                    }
                    return
                }

                // A freshly saved manager must be reloaded before it can start.
                manager.loadFromPreferences { _ in
                    DispatchQueue.main.async {
                        self.loaded = nil
                        self.whenStopped(manager.connection) { stopped in
                            do {
                                if stopped {
                                    TunnelLink.renew()
                                } else {
                                    TunnelLink.open(create: true)
                                }

                                try manager.connection.startVPNTunnel()
                                self.used(id)
                                result("")
                            } catch {
                                result(FlutterError(code: "failed", message: error.localizedDescription, details: nil))
                            }
                        }
                    }
                }
            }
        }
    }

    private func used(_ id: String) {
        var list = UserDefaults.standard.array(forKey: Self.tunnelsKey) as? [[String: Any]] ?? []

        if let at = list.firstIndex(where: { $0["id"] as? String == id }), at > 0 {
            list.insert(list.remove(at: at), at: 0)
            save(list)
        }
    }

    private static var onDemandRules: [NEOnDemandRule] {
        let rule = NEOnDemandRuleConnect()
        rule.interfaceTypeMatch = .any

        return [rule]
    }

    private func onDemand(_ manager: NETunnelProviderManager, _ on: Bool,
                          _ done: @escaping () -> Void) {
        guard manager.isOnDemandEnabled != on else {
            done()
            return
        }

        manager.isOnDemandEnabled = on

        manager.saveToPreferences { error in
            if let error {
                NSLog("vigil: failed to turn on demand off: \(error.localizedDescription)")
            }

            DispatchQueue.main.async {
                self.loaded = nil
                done()
            }
        }
    }

    private func whenStopped(_ connection: NEVPNConnection, _ done: @escaping (Bool) -> Void) {
        let stopped: (NEVPNStatus) -> Bool = { $0 == .disconnected || $0 == .invalid }

        if stopped(connection.status) {
            done(true)
            return
        }

        var observer: NSObjectProtocol?
        var finished = false

        let finish: (Bool) -> Void = { clean in
            guard !finished else { return }
            finished = true

            if let observer {
                NotificationCenter.default.removeObserver(observer)
            }

            done(clean)
        }

        observer = NotificationCenter.default.addObserver(
            forName: .NEVPNStatusDidChange, object: connection, queue: .main
        ) { _ in
            if stopped(connection.status) {
                finish(true)
            }
        }

        connection.stopVPNTunnel()
        DispatchQueue.main.asyncAfter(deadline: .now() + 10) { finish(stopped(connection.status)) }
    }

    private func disconnect(result: @escaping FlutterResult) {
        manager { manager in
            let id = manager.flatMap { self.tunnelId($0) }

            if Self.gospelEnabled, let voice = self.voiceTunnel(), let id, id != voice.id,
               let status = manager?.connection.status, status != .disconnected, status != .invalid {
                self.connect(voice.id, result: result)
                return
            }

            guard let manager else {
                result("")
                return
            }

            self.onDemand(manager, false) {
                manager.connection.stopVPNTunnel()
                result("")
            }
        }
    }

    private func gospel(_ on: Bool, result: @escaping FlutterResult) {
        ChatVault.defaults.set(on, forKey: "gospelEnabled")

        guard let voice = voiceTunnel() else {
            result("")
            return
        }

        manager { manager in
            guard let manager, let session = manager.connection as? NETunnelProviderSession,
                  [.connected, .connecting, .reasserting].contains(session.status) else {
                if on {
                    self.connect(voice.id, result: result)
                } else {
                    result("")
                }
                return
            }

            if self.tunnelId(manager) == voice.id {
                if !on {
                    self.onDemand(manager, false) { session.stopVPNTunnel() }
                }
                result("")
                return
            }

            TunnelLink.request(session, "gospel") { _ in }
            result("")
        }
    }

    private func voice(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        let args = call.arguments as? [String: Any] ?? [:]
        let hex = (call.arguments as? String) ?? (args["peer"] as? String) ?? ""
        let voice = VigilVoice.shared

        do {
            switch call.method {
            case "voiceRecord":
                guard ChatVault.chat(hex) != nil else {
                    throw ChatVault.Failure(description: "There is no chat \(hex).")
                }

                let session = AVAudioSession.sharedInstance()

                if session.recordPermission == .undetermined {
                    session.requestRecordPermission { _ in }
                    throw ChatVault.Failure(description: "Allow the microphone, then hold to record.")
                }

                guard session.recordPermission == .granted else {
                    throw ChatVault.Failure(description: "Voice messages need access to the microphone.")
                }

                try voice.record()
                result("")
            case "voiceSend":
                guard var data = voice.finish() else {
                    throw ChatVault.Failure(description: "Nothing was recorded.")
                }

                defer { data.resetBytes(in: 0..<data.count) }

                let topic = try ChatVault.topicId(args["topic"])

                result(try dataSend(hex, media: "voice", name: VigilVoice.name(), data: data, topic: topic))
            case "voiceCancel":
                if var data = voice.finish() {
                    data.resetBytes(in: 0..<data.count)
                }
                result("")
            case "voicePlay":
                guard let data = try ChatVault.shared.data(hex, file: args["data"] as? String ?? "") else {
                    throw ChatVault.Failure(description: "That voice message is gone.")
                }

                let at = TimeInterval(args["at"] as? Int ?? 0) / 1000
                let rate = Float(args["rate"] as? Double ?? 1)

                try voice.play(data, at: at, rate: rate) { result("") }
            case "voiceSeek":
                voice.seek(TimeInterval(call.arguments as? Int ?? 0) / 1000)
                result("")
            case "voiceSpeed":
                voice.speed(Float(call.arguments as? Double ?? 1))
                result("")
            case "voicePosition":
                result(voice.position())
            default:
                voice.stop()
                result("")
            }
        } catch let error as ChatVault.Locked {
            result(FlutterError(code: "locked", message: error.description, details: nil))
        } catch let error as ChatVault.Failure {
            result(FlutterError(code: "invalid", message: error.description, details: nil))
        } catch {
            result(FlutterError(code: "failed", message: "\(error)", details: nil))
        }
    }

    private func dataSend(_ hex: String, media: String, name: String, data: Data,
                          topic: String = "") throws -> String {
        guard !data.isEmpty, data.count <= Int(VIGIL_CHAT_DATA_MAX) else {
            throw ChatVault.Failure(description: "A file is at most 4 MB.")
        }

        let (ref, _) = try ChatVault.shared.sendData(hex, media: media, name: name, data: data, topic: topic,
                                                      local: try Self.local(hex))

        VigilCall.shared.send?("chat")
        version += 1

        return "\(ref)"
    }

    private static func selves() -> [String: String] {
        ChatVault.defaults.dictionary(forKey: "chatSelves") as? [String: String] ?? [:]
    }

    private static func local(_ chat: String) throws -> String {
        guard let local = selves()[Flock.of(chat)], !local.isEmpty else {
            throw ChatVault.Failure(description: "Gospel is not running for flock \(Flock.of(chat)).")
        }

        return local
    }

    private func groupCreate(_ args: [String: Any]) throws -> String {
        let flock = Flock.hex(args["flock"] as? String ?? "0")
        let local = try Self.local("\(flock)-")
        let chat = try ChatVault.shared.groupCreate(flock: flock, name: args["name"] as? String ?? "",
            members: ChatVault.picked(args["members"], flock: flock, without: local) ?? [], local: local)

        changed()
        return chat
    }

    private func groupEdit(_ chat: String, _ args: [String: Any]) throws {
        let local = try Self.local(chat)

        Self.drop(try ChatVault.shared.groupEdit(chat, name: args["name"] as? String,
            members: ChatVault.picked(args["members"], flock: Flock.of(chat), without: local), local: local))
        changed()
    }

    private func groupLeave(_ chat: String) throws {
        Self.drop(try ChatVault.shared.groupLeave(chat, local: try Self.local(chat)))
        changed()
    }

    private func changed() {
        VigilCall.shared.send?("chat")
        renamed += 1
        version += 1
    }

    private static func drop(_ dropped: [(String, Int64)]) {
        for (peer, ref) in dropped {
            VigilCall.shared.send?("chat-drop \(peer) " + String(format: "%016llx", UInt64(bitPattern: ref)))
        }
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        if let (copy, result) = saving {
            saving = nil
            try? FileManager.default.removeItem(at: copy.deletingLastPathComponent())
            result("saved")
            return
        }

        guard let (hex, topic, result) = picking else { return }
        picking = nil

        guard let url = urls.first else {
            result(nil)
            return
        }

        defer { try? FileManager.default.removeItem(at: url) }

        do {
            let size = (try url.resourceValues(forKeys: [.fileSizeKey])).fileSize ?? 0

            guard size <= Int(VIGIL_CHAT_DATA_MAX) else {
                throw ChatVault.Failure(description: "A file is at most 4 MB.")
            }

            let data = try Data(contentsOf: url)
            result(try dataSend(hex, media: "file", name: Self.fileName(url.lastPathComponent), data: data,
                                topic: topic))
        } catch let error as ChatVault.Failure {
            result(FlutterError(code: "invalid", message: error.description, details: nil))
        } catch let error as ChatVault.Locked {
            result(FlutterError(code: "locked", message: error.description, details: nil))
        } catch {
            result(FlutterError(code: "failed", message: "\(error)", details: nil))
        }
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        if let (copy, result) = saving {
            saving = nil
            try? FileManager.default.removeItem(at: copy.deletingLastPathComponent())
            result("")
            return
        }

        picking?.2(nil)
        picking = nil
    }

    private func open(_ hex: String, file: String, name: String) throws {
        try ChatVault.shared.merge()

        guard let data = try ChatVault.shared.data(hex, file: file) else {
            throw ChatVault.Failure(description: "That file is gone.")
        }

        let folder = Self.openFolder
        try? FileManager.default.removeItem(at: folder)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)

        let ext = (name as NSString).pathExtension
        var copy = folder.appendingPathComponent(UUID().uuidString)
        if !ext.isEmpty { copy.appendPathExtension(ext) }

        try data.write(to: copy, options: [.atomic, .completeFileProtection])

        guard let top = Self.top() else { throw ChatVault.Failure(description: "Cannot open a file now.") }

        preview = OpenItem(url: copy, title: name)

        let controller = QLPreviewController()
        controller.dataSource = self
        controller.delegate = self
        top.present(controller, animated: true)
    }

    private func savePhoto(_ hex: String, file: String, result: @escaping FlutterResult) throws {
        try ChatVault.shared.merge()

        guard let data = try ChatVault.shared.data(hex, file: file) else {
            throw ChatVault.Failure(description: "That image is gone.")
        }

        PHPhotoLibrary.requestAuthorization(for: .addOnly) { status in
            guard status == .authorized || status == .limited else {
                DispatchQueue.main.async {
                    result(FlutterError(code: "invalid",
                                        message: "Vigil may not add photos, allow it in Settings.",
                                        details: nil))
                }
                return
            }

            PHPhotoLibrary.shared().performChanges({
                PHAssetCreationRequest.forAsset().addResource(with: .photo, data: data, options: nil)
            }) { saved, error in
                DispatchQueue.main.async {
                    if saved {
                        result("saved")
                    } else {
                        result(FlutterError(code: "failed",
                                            message: error?.localizedDescription ?? "Could not save the photo.",
                                            details: nil))
                    }
                }
            }
        }
    }

    private func save(_ hex: String, file: String, name: String, result: @escaping FlutterResult) throws {
        guard picking == nil, saving == nil, let top = Self.top() else {
            throw ChatVault.Failure(description: "Cannot save a file now.")
        }

        try ChatVault.shared.merge()

        guard let data = try ChatVault.shared.data(hex, file: file) else {
            throw ChatVault.Failure(description: "That file is gone.")
        }

        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)

        let copy = folder.appendingPathComponent(Self.fileName(name))

        do {
            try data.write(to: copy, options: [.atomic, .completeFileProtection])
        } catch {
            try? FileManager.default.removeItem(at: folder)
            throw error
        }

        let picker = UIDocumentPickerViewController(forExporting: [copy], asCopy: true)
        picker.delegate = self
        saving = (copy, result)
        top.present(picker, animated: true)
    }

    func numberOfPreviewItems(in controller: QLPreviewController) -> Int { preview == nil ? 0 : 1 }

    func previewController(_ controller: QLPreviewController, previewItemAt index: Int) -> QLPreviewItem {
        preview ?? OpenItem(url: FileManager.default.temporaryDirectory, title: "")
    }

    func previewControllerDidDismiss(_ controller: QLPreviewController) {
        preview = nil
        try? FileManager.default.removeItem(at: Self.openFolder)
    }

    private static func top() -> UIViewController? {
        let window = UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .flatMap { $0.windows }
            .first { $0.isKeyWindow }
        var top = window?.rootViewController

        while let next = top?.presentedViewController { top = next }

        return top
    }

    private static func photoName() -> String {
        let format = DateFormatter()
        format.dateFormat = "yyyyMMdd-HHmmss"
        return "photo-\(format.string(from: Date())).jpg"
    }

    private static func fileName(_ name: String) -> String {
        var clean = String(name.unicodeScalars.filter {
            $0.value >= 0x20 && $0.value != 0x7f && $0 != "/" && $0 != "\\"
        })

        while clean.hasPrefix(".") { clean.removeFirst() }
        if clean.isEmpty { clean = "file" }

        while clean.utf8.count > Int(VIGIL_CHAT_DATA_NAME_MAX) { clean.removeLast() }

        return clean
    }

    private static func jpeg(_ path: String) throws -> Data {
        defer {
            if path.hasPrefix(NSTemporaryDirectory()) {
                try? FileManager.default.removeItem(atPath: path)
            }
        }

        return try jpeg(CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil))
    }

    private static func pasted() throws -> Data {
        let board = UIPasteboard.general

        guard let type = board.types.first(where: { UTType($0)?.conforms(to: .image) ?? false }),
              let data = board.data(forPasteboardType: type) else {
            throw ChatVault.Failure(description: "There is no image to paste.")
        }

        return try jpeg(CGImageSourceCreateWithData(data as CFData, nil))
    }

    private static func jpeg(_ source: CGImageSource?) throws -> Data {
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: 2048,
        ]

        guard let source,
              let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary),
              let jpeg = UIImage(cgImage: image).jpegData(compressionQuality: 0.85) else {
            throw ChatVault.Failure(description: "That is not an image.")
        }

        guard jpeg.count <= Int(VIGIL_CHAT_DATA_MAX) else {
            throw ChatVault.Failure(description: "That image is too large to send.")
        }

        return jpeg
    }

    static var gospelEnabled: Bool {
        ChatVault.defaults.object(forKey: "gospelEnabled") as? Bool ?? true
    }

    private func status(result: @escaping FlutterResult) {
        if !resyncing {
            resyncing = true
            resyncWork.async {
                ChatVault.shared.resyncStep()
                DispatchQueue.main.async { self.resyncing = false }
            }
        }

        current { manager in
            guard let manager,
                  let session = manager.connection as? NETunnelProviderSession,
                  [.connected, .connecting, .reasserting].contains(session.status) else {
                CandleActivity.update(since: nil)
                VigilCall.shared.update(nil)
                GroupVoice.shared.poll()
                VigilCall.shared.send = nil
                self.last = nil
                self.silent = nil
                self.linking = false
                result("{\"tunnel\":null,\"status\":{\"running\":false},\"chats\":\(self.chats())," +
                       "\"gospel\":\(Self.gospelEnabled)}")
                return
            }

            if let since = session.connectedDate {
                CandleActivity.update(since: since)
            }

            let id = self.tunnelId(manager)
            let refs = Dictionary(self.gospels().compactMap { entry -> (String, Data)? in
                guard let flock = entry["flock"] as? String, let ref = entry["ref"] as? Data else { return nil }
                return (flock, ref)
            }, uniquingKeysWith: { first, _ in first })

            VigilCall.shared.config = { flock in refs[flock].flatMap { try? TunnelKeychain.load(ref: $0) } }

            VigilCall.shared.send = { message in
                TunnelLink.request(session, message) { reply in
                    guard reply == nil else { return }
                    DispatchQueue.main.async { self.unsent(message) }
                }
            }

            TunnelLink.request(session, "status") { response in
                DispatchQueue.main.async {
                    let tunnel = id.map { "\"\($0)\"" } ?? "null"

                    let now = ProcessInfo.processInfo.systemUptime

                    guard let response,
                          let status = String(data: response, encoding: .utf8) else {
                        let since = self.silent ?? now
                        self.silent = since

                        let linked = now - since < Self.silence

                        if !TunnelLink.ready {
                            self.linking = true
                        }

                        result(self.combined(tunnel, self.last ?? "{\"running\":true}",
                                             linked: linked, linking: linked && self.linking))
                        return
                    }

                    self.last = status
                    self.silent = nil
                    self.linking = false

                    VigilCall.shared.update(
                        (try? JSONSerialization.jsonObject(with: response)) as? [String: Any])
                    GroupVoice.shared.poll()

                    result(self.combined(tunnel, status, linked: true, linking: false))
                }
            }
        }
    }

    private func unsent(_ message: String) {
        if message.split(separator: " ").first == "chat" {
            NSLog("vigil: the tunnel did not hear about our outbox")
        }
    }

    private func combined(_ tunnel: String, _ status: String, linked: Bool, linking: Bool) -> String {
        let phone = VigilCall.shared.json().map { json($0) } ?? "null"

        return "{\"tunnel\":\(tunnel),\"status\":\(status)," +
            "\"voice\":\(voice()),\"phone\":\(phone)," +
            "\"chats\":\(chats()),\"gospel\":\(Self.gospelEnabled)," +
            "\"groupVoice\":\(json(GroupVoice.shared.json()))," +
            "\"link\":\(linked),\"linking\":\(linking)}"
    }

    private func groupVoiceTalk(_ on: Bool, result: @escaping FlutterResult) {
        let session = AVAudioSession.sharedInstance()

        guard on else {
            GroupVoice.shared.talk(false)
            result("")
            return
        }

        if session.recordPermission == .undetermined {
            session.requestRecordPermission { _ in }
            result(FlutterError(code: "invalid", message: "Allow the microphone, then hold to talk.",
                                details: nil))
            return
        }

        guard session.recordPermission == .granted else {
            result(FlutterError(code: "invalid", message: "Talking needs access to the microphone.",
                                details: nil))
            return
        }

        GroupVoice.shared.talk(true)
        result("")
    }

    private func safetyCode(_ call: FlutterMethodCall) throws -> String {
        let vault = ChatVault.shared
        let args = call.arguments as? [String: Any] ?? [:]
        let hex = args["peer"] as? String ?? ""
        let clear = args["clear"] as? Bool == true
        let on = args["on"] as? Bool
        let peer = try Self.safetyPeer(hex)

        if let on {
            ChatVault.code(hex, on: on)
        }

        if clear {
            try vault.safetyCode(peer, nil)
        }

        var stored = try vault.safetyCode(peer) ?? Data()
        var out = [CChar](repeating: 0, count: Int(VIGIL_SAFETY_CODE_LEN))
        var code = ""

        if stored.count == Int(VIGIL_ADDITIONAL_INPUT_LEN) {
            let ret = stored.withUnsafeBytes { raw in
                vigil_safety_code(raw.baseAddress?.assumingMemoryBound(to: UInt8.self), raw.count,
                                  &out, out.count)
            }

            if ret == 0 {
                code = String(cString: out)
            }
        }

        stored.resetBytes(in: 0..<stored.count)

        if on != nil || clear {
            ChatVault.safetyApply(peer)
            VigilCall.shared.send?("chat-safety \(hex)")
        }

        return json(["set": !code.isEmpty, "code": code, "since": try vault.safetyTime(peer),
                     "on": ChatVault.codes().contains(hex)])
    }

    private func safetyBegin(_ call: FlutterMethodCall) throws -> String {
        let args = call.arguments as? [String: Any] ?? [:]
        let peer = try Self.safetyPeer(args["peer"] as? String ?? "")
        var half = [UInt8](repeating: 0, count: Int(VIGIL_SAFETY_HALF_LEN))
        var words = [CChar](repeating: 0, count: Int(VIGIL_SAFETY_WORDS) * 10 + 1)
        defer { TunnelKeychain.wipe(&words) }

        guard SecRandomCopyBytes(kSecRandomDefault, half.count, &half) == errSecSuccess else {
            throw ChatVault.Failure(description: "No random bytes for the safety code.")
        }

        safetyStop()

        guard vigil_safety_words(half, half.count, &words, words.count) == 0 else {
            TunnelKeychain.wipe(&half)
            throw ChatVault.Failure(description: "No words for our half.")
        }

        safetyHalf = half
        safetyFor = peer

        return String(cString: words)
    }

    private func safetyWords(_ call: FlutterMethodCall) throws -> String {
        let args = call.arguments as? [String: Any] ?? [:]
        let hex = args["peer"] as? String ?? ""
        let peer = try Self.safetyPeer(hex)
        var words = Self.bytes(args["words"])
        var theirs = [UInt8](repeating: 0, count: Int(VIGIL_SAFETY_HALF_LEN))
        words.append(0)
        defer {
            TunnelKeychain.wipe(&words)
            TunnelKeychain.wipe(&theirs)
        }

        guard let own = safetyHalf, safetyFor == peer else {
            throw ChatVault.Failure(description: "Open the safety code again.")
        }

        let parsed = words.withUnsafeBytes { raw in
            vigil_safety_half(raw.baseAddress?.assumingMemoryBound(to: CChar.self), &theirs, theirs.count)
        }

        guard parsed == 0 else {
            throw ChatVault.Failure(description: "Those are not \(VIGIL_SAFETY_WORDS) words from the list.")
        }

        guard own != theirs else {
            throw ChatVault.Failure(description: "Those are your own words.")
        }

        let mine = own.map { String(format: "%02x", $0) }.joined()
        let other = theirs.map { String(format: "%02x", $0) }.joined()

        guard let send = VigilCall.shared.send else {
            throw ChatVault.Failure(description: "Gospel is not running.")
        }

        send("chat-safety-start \(hex) \(mine) \(other)")

        return ""
    }

    private func safetyStop() {
        VigilCall.shared.send?("chat-safety-stop")

        if safetyHalf != nil {
            TunnelKeychain.wipe(&safetyHalf!)
        }

        safetyHalf = nil
        safetyFor = ""
    }

    private static func asking() -> Set<String> {
        return Set(ChatVault.defaults.stringArray(forKey: "chatAsking") ?? [])
    }

    private static func requests(_ listed: Set<String>) -> [[String: Any]] {
        return asking().subtracting(listed).sorted().map {
            ["peer": $0, "unread": 0, "last": NSNull(), "topics": [Any](), "request": true]
        }
    }

    private static func safetyPeer(_ hex: String) throws -> String {
        guard Flock.valid(hex) else {
            throw ChatVault.Failure(description: "There is no peer \(hex).")
        }

        return hex
    }

    private func chat(_ call: FlutterMethodCall) throws -> String {
        let vault = ChatVault.shared
        let defaults = ChatVault.defaults
        let args = call.arguments as? [String: Any] ?? [:]
        let hex = (call.arguments as? String) ?? (args["peer"] as? String) ?? ""
        let group = ChatVault.group(hex)

        if call.method == "groupCreate" {
            return try groupCreate(args)
        }

        if call.method == "chatOrder" {
            let chats = call.arguments as? [String] ?? []

            guard chats.allSatisfy({ ChatVault.chat($0) != nil }) else {
                throw ChatVault.Failure(description: "There is no such chat to order.")
            }

            ChatVault.order(chats)
            version += 1
            return ""
        }

        guard call.method == "chats" || call.method == "chatSettings" ||
              ChatVault.chat(hex) != nil else {
            throw ChatVault.Failure(description: "There is no chat \(hex).")
        }

        switch call.method {
        case "chats":
            let before = ChatVault.shared.unseen()

            try vault.merge()

            for (flock, local) in Self.selves() {
                try vault.topicsFill(flock, local: local)
            }

            var list = try vault.conversations()
            let listed = Set(list.compactMap { $0["peer"] as? String })

            list += Self.requests(listed)

            let known = Set(list.compactMap { $0["peer"] as? String })

            for chat in before.keys where !known.contains(chat) {
                ChatVault.seen(chat)
            }

            return json(ChatVault.arrange(list))
        case "chat":
            let epoch = (args["epoch"] as? NSNumber)?.int64Value ?? 0
            let stamp = (args["stamp"] as? NSNumber)?.int64Value ?? 0

            try vault.merge()

            return json(try vault.messages(hex, epoch: epoch, stamp: stamp))
        case "chatSend":
            let text = args["text"] as? String ?? ""
            let topic = try ChatVault.topicId(args["topic"])
            let size = text.utf8.count
            var max = Int(VIGIL_CHAT_MESSAGE_MAX)
            var quote: ChatVault.Quote?

            if let reply = (args["reply"] as? NSNumber)?.int64Value, reply != 0 {
                quote = try vault.quoted(hex, ref: reply)
                max = Int(VIGIL_CHAT_REPLY_MAX)
            }

            if !topic.isEmpty {
                max = Int(VIGIL_CHAT_TOPIC_MAX)
            }

            if group != nil {
                max = Int(quote == nil ? VIGIL_CHAT_GROUP_MAX : VIGIL_CHAT_GROUP_REPLY_MAX)
            }

            if group != nil, !topic.isEmpty {
                max = Int(VIGIL_CHAT_GROUP_TOPIC_MAX)
            }

            guard size > 0, size <= max else {
                throw ChatVault.Failure(description: "A message is at most \(max) bytes.")
            }

            let local = try group == nil ? "" : Self.local(hex)
            let ref = try vault.send(hex, text: text, quote: quote, topic: topic, local: local)

            VigilCall.shared.send?("chat")
            version += 1

            return "\(ref)"
        case "chatImageSend":
            let topic = try ChatVault.topicId(args["topic"])
            let image = try Self.jpeg(args["path"] as? String ?? "")
            return try dataSend(hex, media: "image", name: Self.photoName(), data: image, topic: topic)
        case "chatImageData":
            let topic = try ChatVault.topicId(args["topic"])

            guard let image = (args["data"] as? FlutterStandardTypedData)?.data else {
                throw ChatVault.Failure(description: "That is not an image.")
            }

            return try dataSend(hex, media: "image", name: Self.photoName(), data: image, topic: topic)
        case "chatRetract":
            guard let ref = (args["ref"] as? NSNumber)?.int64Value else {
                throw ChatVault.Failure(description: "There is no such image.")
            }

            let local = try group == nil ? "" : Self.local(hex)

            for (peer, dropped) in try vault.retract(hex, ref: ref, local: local) {
                VigilCall.shared.send?("chat-drop \(peer) " + String(format: "%016llx", UInt64(bitPattern: dropped)))
            }

            VigilCall.shared.send?("chat")
            version += 1
            return ""
        case "chatTopic":
            let local = try group == nil ? "" : Self.local(hex)
            let topic = try vault.topicSet(hex, topic: args["topic"], name: args["name"] as? String ?? "",
                                           color: (args["color"] as? NSNumber)?.intValue ?? 0, local: local)

            VigilCall.shared.send?("chat")
            version += 1

            return topic
        case "chatRead":
            try vault.read(hex, topic: try ChatVault.topicId(args["topic"]))
            ChatVault.seen(hex)
            version += 1
            return ""
        case "chatFavorite":
            guard let ref = (args["ref"] as? NSNumber)?.int64Value else {
                throw ChatVault.Failure(description: "There is no such message.")
            }

            try vault.favorite(hex, ref: ref, on: args["on"] as? Bool ?? false)
            version += 1
            return ""
        case "chatPin":
            guard let ref = (args["ref"] as? NSNumber)?.int64Value else {
                throw ChatVault.Failure(description: "There is no such message.")
            }

            try vault.pin(hex, ref: ref, on: args["on"] as? Bool ?? false)
            version += 1
            return ""
        case "groupEdit":
            try groupEdit(hex, args)
            return ""
        case "groupLeave":
            try groupLeave(hex)
            return ""
        case "chatOpen":
            let on = args["on"] as? Bool ?? false

            opened = on ? hex : nil

            if on {
                defaults.set(hex, forKey: "chatOpen")
                ChatVault.seen(hex)
            } else {
                defaults.removeObject(forKey: "chatOpen")
            }

            return ""
        case "chatTyping":
            let on = args["on"] as? Bool == true ? 1 : 0
            let topic = try ChatVault.topicId(args["topic"])
            let inside = topic.isEmpty ? "0" : topic

            if let group {
                if on == 1 {
                    VigilCall.shared.send?("chat-group-typing \(hex) \(inside)")
                } else {
                    VigilCall.shared.send?("chat-group-typing - 0")
                }
            } else {
                VigilCall.shared.send?("chat-typing \(hex) \(on) \(inside)")
            }
            return ""
        case "chatMute":
            if let until = (args["until"] as? NSNumber)?.intValue {
                ChatVault.mute(hex, until: until)
            }

            return "\(ChatVault.muted(hex))"
        case "chatStar":
            ChatVault.star(hex, on: args["on"] as? Bool ?? false)
            version += 1
            return ""
        case "chatCompact":
            ChatVault.compact(hex, on: args["on"] as? Bool ?? false)
            version += 1
            return ""
        case "chatAccept":
            guard group == nil else {
                throw ChatVault.Failure(description: "A group is not a request.")
            }

            if args["on"] as? Bool == true {
                try vault.accept(hex, true)
            }

            VigilCall.shared.send?("chat-answer \(hex)")
            ChatVault.seen(hex)
            version += 1
            return ""
        case "chatIgnore":
            guard group == nil else {
                throw ChatVault.Failure(description: "A group can not be ignored.")
            }

            try vault.ignore(hex, args["on"] as? Bool == true)
            VigilCall.shared.send?("chat-answer \(hex)")
            ChatVault.seen(hex)
            version += 1
            return ""
        case "chatDelete":
            if group == nil, Self.asking().contains(hex) {
                VigilCall.shared.send?("chat-answer \(hex)")
                ChatVault.seen(hex)
                version += 1
                return ""
            }

            let local = try group == nil ? "" : Self.local(hex)

            Self.drop(try vault.deleteChat(hex, local: local))

            if group == nil {
                VigilCall.shared.send?("chat-forget \(hex)")
                version += 1
            } else {
                changed()
            }

            return ""
        default:
            if let preview = args["preview"] as? Bool {
                defaults.set(preview, forKey: "chatPreview")
            }

            return json([
                "preview": defaults.object(forKey: "chatPreview") as? Bool ?? false,
            ])
        }
    }

    private func badge(_ count: Int) {
        guard count != badged else { return }

        badged = count

        UNUserNotificationCenter.current().setBadgeCount(count)
    }

    private func chats() -> String {
        let defaults = ChatVault.defaults
        let unseen = ChatVault.shared.unseen()
        let open = Self.requested
        let topic = Self.requestedTopic
        Self.requested = nil
        Self.requestedTopic = nil

        badge(unseen.values.reduce(0, +))

        return json([
            "version": defaults.integer(forKey: "chatVersion") + version,
            "avatars": defaults.integer(forKey: "chatAvatars") + avatars,
            "names": defaults.integer(forKey: "chatNames") + renamed,
            "unseen": unseen.values.reduce(0, +),
            "hardware": ChatVault.shared.hardware,
            "open": open ?? NSNull(),
            "openTopic": topic ?? NSNull(),
        ] as [String: Any])
    }

    private func withCamera(_ wanted: Bool, result: @escaping FlutterResult, run: @escaping (Bool) -> Void) {
        guard wanted else {
            run(false)
            result("")
            return
        }

        AVCaptureDevice.requestAccess(for: .video) { granted in
            DispatchQueue.main.async {
                run(granted)

                if granted {
                    result("")
                } else {
                    result(FlutterError(code: "denied", message: "Video calls need access to the camera.",
                                        details: nil))
                }
            }
        }
    }

    private func erase(result: @escaping FlutterResult) {
        if var data = VigilVoice.shared.finish() {
            data.resetBytes(in: 0..<data.count)
        }

        VigilVoice.shared.stop()
        VigilScanner.shared.stop()
        VigilCall.shared.hangup()
        CandleActivity.update(since: nil)

        NETunnelProviderManager.loadAllFromPreferences { managers, _ in
            let removed = DispatchGroup()

            for manager in managers ?? [] {
                manager.connection.stopVPNTunnel()
                removed.enter()
                manager.removeFromPreferences { _ in removed.leave() }
            }

            removed.notify(queue: .main) {
                self.loaded = nil
                ChatVault.shared.lock()
                Self.wipe()
                result("")

                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { exit(0) }
            }
        }
    }

    private static func wipe() {
        for kind in [kSecClassGenericPassword, kSecClassInternetPassword, kSecClassKey,
                     kSecClassCertificate, kSecClassIdentity] {
            let status = SecItemDelete([
                kSecClass as String: kind,
                kSecAttrSynchronizable as String: kSecAttrSynchronizableAny,
            ] as CFDictionary)

            if status != errSecSuccess && status != errSecItemNotFound {
                NSLog("vigil: erase could not remove keychain items of \(kind): \(status)")
            }
        }

        if let bundle = Bundle.main.bundleIdentifier {
            UserDefaults.standard.removePersistentDomain(forName: bundle)
        }

        UserDefaults(suiteName: TunnelKeychain.group)?.removePersistentDomain(forName: TunnelKeychain.group)

        let files = FileManager.default
        var folders = [files.temporaryDirectory]

        for kind: FileManager.SearchPathDirectory in [.documentDirectory, .applicationSupportDirectory,
                                                      .cachesDirectory] {
            folders += files.urls(for: kind, in: .userDomainMask)
        }

        if let shared = files.containerURL(forSecurityApplicationGroupIdentifier: TunnelKeychain.group) {
            folders.append(shared)
        }

        for folder in folders {
            let items: [URL]

            do {
                items = try files.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)
            } catch {
                NSLog("vigil: erase could not list \(folder.path): \(error)")
                continue
            }

            for item in items {
                do {
                    try files.removeItem(at: item)
                } catch {
                    NSLog("vigil: erase could not remove \(item.path): \(error)")
                }
            }
        }

        let center = UNUserNotificationCenter.current()
        center.removeAllDeliveredNotifications()
        center.removeAllPendingNotificationRequests()
        center.setBadgeCount(0)
    }

    private func voice() -> String {
        var buf = [CChar](repeating: 0, count: 1024)
        guard vigil_voice_status(&buf, buf.count) > 0 else { return "{\"running\":false}" }
        return String(cString: buf)
    }

    private static func reply(_ body: () throws -> Any?) -> Any? {
        do {
            return try body()
        } catch let error as ChatVault.Locked {
            return FlutterError(code: "locked", message: error.description, details: nil)
        } catch let error as ChatVault.Failure {
            return FlutterError(code: "invalid", message: error.description, details: nil)
        } catch {
            return FlutterError(code: "failed", message: "\(error)", details: nil)
        }
    }

    private func json(_ value: Any) -> String {
        guard let data = try? JSONSerialization.data(withJSONObject: value) else { return "null" }
        return String(data: data, encoding: .utf8) ?? "null"
    }
}

enum Handoff {
    static let gone = "The handoff expired, hand the device over again"

    private static let service = "se.sanctorum.handoff"
    private static let keep: TimeInterval = 5 * 60
    private static let prefix = Array("handoff ".utf8)

    private static var config: [UInt8] = []
    private static var issued: [UInt8] = []
    private static var made = Date.distantPast

    private static var group: String {
        let prefix = Bundle.main.object(forInfoDictionaryKey: "AppIdentifierPrefix") as? String ?? ""
        let name = Bundle.main.object(forInfoDictionaryKey: "VigilHandoffGroup") as? String ?? ""
        return prefix + name
    }

    private static var query: [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: "config",
            kSecAttrAccessGroup as String: group,
        ]
    }

    static func token() -> [UInt8] {
        guard take() else { return [] }

        var random = [UInt8](repeating: 0, count: 16)
        guard SecRandomCopyBytes(kSecRandomDefault, random.count, &random) == errSecSuccess else {
            clear()
            return []
        }

        issued = prefix + Array(random.map { String(format: "%02x", $0) }.joined().utf8)

        return issued
    }

    static func ours(_ token: [UInt8]) -> Bool {
        token.starts(with: prefix)
    }

    static func held(_ token: [UInt8], _ work: ([UInt8]) -> Void) -> Bool {
        if expired() {
            clear()
        }

        guard !config.isEmpty, !issued.isEmpty, token == issued else { return false }

        work(config)

        return true
    }

    static func clear() {
        TunnelKeychain.wipe(&config)
        config = []
        issued = []
    }

    private static func expired() -> Bool {
        Date().timeIntervalSince(made) > keep
    }

    private static func take() -> Bool {
        var q = query
        q[kSecReturnData as String] = true
        q[kSecReturnAttributes as String] = true

        var item: CFTypeRef?
        let status = SecItemCopyMatching(q as CFDictionary, &item)
        let removed = SecItemDelete(query as CFDictionary)

        guard status == errSecSuccess, removed == errSecSuccess, let found = item as? [String: Any],
              var data = found[kSecValueData as String] as? Data,
              let date = found[kSecAttrCreationDate as String] as? Date else { return false }
        defer { data.resetBytes(in: 0..<data.count) }

        guard Date().timeIntervalSince(date) <= keep else { return false }

        clear()
        config = [UInt8](data)
        made = date

        return true
    }
}

final class OpenItem: NSObject, QLPreviewItem {
    let previewItemURL: URL?
    let previewItemTitle: String?

    init(url: URL, title: String) {
        previewItemURL = url
        previewItemTitle = title
    }
}
