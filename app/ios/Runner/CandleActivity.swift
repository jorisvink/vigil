import ActivityKit
import Foundation
import UIKit

enum CandleActivity {
    static let offKey = "ui.candleOff"

    private static let relight: TimeInterval = 4 * 3600

    static func update(since: Date?) {
        guard #available(iOS 16.2, *) else { return }

        let lit = Activity<VigilAttributes>.activities

        guard let since, !UserDefaults.standard.bool(forKey: offKey) else {
            end(lit)
            return
        }

        if let current = lit.first, lit.count == 1, current.activityState == .active,
           current.content.state.since == since,
           Date().timeIntervalSince(current.attributes.lit) < relight {
            return
        }

        guard UIApplication.shared.applicationState == .active,
              ActivityAuthorizationInfo().areActivitiesEnabled else { return }

        end(lit)

        let now = Date()
        let content = ActivityContent(state: VigilAttributes.ContentState(since: since),
                                      staleDate: now.addingTimeInterval(VigilAttributes.burn))

        _ = try? Activity.request(attributes: VigilAttributes(lit: now), content: content, pushType: nil)
    }

    @available(iOS 16.2, *)
    private static func end(_ lit: [Activity<VigilAttributes>]) {
        for activity in lit {
            Task { await activity.end(nil, dismissalPolicy: .immediate) }
        }
    }
}
