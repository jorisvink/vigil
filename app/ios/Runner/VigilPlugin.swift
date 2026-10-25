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
    private static let provider = "se.sanctorum.vigil.PacketTunnel"
    private static let tunnelsKey = "tunnels"

    static var requested: String?

    private var version = 0
    private var opened: String?
    private var picking: (String, FlutterResult)?
    private var saving: (URL, FlutterResult)?
    private var preview: OpenItem?

    static func register(with registrar: FlutterPluginRegistrar) {
        let channel = FlutterMethodChannel(name: channel, binaryMessenger: registrar.messenger())
        let plugin = VigilPlugin()

        registrar.addMethodCallDelegate(plugin, channel: channel)

        VigilVideo.shared.registry = registrar.textures()
        VigilVideo.install()

        for name in [UIApplication.didEnterBackgroundNotification,
                     UIApplication.protectedDataWillBecomeUnavailableNotification] {
            NotificationCenter.default.addObserver(
                forName: name, object: nil, queue: .main
            ) { [weak plugin] _ in plugin?.background() }
        }

        NotificationCenter.default.addObserver(
            forName: UIScene.willDeactivateNotification, object: nil, queue: .main
        ) { note in cover(note.object as? UIWindowScene, on: true) }

        NotificationCenter.default.addObserver(
            forName: UIScene.didActivateNotification, object: nil, queue: .main
        ) { note in cover(note.object as? UIWindowScene, on: false) }

        try? FileManager.default.removeItem(at: openFolder)
        try? ChatVault.shared.prepare()
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
            view.backgroundColor = UIColor(red: 0x0b / 255, green: 0x0c / 255, blue: 0x0e / 255, alpha: 1)
            view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            window.addSubview(view)
        }
    }

    private static let coverTag = 0x76676c

    private func background() {
        ChatVault.shared.lock()

        if let chat = opened {
            if ChatVault.group(chat) != nil {
                VigilCall.shared.send?("chat-group-typing 0")
            } else {
                VigilCall.shared.send?("chat-open \(chat) 0")
                VigilCall.shared.send?("chat-typing \(chat) 0")
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

            result(check(config).1)
        case "import":
            var config = Self.bytes(call.arguments)
            defer { TunnelKeychain.wipe(&config) }

            importTunnel(config, result: result)
        case "handoff":
            var config = Handoff.take() ?? []
            defer { TunnelKeychain.wipe(&config) }

            result(FlutterStandardTypedData(bytes: Data(config)))
        case "edit":
            let args = call.arguments as? [String: Any] ?? [:]
            edit(args["id"] as? String ?? "", edits: args["edits"] as? String ?? "", result: result)
        case "remove":
            remove(call.arguments as? String ?? "", result: result)
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
                VigilCall.shared.dial(UInt8(peer, radix: 16) ?? 0, video: granted)
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
                result(json(VigilCall.save(names)))
            } else {
                result(json(VigilCall.names()))
            }
        case "chatData":
            let args = call.arguments as? [String: Any] ?? [:]

            do {
                let data = try ChatVault.shared.data(args["peer"] as? String ?? "",
                                                     file: args["data"] as? String ?? "")
                result(data.map { FlutterStandardTypedData(bytes: $0) })
            } catch let error as ChatVault.Locked {
                result(FlutterError(code: "locked", message: error.description, details: nil))
            } catch {
                result(FlutterError(code: "failed", message: "\(error)", details: nil))
            }
        case "chatDataOpen":
            let args = call.arguments as? [String: Any] ?? [:]

            do {
                try open(args["peer"] as? String ?? "", file: args["data"] as? String ?? "",
                         name: args["name"] as? String ?? "file")
                result("")
            } catch let error as ChatVault.Locked {
                result(FlutterError(code: "locked", message: error.description, details: nil))
            } catch {
                result(FlutterError(code: "failed", message: "\(error)", details: nil))
            }
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
            let hex = call.arguments as? String ?? ""

            guard ChatVault.chat(hex) != nil, picking == nil, saving == nil, let top = Self.top() else {
                result(FlutterError(code: "invalid", message: "Cannot pick a file now.", details: nil))
                return
            }

            let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.item], asCopy: true)
            picker.delegate = self
            picking = (hex, result)
            top.present(picker, animated: true)
        case "chats", "chat", "chatSend", "chatImageSend", "chatRead", "chatOpen", "chatTyping",
             "chatDelete", "chatSettings", "chatGroup":
            do {
                result(try chat(call))
            } catch let error as ChatVault.Locked {
                result(FlutterError(code: "locked", message: error.description, details: nil))
            } catch let error as ChatVault.Failure {
                result(FlutterError(code: "invalid", message: error.description, details: nil))
            } catch {
                result(FlutterError(code: "failed", message: "\(error)", details: nil))
            }
        case "flag":
            let args = call.arguments as? [String: Any] ?? [:]
            let key = "ui.\(args["key"] as? String ?? "")"

            if let value = args["value"] as? Bool {
                UserDefaults.standard.set(value, forKey: key)
            }

            result(UserDefaults.standard.bool(forKey: key) ? "true" : "false")
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
        tunnels().first { $0.summary["mode"] as? String == "gospel" }
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

        if mode == "gospel" && voiceTunnel() != nil {
            result(FlutterError(code: "invalid",
                                message: "There is already a Gospel configuration, remove it first.",
                                details: nil))
            return
        }

        let id = UUID().uuidString.lowercased()

        do {
            let ref = try TunnelKeychain.store(id: id, config: config)
            var list = UserDefaults.standard.array(forKey: Self.tunnelsKey) as? [[String: Any]] ?? []
            list.insert(["id": id, "summary": summary, "ref": ref], at: 0)
            save(list)

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

        let parsed = (try? JSONSerialization.jsonObject(with: Data(summary.utf8))) ?? [:]
        result(json(["id": id, "summary": parsed]))
    }

    private func remove(_ id: String, result: @escaping FlutterResult) {
        TunnelKeychain.remove(id: id)

        let list = (UserDefaults.standard.array(forKey: Self.tunnelsKey) as? [[String: Any]] ?? [])
            .filter { ($0["id"] as? String) != id }
        save(list)

        manager { manager in
            if let manager, self.tunnelId(manager) == id {
                manager.removeFromPreferences { _ in result("") }
            } else {
                result("")
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

            do {
                try session.sendProviderMessage(Data(message.utf8)) { _ in finish() }
            } catch {
                finish()
            }
        }
    }

    private func manager(_ done: @escaping (NETunnelProviderManager?) -> Void) {
        NETunnelProviderManager.loadAllFromPreferences { managers, _ in
            DispatchQueue.main.async { done(managers?.first) }
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

        let voice = voiceTunnel()
        let alone = tunnel.summary["mode"] as? String == "gospel"

        if voice != nil {
            AVAudioSession.sharedInstance().requestRecordPermission { _ in }
            UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
        }

        manager { existing in
            let manager = existing ?? NETunnelProviderManager()
            let proto = NETunnelProviderProtocol()

            proto.providerBundleIdentifier = Self.provider
            proto.serverAddress = (tunnel.summary["peer"] ?? tunnel.summary["cathedral"]) as? String ?? "sanctum"
            proto.passwordReference = tunnel.ref
            var configuration: [String: Any] = ["id": id]
            if let voice, !alone { configuration["voice"] = voice.ref }
            proto.providerConfiguration = configuration

            manager.localizedDescription = "Vigil: \(tunnel.summary["instance"] as? String ?? id)"
            manager.protocolConfiguration = proto
            manager.isEnabled = true

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
                        self.whenStopped(manager.connection) {
                            do {
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

    private func whenStopped(_ connection: NEVPNConnection, _ done: @escaping () -> Void) {
        let stopped: (NEVPNStatus) -> Bool = { $0 == .disconnected || $0 == .invalid }

        if stopped(connection.status) {
            done()
            return
        }

        var observer: NSObjectProtocol?
        var finished = false

        let finish: () -> Void = {
            guard !finished else { return }
            finished = true

            if let observer {
                NotificationCenter.default.removeObserver(observer)
            }

            done()
        }

        observer = NotificationCenter.default.addObserver(
            forName: .NEVPNStatusDidChange, object: connection, queue: .main
        ) { _ in
            if stopped(connection.status) {
                finish()
            }
        }

        connection.stopVPNTunnel()
        DispatchQueue.main.asyncAfter(deadline: .now() + 10) { finish() }
    }

    private func disconnect(result: @escaping FlutterResult) {
        manager { manager in
            let id = manager.flatMap { self.tunnelId($0) }

            if Self.gospelEnabled, let voice = self.voiceTunnel(), let id, id != voice.id,
               let status = manager?.connection.status, status != .disconnected, status != .invalid {
                self.connect(voice.id, result: result)
                return
            }

            manager?.connection.stopVPNTunnel()
            result("")
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
                if !on { session.stopVPNTunnel() }
                result("")
                return
            }

            try? session.sendProviderMessage(Data("gospel".utf8)) { _ in }
            result("")
        }
    }

    private func dataSend(_ hex: String, media: String, name: String, data: Data) throws -> String {
        let group = ChatVault.group(hex)
        let defaults = ChatVault.defaults

        guard !data.isEmpty, data.count <= Int(VIGIL_CHAT_DATA_MAX) else {
            throw ChatVault.Failure(description: "A file is at most 4 MB.")
        }

        if let group, Int(group) != defaults.integer(forKey: "chatGroup") {
            throw ChatVault.Failure(description: "You are not in this group anymore.")
        }

        if group != nil, !Self.gospelEnabled || VigilCall.shared.send == nil {
            throw ChatVault.Failure(
                description: "Gospel is not running, group files only go to members that are around.")
        }

        let (ref, file) = try ChatVault.shared.sendData(hex, media: media, name: name, data: data)

        if group != nil {
            let label = Data(name.utf8).base64EncodedString()
            VigilCall.shared.send?("chat-group-data \(ref) \(file) \(hex) \(media) \(label)")
        } else {
            VigilCall.shared.send?("chat")
        }

        version += 1

        return "\(ref)"
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        if let (copy, result) = saving {
            saving = nil
            try? FileManager.default.removeItem(at: copy.deletingLastPathComponent())
            result("saved")
            return
        }

        guard let (hex, result) = picking else { return }
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
            result(try dataSend(hex, media: "file", name: Self.fileName(url.lastPathComponent), data: data))
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

        picking?.1(nil)
        picking = nil
    }

    private func open(_ hex: String, file: String, name: String) throws {
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

        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: 2048,
        ]

        guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
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
        let idle = "{\"tunnel\":null,\"status\":{\"running\":false},\"chats\":\(chats())," +
            "\"gospel\":\(Self.gospelEnabled)}"

        manager { manager in
            guard let manager,
                  let session = manager.connection as? NETunnelProviderSession,
                  [.connected, .connecting, .reasserting].contains(session.status) else {
                VigilCall.shared.update(nil)
                VigilCall.shared.send = nil
                result(idle)
                return
            }

            let id = self.tunnelId(manager)
            let ref = self.voiceTunnel()?.ref

            VigilCall.shared.config = { ref.flatMap { try? TunnelKeychain.load(ref: $0) } }
            VigilCall.shared.send = { message in
                try? session.sendProviderMessage(Data(message.utf8)) { _ in }
            }

            do {
                try session.sendProviderMessage(Data("status".utf8)) { response in
                    DispatchQueue.main.async {
                        guard let response,
                              let status = String(data: response, encoding: .utf8) else {
                            result(idle)
                            return
                        }
                        VigilCall.shared.update(
                            (try? JSONSerialization.jsonObject(with: response)) as? [String: Any])

                        let tunnel = id.map { "\"\($0)\"" } ?? "null"
                        let phone = VigilCall.shared.json().map { self.json($0) } ?? "null"
                        result("{\"tunnel\":\(tunnel),\"status\":\(status)," +
                            "\"voice\":\(self.voice()),\"phone\":\(phone)," +
                            "\"chats\":\(self.chats()),\"gospel\":\(Self.gospelEnabled)}")
                    }
                }
            } catch {
                result(idle)
            }
        }
    }

    private func chat(_ call: FlutterMethodCall) throws -> String {
        let vault = ChatVault.shared
        let defaults = ChatVault.defaults
        let args = call.arguments as? [String: Any] ?? [:]
        let hex = (call.arguments as? String) ?? (args["peer"] as? String) ?? ""
        let group = ChatVault.group(hex)

        if call.method == "chatGroup" {
            let id = hex.isEmpty ? 0 : UInt16(hex, radix: 16)
            guard let id, hex.count <= 4 else {
                throw ChatVault.Failure(description: "A group is 1 to 4 hex digits.")
            }

            defaults.set(Int(id), forKey: "chatGroup")
            VigilCall.shared.send?("chat-group")
            version += 1

            return ""
        }

        guard call.method == "chats" || call.method == "chatSettings" ||
              ChatVault.chat(hex) != nil else {
            throw ChatVault.Failure(description: "There is no chat \(hex).")
        }

        switch call.method {
        case "chats":
            try vault.merge()
            return json(try vault.conversations())
        case "chat":
            try vault.merge()
            return json(try vault.messages(hex))
        case "chatSend":
            let text = args["text"] as? String ?? ""
            let size = text.utf8.count

            guard size > 0, size <= Int(VIGIL_CHAT_MESSAGE_MAX) else {
                throw ChatVault.Failure(description: "A message is at most \(VIGIL_CHAT_MESSAGE_MAX) bytes.")
            }

            if let group, Int(group) != defaults.integer(forKey: "chatGroup") {
                throw ChatVault.Failure(description: "You are not in this group anymore.")
            }

            if group != nil, !Self.gospelEnabled || VigilCall.shared.send == nil {
                throw ChatVault.Failure(
                    description: "Gospel is not running, group messages only go to members that are around.")
            }

            let ref = try vault.send(hex, text: text)

            if group != nil {
                VigilCall.shared.send?("chat-group-send \(ref) \(Data(text.utf8).base64EncodedString())")
            } else {
                VigilCall.shared.send?("chat")
            }

            version += 1

            return "\(ref)"
        case "chatImageSend":
            let image = try Self.jpeg(args["path"] as? String ?? "")
            return try dataSend(hex, media: "image", name: Self.photoName(), data: image)
        case "chatRead":
            try vault.read(hex)
            seen(hex)
            version += 1
            return ""
        case "chatOpen":
            let on = args["on"] as? Bool ?? false

            if let previous = opened, previous != hex {
                VigilCall.shared.send?("chat-open \(previous) 0")
            }

            opened = on ? hex : nil

            if on {
                defaults.set(hex, forKey: "chatOpen")
                seen(hex)
            } else {
                defaults.removeObject(forKey: "chatOpen")
            }

            if group == nil {
                VigilCall.shared.send?("chat-open \(hex) \(on ? 1 : 0)")
            }
            return ""
        case "chatTyping":
            let on = args["on"] as? Bool == true ? 1 : 0

            if group != nil {
                VigilCall.shared.send?("chat-group-typing \(on)")
            } else {
                VigilCall.shared.send?("chat-typing \(hex) \(on)")
            }
            return ""
        case "chatDelete":
            try vault.delete(hex)
            if group == nil {
                VigilCall.shared.send?("chat-forget \(hex)")
            }
            version += 1
            return ""
        default:
            if let nick = args["nick"] as? String {
                guard nick.withCString({ vigil_chat_nick($0) }) == 0 else {
                    throw ChatVault.Failure(description: "A name is 2 to 16 letters or digits.")
                }

                defaults.set(nick, forKey: "chatNick")
                VigilCall.shared.send?("chat-nick")
            }

            if let preview = args["preview"] as? Bool {
                defaults.set(preview, forKey: "chatPreview")
            }

            return json([
                "nick": defaults.string(forKey: "chatNick") ?? "",
                "preview": defaults.object(forKey: "chatPreview") as? Bool ?? false,
            ])
        }
    }

    private func seen(_ hex: String) {
        var unseen = ChatVault.defaults.dictionary(forKey: "chatUnseen") as? [String: Int] ?? [:]
        unseen[hex] = nil
        ChatVault.defaults.set(unseen, forKey: "chatUnseen")

        let center = UNUserNotificationCenter.current()
        center.getDeliveredNotifications { delivered in
            center.removeDeliveredNotifications(
                withIdentifiers: delivered
                    .filter { $0.request.content.threadIdentifier == "chat-\(hex)" }
                    .map { $0.request.identifier })
        }
    }

    private func chats() -> String {
        let defaults = ChatVault.defaults
        let unseen = defaults.dictionary(forKey: "chatUnseen") as? [String: Int] ?? [:]
        let open = Self.requested
        let group = defaults.integer(forKey: "chatGroup")
        Self.requested = nil

        return json([
            "version": defaults.integer(forKey: "chatVersion") + version,
            "unseen": unseen.values.reduce(0, +),
            "hardware": ChatVault.shared.hardware,
            "open": open ?? NSNull(),
            "group": group != 0 ? ChatVault.groupChat(UInt16(truncatingIfNeeded: group)) as Any : NSNull(),
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

    private func voice() -> String {
        var buf = [CChar](repeating: 0, count: 1024)
        guard vigil_voice_status(&buf, buf.count) > 0 else { return "{\"running\":false}" }
        return String(cString: buf)
    }

    private func json(_ value: Any) -> String {
        guard let data = try? JSONSerialization.data(withJSONObject: value) else { return "null" }
        return String(data: data, encoding: .utf8) ?? "null"
    }
}

enum Handoff {
    private static let service = "se.sanctorum.handoff"
    private static let keep: TimeInterval = 5 * 60

    private static var group: String {
        let prefix = Bundle.main.object(forInfoDictionaryKey: "AppIdentifierPrefix") as? String ?? ""
        return prefix + "se.sanctorum.handoff"
    }

    private static var query: [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: "config",
            kSecAttrAccessGroup as String: group,
        ]
    }

    /// The caller must wipe the returned configuration.
    static func take() -> [UInt8]? {
        var q = query
        q[kSecReturnData as String] = true
        q[kSecReturnAttributes as String] = true

        var item: CFTypeRef?
        let status = SecItemCopyMatching(q as CFDictionary, &item)
        SecItemDelete(query as CFDictionary)

        guard status == errSecSuccess, let found = item as? [String: Any],
              var data = found[kSecValueData as String] as? Data else { return nil }
        defer { data.resetBytes(in: 0..<data.count) }

        if let made = found[kSecAttrCreationDate as String] as? Date,
           Date().timeIntervalSince(made) > keep {
            return nil
        }

        return [UInt8](data)
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
