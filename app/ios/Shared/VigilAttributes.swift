import ActivityKit
import Foundation

struct VigilAttributes: ActivityAttributes {
    struct ContentState: Codable, Hashable {
        var since: Date
    }

    static let burn: TimeInterval = 7.5 * 3600

    var lit: Date
}
