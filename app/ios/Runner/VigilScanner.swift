import AVFoundation
import CoreImage
import Flutter
import Foundation

final class VigilScanner: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate,
                          AVCaptureMetadataOutputObjectsDelegate {
    static let shared = VigilScanner()

    var registry: FlutterTextureRegistry?

    private let lock = NSLock()
    private let queue = DispatchQueue(label: "se.sanctorum.vigil.scanner")

    private var session: AVCaptureSession?
    private var texture: VigilVideo.Texture?
    private var value: String?
    private var width = 0
    private var height = 0

    func start(result: @escaping FlutterResult) {
        AVCaptureDevice.requestAccess(for: .video) { granted in
            DispatchQueue.main.async {
                guard granted else {
                    result(FlutterError(code: "denied", message: "Camera access was denied.", details: nil))
                    return
                }

                do {
                    result(try self.open())
                } catch {
                    result(FlutterError(code: "unsupported", message: "The camera could not be started.",
                                        details: nil))
                }
            }
        }
    }

    func state() -> [String: Any] {
        locked { () -> [String: Any] in
            let found = value
            value = nil
            return ["value": found ?? NSNull(), "width": width, "height": height]
        }
    }

    func stop() {
        let (session, texture) = locked { () -> (AVCaptureSession?, VigilVideo.Texture?) in
            let running = (self.session, self.texture)

            self.session = nil
            self.texture = nil
            value = nil
            width = 0
            height = 0

            return running
        }

        texture?.release()

        guard let session else { return }

        queue.async { session.stopRunning() }
    }

    func image(_ path: String) throws -> String? {
        guard let image = CIImage(contentsOf: URL(fileURLWithPath: path)) else {
            throw VigilScannerError.image
        }

        let detector = CIDetector(ofType: CIDetectorTypeQRCode, context: nil,
                                  options: [CIDetectorAccuracy: CIDetectorAccuracyHigh])

        return detector?.features(in: image)
            .compactMap { ($0 as? CIQRCodeFeature)?.messageString }
            .first
    }

    private func open() throws -> String {
        stop()

        guard let registry else { throw VigilScannerError.camera }

        let session = AVCaptureSession()
        let output = AVCaptureVideoDataOutput()
        let metadata = AVCaptureMetadataOutput()

        session.beginConfiguration()

        if session.canSetSessionPreset(.hd1920x1080) {
            session.sessionPreset = .hd1920x1080
        }

        guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
                ?? AVCaptureDevice.default(for: .video),
              let input = try? AVCaptureDeviceInput(device: device),
              session.canAddInput(input) else {
            session.commitConfiguration()
            throw VigilScannerError.camera
        }

        session.addInput(input)

        output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
        output.alwaysDiscardsLateVideoFrames = true
        output.setSampleBufferDelegate(self, queue: queue)

        guard session.canAddOutput(output), session.canAddOutput(metadata) else {
            session.commitConfiguration()
            throw VigilScannerError.camera
        }

        session.addOutput(output)
        session.addOutput(metadata)

        guard metadata.availableMetadataObjectTypes.contains(.qr) else {
            session.commitConfiguration()
            throw VigilScannerError.camera
        }

        metadata.metadataObjectTypes = [.qr]
        metadata.setMetadataObjectsDelegate(self, queue: queue)

        if let connection = output.connection(with: .video), connection.isVideoRotationAngleSupported(90) {
            connection.videoRotationAngle = 90
        }

        session.commitConfiguration()

        let texture = VigilVideo.Texture(registry)

        locked {
            self.session = session
            self.texture = texture
            value = nil
        }

        queue.async { session.startRunning() }

        let reply: [String: Any] = ["texture": texture.id]
        let data = try JSONSerialization.data(withJSONObject: reply)

        return String(data: data, encoding: .utf8) ?? "null"
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sample: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        guard let image = CMSampleBufferGetImageBuffer(sample) else { return }

        let texture = locked { () -> VigilVideo.Texture? in
            width = CVPixelBufferGetWidth(image)
            height = CVPixelBufferGetHeight(image)
            return self.texture
        }

        texture?.show(image)
    }

    func metadataOutput(_ output: AVCaptureMetadataOutput, didOutput objects: [AVMetadataObject],
                        from connection: AVCaptureConnection) {
        guard let code = objects.compactMap({ ($0 as? AVMetadataMachineReadableCodeObject)?.stringValue }).first
        else { return }

        locked {
            if value == nil {
                value = code
            }
        }
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}

enum VigilScannerError: Error {
    case camera
    case image
}
