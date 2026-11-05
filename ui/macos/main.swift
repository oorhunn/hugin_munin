import AppKit

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let monitor = Monitor()
    private let settings = Settings()
    private lazy var settingsWindow = SettingsWindowController(settings: settings)
    private var notch: NotchController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        monitor.start(interval: settings.interval)
        settings.onIntervalChange = { [monitor] in monitor.setInterval($0) }
        notch = NotchController(monitor: monitor, settings: settings) { [weak self] in
            self?.settingsWindow.show()
        }
        notch?.show()
    }

    func applicationWillTerminate(_ notification: Notification) {
        monitor.stop()
    }
}

MainActor.assumeIsolated {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    app.run()
}
