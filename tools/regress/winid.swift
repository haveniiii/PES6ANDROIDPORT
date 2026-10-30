import CoreGraphics
import Foundation
// Prints "<window id> <x> <y> <w> <h>" for on-screen windows of the owner named in argv[1].
let owner = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "PPSSPP"
let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
for w in list {
    guard let name = w[kCGWindowOwnerName as String] as? String, name.contains(owner),
          let id = w[kCGWindowNumber as String] as? Int,
          let b = w[kCGWindowBounds as String] as? [String: Double], (b["Width"] ?? 0) > 200 else { continue }
    print(id, Int(b["X"] ?? 0), Int(b["Y"] ?? 0), Int(b["Width"] ?? 0), Int(b["Height"] ?? 0))
}
