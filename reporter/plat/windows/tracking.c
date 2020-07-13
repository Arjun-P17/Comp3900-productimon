#include "reporter/plat/tracking.h"

#include <processthreadsapi.h>
#include <psapi.h>
#include <shlwapi.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winuser.h>
#include <winver.h>
#include <wtsapi32.h>

#include "reporter/core/core.h"

#define STOP_MSG (WM_USER + 1)
#define STOP_W_PARAM ((WPARAM)0xDEADBEEF)
#define STOP_L_PARAM ((LPARAM)0xBADDCAFE)
#define IS_STOP_MSG(msg)                                    \
  (msg.message == STOP_MSG && msg.wParam == STOP_W_PARAM && \
   msg.lParam == STOP_L_PARAM)

static bool tracking_started = false;
static HANDLE tracking_thread = NULL;
static DWORD tracking_thread_id;
static tracking_opt_t *tracking_opts;

static HWND window_handle;
static HHOOK mouse_hook;
static HHOOK key_hook;
static HWINEVENTHOOK window_change_hook;

static HANDLE event_loop_finished;

static int get_name_from_handle(HWND hwnd, char *buf, size_t size) {
  int ret = 1;
  DWORD pid;
  GetWindowThreadProcessId(hwnd, &pid);

  HANDLE proc_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
  if (proc_handle == NULL) {
    error("OpenProcess failed: %d\n", GetLastError());
    return 1;
  }

  char path[MAX_PATH];
  if (GetModuleFileNameExA(proc_handle, NULL, path, MAX_PATH) == 0) {
    error("Failed to get executable path %d\n", GetLastError);
    goto error_close_handle;
  }
  debug("exec full path: %s\n", path);

  DWORD ver_info_size = GetFileVersionInfoSizeA(path, NULL);
  if (ver_info_size == 0) {
    debug("Failed to get version info size\n");
    PathStripPathA(path);
    snprintf(buf, size, "%s", path);
    CloseHandle(proc_handle);
    return 0;
  }

  void *version_buf = malloc(ver_info_size);
  if (version_buf == NULL) {
    error("Failed to allocate buffer\n");
    goto error_close_handle;
  }

  if (!GetFileVersionInfoA(path, 0, ver_info_size, version_buf)) {
    error("Failed to get version info\n");
    goto error_free_version_buf;
  }

  /* Get all version info translations */
  static struct LANGANDCODEPAGE {
    WORD wLanguage;
    WORD wCodePage;
  } * translate;

  int translate_size;
  VerQueryValueA(version_buf, TEXT("\\VarFileInfo\\Translation"),
                 (LPVOID *)&translate, &translate_size);

  int n_translations = translate_size / sizeof(struct LANGANDCODEPAGE);

  // NOTE: it seems like all app on my windows have one translation
  if (n_translations < 1) {
    debug("Failed to get any version translations, using exec name instead\n");
    goto use_exec_name;
  }

  char query_str[64];
  snprintf(query_str, 64, "\\StringFileInfo\\%04x%04x\\FileDescription",
           translate[0].wLanguage, translate[0].wCodePage);

  char *file_description;
  /* Get program description from the version info */
  if (!VerQueryValueA(version_buf, query_str, (LPVOID *)&file_description,
                      NULL)) {
    debug(
        "Failed to get description from version info, use exec name instead\n");
    goto use_exec_name;
  }
  snprintf(buf, size, "%s", file_description);
  goto success;

use_exec_name:
  PathStripPathA(path);
  snprintf(buf, size, "%s", path);
success:
  ret = 0;

error_free_version_buf:
  free(version_buf);
error_close_handle:
  CloseHandle(proc_handle);
  return ret;
}

static VOID CALLBACK callback(HWINEVENTHOOK hWinEventHook, DWORD dwEvent,
                              HWND hwnd, LONG idObject, LONG idChild,
                              DWORD dwEventThread, DWORD dwmsEventTime) {
  debug("Callback: event %ld, hwnd %d, idObject %ld, idChild %ld, time %ld\n",
        dwEvent, hwnd, idObject, idChild, dwmsEventTime);

  static char prog_name[512] = {0};
  char new_prog_name[512];
  if (get_name_from_handle(hwnd, new_prog_name, 512)) {
    // TODO handle error here
    // we either stop tracking or send an event to switch to an "unknwon"
    // program
    // otherwise viewer will think the user was using the old program all
    // the time...
    // same on linux
    error("Failed to get a name for new window\n");
    return;
  }
  debug("=======> %s <=======\n", new_prog_name);
  if (strcmp(prog_name, new_prog_name) == 0) {
    debug("Switch event triggered but program name is the same\n");
    return;
  }
  strcpy(prog_name, new_prog_name);
  printf("Got new program: %s\n", prog_name);
  SendWindowSwitchEvent(prog_name);
}
static LRESULT CALLBACK keystroke_callback(_In_ int nCode, _In_ WPARAM wParam,
                                           _In_ LPARAM lParam) {
  /* following what the documentation says */
  if (nCode < 0) return CallNextHookEx(NULL, nCode, wParam, lParam);

  /* only do reporting for key down event */
  if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
    HandleKeystroke();
  }
  return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static LRESULT CALLBACK mouseclick_callback(_In_ int nCode, _In_ WPARAM wParam,
                                            _In_ LPARAM lParam) {
  /* following what the documentation says */
  if (nCode < 0) return CallNextHookEx(NULL, nCode, wParam, lParam);

  /* only do reporting for key down event */
  if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
      wParam == WM_MOUSEWHEEL) {
    HandleMouseClick();
  }
  return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static int install_hooks(bool register_session_ntfn) {
  if (tracking_opts->foreground_program) {
    // https://docs.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook?redirectedfrom=MSDN
    window_change_hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, callback, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    debug("SetWinEventHook got %d\n", window_change_hook);
    if (window_change_hook == NULL) {
      error("Failed to set hook for keyboard: %d\n", GetLastError());
      return 1;
    }
  }

  if (tracking_opts->keystroke) {
    key_hook = SetWindowsHookExA(WH_KEYBOARD_LL, keystroke_callback, NULL, 0);
    debug("SetWindowsHookExA for keyboard got %d\n", key_hook);
    if (key_hook == NULL) {
      error("Failed to set hook for keyboard: %d\n", GetLastError());
      return 1;
    }
  }

  if (tracking_opts->mouse_click) {
    mouse_hook = SetWindowsHookExA(WH_MOUSE_LL, mouseclick_callback, NULL, 0);
    debug("SetWindowsHookExA for mouseclick got %d\n", mouse_hook);
    if (mouse_hook == NULL) {
      error("Failed to set hook for mouseclick: %d\n", GetLastError());
      return 1;
    }
  }

  /* register for lock/unlock and login/logout events
   * doing this twice within the same thread can undo the effect
   * thus the boolean param
   */
  if (register_session_ntfn &&
      !WTSRegisterSessionNotification(window_handle, NOTIFY_FOR_THIS_SESSION)) {
    error("Failed to regitster for session change notifications: %d\n",
          GetLastError());
    return 1;
  }
  return 0;
}

static void suspend_tracking() {
  SendStopTrackingEvent();

  if (tracking_opts->foreground_program && !UnhookWinEvent(window_change_hook))
    error("Failed to remove window change hook: %d\n", GetLastError());

  if (tracking_opts->keystroke && !UnhookWindowsHookEx(key_hook))
    error("Failed to remove key hook: %d\n", GetLastError());

  if (tracking_opts->mouse_click && !UnhookWindowsHookEx(mouse_hook))
    error("Failed to remove mosue hook: %d\n", GetLastError());

  tracking_started = false;
}

static void resume_tracking() {
  SendStartTrackingEvent();

  install_hooks(false);

  tracking_started = true;
}

static LRESULT CALLBACK session_change_callback(WPARAM type) {
  switch (type) {
    case WTS_SESSION_LOCK:
      debug("system about to lock, suspend tracking...\n");
      suspend_tracking();
      break;
    case WTS_SESSION_UNLOCK:
      debug("login detected, resume tracking\n");
      resume_tracking();
      break;
    case WTS_SESSION_LOGOFF:
      // TODO
      break;
  }
}

static DWORD WINAPI tracking_loop(_In_ LPVOID lpParameter) {
  SendStartTrackingEvent();
  debug("Tracking started\n");

  /* create a hidden message window */
  window_handle = CreateWindowExA(WS_EX_ACCEPTFILES, "Button", "null", 0, 0, 0,
                                  0, 0, HWND_MESSAGE, NULL, NULL, NULL);
  if (window_handle == NULL) {
    error("Failed to create a message window: %d\n", GetLastError());
    return 0;  // use synchronisation primitives here to notify the failure to
               // start_tracking
  }

  if (install_hooks(true)) {
    return 1;  // TODO use sync primitives to have start_tracking wait for this
               // failure
  }

  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0)) {
    if (IS_STOP_MSG(msg)) break;

    if (msg.message == WM_WTSSESSION_CHANGE) {
      session_change_callback(msg.wParam);
    }

    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  SendStopTrackingEvent();
  debug("Tracking stopped\n");
  return 0;
}

int init_tracking() { return 0; }

int start_tracking(tracking_opt_t *opts) {
  if (tracking_started) {
    error("tracking started already!\n");
    return 1;
  }

  tracking_opts = opts;
  if (!(opts->foreground_program || opts->mouse_click || opts->keystroke)) {
    debug("Nothing to be tracked, not doing anything\n");
    return 1;
  }

  tracking_thread =
      CreateThread(NULL, 0, tracking_loop, NULL, 0, &tracking_thread_id);
  if (tracking_thread == NULL) {
    error("Failed to create tracking thread\n");
    return 1;
  }

  tracking_started = true;
  return 0;
}

void stop_tracking() {
  if (!tracking_started) {
    error("tracking stopped already!\n");
    return;
  }

  if (!PostThreadMessageA(tracking_thread_id, STOP_MSG, STOP_W_PARAM,
                          STOP_L_PARAM))
    error("Failed to send stop message: %lu\n", GetLastError());

  WaitForSingleObject(tracking_thread, INFINITE);
  tracking_started = false;

  tracking_thread = NULL;
  tracking_thread_id = 0;
  tracking_opts = NULL;
  window_handle = NULL;
  mouse_hook = NULL;
  key_hook = NULL;
  window_change_hook = NULL;
  return;
}

bool is_tracking() { return tracking_started; }

void run_event_loop() {
  event_loop_finished = CreateSemaphore(NULL, 0, 1, NULL);
  if (event_loop_finished == NULL) {
    error("Failed to create sem: %d\n", GetLastError());
  }
  WaitForSingleObject(event_loop_finished, INFINITE);
}

void stop_event_loop() {
  if (!ReleaseSemaphore(event_loop_finished, 1, NULL)) {
    error("ReleaseSemaphore error: %d\n", GetLastError());
  }
}