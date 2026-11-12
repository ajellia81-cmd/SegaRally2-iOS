// iOS port: the app going to the background and coming back, from iOS's own
// notifications. Inside LiveContainer, SDL's application delegate never hears
// of them: the game kept running, and sending GPU work, in the background.
#import <UIKit/UIKit.h>

#include <atomic>

#include "windows.h"   // platform/compat: compat_set_focus

std::atomic<bool> g_sr2_backgrounded{false};   // set when it goes; app.cpp opens the pause menu

void platform_watch_lifecycle() {
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    void (^leave)(NSNotification*) = ^(NSNotification*) {
        compat_set_focus(false);
        compat_release_keys();
        g_sr2_backgrounded = true;
    };
    [center addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:nil usingBlock:leave];
    [center addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:nil usingBlock:leave];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) { compat_set_focus(true); }];
}
