// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The system's side of the iOS and macOS apps (docs/APPLE.md), for src/gui/apple.m: the document picker SDL lacks on
// iOS, and the log handed to the user. The menu and the runtime are SDL's window; this only presents over it.
import Foundation

#if os(iOS)
import UIKit
import UniformTypeIdentifiers

/// apple.m's callback: the path picked, nil for none
public typealias SNXPicked = @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<CChar>?) -> Void

/// The view controller on top of the window's: one presenting another can't present a third
private func presenter(_ window: UnsafeMutableRawPointer) -> UIViewController? {
    var top = Unmanaged<UIWindow>.fromOpaque(window).takeUnretainedValue().rootViewController
    while let shown = top?.presentedViewController { top = shown }
    return top
}

/// The picker's delegate, kept while it shows: UIKit holds its delegate weakly
private final class Picker: NSObject, UIDocumentPickerDelegate {
    static var showing: Picker?
    let done: SNXPicked
    let data: UnsafeMutableRawPointer?

    init(done: SNXPicked, data: UnsafeMutableRawPointer?) {
        self.done = done
        self.data = data
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        // a dump is read where it is, by its path, all the app's run: its scope stays open
        guard let url = urls.first else { return finish(nil) }
        _ = url.startAccessingSecurityScopedResource()
        finish(url.path)
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) { finish(nil) }

    private func finish(_ path: String?) {
        if let path {
            path.withCString { done(data, $0) }
        } else {
            done(data, nil)
        }
        Picker.showing = nil
    }
}

@_cdecl("snx_apple_pick")
public func snxApplePick(_ window: UnsafeMutableRawPointer, _ folder: Bool, _ done: SNXPicked,
                         _ data: UnsafeMutableRawPointer?) -> Bool {
    guard Picker.showing == nil, let top = presenter(window) else { return false }
    // opened in place, not copied: a dump is many gigabytes
    let picker = UIDocumentPickerViewController(forOpeningContentTypes: folder ? [.folder] : [.item], asCopy: false)
    let delegate = Picker(done: done, data: data)
    picker.delegate = delegate
    picker.allowsMultipleSelection = false
    Picker.showing = delegate
    top.present(picker, animated: true)
    return true
}

@_cdecl("snx_apple_share")
public func snxAppleShare(_ window: UnsafeMutableRawPointer, _ path: UnsafePointer<CChar>) -> Bool {
    guard let top = presenter(window) else { return false }
    let sheet = UIActivityViewController(activityItems: [URL(fileURLWithPath: String(cString: path))],
                                         applicationActivities: nil)
    // an iPad shows it as a popover, which needs somewhere to point
    if let popover = sheet.popoverPresentationController {
        popover.sourceView = top.view
        popover.sourceRect = CGRect(x: top.view.bounds.midX, y: top.view.bounds.midY, width: 0, height: 0)
        popover.permittedArrowDirections = []
    }
    top.present(sheet, animated: true)
    return true
}

#elseif os(macOS)
import AppKit

/// SDL has macOS's own file and folder dialogs: apple.m asks for none here
@_cdecl("snx_apple_pick")
public func snxApplePick(_ window: UnsafeMutableRawPointer, _ folder: Bool,
                         _ done: @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<CChar>?) -> Void,
                         _ data: UnsafeMutableRawPointer?) -> Bool {
    false
}

/// The log, selected in a Finder window, to send or keep
@_cdecl("snx_apple_share")
public func snxAppleShare(_ window: UnsafeMutableRawPointer, _ path: UnsafePointer<CChar>) -> Bool {
    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: String(cString: path))])
    return true
}
#endif
