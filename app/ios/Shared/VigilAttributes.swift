import ActivityKit
import Foundation

@available(iOS 16.1, *)
struct VigilAttributes: ActivityAttributes {
    struct ContentState: Codable, Hashable {
        var since: Date
    }

    static let burn: TimeInterval = 7.5 * 3600

    var lit: Date
}
