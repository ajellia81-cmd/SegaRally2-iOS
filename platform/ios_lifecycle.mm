// iOS port: the app going to the background and coming back, from iOS's own
// notifications. Inside LiveContainer, SDL's application delegate never hears
// of them: the game kept running, and sending GPU work, in the background.
#import <UIKit/UIKit.h>

#include <atomic>

// platform/compat/win_compat.cpp (not its windows.h: Objective-C has a BOOL of its own)
void compat_set_focus(bool focused);
void compat_release_keys();

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

// The device's thermal state for the log (0 nominal, 1 fair, 2 serious,
// 3 critical): iOS lowers the GPU's clock as it rises, and every pass then
// takes longer at once
int platform_thermal_state() {
    return int([[NSProcessInfo processInfo] thermalState]);
}
