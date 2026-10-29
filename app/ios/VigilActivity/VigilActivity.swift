import ActivityKit
import SwiftUI
import WidgetKit

@main
struct VigilWidgets: WidgetBundle {
    var body: some Widget {
        VigilCandle()
    }
}

struct VigilCandle: Widget {
    static let bg = Color(red: 0x0b / 255, green: 0x0c / 255, blue: 0x0e / 255)
    static let fg = Color(red: 0xe8 / 255, green: 0xe6 / 255, blue: 0xe1 / 255)
    static let amber = Color(red: 0xff / 255, green: 0xb0 / 255, blue: 0x00 / 255)

    var body: some WidgetConfiguration {
        ActivityConfiguration(for: VigilAttributes.self) { context in
            Card(since: context.state.since, stale: context.isStale)
                .activityBackgroundTint(Self.bg)
                .activitySystemActionForegroundColor(Self.amber)
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    Candle(lit: !context.isStale).frame(width: 14, height: 30)
                }
                DynamicIslandExpandedRegion(.center) {
                    Line(since: context.state.since, stale: context.isStale)
                }
            } compactLeading: {
                Flame().fill(Self.amber).frame(width: 9, height: 14)
            } compactTrailing: {
                EmptyView()
            } minimal: {
                Flame().fill(Self.amber).frame(width: 9, height: 14)
            }
        }
    }
}

private struct Card: View {
    let since: Date
    let stale: Bool

    var body: some View {
        HStack(spacing: 16) {
            Candle(lit: !stale).frame(width: 18, height: 44)

            VStack(alignment: .leading, spacing: 4) {
                Text("VIGIL")
                    .font(.system(size: 13, weight: .semibold, design: .monospaced))
                    .tracking(3)
                    .foregroundColor(VigilCandle.amber)
                Line(since: since, stale: stale)
            }

            Spacer()
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 16)
    }
}

private struct Line: View {
    let since: Date
    let stale: Bool

    var body: some View {
        if stale {
            Text("Open Vigil to relight")
                .font(.system(size: 15, design: .monospaced))
                .foregroundColor(VigilCandle.fg.opacity(0.6))
        } else {
            Text("keeping vigil since \(since, style: .time)")
                .font(.system(size: 15, design: .monospaced))
                .foregroundColor(VigilCandle.fg)
        }
    }
}

private struct Candle: View {
    let lit: Bool

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let h = geo.size.height

            ZStack(alignment: .top) {
                if lit {
                    Flame()
                        .fill(VigilCandle.amber)
                        .frame(width: w * 0.55, height: h * 0.36)
                        .shadow(color: VigilCandle.amber.opacity(0.6), radius: 4)
                }

                Rectangle()
                    .fill(VigilCandle.fg.opacity(0.6))
                    .frame(width: 1.5, height: h * 0.08)
                    .offset(y: h * 0.34)

                Rectangle()
                    .fill(VigilCandle.fg.opacity(0.85))
                    .frame(width: w, height: h * 0.58)
                    .offset(y: h * 0.42)
            }
            .frame(width: w, height: h, alignment: .top)
        }
    }
}

private struct Flame: Shape {
    func path(in rect: CGRect) -> Path {
        var path = Path()

        path.move(to: CGPoint(x: rect.midX, y: rect.minY))
        path.addQuadCurve(to: CGPoint(x: rect.maxX, y: rect.maxY * 0.7),
                          control: CGPoint(x: rect.maxX, y: rect.maxY * 0.35))
        path.addQuadCurve(to: CGPoint(x: rect.minX, y: rect.maxY * 0.7),
                          control: CGPoint(x: rect.midX, y: rect.maxY * 1.15))
        path.addQuadCurve(to: CGPoint(x: rect.midX, y: rect.minY),
                          control: CGPoint(x: rect.minX, y: rect.maxY * 0.35))
        path.closeSubpath()

        return path
    }
}
