#include "reporter/plat/tracking.h"

// TODO check for mem leaks
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "inhibit.h"
#include "reporter/core/cgo/cgo.h"

#ifdef __linux__

#define XCLIENTMESSAGE_MAGIC_STOP "Productimon is great"

static Atom active_window_prop;
static Display *display;
static Window root_window;

static pthread_t tracking_thread;
static pthread_mutex_t tracking_mutex;
static sem_t event_loop_finished;

static int xi_major_opcode;

static void suspend_tracking();
static void resume_tracking();

static int xlib_error_handler(Display *display, XErrorEvent *event) {
  char buf[256];
  XGetErrorText(display, event->type, buf, 256);
  prod_error("xlib error: %s\n", buf);
  XCloseDisplay(display);
  exit(1);
}

static int get_current_window_name(Display *display, Window root, char *buf,
                                   int size) {
  Atom actual_type_ret;
  int actual_format_ret;
  unsigned long nitems_return;
  unsigned long bytes_after_return;
  unsigned char *prop_return;

  /* Get current active window's ID */
  int ret =
      XGetWindowProperty(display, root, active_window_prop, 0, 4, 0,
                         AnyPropertyType, &actual_type_ret, &actual_format_ret,
                         &nitems_return, &bytes_after_return, &prop_return);

  if (ret || actual_format_ret != 32 || nitems_return != 1) {
    prod_debug("atom returned %s\n", XGetAtomName(display, actual_type_ret));
    prod_error("Failed to get active window\n");
    return -1;
  }
  /*prod_debug("fmt ret: %d, %lu items, %lu bytes remains, prop @ %p\n", */
  /*         actual_format_ret, nitems_return, bytes_after_return,
   *         prop_return); */

  Window active_window = *(Window *)prop_return;
  prod_debug("Got active window ID 0x%lX\n", active_window);
  XFree(prop_return);

  if (active_window == 0) {
    strncpy(buf, "Desktop", size);
    return 0;
  }

  /* Get its WM_CLASS class name */
  XClassHint class_hint;
  memset(&class_hint, 0, sizeof(class_hint));
  XGetClassHint(display, active_window, &class_hint);
  prod_debug("WM_CLASS: %s\n", class_hint.res_class);
  if (class_hint.res_class != NULL) {
    snprintf(buf, size, "%s", class_hint.res_class);
  } else if (class_hint.res_name != NULL) {
    prod_debug("fallback to res_name: %s\n", class_hint.res_name);
    snprintf(buf, size, "%s", class_hint.res_name);
  } else {
    buf[0] = '\0';  // core will set it to Unknown
  }
  XFree(class_hint.res_name);
  XFree(class_hint.res_class);
  return 0;
}

static void handle_window_change(Display *display, Window window) {
  char prog_name[512];

  get_current_window_name(display, window, prog_name, 512);

  ProdCoreSwitchWindow(prog_name);
}

static int check_x_input_lib(Display *display) {
  int unused1, unused2;
  xi_major_opcode = 0;
  if (!XQueryExtension(display, "XInputExtension", &xi_major_opcode, &unused1,
                       &unused2)) {
    prod_error("X Input extension not available\n");
    return 1;
  }
  /* request XI 2.0 */
  int major = 2, minor = 0;
  int queryResult = XIQueryVersion(display, &major, &minor);
  if (queryResult == BadRequest) {
    prod_error("Need X Input 2.0 (got %d.%d)\n", major, minor);
    return 1;
  } else if (queryResult != Success) {
    prod_error("XIQueryVersion failed\n");
    return 1;
  }
  prod_debug("X Input Extension (%d.%d)\n", major, minor);
  return 0;
}

static void *event_loop(UNUSED void *arg) {
  prod_debug("Starting tracking with opts: %d|%d|%d (prog|mouse|key)\n",
             get_option("foreground_program"), get_option("mouse_click"),
             get_option("keystroke"));

  // TODO: this thread doesn't exit if foreground tracking is not on
  long event_mask = 0;
  if (get_option("foreground_program")) {
    // receive property change events
    event_mask |= PropertyChangeMask;
    XSelectInput(display, root_window, event_mask);
  }

  XIEventMask xi_event_mask;
  if (get_option("keystroke") || get_option("mouse_click")) {
    unsigned char xi_mask_val[(XI_LASTEVENT + 7 / 8)] = {0};
    xi_event_mask.deviceid = XIAllMasterDevices;
    xi_event_mask.mask_len = sizeof(xi_mask_val);
    xi_event_mask.mask = xi_mask_val;
    if (get_option("keystroke")) XISetMask(xi_mask_val, XI_RawKeyPress);
    if (get_option("mouse_click")) XISetMask(xi_mask_val, XI_RawButtonPress);
    XISelectEvents(display, root_window, &xi_event_mask, 1);
  }

  XEvent event;
  XGenericEventCookie *cookie = (XGenericEventCookie *)&event.xcookie;

  while (1) {
    XNextEvent(display, &event);
    if (event.type == ClientMessage &&
        memcmp(event.xclient.data.b, XCLIENTMESSAGE_MAGIC_STOP, 20) == 0)
      break;
    if (event.type == PropertyNotify &&
        event.xproperty.atom == active_window_prop)
      handle_window_change(display, root_window);
    if (XGetEventData(display, cookie) && cookie->type == GenericEvent &&
        cookie->extension == xi_major_opcode) {
      if (cookie->evtype == XI_RawKeyPress) {
        ProdCoreHandleKeystroke();
      } else if (cookie->evtype == XI_RawButtonPress) {
        ProdCoreHandleMouseClick();
      }
    }
  }

  return NULL;
}

int init_tracking() {
  if (strcmp(getenv("XDG_SESSION_TYPE"), "x11")) {
    prod_error(
        "Not using x11 as display server, tracking may not be accurate\n");
  }
  pthread_mutex_init(&tracking_mutex, NULL);
  XSetErrorHandler(xlib_error_handler);
  return 0;
}

static int start_tracking_impl(bool inhibit) {
  pthread_mutex_lock(&tracking_mutex);
  if (ProdCoreIsTracking()) {
    prod_error("Tracking tracking_started already!\n");
    goto exit_success;
  }

  ProdCoreStartTracking();

  // open connection to the X server
  display = XOpenDisplay(NULL);
  if (display == NULL) {
    prod_error("Cannot open connection to X server\n");
    goto exit_error;
  }

  if (check_x_input_lib(display)) {
    xi_major_opcode = 0;
    prod_error("X Input not available, no mouse/key tracking\n");
  }

  // init Atoms
  active_window_prop = XInternAtom(display, "_NET_ACTIVE_WINDOW", 0);

  // get root window
  root_window = XDefaultRootWindow(display);

  if (!(get_option("foreground_program") || get_option("mouse_click") ||
        get_option("keystroke"))) {
    prod_debug("Nothing to be tracked, not doing anything\n");
    goto exit_success;
  }

  if ((get_option("mouse_click") || get_option("keystroke")) &&
      !xi_major_opcode) {
    prod_error("Requested mouse/key stats but X input lib not available!\n");
    goto exit_error;
  }

  if (inhibit && init_inhibit(suspend_tracking, resume_tracking)) {
    goto exit_error;
  }

  int ret = pthread_create(&tracking_thread, NULL, event_loop, display);
  if (ret) {
    perror("Cannot create the tracking thread");
    if (inhibit) wait_inhibit_cleanup();
    goto exit_error;
  }
  printf("Tracking started\n");

exit_success:
  pthread_mutex_unlock(&tracking_mutex);
  return 0;

exit_error:
  ProdCoreStopTracking();
  pthread_mutex_unlock(&tracking_mutex);
  return 1;
}

static void stop_tracking_impl(bool suspend) {
  pthread_mutex_lock(&tracking_mutex);
  if (!ProdCoreIsTracking()) {
    prod_error("Tracking not started, not doing anything\n");
    goto exit;
  }
  prod_debug("Stopping tracking\n");

  if (!suspend) {
    wait_inhibit_cleanup();
  }

  XEvent event;
  memset(&event, 0, sizeof(event));
  event.type = ClientMessage;
  /* view event data as 20 bytes, need this otherwise xlib will complain */
  event.xclient.format = 8;
  memcpy(event.xclient.data.b, XCLIENTMESSAGE_MAGIC_STOP, 20);
  /* manually send a ClientMessage event to the root window
   * in case the tracking thread is blocked at XNextEvent */
  if (!XSendEvent(display, root_window, False, PropertyChangeMask, &event))
    prod_error(
        "Failed to send event to root_window "
        "to unblock the tracking thread\n");
  XFlush(display);

  int ret = pthread_join(tracking_thread, NULL);
  if (ret) {
    perror("Cannot join the tracking thread");
    goto exit;
  }

  XCloseDisplay(display);
  display = NULL;

  ProdCoreStopTracking();
  printf("Tracking stopped\n");

exit:
  pthread_mutex_unlock(&tracking_mutex);
}

int start_tracking() { return start_tracking_impl(true); }

void stop_tracking() { stop_tracking_impl(false); }

static void suspend_tracking() { stop_tracking_impl(true); }

static void resume_tracking() {
  /* this is called from the inhibit module,
   * no need to init it again */
  start_tracking_impl(false);
}

void run_event_loop() {
  if (sem_init(&event_loop_finished, 0, 0)) {
    perror("Failed into init sem");
  }
  sem_wait(&event_loop_finished);
}

void stop_event_loop() { sem_post(&event_loop_finished); }

#else
#error "This code only works on Linux"
#endif /* #ifdef __linux__ */
