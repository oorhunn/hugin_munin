import AppKit
import Observation
import SwiftUI

/// Where the notch is on a screen, in AppKit screen coordinates (origin bottom-left).
struct NotchGeometry: Equatable {
    static let metricWidth: CGFloat = 62
    static let wingPadding: CGFloat = 14
    static let expandedSize = CGSize(width: 640, height: 446)

    let screenFrame: NSRect
    let notchWidth: CGFloat // 0 on screens without a notch
    let height: CGFloat     // notch or menu bar height

    init(screen: NSScreen?) {
        guard let screen else {
            screenFrame = .zero
            notchWidth = 0
            height = 24
            return
        }
        screenFrame = screen.frame
        if screen.safeAreaInsets.top > 0, let left = screen.auxiliaryTopLeftArea,
           let right = screen.auxiliaryTopRightArea {
            notchWidth = screen.frame.width - left.width - right.width
            height = screen.safeAreaInsets.top
        } else {
            // No notch: sit in the middle of the menu bar as a small pill.
            notchWidth = 0
            height = max(screen.frame.maxY - screen.visibleFrame.maxY, 24)
        }
    }

    /// Width beside the notch for `metrics` labels. An empty side keeps room to catch the pointer.
    static func wingWidth(metrics: Int) -> CGFloat {
        wingPadding + metricWidth * CGFloat(max(metrics, 1))
    }

    /// Both wings match the wider one, so the panel stays centred on the notch.
    func collapsedSize(wingMetrics: Int) -> CGSize {
        CGSize(width: notchWidth + 2 * Self.wingWidth(metrics: wingMetrics), height: height)
    }

    /// Rectangle of the given size hanging from the top centre of the screen.
    func rect(_ size: CGSize) -> NSRect {
        NSRect(x: screenFrame.midX - size.width / 2, y: screenFrame.maxY - size.height,
               width: size.width, height: size.height)
    }
}

@MainActor @Observable
final class NotchState {
    var expanded = false
    var dragging = false
    let shelf = FileShelf()
    var geometry: NotchGeometry

    init(geometry: NotchGeometry) { self.geometry = geometry }
}

/// Borderless panel that floats over the menu bar around the notch and expands on hover.
///
/// Hover is detected with tracking areas rather than a system-wide mouse-move monitor, so the
/// app only hears about the pointer when it crosses the notch or the expanded panel.
@MainActor
final class NotchController {
    private let panel: NSPanel
    /// Catches the pointer over the collapsed notch while `panel` lets clicks through.
    private let trigger: NSPanel
    private let state: NotchState
    private let settings: Settings
    private let triggerTracker = HoverTracker()
    private let panelTracker = HoverTracker()
    private var screenObserver: NSObjectProtocol?
    private var collapseWork: DispatchWorkItem?

    init(monitor: Monitor, settings: Settings, openSettings: @escaping () -> Void) {
        self.settings = settings
        state = NotchState(geometry: NotchGeometry(screen: Self.preferredScreen()))
        panel = Self.makePanel(level: Int(CGWindowLevelForKey(.mainMenuWindow)) + 3)
        panel.ignoresMouseEvents = true

        // Sits just under the panel, which draws the notch over it. A fully transparent window
        // would let the pointer through, so it gets an invisible sliver of alpha.
        trigger = Self.makePanel(level: Int(CGWindowLevelForKey(.mainMenuWindow)) + 2)
        let catcher = ShelfDropHosting(rootView: AnyView(Color.clear))
        catcher.wantsLayer = true
        catcher.layer?.backgroundColor = NSColor.black.withAlphaComponent(0.01).cgColor
        trigger.contentView = catcher

        let hosting = ShelfDropHosting(rootView: AnyView(NotchView(monitor: monitor, settings: settings,
                                                        state: state, openSettings: openSettings)))
        hosting.sizingOptions = []
        panel.contentView = hosting
        for receiver in [hosting, catcher] {
            receiver.onEnter = { [weak self] in self?.setExpanded(true) }
            receiver.onExit = { [weak self] in self?.scheduleCollapse() }
            receiver.onDrop = { [weak self] urls in
                self?.state.shelf.add(urls)
                self?.setExpanded(true)
            }
        }

        triggerTracker.onEnter = { [weak self] in self?.setExpanded(true) }
        triggerTracker.attach(to: catcher)
        panelTracker.onEnter = { [weak self] in self?.cancelCollapse() }
        panelTracker.onExit = { [weak self] in self?.scheduleCollapse() }
        layout()
    }

    private static func makePanel(level: Int) -> NSPanel {
        let panel = NSPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel],
                            backing: .buffered, defer: false)
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.level = .init(level)
        // Not .fullScreenAuxiliary, so it stays out of the way of full-screen apps.
        panel.collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle]
        panel.isMovable = false
        return panel
    }

    func show() {
        trigger.orderFrontRegardless()
        panel.orderFrontRegardless()
        screenObserver = NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.layout() }
        }
    }

    /// Prefer the built-in display, which is the one with the notch.
    private static func preferredScreen() -> NSScreen? {
        NSScreen.screens.first { $0.safeAreaInsets.top > 0 } ?? NSScreen.main ?? NSScreen.screens.first
    }

    func layout() {
        state.geometry = NotchGeometry(screen: Self.preferredScreen())
        // The window always has the expanded size; the transparent part ignores clicks while collapsed.
        let size = CGSize(width: NotchGeometry.expandedSize.width,
                          height: NotchGeometry.expandedSize.height + state.geometry.height)
        panel.setFrame(state.geometry.rect(size), display: true)
        let collapsed = state.geometry.collapsedSize(wingMetrics: settings.leftMetrics.count)
        trigger.setFrame(state.geometry.rect(collapsed), display: false)
    }

    private func scheduleCollapse() {
        guard state.expanded, !state.dragging, collapseWork == nil else { return }
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.collapseWork = nil
            if !self.state.dragging { self.setExpanded(false) }
        }
        collapseWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2, execute: work)
    }

    private func cancelCollapse() {
        collapseWork?.cancel()
        collapseWork = nil
    }

    private func setExpanded(_ expanded: Bool) {
        cancelCollapse()
        guard state.expanded != expanded else { return }
        withAnimation(.spring(response: 0.35, dampingFraction: 0.8)) { state.expanded = expanded }
        panel.ignoresMouseEvents = !expanded
        if expanded, let view = panel.contentView {
            // The pointer is already over the panel, so only its exit is reported.
            panelTracker.attach(to: view, assumeInside: true)
        } else {
            panelTracker.detach()
        }
    }

}

/// Owns one tracking area that reports the pointer entering and leaving a view.
@MainActor
private final class HoverTracker: NSResponder {
    var onEnter: (() -> Void)?
    var onExit: (() -> Void)?
    private var area: NSTrackingArea?
    private weak var view: NSView?

    func attach(to view: NSView, assumeInside: Bool = false) {
        detach()
        var options: NSTrackingArea.Options = [.mouseEnteredAndExited, .activeAlways, .inVisibleRect]
        if assumeInside { options.insert(.assumeInside) }
        let area = NSTrackingArea(rect: .zero, options: options, owner: self, userInfo: nil)
        view.addTrackingArea(area)
        self.area = area
        self.view = view
    }

    func detach() {
        if let area { view?.removeTrackingArea(area) }
        area = nil
        view = nil
    }

    override func mouseEntered(with event: NSEvent) { onEnter?() }
    override func mouseExited(with event: NSEvent) { onExit?() }
}

struct NotchView: View {
    let monitor: Monitor
    let settings: Settings
    let state: NotchState
    let openSettings: () -> Void

    var body: some View {
        let geometry = state.geometry
        let size = state.expanded
            ? CGSize(width: NotchGeometry.expandedSize.width,
                     height: NotchGeometry.expandedSize.height + geometry.height)
            : geometry.collapsedSize(wingMetrics: settings.leftMetrics.count)
        let radius: CGFloat = state.expanded ? 24 : 10
        VStack(spacing: 0) {
            ZStack(alignment: .top) {
                UnevenRoundedRectangle(bottomLeadingRadius: radius, bottomTrailingRadius: radius)
                    .fill(.black)
                VStack(spacing: 0) {
                    wings.frame(height: geometry.height)
                    if state.expanded {
                        VStack(spacing: 10) {
                            // An explicit closure avoids Swift 6.3's IR-generation crash when
                            // adapting the isolated bound method to a Boolean callback.
                            NotchPanel(system: monitor.state, openSettings: openSettings,
                                       setPaused: { monitor.setAdmissionsPaused($0) })
                            FileShelfView(state: state)
                        }
                            .padding(.horizontal, 20)
                            .padding(.bottom, 12)
                            .transition(.opacity.combined(with: .scale(scale: 0.9, anchor: .top)))
                    }
                }
            }
            .frame(width: size.width, height: size.height)
            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .foregroundStyle(.white)
        .environment(\.colorScheme, .dark)
    }

    /// The metrics chosen in Settings, split across the two sides of the notch.
    private var wings: some View {
        let width = NotchGeometry.wingWidth(metrics: settings.leftMetrics.count)
        return HStack(spacing: 0) {
            wing(settings.leftMetrics).frame(width: width)
            Spacer(minLength: state.geometry.notchWidth)
            wing(settings.rightMetrics).frame(width: width)
        }
        .padding(.horizontal, state.expanded ? 12 : 0)
    }
}

private extension NotchView {
    func wing(_ metrics: [Metric]) -> some View {
        HStack(spacing: 0) {
            ForEach(metrics) { metric in
                WingLabel(symbol: metric.symbol, value: metric.percent(in: monitor.state))
                    .frame(width: NotchGeometry.metricWidth)
            }
        }
    }
}

private struct WingLabel: View {
    let symbol: String
    let value: Double?

    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: symbol).font(.system(size: 11, weight: .semibold))
                .foregroundStyle(Color.level(value))
            Text(Format.percent(value))
                .font(.system(size: 12, weight: .semibold, design: .rounded))
                .monospacedDigit()
                .contentTransition(.numericText())
        }
    }
}

/// Compact readout shown when the notch is expanded.
private struct NotchPanel: View {
    let system: SystemState
    let openSettings: () -> Void
    let setPaused: (Bool) -> Void

    var body: some View {
        VStack(spacing: 8) {
            HStack(alignment: .top, spacing: 16) {
                Stat(title: "CPU", percent: system.cpuPercent,
                     detail: system.coreCount == 0 ? "–" : "\(system.coreCount) cores")
                Stat(title: "Memory", percent: system.memoryPercent,
                     detail: system.memoryTotal == 0 ? "–"
                         : "\(Format.bytes(system.memoryUsed)) of \(Format.bytes(system.memoryTotal))")
                Stat(title: "Disk", percent: system.diskPercent,
                     detail: system.diskPercent == nil ? "–" : "\(Format.bytes(system.diskFree)) free")
                Stat(title: "GPU", percent: system.gpuPercent,
                     detail: system.gpuCoreCount == 0 ? "–" : "\(system.gpuCoreCount) cores")
            }
            Divider().overlay(.white.opacity(0.15))
            HStack {
                Text("Jobs").font(.caption.weight(.semibold))
                Spacer()
                Text("\(system.registeredAgents) agents · \(system.pressure)")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            if system.jobs.isEmpty {
                Text("No registered jobs")
                    .font(.caption).foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    LazyVStack(spacing: 10) {
                        ForEach(system.jobs) { job in
                            HStack(alignment: .top) {
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(job.title).font(.caption.weight(.medium)).lineLimit(1)
                                    Text("\(job.agent_name) · \(job.project)")
                                        .font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                                }
                                Spacer()
                                VStack(alignment: .trailing, spacing: 3) {
                                    Text(job.state.capitalized).font(.caption2)
                                        .foregroundStyle(job.state == "unresponsive" ? .orange : .secondary)
                                    Text(estimate(job)).font(.caption2).foregroundStyle(.secondary)
                                }
                            }
                            .help(job.title + " — " + job.project)
                        }
                    }
                }
            }
            HStack {
                Text("Estimates supplied by apps").font(.caption2).foregroundStyle(.secondary)
                Spacer()
            }
            HStack {
                Button("Settings", action: openSettings)
                Button(system.admissionsPaused ? "Resume jobs" : "Pause new jobs") {
                    setPaused(!system.admissionsPaused)
                }
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }
            }
            .buttonStyle(.borderless)
            .font(.caption)
        }
        .padding(.top, 8)
    }

    private func estimate(_ job: RegisteredJob) -> String {
        if job.state == "unresponsive" { return "Heartbeat overdue" }
        if job.overdue { return "Estimate exceeded" }
        let seconds = job.state == "queued" ? job.estimated_duration_seconds : job.remaining_seconds
        guard let seconds else { return "No estimate" }
        let minutes = max(1, Int(ceil(seconds / 60)))
        let duration = minutes < 60 ? "~\(minutes)m" : "~\(minutes / 60)h \(minutes % 60)m"
        return job.state == "queued" ? "\(duration) duration · waiting" : "\(duration) left"
    }
}

private struct Stat: View {
    let title: String
    let percent: Double?
    let detail: String

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .firstTextBaseline, spacing: 6) {
                Text(title).font(.caption).foregroundStyle(.secondary)
                Text(Format.percent(percent))
                    .font(.system(size: 18, weight: .semibold, design: .rounded))
                    .foregroundStyle(Color.level(percent))
                    .monospacedDigit()
            }
            Meter(percent: percent)
            Text(detail).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}
