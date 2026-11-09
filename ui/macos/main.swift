import AppKit

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let monitor = Monitor()
    private let settings = Settings()
    private let clipboard = ClipboardStore()
    private lazy var settingsWindow = SettingsWindowController(settings: settings,
                                                                clipboard: clipboard)
    private let diskTracker = DiskTracker()
    private lazy var diskWindow = DiskWindowController(tracker: diskTracker)
    private lazy var hotKeys = HotKeyCenter()
    private var notch: NotchController?
    private var bezel: ClipboardBezelController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        monitor.start(interval: settings.interval)
        settings.onIntervalChange = { [monitor] in monitor.setInterval($0) }
        clipboard.start(capacity: settings.clipboardCapacity,
                        historyFile: settings.clipboardHistoryFile)
        settings.onClipboardCapacityChange = { [clipboard] in clipboard.setCapacity($0) }
        settings.onKeepClipboardHistoryChange = { [clipboard, settings] in
            clipboard.setHistoryFile(settings.clipboardHistoryFile)
        }
        diskTracker.start()
        let notch = NotchController(monitor: monitor, disk: diskTracker, settings: settings,
                                    openSettings: { [weak self] in
            self?.settingsWindow.show()
        }, openDisk: { [weak self] in
            self?.diskWindow.show()
        })
        settings.onWingMetricsChange = { [weak notch] in notch?.layout() }
        self.notch = notch
        notch.show()

        let bezel = ClipboardBezelController(store: clipboard, settings: settings)
        self.bezel = bezel
        hotKeys.onPressed = { [weak bezel] in bezel?.open() }
        settings.onShortcutChange = { [hotKeys] shortcut in hotKeys.register(shortcut) }
        hotKeys.register(settings.shortcut)
    }

    func applicationWillTerminate(_ notification: Notification) {
        monitor.stop()
        clipboard.stop()
        diskTracker.stop()
    }
}

MainActor.assumeIsolated {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    app.run()
}
