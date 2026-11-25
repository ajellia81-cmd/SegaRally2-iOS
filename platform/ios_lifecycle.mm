// iOS port: the app going to the background and coming back, from iOS's own
// notifications. Inside LiveContainer, SDL's application delegate never hears
// of them: the game kept running, and sending GPU work, in the background.
#import <UIKit/UIKit.h>

#include <atomic>

// platform/compat/win_compat.cpp (not its windows.h: Objective-C has a BOOL of its own)
void compat_set_focus(bool focused);
void compat_release_keys();

std::atomic<bool> g_sr2_backgrounded{false};   // set when it goes; app.cpp opens the pause menu
std::atomic<bool> g_sr2_foregrounded{false};   // set when it comes back; app.cpp starts the rumble again

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
                    usingBlock:^(NSNotification*) {
                        compat_set_focus(true);
                        g_sr2_foregrounded = true;
                    }];
}

// The device's thermal state for the log (0 nominal, 1 fair, 2 serious,
// 3 critical): iOS lowers the GPU's clock as it rises, and every pass then
// takes longer at once
int platform_thermal_state() {
    return int([[NSProcessInfo processInfo] thermalState]);
}

// The screen's refresh period (app.cpp's DisplayClock): 1/120 s on ProMotion
double platform_refresh_period() {
    static double period = [] {
        NSInteger fps = UIScreen.mainScreen.maximumFramesPerSecond;
        return 1.0 / double(fps > 0 ? fps : 60);
    }();
    return period;
}

// The pads' rumble (iOS port): gamepad.cpp's strengths, the same as SDL would
// play them (the low part in the left handle, the high in the right, each
// 0..1), on haptics engines of our own. SDL's endless haptic event went dead
// after about 30 s, for good; here each buzz is a finite one, made new before
// it runs out. All of it on a queue of its own: making and starting an engine
// takes a while, and the thread that draws must not wait for it
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#include <cstdio>

static dispatch_queue_t rumble_queue() {
    static dispatch_queue_t queue = dispatch_queue_create("sr2.rumble", DISPATCH_QUEUE_SERIAL);
    return queue;
}

namespace {
struct Motor {
    CHHapticEngine* engine = nil;
    id<CHHapticPatternPlayer> player = nil;
    GCController* pad = nil;
    double started = 0;   // when the player's buzz began (CACurrentMediaTime)
};
Motor g_motor[2];
std::atomic<bool> g_motor_ok[2];
constexpr double BUZZ_LENGTH = 20.0, BUZZ_RENEW = 15.0;   // (iOS ends one at 30 s)

GCController* haptic_pad() {
    for (GCController* c in GCController.controllers)
        if (c.haptics) return c;
    return nil;
}

// The motor's engine, made and started if there is none (or iOS stopped it)
bool motor_engine(int m, GCController* pad) {
    Motor& o = g_motor[m];
    if (o.engine && g_motor_ok[m].load() && o.pad == pad) return true;
    if (o.engine) [o.engine stopWithCompletionHandler:nil];
    o = Motor{};
    o.engine = [pad.haptics createEngineWithLocality:m == 0 ? GCHapticsLocalityLeftHandle : GCHapticsLocalityRightHandle];
    if (!o.engine) return false;
    o.pad = pad;
    o.engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
        g_motor_ok[m] = false;
        printf("[rumble] engine %d stopped (%ld)\n", m, long(reason));
    };
    o.engine.resetHandler = ^{ g_motor_ok[m] = false; };
    NSError* error = nil;
    if (![o.engine startAndReturnError:&error]) {
        printf("[rumble] engine %d not started: %s\n", m, error.localizedDescription.UTF8String);
        o = Motor{};
        return false;
    }
    g_motor_ok[m] = true;
    return true;
}

void motor_stop(int m) {
    Motor& o = g_motor[m];
    if (o.player) [o.player stopAtTime:CHHapticTimeImmediate error:nil];
    o.player = nil;
}

void motor_set(int m, GCController* pad, float level) {
    if (level <= 0.0f) {
        motor_stop(m);
        return;
    }
    if (!motor_engine(m, pad)) return;
    Motor& o = g_motor[m];
    NSError* error = nil;
    double now = CACurrentMediaTime();
    if (o.player && now - o.started > BUZZ_RENEW) motor_stop(m);   // (a new buzz before this one runs out)
    CHHapticDynamicParameter* p = [[CHHapticDynamicParameter alloc]
        initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:level > 1.0f ? 1.0f : level relativeTime:0];
    if (!o.player) {
        CHHapticEvent* buzz = [[CHHapticEvent alloc]
            initWithEventType:CHHapticEventTypeHapticContinuous
                   parameters:@[[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f]]
                 relativeTime:0
                     duration:BUZZ_LENGTH];
        CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[buzz] parameters:@[] error:&error];
        o.player = pattern ? [o.engine createPlayerWithPattern:pattern error:&error] : nil;
        if (!o.player || ![o.player sendParameters:@[p] atTime:CHHapticTimeImmediate error:&error] ||
            ![o.player startAtTime:CHHapticTimeImmediate error:&error]) {
            printf("[rumble] motor %d not played: %s\n", m, error ? error.localizedDescription.UTF8String : "?");
            o.player = nil;
            g_motor_ok[m] = false;   // (a new engine next time)
            return;
        }
        o.started = now;
        return;
    }
    if (![o.player sendParameters:@[p] atTime:CHHapticTimeImmediate error:&error]) {
        o.player = nil;
        g_motor_ok[m] = false;
    }
}

std::atomic<uint64_t> g_rumble_serial{0};
}  // namespace

void platform_pad_rumble(float low, float high, int ms) {
    uint64_t serial = ++g_rumble_serial;
    dispatch_async(rumble_queue(), ^{
        if (serial != g_rumble_serial.load()) return;   // (a newer one is on its way)
        @autoreleasepool {
            GCController* pad = haptic_pad();
            if (!pad) return;
            motor_set(0, pad, low);
            motor_set(1, pad, high);
        }
    });
    // Let go when the time asked for runs out with nothing newer (as SDL does)
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(ms > 0 ? ms : 1) * NSEC_PER_MSEC), rumble_queue(), ^{
        if (serial != g_rumble_serial.load()) return;
        @autoreleasepool {
            motor_stop(0);
            motor_stop(1);
        }
    });
}

// The engines made and started now (a pad connected, the app back from the
// background), so that the first rumble does not wait for them
void platform_pad_rumble_ready() {
    dispatch_async(rumble_queue(), ^{
        @autoreleasepool {
            if (GCController* pad = haptic_pad()) motor_engine(0, pad), motor_engine(1, pad);
        }
    });
}
