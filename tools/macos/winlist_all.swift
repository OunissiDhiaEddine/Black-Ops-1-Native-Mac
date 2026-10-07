import CoreGraphics
import Foundation
if let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]] {
    for w in list {
        let owner = w[kCGWindowOwnerName as String] as? String ?? ""
        if !owner.lowercased().contains("bo1") { continue }
        let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
        print(owner, w[kCGWindowIsOnscreen as String] ?? "offscreen", w[kCGWindowLayer as String] ?? "", b["X"] ?? 0, b["Y"] ?? 0, b["Width"] ?? 0, b["Height"] ?? 0, w[kCGWindowAlpha as String] ?? "")
    }
}
