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
            Card(since: context.state.since, lit: context.isStale ? nil : context.attributes.lit)
                .activityBackgroundTint(Self.bg)
                .activitySystemActionForegroundColor(Self.amber)
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    Candle(lit: context.isStale ? nil : context.attributes.lit, scale: 0.7)
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
    let lit: Date?

    var body: some View {
        HStack(spacing: 16) {
            Candle(lit: lit)

            VStack(alignment: .leading, spacing: 4) {
                Text("VIGIL")
                    .font(.system(size: 13, weight: .semibold, design: .monospaced))
                    .tracking(3)
                    .foregroundColor(VigilCandle.amber)
                Line(since: since, stale: lit == nil)
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
    let lit: Date?
    var scale: CGFloat = 1

    var body: some View {
        VStack(spacing: 0) {
            Flame()
                .fill(lit == nil ? Color.clear : VigilCandle.amber)
                .frame(width: 10 * scale, height: 16 * scale)
                .shadow(color: VigilCandle.amber.opacity(lit == nil ? 0 : 0.7), radius: 5 * scale)
            Rectangle()
                .fill(VigilCandle.fg.opacity(0.6))
                .frame(width: 1.5, height: 3 * scale)
            Wax(lit: lit)
                .frame(width: 18 * scale, height: 38 * scale)
        }
    }
}

private struct Wax: View {
    static let bar: CGFloat = 4

    let lit: Date?

    var body: some View {
        GeometryReader { geo in
            if let lit {
                ProgressView(timerInterval: lit...lit.addingTimeInterval(VigilAttributes.burn),
                             countsDown: true) {
                    EmptyView()
                } currentValueLabel: {
                    EmptyView()
                }
                .progressViewStyle(.linear)
                .tint(VigilCandle.fg.opacity(0.85))
                .frame(width: geo.size.height)
                .scaleEffect(x: 1, y: geo.size.width / Self.bar)
                .rotationEffect(.degrees(-90))
                .frame(width: geo.size.width, height: geo.size.height)
            } else {
                Rectangle()
                    .fill(VigilCandle.fg.opacity(0.85))
                    .frame(height: geo.size.height * 0.12)
                    .frame(maxHeight: .infinity, alignment: .bottom)
            }
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
