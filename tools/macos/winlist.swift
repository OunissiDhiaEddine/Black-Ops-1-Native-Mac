// Lists on-screen windows as "<id>\t<owner>\t<title>\t<w>x<h>" (optionally filtered by owner
// substring). Used by scripts/run_baseline.sh to screenshot only the game window.
// Window titles need the Screen Recording permission for the calling app.
import CoreGraphics
import Foundation

let filter = CommandLine.arguments.count > 1 ? CommandLine.arguments[1].lowercased() : ""
let opts: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
if let list = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]] {
    for w in list {
        let owner = w[kCGWindowOwnerName as String] as? String ?? ""
        if !filter.isEmpty && !owner.lowercased().contains(filter) { continue }
        let id = w[kCGWindowNumber as String] as? Int ?? 0
        let name = w[kCGWindowName as String] as? String ?? ""
        let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
        print("\(id)\t\(owner)\t\(name)\t\(b["Width"] ?? 0)x\(b["Height"] ?? 0)")
    }
}
