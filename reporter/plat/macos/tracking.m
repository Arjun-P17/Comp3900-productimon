#include "reporter/plat/tracking.h"

#import <AppKit/AppKit.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#include "reporter/core/core.h"

static tracking_opt_t *tracking_opts = NULL;
static bool tracking_started = false;
static pthread_mutex_t tracking_mutex;

@interface Tracking : NSObject
- (void)init_observers:(tracking_opt_t *)opts;
- (void)remove_observers;
@end

@implementation Tracking
id event_handler;
NSNotificationCenter *shared_center;
NSDistributedNotificationCenter *distributed_center;

- (void)suspend_handler:(NSNotification *)note {
  pthread_mutex_lock(&tracking_mutex);
  if (!tracking_started) {
    pthread_mutex_unlock(&tracking_mutex);
    return;
  }
  NSLog(@"%@", note);
  SendStopTrackingEvent();
  tracking_started = false;
  pthread_mutex_unlock(&tracking_mutex);
}

- (void)resume_handler:(NSNotification *)note {
  pthread_mutex_lock(&tracking_mutex);
  if (tracking_started) {
    pthread_mutex_unlock(&tracking_mutex);
    return;
  }
  NSLog(@"%@", note);
  SendStartTrackingEvent();
  tracking_started = true;
  pthread_mutex_unlock(&tracking_mutex);
}

- (void)init_observers:(tracking_opt_t *)opts {
  shared_center = [[NSWorkspace sharedWorkspace] notificationCenter];
  distributed_center = [NSDistributedNotificationCenter defaultCenter];
  if (opts->foreground_program) {
    [shared_center addObserver:self
                      selector:@selector(app_switch_handler:)
                          name:@"NSWorkspaceDidActivateApplicationNotification"
                        object:nil];
  }

  NSEventMask mask = 0;
  if (opts->keystroke) mask |= NSEventMaskKeyDown;
  if (opts->keystroke)
    mask |= NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown | NSEventMaskScrollWheel;
  event_handler =
      [NSEvent addGlobalMonitorForEventsMatchingMask:mask
                                             handler:^(NSEvent *event) {
                                               switch (event.type) {
                                                 case NSEventTypeLeftMouseDown:
                                                 case NSEventTypeRightMouseDown:
                                                 case NSEventTypeScrollWheel:
                                                   HandleMouseClick();
                                                   break;
                                                 case NSEventTypeKeyDown:
                                                   HandleKeystroke();
                                                   break;
                                                 default:
                                                   NSLog(@"Unexpected event %@\n", event);
                                                   break;
                                               }
                                             }];
  NSLog(@"Got event handler %@\n", event_handler);

  [distributed_center addObserver:self
                         selector:@selector(suspend_handler:)
                             name:@"com.apple.screenIsLocked"
                           object:NULL];
  [distributed_center addObserver:self
                         selector:@selector(resume_handler:)
                             name:@"com.apple.screenIsUnlocked"
                           object:NULL];

  // TODO NSWorkspaceWillPowerOffNotification for power off
}

- (void)remove_observers {
  [shared_center removeObserver:self];
  [distributed_center removeObserver:self];
  [NSEvent removeMonitor:event_handler];
}

- (void)app_switch_handler:(NSNotification *)notification {
  NSRunningApplication *app = notification.userInfo[@"NSWorkspaceApplicationKey"];
  const char *app_name = [app.localizedName UTF8String];
  prod_debug("Switched to %s\n", app_name);
  SendWindowSwitchEvent((char *)app_name);
}
@end

static Tracking *tracking;

void run_event_loop() {
  tracking = [Tracking new];
  [NSApplication sharedApplication];
  [NSApp run];
}

void stop_event_loop() { [NSApp terminate:nil]; }

int init_tracking() {
  pthread_mutex_init(&tracking_mutex, NULL);
  return 0;
}

int start_tracking(tracking_opt_t *opts) {
  pthread_mutex_lock(&tracking_mutex);
  if (tracking_started) {
    prod_error("Tracking started already!\n");
    pthread_mutex_unlock(&tracking_mutex);
    return 1;
  }
  tracking_opts = opts;

  SendStartTrackingEvent();
  [tracking init_observers:tracking_opts];
  prod_debug("Tracking started\n");

  tracking_started = true;
  pthread_mutex_unlock(&tracking_mutex);
  return 0;
}

void stop_tracking() {
  pthread_mutex_lock(&tracking_mutex);
  if (!tracking_started) {
    prod_error("Tracking stopped already!\n");
    pthread_mutex_unlock(&tracking_mutex);
    return;
  }

  [tracking remove_observers];
  SendStopTrackingEvent();
  prod_debug("Tracking stopped\n");
  tracking_started = false;
  pthread_mutex_unlock(&tracking_mutex);
}

bool is_tracking() { return tracking_started; }
