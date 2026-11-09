import AppKit
import Observation
import SwiftUI

/// Where the notch is on a screen, in AppKit screen coordinates (origin bottom-left).
struct NotchGeometry: Equatable {
    static let metricWidth: CGFloat = 62
    static let wingPadding: CGFloat = 14
    static let expandedWidth: CGFloat = 640
    static let expandedHeightWithShelf: CGFloat = 446
    /// Drops the shelf and the gap above it, so the jobs list keeps its height.
    static let expandedHeightWithoutShelf: CGFloat = 319
    /// Height of the slides at the top of the expanded panel. Fixed so that a swipe can be told
    /// apart from one over the file shelf below, which scrolls sideways itself.
    static let slideHeight: CGFloat = 62
    static let slideTop: CGFloat = 8

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

    /// Full expanded size including the notch or menu bar strip.
    func expandedSize(showingShelf: Bool) -> CGSize {
        CGSize(width: Self.expandedWidth,
               height: (showingShelf ? Self.expandedHeightWithShelf : Self.expandedHeightWithoutShelf) + height)
    }

    /// Rectangle of the given size hanging from the top centre of the screen.
    func rect(_ size: CGSize) -> NSRect {
        NSRect(x: screenFrame.midX - size.width / 2, y: screenFrame.maxY - size.height,
               width: size.width, height: size.height)
    }
}

/// A slide of the expanded panel. The declaration order is the order on screen.
enum NotchPage: String, CaseIterable, Identifiable {
    case system = "System", disk = "Disk"

    var id: Self { self }
}

@MainActor @Observable
final class NotchState {
    var expanded = false
    var dragging = false
    let shelf = FileShelf()
    var geometry: NotchGeometry
    var page = NotchPage.system

    init(geometry: NotchGeometry) { self.geometry = geometry }

    /// Moves to the slide `offset` places along, and stays put at either end.
    func movePage(by offset: Int) {
        let pages = NotchPage.allCases
        guard let index = pages.firstIndex(of: page), pages.indices.contains(index + offset) else { return }
        show(pages[index + offset])
    }

    func show(_ page: NotchPage) {
        withAnimation(.easeInOut(duration: 0.25)) { self.page = page }
    }
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
    private var swipeDistance: CGFloat = 0
    private var swipeUsed = false

    init(monitor: Monitor, disk: DiskTracker, settings: Settings,
         openSettings: @escaping () -> Void, openDisk: @escaping () -> Void) {
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

        state.shelf.onChange = { [weak self] in self?.layout() }

        let hosting = ShelfDropHosting(rootView: AnyView(NotchView(monitor: monitor, disk: disk, settings: settings,
                                                        state: state, openSettings: openSettings,
                                                        openDisk: openDisk)))
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
        NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { [weak self] event in
            MainActor.assumeIsolated { self?.handleScroll(event) }
            return event
        }
        layout()
    }

    /// True when the pointer is over the slides, as opposed to the lists below them.
    private func isOverSlides(_ event: NSEvent) -> Bool {
        let fromTop = panel.frame.height - event.locationInWindow.y - state.geometry.height
        return (0...(NotchGeometry.slideTop + NotchGeometry.slideHeight)).contains(fromTop)
    }

    /// Turns a horizontal swipe over the slides into one change of slide. A trackpad swipe reports
    /// phases and a mouse does not, so a mouse wheel can move on again once it has travelled as far.
    private func handleScroll(_ event: NSEvent) {
        guard event.window === panel, state.expanded,
              isOverSlides(event), event.momentumPhase == [] else { return }
        if event.phase == .began {
            swipeDistance = 0
            swipeUsed = false
        }
        guard !swipeUsed else { return }
        swipeDistance += event.scrollingDeltaX
        guard abs(swipeDistance) >= 30 else { return }
        state.movePage(by: swipeDistance < 0 ? 1 : -1)
        swipeDistance = 0
        swipeUsed = event.phase != []
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
        // The window keeps its expanded size while collapsed, when the transparent part ignores
        // clicks. It follows the shelf so an empty shelf leaves no invisible area catching the pointer.
        let size = state.geometry.expandedSize(showingShelf: !state.shelf.urls.isEmpty)
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
    let disk: DiskTracker
    let settings: Settings
    let state: NotchState
    let openSettings: () -> Void
    let openDisk: () -> Void

    var body: some View {
        let geometry = state.geometry
        let size = state.expanded
            ? geometry.expandedSize(showingShelf: !state.shelf.urls.isEmpty)
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
                            NotchPanel(system: monitor.state, disk: disk, state: state,
                                       openSettings: openSettings, openDisk: openDisk,
                                       setPaused: { monitor.setAdmissionsPaused($0) })
                            if !state.shelf.urls.isEmpty {
                                FileShelfView(state: state)
                            }
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

/// Compact readout shown when the notch is expanded, with the jobs list below it. The top is a
/// row of slides that a swipe or the labels in the footer move between. Each slide's position
/// comes straight from the current page, because a ScrollView rebuilt on every expand came to
/// rest a few points out of place.
private struct NotchPanel: View {
    let system: SystemState
    let disk: DiskTracker
    let state: NotchState
    let openSettings: () -> Void
    let openDisk: () -> Void
    let setPaused: (Bool) -> Void

    var body: some View {
        VStack(spacing: 8) {
            GeometryReader { geometry in
                HStack(spacing: 0) {
                    ForEach(NotchPage.allCases) { page in
                        slide(page).frame(width: geometry.size.width, height: geometry.size.height)
                    }
                }
                .offset(x: -geometry.size.width * pageIndex)
            }
            .frame(height: NotchGeometry.slideHeight)
            .clipped()
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
                Button("Disk Tracker", action: openDisk)
                Spacer()
                pageLabels
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }
            }
            .buttonStyle(.borderless)
            .font(.caption)
        }
        .padding(.top, NotchGeometry.slideTop)
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

    private var pageIndex: CGFloat {
        CGFloat(NotchPage.allCases.firstIndex(of: state.page) ?? 0)
    }

    @ViewBuilder
    private func slide(_ page: NotchPage) -> some View {
        switch page {
        case .system: systemSlide
        case .disk: DiskSlide(tracker: disk)
        }
    }

    private var systemSlide: some View {
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
        .frame(maxHeight: .infinity, alignment: .top)
    }

    private var pageLabels: some View {
        HStack(spacing: 10) {
            ForEach(NotchPage.allCases) { page in
                Button(page.rawValue) { state.show(page) }
                    .foregroundStyle(page == state.page ? .primary : .secondary)
            }
        }
    }
}

/// The disk tracker's summary: free space, and the top of what used it.
private struct DiskSlide: View {
    let tracker: DiskTracker

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            if let report = tracker.report {
                Text(report.headline).font(.caption.weight(.semibold)).lineLimit(1)
                if report.hotspots.isEmpty {
                    Text(report.baseline == nil ? "One scan so far." : "Nothing grew by 50 MB or more.")
                        .foregroundStyle(.secondary)
                }
                ForEach(report.hotspots.prefix(tracker.status.scanning ? 2 : 3)) { item in
                    HStack(spacing: 6) {
                        Text(item.displayPath).lineLimit(1).truncationMode(.middle)
                        Spacer()
                        Text(Format.change(item.change)).monospacedDigit().foregroundStyle(.red)
                    }
                }
            } else if !tracker.status.scanning {
                Text("No scans yet").font(.caption.weight(.semibold))
                Text("Open Disk Tracker and click Scan Now.").foregroundStyle(.secondary)
            }
            if tracker.status.scanning {
                Text("Scanning: \(tracker.status.filesSeen.formatted()) files")
                    .foregroundStyle(.secondary)
            }
        }
        .font(.caption2)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
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
