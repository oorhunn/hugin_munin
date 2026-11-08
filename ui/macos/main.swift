import AppKit

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let monitor = Monitor()
    private let settings = Settings()
    private let clipboard = ClipboardStore()
    private lazy var settingsWindow = SettingsWindowController(settings: settings)
    private lazy var hotKeys = HotKeyCenter()
    private var notch: NotchController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        monitor.start(interval: settings.interval)
        settings.onIntervalChange = { [monitor] in monitor.setInterval($0) }
        clipboard.start(capacity: settings.clipboardCapacity)
        settings.onClipboardCapacityChange = { [clipboard] in clipboard.setCapacity($0) }
        let notch = NotchController(monitor: monitor, settings: settings, clipboard: clipboard) {
            [weak self] in
            self?.settingsWindow.show()
        }
        settings.onWingMetricsChange = { [weak notch] in notch?.layout() }
        self.notch = notch
        notch.show()

        hotKeys.onPressed = { [weak notch] in notch?.openClipboardPreview() }
        settings.onShortcutChange = { [hotKeys] shortcut in hotKeys.register(shortcut) }
        hotKeys.register(settings.shortcut)
    }

    func applicationWillTerminate(_ notification: Notification) {
        monitor.stop()
        clipboard.stop()
    }
}

MainActor.assumeIsolated {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    app.run()
}
