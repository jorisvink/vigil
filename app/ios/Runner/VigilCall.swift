import AVFoundation
import CallKit
import Foundation
import Network
import UIKit
import UserNotifications
import Vigil

final class VigilCall: NSObject, CXProviderDelegate {
    static let shared = VigilCall()

    enum Phase: String {
        case ringing, dialing, alerting, connecting, active
    }

    var config: (() -> [UInt8]?)?
    var send: ((String) -> Void)?
    var answering: UInt8?

    private let provider: CXProvider
    private let controller = CXCallController()
    private let queue = DispatchQueue(label: "se.sanctorum.vigil.voice")
    private let paths = NWPathMonitor()
    private var network: String?

    private var uuid = UUID()
    private var peer: UInt8 = 0
    private var incoming = false
    private var phase: Phase? { didSet { awake() } }
    private var muted = false
    private var speaker = false
    private var video = false { didSet { awake() } }
    private var remoteVideo = false { didSet { awake() } }
    private var started = 0.0
    private var voice: UInt64 = 0
    private var pending: Int32?
    private var cathedral: String?
    private var ignored = Set<UInt8>()

    private override init() {
        let configuration = CXProviderConfiguration()
        configuration.supportsVideo = true
        configuration.maximumCallGroups = 1
        configuration.maximumCallsPerCallGroup = 1
        configuration.supportedHandleTypes = [.generic]

        provider = CXProvider(configuration: configuration)

        super.init()

        provider.setDelegate(self, queue: nil)

        paths.pathUpdateHandler = { [weak self] path in
            guard let self, path.status == .satisfied else { return }

            let network = path.availableInterfaces.first.map { "\($0.name)" }
            if self.network != nil && network != self.network {
                vigil_rebind()
            }

            self.network = network
        }

        paths.start(queue: .main)

        let center = NotificationCenter.default

        center.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: nil) { note in
            guard let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
                  AVAudioSession.InterruptionType(rawValue: raw) == .ended else { return }

            vigil_ios_audio_restart()
        }

        center.addObserver(forName: AVAudioSession.mediaServicesWereResetNotification, object: nil,
                           queue: .main) { [weak self] _ in
            self?.audioSession()
            vigil_ios_audio_restart()
        }
    }

    var busy: Bool { phase != nil }

    func json() -> [String: Any]? {
        guard let phase else { return nil }

        return [
            "peer": Self.hex(peer),
            "incoming": incoming,
            "state": phase.rawValue,
            "muted": muted,
            "speaker": speaker,
            "video": video,
            "front": VigilVideo.shared.front,
            "since": phase == .active ? Int(started * 1000) : 0,
        ]
    }

    func dial(_ peer: UInt8, video: Bool) {
        guard !busy, peer > 0, peer < 0xff else { return }

        begin(peer, incoming: false, phase: .dialing)
        camera(video)
        send?("dial \(Self.hex(peer))")

        let action = CXStartCallAction(call: uuid, handle: CXHandle(type: .generic, value: Self.name(peer)))
        action.isVideo = video

        controller.request(CXTransaction(action: action)) { error in
            guard error != nil else { return }
            DispatchQueue.main.async { self.end(nil) }
        }
    }

    func answer(video: Bool) {
        guard phase == .ringing else { return }
        camera(video)
        controller.request(CXTransaction(action: CXAnswerCallAction(call: uuid))) { _ in }
    }

    func hangup() {
        guard busy else { return }
        controller.request(CXTransaction(action: CXEndCallAction(call: uuid))) { _ in }
    }

    func mute(_ on: Bool) {
        guard busy else { return }
        controller.request(CXTransaction(action: CXSetMutedCallAction(call: uuid, muted: on))) { _ in }
    }

    func camera(_ on: Bool) {
        video = on
        vigil_video_camera(on ? 1 : 0)

        guard busy else { return }

        if on && !speaker {
            speaker(true)
        }

        if phase != .dialing {
            report()
        }
    }

    func speaker(_ on: Bool) {
        guard busy else { return }

        speaker = on
        try? AVAudioSession.sharedInstance().overrideOutputAudioPort(on ? .speaker : .none)
    }

    func update(_ status: [String: Any]?) {
        let call = status?["call"] as? [String: Any]
        cathedral = call?["cathedral"] as? String
        let callers = Set((call?["incoming"] as? [String] ?? []).compactMap { UInt8($0, radix: 16) })

        ignored.formIntersection(callers)

        if let answering, !callers.contains(answering) {
            self.answering = nil
        }

        guard let phase else {
            if let caller = callers.first(where: { !ignored.contains($0) }) {
                ring(caller)
            }
            return
        }

        if phase == .ringing && !callers.contains(peer) {
            end(.unanswered)
            return
        }

        let voice = voiceStatus()
        let state = voice?["state"] as? String
        let remote = (voice?["video"] as? [String: Any])?["remote"] as? Bool ?? false

        if remote != remoteVideo {
            remoteVideo = remote
            report()
        }

        if phase == .dialing && state == "online" {
            self.phase = .alerting
        }

        if phase != .active && phase != .ringing && state == "talking" {
            self.phase = .active
            started = Date().timeIntervalSince1970

            if !incoming {
                provider.reportOutgoingCall(with: uuid, connectedAt: nil)
            }
        }
    }

    func providerDidReset(_ provider: CXProvider) {
        if voice != 0 { vigil_voice_stop(voice) }
        reset()
        phase = nil
    }

    func provider(_ provider: CXProvider, perform action: CXStartCallAction) {
        audioSession()
        pending = Int32(VIGIL_VOICE_MODE_CALL)
        provider.reportOutgoingCall(with: uuid, startedConnectingAt: nil)
        action.fulfill()
    }

    func provider(_ provider: CXProvider, perform action: CXAnswerCallAction) {
        audioSession()
        phase = .connecting
        pending = Int32(VIGIL_VOICE_MODE_ANSWER)
        action.fulfill()
    }

    func provider(_ provider: CXProvider, didActivate audioSession: AVAudioSession) {
        guard busy, let mode = pending else {
            vigil_ios_audio_restart()
            return
        }

        pending = nil

        if speaker {
            try? audioSession.overrideOutputAudioPort(.speaker)
        }

        if mode == Int32(VIGIL_VOICE_MODE_ANSWER) {
            vigil_voice_answer()
        } else {
            startVoice(mode)
        }
    }

    func provider(_ provider: CXProvider, perform action: CXEndCallAction) {
        if voice != 0 { vigil_voice_stop(voice) }

        send?("done \(Self.hex(peer))")
        ignored.insert(peer)

        reset()
        phase = nil
        voice = 0
        pending = nil
        action.fulfill()
    }

    func provider(_ provider: CXProvider, perform action: CXSetMutedCallAction) {
        muted = action.isMuted
        vigil_voice_mute(muted ? 1 : 0)
        action.fulfill()
    }

    private func ring(_ caller: UInt8) {
        begin(caller, incoming: true, phase: .ringing)

        let update = CXCallUpdate()
        update.remoteHandle = CXHandle(type: .generic, value: Self.name(caller))
        update.localizedCallerName = Self.name(caller)
        update.hasVideo = false
        update.supportsHolding = false
        update.supportsGrouping = false
        update.supportsUngrouping = false
        update.supportsDTMF = false

        UNUserNotificationCenter.current()
            .removeDeliveredNotifications(withIdentifiers: ["call-\(Self.hex(caller))"])

        provider.reportNewIncomingCall(with: uuid, update: update) { error in
            DispatchQueue.main.async {
                guard error == nil else {
                    self.end(nil)
                    return
                }

                guard self.answering == caller else { return }

                self.answering = nil
                self.answer(video: false)
            }
        }

        startVoice(Int32(VIGIL_VOICE_MODE_RING))
    }

    private func begin(_ peer: UInt8, incoming: Bool, phase: Phase) {
        self.uuid = UUID()
        self.peer = peer
        self.incoming = incoming
        self.phase = phase
        self.muted = false
        self.speaker = false
        self.video = false
        self.remoteVideo = false
        self.started = 0
        self.voice = 0
        self.pending = nil
    }

    private func end(_ reason: CXCallEndedReason?) {
        guard busy else { return }

        if let reason {
            provider.reportCall(with: uuid, endedAt: nil, reason: reason)
        }

        if voice != 0 { vigil_voice_stop(voice) }
        send?("done \(Self.hex(peer))")

        ignored.insert(peer)
        reset()
        phase = nil
        voice = 0
    }

    private func report() {
        let update = CXCallUpdate()
        update.hasVideo = video || remoteVideo
        provider.reportCall(with: uuid, updated: update)
    }

    private func awake() {
        let on = phase != nil && (video || remoteVideo)

        if UIApplication.shared.isIdleTimerDisabled != on {
            UIApplication.shared.isIdleTimerDisabled = on
        }
    }

    private func reset() {
        video = false
        remoteVideo = false
        vigil_video_camera(0)
    }

    private func startVoice(_ mode: Int32) {
        guard var config = config?(), !config.isEmpty else {
            end(.failed)
            return
        }

        let id = vigil_voice_run_id()
        let peer = self.peer
        let cathedral = self.cathedral

        if mode != Int32(VIGIL_VOICE_MODE_DECLINE) { voice = id }

        queue.async {
            let ret = config.withUnsafeBytes { raw in
                let ptr = raw.baseAddress?.assumingMemoryBound(to: CChar.self)

                guard let cathedral else {
                    return vigil_voice_run(id, ptr, raw.count, peer, mode, nil)
                }

                return cathedral.withCString { via in
                    vigil_voice_run(id, ptr, raw.count, peer, mode, via)
                }
            }

            TunnelKeychain.wipe(&config)

            DispatchQueue.main.async { self.finished(id, ret) }
        }
    }

    private func finished(_ id: UInt64, _ ret: Int32) {
        guard id == voice, busy else { return }

        switch ret {
        case Int32(VIGIL_VOICE_END_REMOTE):
            end(.remoteEnded)
        case Int32(VIGIL_VOICE_END_TIMEOUT):
            end(phase == .active ? .remoteEnded : .unanswered)
        default:
            end(.failed)
        }
    }

    private func voiceStatus() -> [String: Any]? {
        var buf = [CChar](repeating: 0, count: 1024)
        guard vigil_voice_status(&buf, buf.count) > 0 else { return nil }
        return try? JSONSerialization.jsonObject(with: Data(String(cString: buf).utf8)) as? [String: Any]
    }

    private func audioSession() {
        try? AVAudioSession.sharedInstance().setCategory(
            .playAndRecord, mode: .voiceChat, options: [.allowBluetooth])
    }

    static func hex(_ peer: UInt8) -> String { String(format: "%02x", peer) }

    static func name(_ peer: UInt8) -> String { ChatVault.name(peer) }

    static func names() -> [String: String] { ChatVault.names() }

    static func save(_ names: [String: Any]) -> [String: String] { ChatVault.save(names) }
}
