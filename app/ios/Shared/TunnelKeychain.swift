// Tunnel configurations live in a keychain access group shared between
// the app and its packet tunnel provider.

import Foundation
import Security

enum TunnelKeychain {
    static let group = "group.se.sanctorum.vigil"

    private static let service = "se.sanctorum.vigil.tunnel"

    struct Failure: Error, CustomStringConvertible {
        let status: OSStatus
        var description: String { "keychain error \(status)" }
    }

    static func store(id: String, config: [UInt8]) throws -> Data {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: id,
            kSecAttrAccessGroup as String: group,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
            kSecValueData as String: Data(config),
            kSecReturnPersistentRef as String: true,
        ]

        var ref: CFTypeRef?
        let status = SecItemAdd(query as CFDictionary, &ref)

        guard status == errSecSuccess, let data = ref as? Data else {
            throw Failure(status: status)
        }

        return data
    }

    static func update(id: String, config: [UInt8]) throws {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: id,
            kSecAttrAccessGroup as String: group,
        ]

        let changes: [String: Any] = [
            kSecValueData as String: Data(config),
        ]

        let status = SecItemUpdate(query as CFDictionary, changes as CFDictionary)

        guard status == errSecSuccess else {
            throw Failure(status: status)
        }
    }

    /// The caller must wipe the returned configuration.
    static func load(ref: Data) throws -> [UInt8] {
        let query: [String: Any] = [
            kSecValuePersistentRef as String: ref,
            kSecReturnData as String: true,
        ]

        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)

        guard status == errSecSuccess, let data = item as? Data else {
            throw Failure(status: status)
        }

        return [UInt8](data)
    }

    /// memset_s() so the compiler cannot drop the writes.
    static func wipe<T>(_ bytes: inout [T]) {
        bytes.withUnsafeMutableBytes { raw in
            guard let base = raw.baseAddress else { return }
            _ = memset_s(base, raw.count, 0, raw.count)
        }
    }

    static func remove(id: String) {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: id,
            kSecAttrAccessGroup as String: group,
        ]

        SecItemDelete(query as CFDictionary)
    }
}
