import Flutter
import UIKit
import UserNotifications

@main
@objc class AppDelegate: FlutterAppDelegate, FlutterImplicitEngineDelegate {
  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    UNUserNotificationCenter.current().delegate = self
    ChatVault.callActions()
    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  override func userNotificationCenter(
    _ center: UNUserNotificationCenter,
    didReceive response: UNNotificationResponse,
    withCompletionHandler completionHandler: @escaping () -> Void
  ) {
    let info = response.notification.request.content.userInfo

    if let peer = info["chat"] as? String {
      VigilPlugin.requested = peer
      VigilPlugin.requestedTopic = info["topic"] as? String
    }

    if let caller = (info["call"] as? String).flatMap({ Flock.valid($0) ? $0 : nil }) {
      if response.actionIdentifier == "decline" {
        VigilPlugin.send("done \(caller)") {
          super.userNotificationCenter(center, didReceive: response, withCompletionHandler: completionHandler)
        }
        return
      }

      VigilCall.shared.answering = response.actionIdentifier == "answer" ? caller : nil
    }

    super.userNotificationCenter(center, didReceive: response, withCompletionHandler: completionHandler)
  }

  func didInitializeImplicitFlutterEngine(_ engineBridge: FlutterImplicitEngineBridge) {
    GeneratedPluginRegistrant.register(with: engineBridge.pluginRegistry)

    if let registrar = engineBridge.pluginRegistry.registrar(forPlugin: "VigilPlugin") {
      VigilPlugin.register(with: registrar)
    }
  }
}
