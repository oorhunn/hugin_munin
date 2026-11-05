import SwiftUI

enum Format {
    static func bytes(_ value: UInt64) -> String {
        let units = ["B", "KB", "MB", "GB", "TB"]
        var value = Double(value)
        var unit = 0
        while value >= 1024 && unit < units.count - 1 {
            value /= 1024
            unit += 1
        }
        return String(format: unit >= 3 ? "%.1f %@" : "%.0f %@", value, units[unit])
    }

    static func percent(_ value: Double?) -> String {
        guard let value else { return "–" }
        return String(format: "%.0f%%", value)
    }
}

extension Color {
    /// Green below 50%, yellow below 80%, red above.
    static func level(_ percent: Double?) -> Color {
        guard let percent else { return .secondary }
        return percent < 50 ? .green : percent < 80 ? .yellow : .red
    }
}

/// Horizontal meter with a rounded track.
struct Meter: View {
    let percent: Double?

    var body: some View {
        GeometryReader { geometry in
            ZStack(alignment: .leading) {
                Capsule().fill(.quaternary)
                Capsule().fill(Color.level(percent))
                    .frame(width: geometry.size.width * min(max((percent ?? 0) / 100, 0), 1))
            }
        }
        .frame(height: 6)
    }
}
