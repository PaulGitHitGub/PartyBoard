// Menu > Send Logs (macOS): zips the latest PartyBoard sessions' logs, and any
// PartyBoard crash reports from the last few days, into Downloads, then opens
// the system Share window (AirDrop, Messages, Mail) so a player can send them
// to whoever is helping. Nothing is sent unless the player picks someone.
#import <AppKit/AppKit.h>

#include "port/send_logs.h"

static const NSUInteger kSessions = 3;              // latest launches, this one included
static const NSTimeInterval kCrashWindow = 3 * 86400; // crash reports this recent

static NSURL *BuildArchive(void)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *home = fm.homeDirectoryForCurrentUser;
    NSURL *logs = [home URLByAppendingPathComponent:@"Library/Logs/PartyBoard" isDirectory:YES];
    NSURL *reports = [home URLByAppendingPathComponent:@"Library/Logs/DiagnosticReports" isDirectory:YES];

    NSDateFormatter *fmt = [NSDateFormatter new];
    fmt.dateFormat = @"yyyy-MM-dd HH-mm";
    NSString *name = [NSString stringWithFormat:@"PartyBoard Logs %@", [fmt stringFromDate:NSDate.date]];
    NSURL *staging = [[NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES]
        URLByAppendingPathComponent:name isDirectory:YES];
    [fm removeItemAtURL:staging error:nil];
    if (![fm createDirectoryAtURL:staging withIntermediateDirectories:YES attributes:nil error:nil]) return nil;

    // Session folders are named by start time, so the last ones by name are the newest.
    NSArray<NSString *> *sessions = [[fm contentsOfDirectoryAtPath:logs.path error:nil]
        sortedArrayUsingSelector:@selector(compare:)];
    NSUInteger copied = 0;
    for (NSString *entry in sessions.reverseObjectEnumerator) {
        BOOL dir = NO;
        NSURL *src = [logs URLByAppendingPathComponent:entry];
        if (![fm fileExistsAtPath:src.path isDirectory:&dir] || !dir) continue;
        [fm copyItemAtURL:src toURL:[staging URLByAppendingPathComponent:entry] error:nil];
        if (++copied == kSessions) break;
    }

    NSURL *crashes = [staging URLByAppendingPathComponent:@"Crash reports" isDirectory:YES];
    for (NSString *entry in [fm contentsOfDirectoryAtPath:reports.path error:nil]) {
        if (![entry hasPrefix:@"PartyBoard"]) continue;
        NSURL *src = [reports URLByAppendingPathComponent:entry];
        NSDate *modified = [fm attributesOfItemAtPath:src.path error:nil].fileModificationDate;
        if (!modified || -modified.timeIntervalSinceNow > kCrashWindow) continue;
        [fm createDirectoryAtURL:crashes withIntermediateDirectories:YES attributes:nil error:nil];
        [fm copyItemAtURL:src toURL:[crashes URLByAppendingPathComponent:entry] error:nil];
    }

    NSURL *downloads = [fm URLsForDirectory:NSDownloadsDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *zip = [downloads URLByAppendingPathComponent:[name stringByAppendingPathExtension:@"zip"]];
    [fm removeItemAtURL:zip error:nil];
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/ditto"];
    task.arguments = @[ @"-c", @"-k", @"--norsrc", @"--noextattr", @"--noacl", @"--keepParent", staging.path, zip.path ];
    BOOL ok = [task launchAndReturnError:nil];
    if (ok) {
        [task waitUntilExit];
        ok = task.terminationStatus == 0;
    }
    [fm removeItemAtURL:staging error:nil];
    return ok ? zip : nil;
}

void PartyBoard_SendLogs(void)
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSURL *zip = BuildArchive();
        dispatch_async(dispatch_get_main_queue(), ^{
            if (zip == nil) return;
            // Shown in Finder either way, so the file is easy to find if the
            // Share window is dismissed.
            [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ zip ]];
            NSWindow *window = NSApp.keyWindow ?: NSApp.mainWindow ?: NSApp.windows.firstObject;
            NSView *view = window.contentView;
            if (view == nil) return;
            [NSApp activateIgnoringOtherApps:YES];
            NSSharingServicePicker *picker = [[NSSharingServicePicker alloc] initWithItems:@[ zip ]];
            NSRect bounds = view.bounds;
            NSRect anchor = NSMakeRect(NSMidX(bounds) - 1, NSMidY(bounds) - 1, 2, 2);
            [picker showRelativeToRect:anchor ofView:view preferredEdge:NSRectEdgeMinY];
        });
    });
}
