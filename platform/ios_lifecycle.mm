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

// A knock felt in the controller's hands when the car hits something (iOS
// port; runtime.cpp hears the crash sounds, app_input.cpp calls this). SDL's
// rumble is one endless pattern whose strength it changes and which it stops
// after each quiet spell: the knocks at low speed, coming after such a spell,
// were logged as sent at full and still not felt. Each knock here is a short
// pattern of its own, played on an engine of our own: a sharp tap, then a
// fading buzz
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#include <cstdio>

static CHHapticEngine* g_knock_engine = nil;   // (ARC: CMakeLists.txt)
static GCController* g_knock_pad = nil;
static std::atomic<bool> g_knock_ok{false};

static void knock_now(float strength, int where) {
    if (strength <= 0.0f) return;
    {
        @autoreleasepool {
            GCController* pad = nil;
            for (GCController* c in GCController.controllers)
                if (c.haptics) {
                    pad = c;
                    break;
                }
            if (!pad) return;
            static int logged = 0;   // (diagnosis: where the pad can vibrate)
            if (!logged++) {
                NSMutableString* list = [NSMutableString string];
                for (GCHapticsLocality l in pad.haptics.supportedLocalities) [list appendFormat:@"%@ ", l];
                printf("[knock] %s: haptics at %s\n", pad.vendorName.UTF8String, list.UTF8String);
            }
            NSError* error = nil;
            static int engine_where = -1;
            if (!g_knock_engine || !g_knock_ok.load() || g_knock_pad != pad || engine_where != where) {
                if (g_knock_engine) [g_knock_engine stopWithCompletionHandler:nil];
                engine_where = where;
                g_knock_engine = [pad.haptics createEngineWithLocality:where == 1 ? GCHapticsLocalityHandles
                                                                     : where == 2 ? GCHapticsLocalityLeftHandle
                                                                                  : GCHapticsLocalityDefault];
                g_knock_pad = pad;
                if (!g_knock_engine) {
                    printf("[knock] no haptics engine\n");
                    return;
                }
                __weak CHHapticEngine* engine = g_knock_engine;
                g_knock_engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
                    g_knock_ok = false;
                    printf("[knock] engine stopped (%ld)\n", long(reason));
                };
                g_knock_engine.resetHandler = ^{
                    g_knock_ok = [engine startAndReturnError:nil];
                };
                if (![g_knock_engine startAndReturnError:&error]) {
                    printf("[knock] engine not started: %s\n", error.localizedDescription.UTF8String);
                    g_knock_engine = nil;
                    return;
                }
                g_knock_ok = true;
            }
            float s = strength > 1.0f ? 1.0f : strength;
            CHHapticEvent* tap = [[CHHapticEvent alloc]
                initWithEventType:CHHapticEventTypeHapticTransient
                       parameters:@[[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:s],
                                    [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f]]
                     relativeTime:0];
            CHHapticEvent* buzz = [[CHHapticEvent alloc]
                initWithEventType:CHHapticEventTypeHapticContinuous
                       parameters:@[[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:s],
                                    [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.2f]]
                     relativeTime:0
                         duration:0.25];
            CHHapticParameterCurve* fade = [[CHHapticParameterCurve alloc]
                initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                      controlPoints:@[[[CHHapticParameterCurveControlPoint alloc] initWithRelativeTime:0 value:1.0f],
                                      [[CHHapticParameterCurveControlPoint alloc] initWithRelativeTime:0.1 value:1.0f],
                                      [[CHHapticParameterCurveControlPoint alloc] initWithRelativeTime:0.25 value:0.0f]]
                       relativeTime:0];
            CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[tap, buzz] parameterCurves:@[fade] error:&error];
            if (!pattern) {
                printf("[knock] no pattern: %s\n", error.localizedDescription.UTF8String);
                return;
            }
            id<CHHapticPatternPlayer> player = [g_knock_engine createPlayerWithPattern:pattern error:&error];
            if (!player || ![player startAtTime:CHHapticTimeImmediate error:&error]) {
                printf("[knock] not played: %s\n", error ? error.localizedDescription.UTF8String : "?");
                g_knock_ok = false;   // a new engine next time
                return;
            }
        }
    }
}

// Called from the input poll, on the thread that draws: the work above goes to
// a queue of its own. Making and starting the engine on the first knock took
// that thread for a while (a long stutter at the first crash of a race)
void platform_crash_haptic(float strength, int where) {
    if (strength <= 0.0f) return;
    static dispatch_queue_t queue = dispatch_queue_create("sr2.knock", DISPATCH_QUEUE_SERIAL);
    dispatch_async(queue, ^{
        knock_now(strength, where);
    });
}
