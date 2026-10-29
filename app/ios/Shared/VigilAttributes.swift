import ActivityKit
import Foundation

@available(iOS 16.1, *)
struct VigilAttributes: ActivityAttributes {
    struct ContentState: Codable, Hashable {
        var since: Date
    }

    var lit: Date
}
