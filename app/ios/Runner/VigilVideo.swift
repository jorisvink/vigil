import AVFoundation
import CoreMedia
import Flutter
import Foundation
import VideoToolbox
import Vigil

final class VigilVideo: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    static let shared = VigilVideo()

    private static let rotation = 1
    private static let keyInterval = 60
    private static let fps = 30
    private static let start: [UInt8] = [0, 0, 0, 1]

    var registry: FlutterTextureRegistry?

    private let lock = NSLock()
    private let queue = DispatchQueue(label: "se.sanctorum.vigil.camera")

    private var decoding = false
    private var capturing = false
    private var keyframe = false
    private var bitrate: UInt32 = 1_000_000
    private var stopped: DispatchSemaphore?
    private var facing = true
    private var remoteRotation = 0
    private var remoteWidth = 0
    private var remoteHeight = 0
    private var localWidth = 960
    private var localHeight = 540
    private var remote: Texture?
    private var local: Texture?

    private var session: AVCaptureSession?
    private var output: AVCaptureVideoDataOutput?
    private var compressor: VTCompressionSession?
    private var size: (Int32, Int32) = (0, 0)

    private var decompressor: VTDecompressionSession?
    private var format: CMVideoFormatDescription?
    private var sps: Data?
    private var pps: Data?

    var front: Bool { locked { facing } }

    static func install() {
        var hooks = vigil_ios_video(
            start: { VigilVideo.shared.coreStart() },
            stop: { VigilVideo.shared.coreStop() },
            camera: { VigilVideo.shared.coreCamera($0 != 0) },
            keyframe: { VigilVideo.shared.coreKeyframe() },
            bitrate: { VigilVideo.shared.coreBitrate($0) }
        )

        vigil_ios_video_set(&hooks)
    }

    func textures() throws -> [String: Int64] {
        guard let registry else {
            throw VigilVideoError.textures
        }

        let (remote, local) = locked { () -> (Texture, Texture) in
            let remote = self.remote ?? Texture(registry)
            let local = self.local ?? Texture(registry)

            self.remote = remote
            self.local = local

            return (remote, local)
        }

        return ["remote": remote.id, "local": local.id]
    }

    func release() {
        let (remote, local) = locked { () -> (Texture?, Texture?) in
            let textures = (self.remote, self.local)

            self.remote = nil
            self.local = nil

            return textures
        }

        remote?.release()
        local?.release()
    }

    func state() -> [String: Any] {
        locked { () -> [String: Any] in
            [
                "decoding": decoding,
                "capturing": capturing,
                "front": facing,
                "rotation": remoteRotation,
                "width": remoteWidth,
                "height": remoteHeight,
                "localRotation": Self.rotation,
                "localMirror": facing,
                "localWidth": localWidth,
                "localHeight": localHeight,
            ]
        }
    }

    func flip() {
        let on = locked { () -> Bool in
            facing.toggle()
            return capturing
        }

        guard on else { return }

        queue.async { self.reconfigure() }
    }

    private func coreStart() -> Int32 {
        locked {
            guard !decoding else { return }

            let stopped = DispatchSemaphore(value: 0)

            decoding = true
            self.stopped = stopped

            let thread = Thread { [self] in
                decode()
                stopped.signal()
            }

            thread.name = "vigil-video"
            thread.qualityOfService = .userInteractive
            thread.start()
        }

        return 0
    }

    private func coreStop() {
        coreCamera(false)

        let stopped = locked { () -> DispatchSemaphore? in
            let stopped = self.stopped

            decoding = false
            self.stopped = nil

            return stopped
        }

        stopped?.wait()

        locked {
            remoteWidth = 0
            remoteHeight = 0
        }
    }

    private func coreCamera(_ on: Bool) {
        let changed = locked { () -> Bool in
            guard capturing != on else { return false }
            capturing = on
            return true
        }

        guard changed else { return }

        queue.async {
            if on {
                self.open()
            } else {
                self.close()
            }
        }
    }

    private func coreKeyframe() {
        locked { keyframe = true }
    }

    private func coreBitrate(_ bps: UInt32) {
        locked { bitrate = bps }

        queue.async {
            guard let compressor = self.compressor else { return }
            self.rate(compressor, bps)
        }
    }

    private func open() {
        guard session == nil, locked({ capturing }) else { return }

        guard AVCaptureDevice.authorizationStatus(for: .video) == .authorized else {
            NSLog("vigil: no camera access")
            return
        }

        let session = AVCaptureSession()
        let output = AVCaptureVideoDataOutput()

        session.automaticallyConfiguresApplicationAudioSession = false
        session.beginConfiguration()

        if session.canSetSessionPreset(.iFrame960x540) {
            session.sessionPreset = .iFrame960x540
        } else if session.canSetSessionPreset(.hd1280x720) {
            session.sessionPreset = .hd1280x720
        }

        guard attach(session) else {
            session.commitConfiguration()
            NSLog("vigil: no camera")
            return
        }

        output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
        output.alwaysDiscardsLateVideoFrames = true
        output.setSampleBufferDelegate(self, queue: queue)

        guard session.canAddOutput(output) else {
            session.commitConfiguration()
            NSLog("vigil: no camera output")
            return
        }

        session.addOutput(output)
        orient(output)
        session.commitConfiguration()
        session.startRunning()

        self.session = session
        self.output = output
    }

    private func close() {
        output?.setSampleBufferDelegate(nil, queue: nil)
        session?.stopRunning()

        session = nil
        output = nil

        if let compressor {
            VTCompressionSessionCompleteFrames(compressor, untilPresentationTimeStamp: .invalid)
            VTCompressionSessionInvalidate(compressor)
        }

        compressor = nil
        size = (0, 0)

        if locked({ capturing }) {
            open()
        }
    }

    private func reconfigure() {
        guard let session, let output else { return }

        session.beginConfiguration()

        for input in session.inputs {
            session.removeInput(input)
        }

        if !attach(session) {
            NSLog("vigil: no camera to flip to")
        }

        orient(output)
        session.commitConfiguration()

        locked { keyframe = true }
    }

    private func attach(_ session: AVCaptureSession) -> Bool {
        let position: AVCaptureDevice.Position = locked({ facing }) ? .front : .back

        guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: position)
                ?? AVCaptureDevice.default(for: .video),
              let input = try? AVCaptureDeviceInput(device: device),
              session.canAddInput(input) else { return false }

        session.addInput(input)

        return true
    }

    private func orient(_ output: AVCaptureVideoDataOutput) {
        guard let connection = output.connection(with: .video) else { return }

        if #available(iOS 17.0, *), connection.isVideoRotationAngleSupported(0) {
            connection.videoRotationAngle = 0
        } else if connection.isVideoOrientationSupported {
            connection.videoOrientation = .landscapeRight
        }

        if connection.isVideoMirroringSupported {
            connection.automaticallyAdjustsVideoMirroring = false
            connection.isVideoMirrored = false
        }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sample: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        guard let image = CMSampleBufferGetImageBuffer(sample) else { return }

        locked({ local })?.show(image)

        let width = Int32(CVPixelBufferGetWidth(image))
        let height = Int32(CVPixelBufferGetHeight(image))

        locked {
            localWidth = Int(width)
            localHeight = Int(height)
        }

        if compressor == nil || size != (width, height) {
            if let compressor {
                VTCompressionSessionInvalidate(compressor)
            }

            compressor = compression(width, height)
            size = (width, height)
            locked { keyframe = true }
        }

        guard let compressor else { return }

        let key = locked { () -> Bool in
            let key = keyframe
            keyframe = false
            return key
        }

        let properties = key ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil

        let status = VTCompressionSessionEncodeFrame(
            compressor,
            imageBuffer: image,
            presentationTimeStamp: CMSampleBufferGetPresentationTimeStamp(sample),
            duration: .invalid,
            frameProperties: properties,
            infoFlagsOut: nil
        ) { [weak self] status, _, encoded in
            guard status == noErr, let encoded else { return }
            self?.encoded(encoded)
        }

        if status != noErr {
            VTCompressionSessionInvalidate(compressor)
            self.compressor = nil
        }
    }

    private func compression(_ width: Int32, _ height: Int32) -> VTCompressionSession? {
        var made: VTCompressionSession?

        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: width,
            height: height,
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: nil,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: nil,
            refcon: nil,
            compressionSessionOut: &made
        )

        guard status == noErr, let session = made else {
            NSLog("vigil: no encoder: \(status)")
            return nil
        }

        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ProfileLevel,
                             value: kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_MaxKeyFrameInterval,
                             value: NSNumber(value: Self.keyInterval))
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ExpectedFrameRate,
                             value: NSNumber(value: Self.fps))

        rate(session, locked { bitrate })
        VTCompressionSessionPrepareToEncodeFrames(session)

        return session
    }

    private func rate(_ session: VTCompressionSession, _ bps: UInt32) {
        let limit = [NSNumber(value: Double(bps) / 8 * 1.5), NSNumber(value: 1)] as CFArray

        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: bps))
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_DataRateLimits, value: limit)
    }

    private func encoded(_ sample: CMSampleBuffer) {
        guard let block = CMSampleBufferGetDataBuffer(sample),
              let description = CMSampleBufferGetFormatDescription(sample) else { return }

        var key = true

        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false)
            as? [[CFString: Any]],
           let first = attachments.first,
           first[kCMSampleAttachmentKey_NotSync] as? Bool == true {
            key = false
        }

        var sets = 0
        var header: Int32 = 0

        guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            description, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil,
            parameterSetCountOut: &sets, nalUnitHeaderLengthOut: &header) == noErr,
              header > 0, header <= 4 else { return }

        var frame = Data()

        if key {
            for idx in 0..<sets {
                var pointer: UnsafePointer<UInt8>?
                var length = 0

                guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    description, parameterSetIndex: idx, parameterSetPointerOut: &pointer,
                    parameterSetSizeOut: &length, parameterSetCountOut: nil,
                    nalUnitHeaderLengthOut: nil) == noErr, let pointer else { return }

                frame.append(contentsOf: Self.start)
                frame.append(pointer, count: length)
            }
        }

        let length = CMBlockBufferGetDataLength(block)
        var data = [UInt8](repeating: 0, count: length)

        guard CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length,
                                         destination: &data) == noErr else { return }

        var offset = 0
        let prefix = Int(header)

        while offset + prefix <= length {
            var size = 0

            for idx in 0..<prefix {
                size = size << 8 | Int(data[offset + idx])
            }

            offset += prefix

            guard size > 0, offset + size <= length else { break }

            frame.append(contentsOf: Self.start)
            frame.append(contentsOf: data[offset..<offset + size])
            offset += size
        }

        guard !frame.isEmpty, frame.count <= Int(VIGIL_VIDEO_FRAME_MAX) else { return }

        var flags = Int32(Self.rotation & Int(VIGIL_VIDEO_ROTATE_MASK)) << VIGIL_VIDEO_ROTATE_SHIFT

        if key {
            flags |= Int32(VIGIL_VIDEO_FLAG_KEY)
        }

        feed(frame, flags)
    }

    private func feed(_ frame: Data, _ flags: Int32) {
        lock.lock()
        defer { lock.unlock() }

        guard capturing else { return }

        frame.withUnsafeBytes { raw in
            guard let base = raw.bindMemory(to: UInt8.self).baseAddress else { return }
            _ = vigil_video_input(base, raw.count, flags)
        }
    }

    private func decode() {
        let max = Int(VIGIL_VIDEO_FRAME_MAX)
        let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: max)
        var flags: Int32 = 0

        defer {
            buffer.deallocate()
            reset()
        }

        while locked({ decoding }) {
            let length = vigil_video_take(buffer, max, &flags)

            guard length > 0 else {
                usleep(5000)
                continue
            }

            let rotation = Int(flags >> VIGIL_VIDEO_ROTATE_SHIFT) & Int(VIGIL_VIDEO_ROTATE_MASK)

            guard decode(UnsafeBufferPointer(start: buffer, count: length), rotation) else {
                reset()
                vigil_video_refresh()
                continue
            }
        }
    }

    private func decode(_ frame: UnsafeBufferPointer<UInt8>, _ rotation: Int) -> Bool {
        var units: [Range<Int>] = []
        var parameters = (sps, pps)

        for unit in Self.units(frame) where !unit.isEmpty {
            switch frame[unit.lowerBound] & 0x1f {
            case 7:
                parameters.0 = Data(frame[unit])
            case 8:
                parameters.1 = Data(frame[unit])
            case 9:
                continue
            default:
                units.append(unit)
            }
        }

        if let sps = parameters.0, let pps = parameters.1,
           decompressor == nil || sps != self.sps || pps != self.pps {
            reset()

            guard decompression(sps, pps) else { return false }
        }

        guard let decompressor, let format, !units.isEmpty else { return true }

        var avcc = [UInt8]()

        for unit in units {
            let size = UInt32(unit.count).bigEndian
            withUnsafeBytes(of: size) { avcc.append(contentsOf: $0) }
            avcc.append(contentsOf: frame[unit])
        }

        var block: CMBlockBuffer?

        guard CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: avcc.count,
            blockAllocator: kCFAllocatorDefault, customBlockSource: nil, offsetToData: 0,
            dataLength: avcc.count, flags: 0, blockBufferOut: &block) == noErr,
              let block,
              CMBlockBufferReplaceDataBytes(with: avcc, blockBuffer: block, offsetIntoDestination: 0,
                                            dataLength: avcc.count) == noErr else { return false }

        var sample: CMSampleBuffer?
        var sizes = [avcc.count]

        guard CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format,
            sampleCount: 1, sampleTimingEntryCount: 0, sampleTimingArray: nil,
            sampleSizeEntryCount: 1, sampleSizeArray: &sizes, sampleBufferOut: &sample) == noErr,
              let sample else { return false }

        var failed = false

        let status = VTDecompressionSessionDecodeFrame(
            decompressor, sampleBuffer: sample, flags: [], infoFlagsOut: nil
        ) { [weak self] status, _, image, _, _ in
            guard status == noErr else {
                failed = true
                return
            }

            guard let image, let self else { return }
            self.decoded(image, rotation)
        }

        if status != noErr || failed {
            NSLog("vigil: decode failed: \(status)")
            return false
        }

        return true
    }

    private func decompression(_ sps: Data, _ pps: Data) -> Bool {
        var made: CMVideoFormatDescription?

        let status = sps.withUnsafeBytes { s in
            pps.withUnsafeBytes { p in
                let pointers = [s.bindMemory(to: UInt8.self).baseAddress!,
                                p.bindMemory(to: UInt8.self).baseAddress!]
                let sizes = [sps.count, pps.count]

                return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: kCFAllocatorDefault, parameterSetCount: 2,
                    parameterSetPointers: pointers, parameterSetSizes: sizes,
                    nalUnitHeaderLength: 4, formatDescriptionOut: &made)
            }
        }

        guard status == noErr, let made else {
            NSLog("vigil: bad parameter sets: \(status)")
            return false
        }

        let attributes: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
            kCVPixelBufferIOSurfacePropertiesKey: [String: Any]() as CFDictionary,
            kCVPixelBufferMetalCompatibilityKey: true,
        ]

        var session: VTDecompressionSession?

        guard VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault, formatDescription: made, decoderSpecification: nil,
            imageBufferAttributes: attributes as CFDictionary, outputCallback: nil,
            decompressionSessionOut: &session) == noErr, let session else {
            NSLog("vigil: no decoder")
            return false
        }

        VTSessionSetProperty(session, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)

        self.sps = sps
        self.pps = pps
        format = made
        decompressor = session

        return true
    }

    private func reset() {
        if let decompressor {
            VTDecompressionSessionInvalidate(decompressor)
        }

        decompressor = nil
        format = nil
        sps = nil
        pps = nil
    }

    private func decoded(_ image: CVImageBuffer, _ rotation: Int) {
        let width = CVPixelBufferGetWidth(image)
        let height = CVPixelBufferGetHeight(image)

        let remote = locked { () -> Texture? in
            remoteRotation = rotation
            remoteWidth = width
            remoteHeight = height
            return self.remote
        }

        remote?.show(image)
    }

    private static func units(_ data: UnsafeBufferPointer<UInt8>) -> [Range<Int>] {
        var units: [Range<Int>] = []
        var start = -1
        var idx = 0

        while idx + 3 <= data.count {
            guard data[idx] == 0, data[idx + 1] == 0, data[idx + 2] == 1 else {
                idx += 1
                continue
            }

            if start >= 0 {
                var end = idx
                while end > start && data[end - 1] == 0 { end -= 1 }
                units.append(start..<end)
            }

            idx += 3
            start = idx
        }

        if start >= 0 && start < data.count {
            units.append(start..<data.count)
        }

        return units
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }

    final class Texture: NSObject, FlutterTexture {
        private let lock = NSLock()
        private weak var registry: FlutterTextureRegistry?
        private var buffer: CVPixelBuffer?
        private(set) var id: Int64 = 0

        init(_ registry: FlutterTextureRegistry) {
            self.registry = registry
            super.init()
            id = registry.register(self)
        }

        func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
            lock.lock()
            defer { lock.unlock() }

            guard let buffer else { return nil }
            return Unmanaged.passRetained(buffer)
        }

        func show(_ image: CVPixelBuffer) {
            lock.lock()
            buffer = image
            lock.unlock()

            registry?.textureFrameAvailable(id)
        }

        func release() {
            registry?.unregisterTexture(id)

            lock.lock()
            buffer = nil
            lock.unlock()
        }
    }
}

enum VigilVideoError: Error {
    case textures
}
