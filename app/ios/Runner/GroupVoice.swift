import AVFoundation
import Foundation
import Vigil

final class GroupVoice {
    static let shared = GroupVoice()

    private let queue = DispatchQueue(label: "se.sanctorum.vigil.groupvoice")
    private var run: UInt64 = 0
    private var running = ""
    private var sent = ""

    private(set) var talking = false

    private init() {
        NotificationCenter.default.addObserver(forName: AVAudioSession.routeChangeNotification, object: nil,
                                               queue: .main) { [weak self] _ in
            self?.speaker()
        }
    }

    func on(_ chat: String) throws {
        guard ChatVault.group(chat) != nil else {
            throw ChatVault.Failure(description: "Voice is only for groups.")
        }

        guard try ChatVault.shared.groupInfo(chat)?["state"] as? String == "open" else {
            throw ChatVault.Failure(description: "This group has ended.")
        }

        if try ChatVault.shared.voice()["chat"] as? String != chat {
            try save(["chat": chat, "muted": false])
        }
    }

    func off() throws {
        talk(false)
        try save([:])
    }

    func mute(_ on: Bool) throws {
        var state = try ChatVault.shared.voice()

        guard state["chat"] as? String != nil else { return }

        state["muted"] = on
        try save(state)
    }

    func talk(_ on: Bool) {
        talking = on && run != 0 && !VigilCall.shared.busy
        vigil_channel_talk(talking ? 1 : 0)
    }

    func json() -> [String: Any] {
        let state = (try? ChatVault.shared.voice()) ?? [:]

        return [
            "chat": state["chat"] as? String ?? "",
            "muted": state["muted"] as? Bool ?? false,
            "status": status(),
        ]
    }

    func poll() {
        guard let state = try? ChatVault.shared.voice() else { return }

        let chat = state["chat"] as? String ?? ""
        let group = ChatVault.group(chat)
        let info = group == nil ? nil : (try? ChatVault.shared.groupInfo(chat)) ?? nil
        let open = info?["state"] as? String == "open"

        if !chat.isEmpty && info != nil && !open {
            try? ChatVault.shared.voiceSave([:])
        }

        guard let group, let info, open, VigilCall.shared.cathedral(group.flock) != nil else {
            stop()
            return
        }

        var roster = ChatVault.roster(ChatVault.members(info) + [ChatVault.hex(group.owner)])
        let wanted = chat + roster.map { String(format: "%02x", $0) }.joined()

        if running != chat {
            stop()
        }

        if wanted != sent {
            vigil_channel_join(Flock.number(chat), group.owner, group.id, &roster)
            sent = wanted
        }

        vigil_channel_mute(state["muted"] as? Bool == true ? 1 : 0)

        if running.isEmpty {
            start(chat)
        }
    }

    func resume() {
        guard run != 0, !VigilCall.shared.busy else { return }

        audioSession()
        vigil_ios_audio_restart()
    }

    private func start(_ chat: String) {
        guard var config = VigilCall.shared.config?(Flock.of(chat)), !config.isEmpty else { return }

        let ident = vigil_channel_run_id()
        let cathedral = VigilCall.shared.cathedral(Flock.of(chat))

        run = ident
        running = chat

        if !VigilCall.shared.busy {
            audioSession()
        }

        queue.async {
            config.withUnsafeBytes { raw in
                let ptr = raw.baseAddress?.assumingMemoryBound(to: CChar.self)

                guard let cathedral else {
                    _ = vigil_channel_run(ident, ptr, raw.count, nil)
                    return
                }

                cathedral.withCString { via in
                    _ = vigil_channel_run(ident, ptr, raw.count, via)
                }
            }

            TunnelKeychain.wipe(&config)

            DispatchQueue.main.async {
                if self.run == ident {
                    self.run = 0
                    self.running = ""
                    self.talking = false
                }
            }
        }
    }

    private func stop() {
        talk(false)

        if run != 0 {
            vigil_channel_stop(run)
        }

        if !sent.isEmpty {
            vigil_channel_leave()
        }

        run = 0
        sent = ""
        running = ""
    }

    private func status() -> Any {
        var buf = [CChar](repeating: 0, count: Int(VIGIL_CHANNEL_STATUS_MAX))

        guard vigil_channel_status(&buf, buf.count) > 0,
              let status = try? JSONSerialization.jsonObject(with: Data(String(cString: buf).utf8)) else {
            return ["joined": false]
        }

        return status
    }

    private func save(_ state: [String: Any]) throws {
        try ChatVault.shared.voiceSave(state)
        poll()
    }

    private func audioSession() {
        let session = AVAudioSession.sharedInstance()

        try? session.setCategory(.playAndRecord, mode: .videoChat,
                                 options: [.allowBluetooth, .defaultToSpeaker])
        try? session.setActive(true)

        speaker()
    }

    private func speaker() {
        let session = AVAudioSession.sharedInstance()

        guard run != 0, !VigilCall.shared.busy,
              session.currentRoute.outputs.contains(where: { $0.portType == .builtInReceiver }) else {
            return
        }

        try? session.overrideOutputAudioPort(.speaker)
    }
}
