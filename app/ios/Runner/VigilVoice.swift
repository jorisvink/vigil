import AVFoundation
import Foundation
import UIKit
import Vigil

final class VigilVoice: NSObject, AVAudioPlayerDelegate {
    static let shared = VigilVoice()

    private static let bitRate = 32000
    private static let rates: [Double] = [
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
    ]

    private let lock = NSLock()

    private var engine: AVAudioEngine?
    private var converter: AVAudioConverter?
    private var aac: AVAudioFormat?
    private var recorded = [UInt8]()
    private var rate = 0
    private var channels = 0
    private var full = false

    private var player: AVAudioPlayer?
    private var playing: (() -> Void)?
    private var proximity: NSObjectProtocol?

    static func name() -> String {
        let format = DateFormatter()
        format.dateFormat = "yyyyMMdd-HHmmss"
        return "voice-\(format.string(from: Date())).aac"
    }

    func record() throws {
        guard engine == nil else {
            throw ChatVault.Failure(description: "Already recording.")
        }

        guard !VigilCall.shared.busy else {
            throw ChatVault.Failure(description: "Voice messages cannot be recorded during a call.")
        }

        stop()

        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playAndRecord, mode: .default, options: [.defaultToSpeaker])
        try session.setActive(true)

        let engine = AVAudioEngine()
        let input = engine.inputNode
        let format = input.outputFormat(forBus: 0)

        guard let index = Self.rates.firstIndex(of: format.sampleRate),
              format.channelCount > 0, format.channelCount <= 2 else {
            release()
            throw ChatVault.Failure(description: "The microphone cannot be used right now.")
        }

        var desc = AudioStreamBasicDescription(
            mSampleRate: format.sampleRate, mFormatID: kAudioFormatMPEG4AAC, mFormatFlags: 0,
            mBytesPerPacket: 0, mFramesPerPacket: 1024, mBytesPerFrame: 0,
            mChannelsPerFrame: format.channelCount, mBitsPerChannel: 0, mReserved: 0)

        guard let aac = AVAudioFormat(streamDescription: &desc),
              let converter = AVAudioConverter(from: format, to: aac) else {
            release()
            throw ChatVault.Failure(description: "The microphone cannot be used right now.")
        }

        converter.bitRate = Self.bitRate * Int(format.channelCount)

        lock.lock()
        recorded = []
        recorded.reserveCapacity(Int(VIGIL_CHAT_DATA_MAX))
        rate = index
        channels = Int(format.channelCount)
        full = false
        self.aac = aac
        self.converter = converter
        lock.unlock()

        input.installTap(onBus: 0, bufferSize: 4096, format: format) { [weak self] buffer, _ in
            self?.encode(buffer)
        }

        do {
            engine.prepare()
            try engine.start()
        } catch {
            input.removeTap(onBus: 0)
            _ = take()
            release()
            throw ChatVault.Failure(description: "The microphone cannot be used right now.")
        }

        self.engine = engine
    }

    func finish() -> Data? {
        guard let engine else { return nil }

        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        self.engine = nil

        encode(nil)
        release()

        return take()
    }

    func play(_ data: Data, at: TimeInterval, rate: Float, done: @escaping () -> Void) throws {
        stop()

        guard !VigilCall.shared.busy else {
            throw ChatVault.Failure(description: "Voice messages cannot be played during a call.")
        }

        try route(ear: false)

        guard let player = try? AVAudioPlayer(data: data, fileTypeHint: "public.aac-audio") else {
            release()
            throw ChatVault.Failure(description: "That voice message cannot be played.")
        }

        player.delegate = self
        player.enableRate = true
        player.prepareToPlay()
        player.rate = rate
        player.currentTime = at

        guard player.play() else {
            release()
            throw ChatVault.Failure(description: "That voice message cannot be played.")
        }

        self.player = player
        playing = done

        UIDevice.current.isProximityMonitoringEnabled = true
        proximity = NotificationCenter.default.addObserver(
            forName: UIDevice.proximityStateDidChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            try? self?.route(ear: UIDevice.current.proximityState)
        }
    }

    func seek(_ at: TimeInterval) {
        player?.currentTime = at
    }

    func speed(_ rate: Float) {
        player?.rate = rate
    }

    func position() -> String {
        guard let player else { return "" }

        return "\(Int(player.currentTime * 1000)) \(Int(player.duration * 1000))"
    }

    func stop() {
        if let proximity {
            NotificationCenter.default.removeObserver(proximity)
            self.proximity = nil
            UIDevice.current.isProximityMonitoringEnabled = false
        }

        guard let player else { return }

        player.stop()
        self.player = nil
        release()

        playing?()
        playing = nil
    }

    private func route(ear: Bool) throws {
        let session = AVAudioSession.sharedInstance()

        if ear {
            try session.setCategory(.playAndRecord, mode: .default)
            try session.overrideOutputAudioPort(.none)
        } else {
            try session.setCategory(.playback, mode: .spokenAudio)
        }

        try session.setActive(true)
    }

    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        stop()
    }

    func audioPlayerDecodeErrorDidOccur(_ player: AVAudioPlayer, error: Error?) {
        stop()
    }

    private func release() {
        guard !VigilCall.shared.busy else { return }

        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    private func take() -> Data? {
        lock.lock()
        defer { lock.unlock() }

        converter = nil
        aac = nil

        guard !recorded.isEmpty else { return nil }

        let data = Data(recorded)
        TunnelKeychain.wipe(&recorded)
        recorded = []

        return data
    }

    private func encode(_ buffer: AVAudioPCMBuffer?) {
        lock.lock()
        defer { lock.unlock() }

        guard let converter, let aac else { return }

        var fed = false

        while true {
            let out = AVAudioCompressedBuffer(format: aac, packetCapacity: 8,
                                              maximumPacketSize: converter.maximumOutputPacketSize)
            var error: NSError?

            let status = converter.convert(to: out, error: &error) { _, state in
                guard let buffer else {
                    state.pointee = .endOfStream
                    return nil
                }

                if fed {
                    state.pointee = .noDataNow
                    return nil
                }

                fed = true
                state.pointee = .haveData
                return buffer
            }

            append(out)

            if status != .haveData {
                break
            }
        }
    }

    private func append(_ out: AVAudioCompressedBuffer) {
        guard let packets = out.packetDescriptions else { return }

        let base = out.data.assumingMemoryBound(to: UInt8.self)

        for idx in 0..<Int(out.packetCount) {
            let packet = packets[idx]
            let size = Int(packet.mDataByteSize)
            let len = size + 7

            if full || recorded.count + len > Int(VIGIL_CHAT_DATA_MAX) {
                full = true
                return
            }

            recorded.append(contentsOf: [
                0xff, 0xf1,
                UInt8((1 << 6) | (rate << 2) | (channels >> 2)),
                UInt8(((channels & 3) << 6) | (len >> 11)),
                UInt8((len >> 3) & 0xff),
                UInt8(((len & 7) << 5) | 0x1f),
                0xfc,
            ])
            recorded.append(contentsOf: UnsafeBufferPointer(start: base + Int(packet.mStartOffset), count: size))
        }
    }
}
