// The link between the app and its packet tunnel provider, see link.c.
// Its secret is shared through the keychain, never in a message.

import Foundation
import NetworkExtension
import Security
import Vigil

enum TunnelLink {
    private static let service = "se.sanctorum.vigil.link"
    private static let requestMax = 64 * 1024
    private static let replyWait: TimeInterval = 5
    private static let retryWait: useconds_t = 250_000
    private static let attemptMax = replyWait + 0.25
    private static let queue = DispatchQueue(label: "se.sanctorum.vigil.link")
    private static var sequence = UInt64.random(in: 1...(UInt64.max / 2))

    @discardableResult
    static func open(create: Bool) -> Bool {
        var secret = load()
        defer { TunnelKeychain.wipe(&secret) }

        guard secret.count == Int(VIGIL_LINK_SECRET_LEN) else { return create && renew() }

        return vigil_link_open(secret, secret.count) == 0
    }

    /// Only while the provider is not running, it loads the secret once.
    @discardableResult
    static func renew() -> Bool {
        var secret = [UInt8](repeating: 0, count: Int(VIGIL_LINK_SECRET_LEN))
        defer { TunnelKeychain.wipe(&secret) }

        guard SecRandomCopyBytes(kSecRandomDefault, secret.count, &secret) == errSecSuccess,
              store(secret) else { return false }

        return vigil_link_open(secret, secret.count) == 0
    }

    static func close() {
        vigil_link_close()
    }

    static var ready: Bool {
        vigil_link_ready() == 1
    }

    /// The caller must wipe the returned message.
    static func input(_ batch: Data) -> [UInt8]? {
        var msg = [UInt8](repeating: 0, count: Int(VIGIL_LINK_MESSAGE_MAX))
        let msglen = msg.count

        let len = batch.withUnsafeBytes { raw in
            vigil_link_input(raw.baseAddress?.assumingMemoryBound(to: UInt8.self), raw.count,
                             &msg, msglen)
        }

        guard len > 0 else {
            TunnelKeychain.wipe(&msg)
            return nil
        }

        msg.removeSubrange(len...)

        return msg
    }

    static func output(_ message: [UInt8], capacity: Int) -> Data {
        var out = [UInt8](repeating: 0, count: capacity)
        let outlen = out.count

        var len = message.withUnsafeBufferPointer {
            vigil_link_output($0.baseAddress, $0.count, &out, outlen)
        }

        if len == -1 {
            len = vigil_link_output(nil, 0, &out, outlen)
        }

        guard len > 0 else { return Data() }

        return Data(out[0..<len])
    }

    /// Requests go one at a time, each with a sequence number so the
    /// provider does not act twice on one we send again. An attempt
    /// counts for at most attemptMax, so being suspended does not use
    /// up the time we keep trying.
    static func request(_ session: NETunnelProviderSession, _ message: String,
                        done: @escaping (Data?) -> Void) {
        let patience = patience(message)

        queue.async {
            sequence += 1

            var bytes = Array("\(sequence) \(message)".utf8)
            defer { TunnelKeychain.wipe(&bytes) }

            var spent: TimeInterval = 0

            while true {
                let start = ProcessInfo.processInfo.systemUptime

                if var answer = exchange(session, bytes) {
                    done(Data(answer))
                    TunnelKeychain.wipe(&answer)
                    return
                }

                if spent >= patience { break }

                usleep(retryWait)
                spent += min(ProcessInfo.processInfo.systemUptime - start, attemptMax)
            }

            done(nil)
        }
    }

    private static func patience(_ message: String) -> TimeInterval {
        if message == "status" { return 0 }

        for slow in ["chat-group-send ", "chat-group-data ", "done "] where message.hasPrefix(slow) {
            return 60
        }

        return 10
    }

    /// The caller must wipe the returned answer.
    private static func exchange(_ session: NETunnelProviderSession, _ message: [UInt8]) -> [UInt8]? {
        let batch = output(message, capacity: requestMax)
        let replied = DispatchSemaphore(value: 0)
        let lock = NSLock()
        var reply: Data?

        do {
            try session.sendProviderMessage(batch) { data in
                lock.lock()
                reply = data
                lock.unlock()
                replied.signal()
            }
        } catch {
            return nil
        }

        guard replied.wait(timeout: .now() + replyWait) == .success else { return nil }

        lock.lock()
        defer { lock.unlock() }

        return reply.flatMap { input($0) }
    }

    private static var query: [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: "secret",
            kSecAttrAccessGroup as String: TunnelKeychain.group,
        ]
    }

    /// The caller must wipe the returned secret.
    private static func load() -> [UInt8] {
        var search = query
        search[kSecReturnData as String] = true

        var item: CFTypeRef?
        guard SecItemCopyMatching(search as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data else { return [] }

        return [UInt8](data)
    }

    private static func store(_ secret: [UInt8]) -> Bool {
        SecItemDelete(query as CFDictionary)

        var add = query
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        add[kSecValueData as String] = Data(secret)

        return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
    }
}
