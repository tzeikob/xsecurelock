/*
Copyright 2014 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <X11/X.h>     // for Success, None, Atom, KBBellPitch
#include <X11/Xlib.h>  // for DefaultScreen, Screen, XFree, True
#include <locale.h>    // for NULL, setlocale, LC_CTYPE, LC_TIME
#include <stdio.h>
#include <stdlib.h>      // for free, rand, mblen, size_t, EXIT_...
#include <string.h>      // for strlen, memcpy, memset, strcspn
#include <sys/select.h>  // for timeval, select, fd_set, FD_SET
#include <sys/time.h>    // for gettimeofday, timeval
#include <time.h>        // for time, nanosleep, localtime_r
#include <unistd.h>      // for close, _exit, dup2, pipe, dup

#if __STDC_VERSION__ >= 199901L
#include <inttypes.h>
#include <stdint.h>
#endif

#ifdef HAVE_XFT_EXT
#include <X11/Xft/Xft.h>             // for XftColorAllocValue, XftColorFree
#include <X11/extensions/Xrender.h>  // for XRenderColor, XGlyphInfo
#include <fontconfig/fontconfig.h>   // for FcChar8
#endif

#ifdef HAVE_XKB_EXT
#include <X11/XKBlib.h>             // for XkbFreeKeyboard, XkbGetControls
#include <X11/extensions/XKB.h>     // for XkbUseCoreKbd, XkbGroupsWrapMask
#include <X11/extensions/XKBstr.h>  // for _XkbDesc, XkbStateRec, _XkbControls
#endif

#include "../env_info.h"          // for GetHostName, GetUserName
#include "../env_settings.h"      // for GetIntSetting, GetStringSetting
#include "../logging.h"           // for Log, LogErrno
#include "../mlock_page.h"        // for MLOCK_PAGE
#include "../util.h"              // for explicit_bzero
#include "../wait_pgrp.h"         // for WaitPgrp
#include "../wm_properties.h"     // for SetWMProperties
#include "../xscreensaver_api.h"  // for ReadWindowID
#include "authproto.h"            // for WritePacket, ReadPacket, PTYPE_R...
#include "monitors.h"             // for Monitor, GetPrimaryMonitor, IsMonitorC...

#if __STDC_VERSION__ >= 201112L
#define STATIC_ASSERT(state, message) _Static_assert(state, message)
#else
#define STATIC_ASSERT(state, message) \
  extern int statically_asserted(int assertion[(state) ? 1 : -1]);
#endif

//! Number of args.
int argc;

//! Args.
char *const *argv;

//! The authproto helper to use.
const char *authproto_executable;

//! The maximum time to wait at a prompt for user input in seconds.
int prompt_timeout;

//! The prompt input mode, hidden or asterisks
const char *password_prompt;

//! The local hostname.
char hostname[256];

//! The username to authenticate as.
char username[256];

//! The X11 display.
Display *display;

//! The X11 window provided by main. Provided from $XSCREENSAVER_WINDOW.
Window main_window;

//! The main_window's parent. Used to create per-monitor siblings.
Window parent_window;

//! The X11 core font for the PAM messages.
XFontStruct *core_font;

#ifdef HAVE_XFT_EXT
//! The Xft font for the PAM messages.
XftColor xft_color_foreground;
XftColor xft_color_warning;
XftFont *xft_font;
XftFont *xft_font_large;
#endif

//! The background color.
XColor xcolor_background;

//! The foreground color.
XColor xcolor_foreground;

//! The warning color (used as foreground).
XColor xcolor_warning;

//! The main or primary monitor to render the ui
static Monitor main_monitor;

//! The cursor character displayed at the end of the masked password input.
static const char cursor[] = "";

//! Whether to play sounds during authentication.
static int auth_sounds = 0;

#ifdef HAVE_XKB_EXT
//! If set, we show Xkb keyboard layout name.
int show_keyboard_layout = 1;
//! If set, we show Xkb lock/latch status rather than Xkb indicators.
int show_locks_and_latches = 0;
#endif

#define MAIN_WINDOW 0
#define MAX_WINDOWS 2

//! The number of active X11 per-monitor windows.
size_t num_windows = 0;

//! The X11 per-monitor windows to draw on.
Window windows[MAX_WINDOWS];

//! The X11 graphics contexts to draw with.
GC gcs[MAX_WINDOWS];

//! The X11 graphics contexts to draw warnings with.
GC gcs_warning[MAX_WINDOWS];

#ifdef HAVE_XFT_EXT
//! The Xft draw contexts to draw with.
XftDraw *xft_draws[MAX_WINDOWS];
#endif

int have_xkb_ext;

enum Sound { SOUND_PROMPT, SOUND_INFO, SOUND_ERROR, SOUND_SUCCESS };

#define NOTE_DS3 156
#define NOTE_A3 220
#define NOTE_DS4 311
#define NOTE_E4 330
#define NOTE_B4 494
#define NOTE_E5 659
int sounds[][2] = {
    /* SOUND_PROMPT=  */ {NOTE_B4, NOTE_E5},   // V|I I
    /* SOUND_INFO=    */ {NOTE_E5, NOTE_E5},   // I 2x
    /* SOUND_ERROR=   */ {NOTE_A3, NOTE_DS3},  // V7 2x
    /* SOUND_SUCCESS= */ {NOTE_DS4, NOTE_E4},  // V I
};
#define SOUND_SLEEP_MS 125
#define SOUND_TONE_MS 100

/*! \brief Play a sound sequence.
 */
void PlaySound(enum Sound snd) {
  XKeyboardState state;
  XKeyboardControl control;
  struct timespec sleeptime;

  if (!auth_sounds) {
    return;
  }

  XGetKeyboardControl(display, &state);

  // bell_percent changes note length on Linux, so let's use the middle value
  // to get a 1:1 mapping.
  control.bell_percent = 50;
  control.bell_duration = SOUND_TONE_MS;
  control.bell_pitch = sounds[snd][0];
  XChangeKeyboardControl(display, KBBellPercent | KBBellDuration | KBBellPitch,
                         &control);
  XBell(display, 0);

  XFlush(display);

  sleeptime.tv_sec = SOUND_SLEEP_MS / 1000;
  sleeptime.tv_nsec = 1000000L * (SOUND_SLEEP_MS % 1000);
  nanosleep(&sleeptime, NULL);

  control.bell_pitch = sounds[snd][1];
  XChangeKeyboardControl(display, KBBellPitch, &control);
  XBell(display, 0);

  control.bell_percent = state.bell_percent;
  control.bell_duration = state.bell_duration;
  control.bell_pitch = state.bell_pitch;
  XChangeKeyboardControl(display, KBBellPercent | KBBellDuration | KBBellPitch,
                         &control);

  XFlush(display);

  nanosleep(&sleeptime, NULL);
}

/*! \brief Switch to the next keyboard layout.
 */
void SwitchKeyboardLayout(void) {
#ifdef HAVE_XKB_EXT
  if (!have_xkb_ext) {
    return;
  }

  XkbDescPtr xkb;
  xkb = XkbGetMap(display, 0, XkbUseCoreKbd);
  if (XkbGetControls(display, XkbGroupsWrapMask, xkb) != Success) {
    Log("XkbGetControls failed");
    XkbFreeKeyboard(xkb, 0, True);
    return;
  }
  if (xkb->ctrls->num_groups < 1) {
    Log("XkbGetControls returned less than 1 group");
    XkbFreeKeyboard(xkb, 0, True);
    return;
  }
  XkbStateRec state;
  if (XkbGetState(display, XkbUseCoreKbd, &state) != Success) {
    Log("XkbGetState failed");
    XkbFreeKeyboard(xkb, 0, True);
    return;
  }

  XkbLockGroup(display, XkbUseCoreKbd, (state.group + 1) % xkb->ctrls->num_groups);
  XkbFreeKeyboard(xkb, 0, True);
#endif
}

/*! \brief Check which modifiers are active.
 *
 * \param warning Will be set to 1 if something's "bad" with the keyboard
 *     layout (e.g. Caps Lock).
 * \param have_multiple_layouts Will be set to 1 if more than one keyboard
 *     layout is available for switching.
 *
 * \return The current modifier mask as a string.
 */
const char *GetIndicators(int *warning, int *have_multiple_layouts) {
#ifdef HAVE_XKB_EXT
  static char buf[128];
  char *p;

  if (!have_xkb_ext) {
    return "";
  }

  XkbDescPtr xkb;
  xkb = XkbGetMap(display, 0, XkbUseCoreKbd);
  if (XkbGetControls(display, XkbGroupsWrapMask, xkb) != Success) {
    Log("XkbGetControls failed");
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }
  if (XkbGetNames(
          display,
          XkbIndicatorNamesMask | XkbGroupNamesMask | XkbSymbolsNameMask,
          xkb) != Success) {
    Log("XkbGetNames failed");
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }
  XkbStateRec state;
  if (XkbGetState(display, XkbUseCoreKbd, &state) != Success) {
    Log("XkbGetState failed");
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }
  unsigned int istate = 0;
  if (!show_locks_and_latches) {
    if (XkbGetIndicatorState(display, XkbUseCoreKbd, &istate) != Success) {
      Log("XkbGetIndicatorState failed");
      XkbFreeKeyboard(xkb, 0, True);
      return "";
    }
  }

  // Detect Caps Lock.
  // Note: in very pathological cases the modifier might be set without an
  // XkbIndicator for it; then we show the line in red without telling the user
  // why. Such a situation has not been observd yet though.
  unsigned int implicit_mods = state.latched_mods | state.locked_mods;
  if (implicit_mods & LockMask) {
    *warning = 1;
  }

  // Provide info about multiple layouts.
  if (xkb->ctrls->num_groups > 1) {
    *have_multiple_layouts = 1;
  }

  p = buf;

  const char *word = "";
  size_t n = strlen(word);
  if (n >= sizeof(buf) - (p - buf)) {
    Log("Not enough space to store intro '%s'", word);
    XkbFreeKeyboard(xkb, 0, True);
    return "";
  }
  memcpy(p, word, n);
  p += n;

  int have_output = 0;
  if (show_keyboard_layout) {
    Atom layouta = xkb->names->groups[state.group];  // Human-readable.
    if (layouta == None) {
      layouta = xkb->names->symbols;  // Machine-readable fallback.
    }
    if (layouta != None) {
      char *layout = XGetAtomName(display, layouta);
      n = strlen(layout);
      if (n >= sizeof(buf) - (p - buf)) {
        Log("Not enough space to store layout name '%s'", layout);
        XFree(layout);
        XkbFreeKeyboard(xkb, 0, True);
        return "";
      }
      memcpy(p, layout, n);
      XFree(layout);
      p += n;
      have_output = 1;
    }
  }

  if (show_locks_and_latches) {
#define ADD_INDICATOR(mask, name)                                \
  do {                                                           \
    if (!(implicit_mods & (mask))) {                             \
      continue;                                                  \
    }                                                            \
    if (have_output) {                                           \
      if (2 >= sizeof(buf) - (p - buf)) {                        \
        Log("Not enough space to store another modifier name");  \
        break;                                                   \
      }                                                          \
      memcpy(p, ", ", 2);                                        \
      p += 2;                                                    \
    }                                                            \
    size_t n = strlen(name);                                     \
    if (n >= sizeof(buf) - (p - buf)) {                          \
      Log("Not enough space to store modifier name '%s'", name); \
      XFree(name);                                               \
      break;                                                     \
    }                                                            \
    memcpy(p, (name), n);                                        \
    p += n;                                                      \
    have_output = 1;                                             \
  } while (0)
    // TODO(divVerent): There must be a better way to get the names of the
    // modifiers than explicitly enumerating them. Also, there may even be
    // something that knows that Mod1 is Alt/Meta and Mod2 is Num lock.
    ADD_INDICATOR(ShiftMask, "Shift");
    ADD_INDICATOR(LockMask, "Lock");
    ADD_INDICATOR(ControlMask, "Control");
    ADD_INDICATOR(Mod1Mask, "Mod1");
    ADD_INDICATOR(Mod2Mask, "Mod2");
    ADD_INDICATOR(Mod3Mask, "Mod3");
    ADD_INDICATOR(Mod4Mask, "Mod4");
    ADD_INDICATOR(Mod5Mask, "Mod5");
  } else {
    int is_caps_on = 0;

    for (int i = 0; i < XkbNumIndicators; i++) {
      if (!(istate & (1U << i))) {
        continue;
      }

      Atom namea = xkb->names->indicators[i];
      if (namea == None) {
        continue;
      }
      
      char *name = XGetAtomName(display, namea);
      if (strcmp(name, "Caps Lock") == 0) {
        is_caps_on = 1;
        XFree(name);
        break;
      }

      XFree(name);
    }

    char *cpslck = (is_caps_on == 1) ? " [ABC]" : " [Abc]";
    size_t n = strlen(cpslck);
    memcpy(p, cpslck, n);
    p += n;
    have_output = 1;
  }

  *p = 0;
  XkbFreeKeyboard(xkb, 0, True);
  return have_output ? buf : "";
#else
  *warning = *warning;                              // Shut up clang-analyzer.
  *have_multiple_layouts = *have_multiple_layouts;  // Shut up clang-analyzer.
  return "";
#endif
}

void DestroyPerMonitorWindows(size_t keep_windows) {
  for (size_t i = keep_windows; i < num_windows; ++i) {
#ifdef HAVE_XFT_EXT
    XftDrawDestroy(xft_draws[i]);
#endif
    XFreeGC(display, gcs_warning[i]);
    XFreeGC(display, gcs[i]);
    if (i == MAIN_WINDOW) {
      XUnmapWindow(display, windows[i]);
    } else {
      XDestroyWindow(display, windows[i]);
    }
  }
  if (num_windows > keep_windows) {
    num_windows = keep_windows;
  }
}

void CreateOrUpdatePerMonitorWindow(size_t i, const Monitor *monitor, int region_w, int region_h) {
  // Desired box.
  int w = region_w;
  int h = region_h;
  int x = monitor->x + (monitor->width - w) / 2;
  int y = monitor->y + monitor->height - h;

  // Clip to monitor.
  if (x < 0) {
    w += x;
    x = 0;
  }

  if (y < 0) {
    h += y;
    y = 0;
  }

  if (x + w > monitor->x + monitor->width) {
    w = monitor->x + monitor->width - x;
  }

  if (y + h > monitor->y + monitor->height) {
    h = monitor->y + monitor->height - y;
  }

  if (i < num_windows) {
    // Move the existing window.
    XMoveResizeWindow(display, windows[i], x, y, w, h);
    return;
  }

  if (i > num_windows) {
    // Need to add windows in ]num_windows..i[ first.
    Log("Unreachable code - can't create monitor sequences with holes");
    abort();
  }

  // Add a new window.
  XSetWindowAttributes attrs = {0};
  attrs.background_pixel = xcolor_background.pixel;
  if (i == MAIN_WINDOW) {
    // Reuse the main_window (so this window gets protected from overlap by
    // main).
    XMoveResizeWindow(display, main_window, x, y, w, h);
    XChangeWindowAttributes(display, main_window, CWBackPixel, &attrs);
    windows[i] = main_window;
  } else {
    // Create a new window.
    windows[i] =
        XCreateWindow(display, parent_window, x, y, w, h, 0, CopyFromParent,
                      InputOutput, CopyFromParent, CWBackPixel, &attrs);
    SetWMProperties(display, windows[i], "xsecurelock", "auth_x11_screen", argc,
                    argv);
    // We should always make sure that main_window stays on top of all others.
    // I.e. our auth sub-windows shall between "sandwiched" between auth and
    // saver window. That way, main.c's protections of the auth window can stay
    // effective.
    Window stacking_order[2];
    stacking_order[0] = main_window;
    stacking_order[1] = windows[i];
    XRestackWindows(display, stacking_order, 2);
  }

  // Create its data structures.
  XGCValues gcattrs;
  gcattrs.function = GXcopy;
  gcattrs.foreground = xcolor_foreground.pixel;
  gcattrs.background = xcolor_background.pixel;
  if (core_font != NULL) {
    gcattrs.font = core_font->fid;
  }
  gcs[i] = XCreateGC(display, windows[i],
                     GCFunction | GCForeground | GCBackground |
                         (core_font != NULL ? GCFont : 0),
                     &gcattrs);
  gcattrs.foreground = xcolor_warning.pixel;
  gcs_warning[i] = XCreateGC(display, windows[i],
                             GCFunction | GCForeground | GCBackground |
                                 (core_font != NULL ? GCFont : 0),
                             &gcattrs);
#ifdef HAVE_XFT_EXT
  xft_draws[i] = XftDrawCreate(
      display, windows[i], DefaultVisual(display, DefaultScreen(display)),
      DefaultColormap(display, DefaultScreen(display)));
#endif

  // This window is now ready to use.
  XMapWindow(display, windows[i]);
  num_windows = i + 1;
}

void UpdatePerMonitorWindows(Monitor* monitor, int region_w, int region_h) {
  if (monitor == NULL) {
    DestroyPerMonitorWindows(0);
    return;
  }

  Window unused_root, unused_child;
  int unused_root_x, unused_root_y, x, y;
  unsigned int unused_mask;

  XQueryPointer(display, parent_window, &unused_root, &unused_child,
                &unused_root_x, &unused_root_y, &x, &y, &unused_mask);

  if (x >= monitor->x && x < monitor->x + monitor->width &&
      y >= monitor->y && y < monitor->y + monitor->height) {
    CreateOrUpdatePerMonitorWindow(0, monitor, region_w, region_h);
    return;
  }

  CreateOrUpdatePerMonitorWindow(0, monitor, region_w, region_h);
  DestroyPerMonitorWindows(1);

  return;
}

int TextAscent(XftFont *font) {
#ifdef HAVE_XFT_EXT
  if (font != NULL) {
    return font->ascent;
  }
#endif
  return core_font->max_bounds.ascent;
}

int TextDescent(XftFont *font) {
#ifdef HAVE_XFT_EXT
  if (font != NULL) {
    return font->descent;
  }
#endif
  return core_font->max_bounds.descent;
}

#ifdef HAVE_XFT_EXT
// Returns the amount of pixels to expand the logical box in extents so it
// covers the visible box.
int XGlyphInfoExpandAmount(XGlyphInfo *extents) {
  // Use whichever is larger - visible bounding box (bigger if font is italic)
  // or spacing to next character (bigger if last character is a space).
  // Best reference I could find:
  //   https://keithp.com/~keithp/render/Xft.tutorial
  // Visible bounding box: [-x, -x + width[
  // Logical bounding box: [0, xOff[
  // For centering we should always use the logical bounding box, however for
  // erasing we should use the visible bounding box. Thus our goal is to
  // expand the _logical_ box to fully cover the _visible_ box:
  int expand_left = extents->x;
  int expand_right = -extents->x + extents->width - extents->xOff;
  int expand_max = expand_left > expand_right ? expand_left : expand_right;
  int expand_positive = expand_max > 0 ? expand_max : 0;
  return expand_positive;
}
#endif

int TextWidth(XftFont *font, const char *string, int len) {
#ifdef HAVE_XFT_EXT
  if (font != NULL) {
    XGlyphInfo extents;
    XftTextExtentsUtf8(display, font, (const FcChar8 *)string, len, &extents);
    return extents.xOff + 2 * XGlyphInfoExpandAmount(&extents);
  }
#endif
  return XTextWidth(core_font, string, len);
}

void DrawString(int monitor, int x, int y, int is_warning, const char *string, int len, XftFont *font) {
#ifdef HAVE_XFT_EXT
  if (font != NULL) {
    // HACK: Query text extents here to make the text fit into the specified
    // box. For y this is covered by the usual ascent/descent behavior - for x
    // we however do have to work around font descents being drawn to the left
    // of the cursor.
    XGlyphInfo extents;
    XftTextExtentsUtf8(display, font, (const FcChar8 *)string, len, &extents);
    XftDrawStringUtf8(xft_draws[monitor],
                      is_warning ? &xft_color_warning : &xft_color_foreground,
                      font, x + XGlyphInfoExpandAmount(&extents), y,
                      (const FcChar8 *)string, len);
    return;
  }
#endif
  XDrawString(display, windows[monitor],
              is_warning ? gcs_warning[monitor] : gcs[monitor], x, y, string,
              len);
}

void StrAppend(char **output, size_t *output_size, const char *input, size_t input_size) {
  if (*output_size <= input_size) {
    // Cut the input off. Sorry.
    input_size = *output_size - 1;
  }
  memcpy(*output, input, input_size);
  *output += input_size;
  *output_size -= input_size;
}

void BuildLogin(char *output, size_t output_size) {
  size_t username_len = strlen(username);
  StrAppend(&output, &output_size, username, username_len);
  StrAppend(&output, &output_size, "@", 1);
  
  size_t hostname_len = strcspn(hostname, ".");
  StrAppend(&output, &output_size, hostname, hostname_len);

  *output = 0;
}

/*! \brief Render the conext of the auth module.
 *
 * \param prompt A prompt text.
 * \param message A long message.
 * \param is_warning Whether to use the warning style.
 */
void RenderContext(const char *prompt, const char *message, int is_warning) {
  int len_prompt = strlen(prompt);
  int tw_prompt = TextWidth(xft_font_large, prompt, len_prompt);

  int len_message = strlen(message);
  int tw_message = TextWidth(xft_font_large, message, len_message);

  char login[256];
  BuildLogin(login, sizeof(login));
  int len_login = strlen(login);

  int indicators_warning = 0;
  int have_multiple_layouts = 0;
  const char *indicators = GetIndicators(&indicators_warning, &have_multiple_layouts);
  int len_indicators = strlen(indicators);
  int tw_indicators = TextWidth(xft_font, indicators, len_indicators);

  GetPrimaryMonitor(display, parent_window, &main_monitor);
  double scale = main_monitor.ppi/100;

  int region_w = main_monitor.width;
  int region_h = main_monitor.height * 0.55 * scale;

  UpdatePerMonitorWindows(&main_monitor, region_w, region_h);

  int x = region_w / 2;

  int ascent = TextAscent(xft_font_large);
  int descent = TextDescent(xft_font_large);
  int y = (ascent + descent + 30) * scale;

  XClearWindow(display, windows[0]);

  if (strlen(message) > 0) {
    DrawString(0, x - tw_message / 2, y, is_warning, message, len_message, xft_font_large);
  } else {
    DrawString(0, x - tw_prompt / 2, y, is_warning, prompt, len_prompt, xft_font_large);
  }

  x = 5;
  y = region_h - 5;
  DrawString(0, x, y, 0, login, len_login, xft_font);

  x = region_w - tw_indicators - 5;
  DrawString(0, x, y, indicators_warning, indicators, len_indicators, xft_font);

  // Make the things just drawn appear on the screen as soon as possible.
  XFlush(display);
}

void WaitForKeypress(int seconds) {
  // Sleep for up to 1 second _or_ a key press.
  struct timeval timeout;
  timeout.tv_sec = seconds;
  timeout.tv_usec = 0;
  fd_set set;
  memset(&set, 0, sizeof(set));  // For clang-analyzer.
  FD_ZERO(&set);
  FD_SET(0, &set);
  select(1, &set, NULL, NULL, &timeout);
}

//! The size of the buffer to store the password in. Not NUL terminated.
#define PWBUF_SIZE 256

//! The size of the buffer to use for display, with space for cursor and NUL.
#define DISPLAYBUF_SIZE (PWBUF_SIZE + 2)

/*! \brief Ask a question to the user.
 *
 * \param msg The message.
 * \param response The response will be stored in a newly allocated buffer here.
 *   The caller is supposed to eventually free() it.
 * \param echo If true, the input will be shown; otherwise it will be hidden
 *   (password entry).
 * \return 1 if successful, anything else otherwise.
 */
int Prompt(char *msg, char **response, int echo) {
  // Ask something. Return strdup'd string.
  struct {
    // The received X11 event.
    XEvent ev;

    // Input buffer. Not NUL-terminated.
    char pwbuf[PWBUF_SIZE];
    // Current input length.
    size_t pwlen;

    // Display buffer. If echo is 0, this will only contain asterisks, a
    // possible cursor, and be NUL-terminated.
    char displaybuf[DISPLAYBUF_SIZE];
    // Display buffer length.
    size_t displaylen;

    // Character read buffer.
    char inputbuf;

    // The time of last keystroke.
    struct timeval last_keystroke;

    // Temporary position variables that might leak properties about the
    // password and thus are in the private struct too.
    size_t prevpos;
    size_t pos;
    int len;
  } priv;

  if (!echo && MLOCK_PAGE(&priv, sizeof(priv)) < 0) {
    LogErrno("mlock");
    // We continue anyway, as the user being unable to unlock the screen is
    // worse. But let's alert the user.
    RenderContext("", "Password will not be stored securely.", 1);
    WaitForKeypress(1);
  }

  priv.pwlen = 0;

  time_t deadline = time(NULL) + prompt_timeout;

  // Unfortunately we may have to break out of multiple loops at once here but
  // still do common cleanup work. So we have to track the return value in a
  // variable.
  int status = 0;
  int done = 0;
  int played_sound = 0;

  while (!done) {
    if (echo) {
      if (priv.pwlen != 0) {
        memcpy(priv.displaybuf, priv.pwbuf, priv.pwlen);
      }
      priv.displaylen = priv.pwlen;
      // Note that priv.pwlen <= sizeof(priv.pwbuf) and thus
      // priv.pwlen + 2 <= sizeof(priv.displaybuf).
      priv.displaybuf[priv.displaylen] = *cursor;
      priv.displaybuf[priv.displaylen + 1] = '\0';
    } else {
      if (strcmp(password_prompt, "hidden") == 0) {
        priv.displaylen = 0;
        priv.displaybuf[0] = '\0';
      } else {
        mblen(NULL, 0);
        priv.pos = priv.displaylen = 0;
        while (priv.pos < priv.pwlen) {
          ++priv.displaylen;
          // Note: this won't read past priv.pwlen.
          priv.len = mblen(priv.pwbuf + priv.pos, priv.pwlen - priv.pos);
          if (priv.len <= 0) {
            // This guarantees to "eat" one byte each step. Therefore,
            // priv.displaylen <= priv.pwlen is ensured.
            break;
          }
          priv.pos += priv.len;
        }
        memset(priv.displaybuf, '*', priv.displaylen);
        // Note that priv.pwlen <= sizeof(priv.pwbuf) and thus
        // priv.pwlen + 2 <= sizeof(priv.displaybuf).
        priv.displaybuf[priv.displaylen] = *cursor;
        priv.displaybuf[priv.displaylen + 1] = '\0';
      }
    }
    RenderContext(msg, priv.displaybuf, 0);

    if (!played_sound) {
      PlaySound(SOUND_PROMPT);
      played_sound = 1;
    }

    struct timeval timeout;
    timeout.tv_sec = (250 * 1000) / 1000000;
    timeout.tv_usec = (250 * 1000) % 1000000;

    while (!done) {
      fd_set set;
      memset(&set, 0, sizeof(set));  // For clang-analyzer.
      FD_ZERO(&set);
      FD_SET(0, &set);
      int nfds = select(1, &set, NULL, NULL, &timeout);
      if (nfds < 0) {
        LogErrno("select");
        done = 1;
        break;
      }
      time_t now = time(NULL);
      if (now > deadline) {
        Log("AUTH_TIMEOUT hit");
        done = 1;
        break;
      }
      if (deadline > now + prompt_timeout) {
        // Guard against the system clock stepping back.
        deadline = now + prompt_timeout;
      }
      if (nfds == 0) {
        // Blink...
        break;
      }

      // From now on, only do nonblocking selects so we update the screen ASAP.
      timeout.tv_usec = 0;

      // Reset the prompt timeout.
      deadline = now + prompt_timeout;

      ssize_t nread = read(0, &priv.inputbuf, 1);
      if (nread <= 0) {
        Log("EOF on password input - bailing out");
        done = 1;
        break;
      }
      switch (priv.inputbuf) {
        case '\b':      // Backspace.
        case '\177': {  // Delete (note: i3lock does not handle this one).
          // Backwards skip with multibyte support.
          mblen(NULL, 0);
          priv.pos = priv.prevpos = 0;
          while (priv.pos < priv.pwlen) {
            priv.prevpos = priv.pos;
            // Note: this won't read past priv.pwlen.
            priv.len = mblen(priv.pwbuf + priv.pos, priv.pwlen - priv.pos);
            if (priv.len <= 0) {
              // This guarantees to "eat" one byte each step. Therefore,
              // this cannot loop endlessly.
              break;
            }
            priv.pos += priv.len;
          }
          priv.pwlen = priv.prevpos;
          break;
        }
        case '\001':  // Ctrl-A.
          // Clearing input line on just Ctrl-A is odd - but commonly
          // requested. In most toolkits, Ctrl-A does not immediately erase but
          // almost every keypress other than arrow keys will erase afterwards.
          priv.pwlen = 0;
          break;
        case '\023':  // Ctrl-S.
          SwitchKeyboardLayout();
          break;
        case '\025':  // Ctrl-U.
          // Delete the entire input line.
          // i3lock: supports Ctrl-U but not Ctrl-A.
          // xscreensaver: supports Ctrl-U and Ctrl-X but not Ctrl-A.
          priv.pwlen = 0;
          break;
        case 0:       // Shouldn't happen.
        case '\033':  // Escape.
          done = 1;
          break;
        case '\r':  // Return.
        case '\n':  // Return.
          *response = malloc(priv.pwlen + 1);
          if (!echo && MLOCK_PAGE(*response, priv.pwlen + 1) < 0) {
            LogErrno("mlock");
            // We continue anyway, as the user being unable to unlock the screen
            // is worse. But let's alert the user of this.
            RenderContext("", "Password has not been stored securely.", 1);
            WaitForKeypress(1);
          }
          if (priv.pwlen != 0) {
            memcpy(*response, priv.pwbuf, priv.pwlen);
          }
          (*response)[priv.pwlen] = 0;
          status = 1;
          done = 1;
          break;
        default:
          if (priv.inputbuf >= '\000' && priv.inputbuf <= '\037') {
            // Other control character. We ignore them (and specifically do not
            // update the cursor on them) to "discourage" their use in
            // passwords, as most login screens do not support them anyway.
            break;
          }
          if (priv.pwlen < sizeof(priv.pwbuf)) {
            priv.pwbuf[priv.pwlen] = priv.inputbuf;
            ++priv.pwlen;
          } else {
            Log("Password entered is too long - bailing out");
            done = 1;
            break;
          }
          break;
      }
    }
  }

  // priv contains password related data, so better clear it.
  memset(&priv, 0, sizeof(priv));

  if (!done) {
    Log("Unreachable code - the loop above must set done");
  }
  return status;
}

/*! \brief Perform authentication using a helper proxy.
 *
 * \return The authentication status (0 for OK, 1 otherwise).
 */
int Authenticate() {
  int requestfd[2], responsefd[2];
  if (pipe(requestfd)) {
    LogErrno("pipe");
    return 1;
  }
  if (pipe(responsefd)) {
    LogErrno("pipe");
    return 1;
  }

  // Use authproto_pam.
  pid_t childpid = ForkWithoutSigHandlers();
  if (childpid == -1) {
    LogErrno("fork");
    return 1;
  }

  if (childpid == 0) {
    // Child process. Just run authproto_pam.
    // But first, move requestfd[1] to 1 and responsefd[0] to 0.
    close(requestfd[0]);
    close(responsefd[1]);

    if (requestfd[1] == 0) {
      // Tricky case. We don't _expect_ this to happen - after all,
      // initially our own fd 0 should be bound to xsecurelock's main
      // program - but nevertheless let's handle it.
      // At least this implies that no other fd is 0.
      int requestfd1 = dup(requestfd[1]);
      if (requestfd1 == -1) {
        LogErrno("dup");
        _exit(EXIT_FAILURE);
      }
      close(requestfd[1]);
      if (dup2(responsefd[0], 0) == -1) {
        LogErrno("dup2");
        _exit(EXIT_FAILURE);
      }
      close(responsefd[0]);
      if (requestfd1 != 1) {
        if (dup2(requestfd1, 1) == -1) {
          LogErrno("dup2");
          _exit(EXIT_FAILURE);
        }
        close(requestfd1);
      }
    } else {
      if (responsefd[0] != 0) {
        if (dup2(responsefd[0], 0) == -1) {
          LogErrno("dup2");
          _exit(EXIT_FAILURE);
        }
        close(responsefd[0]);
      }
      if (requestfd[1] != 1) {
        if (dup2(requestfd[1], 1) == -1) {
          LogErrno("dup2");
          _exit(EXIT_FAILURE);
        }
        close(requestfd[1]);
      }
    }
    {
      const char *args[2] = {authproto_executable, NULL};
      ExecvHelper(authproto_executable, args);
      sleep(2);  // Reduce log spam or other effects from failed execv.
      _exit(EXIT_FAILURE);
    }
  }

  // Otherwise, we're in the parent process.
  close(requestfd[1]);
  close(responsefd[0]);
  for (;;) {
    char *message;
    char *response;
    char type = ReadPacket(requestfd[0], &message, 1);
    switch (type) {
      case PTYPE_INFO_MESSAGE:
        RenderContext("", message, 0);
        explicit_bzero(message, strlen(message));
        free(message);
        PlaySound(SOUND_INFO);
        WaitForKeypress(1);
        break;
      case PTYPE_ERROR_MESSAGE:
        RenderContext("", message, 1);
        explicit_bzero(message, strlen(message));
        free(message);
        PlaySound(SOUND_ERROR);
        WaitForKeypress(1);
        break;
      case PTYPE_PROMPT_LIKE_PASSWORD:
        if (Prompt(message, &response, 0)) {
          RenderContext("Processing...", "", 0);
          WritePacket(responsefd[1], PTYPE_RESPONSE_LIKE_PASSWORD, response);
          explicit_bzero(response, strlen(response));
          free(response);
        } else {
          WritePacket(responsefd[1], PTYPE_RESPONSE_CANCELLED, "");
        }
        explicit_bzero(message, strlen(message));
        free(message);
        break;
      case 0:
        goto done;
      default:
        Log("Unknown message type %02x", (int)type);
        explicit_bzero(message, strlen(message));
        free(message);
        goto done;
    }
  }
done:
  close(requestfd[0]);
  close(responsefd[1]);
  int status;
  if (!WaitProc("authproto", &childpid, 1, 0, &status)) {
    Log("WaitPgrp returned false but we were blocking");
    abort();
  }
  if (status == 0) {
    PlaySound(SOUND_SUCCESS);
  }
  return status != 0;
}

#ifdef HAVE_XFT_EXT
XftFont *CreateXftFont(Display *display, int screen, const char *font_name, double size) {
  XftFont *xft_font = XftFontOpen (display, screen,
                                 XFT_FAMILY, XftTypeString, font_name,
                                 XFT_SIZE, XftTypeDouble, size, NULL);
#ifdef HAVE_FONTCONFIG
  // Workaround for Xft crashing the process when trying to render a colored
  // font. See https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=916349 and
  // https://gitlab.freedesktop.org/xorg/lib/libxft/issues/6 among others. In
  // the long run this should be ported to a different font rendering library
  // than Xft.
  FcBool iscol;
  if (xft_font != NULL &&
      FcPatternGetBool(xft_font->pattern, FC_COLOR, 0, &iscol) && iscol) {
    Log("Colored font %s is not supported by Xft", font_name);
    XftFontClose(display, xft_font);
    return NULL;
  }
#else
#warning "Xft enabled without fontconfig. May crash trying to use emoji fonts."
  Log("Xft enabled without fontconfig. May crash trying to use emoji fonts.");
#endif
  return xft_font;
}
#endif

/*! \brief The main program.
 *
 * Usage: XSCREENSAVER_WINDOW=window_id ./auth_x11; status=$?
 *
 * \return 0 if authentication successful, anything else otherwise.
 */
int main(int argc_local, char **argv_local) {
  argc = argc_local;
  argv = argv_local;

  setlocale(LC_CTYPE, "");
  setlocale(LC_TIME, "");

  authproto_executable = GetExecutablePathSetting("XSECURELOCK_AUTHPROTO", AUTHPROTO_EXECUTABLE, 0);
  prompt_timeout = GetIntSetting("XSECURELOCK_AUTH_TIMEOUT", 60);
  password_prompt = GetStringSetting("XSECURELOCK_PASSWORD_PROMPT", "asterisks");
  auth_sounds = GetIntSetting("XSECURELOCK_AUTH_SOUNDS", 0);

#ifdef HAVE_XKB_EXT
  show_keyboard_layout = GetIntSetting("XSECURELOCK_SHOW_KEYBOARD_LAYOUT", 1);
  show_locks_and_latches = GetIntSetting("XSECURELOCK_SHOW_LOCKS_AND_LATCHES", 0);
#endif

  if ((display = XOpenDisplay(NULL)) == NULL) {
    Log("Could not connect to $DISPLAY");
    return 1;
  }

#ifdef HAVE_XKB_EXT
  int xkb_opcode, xkb_event_base, xkb_error_base;
  int xkb_major_version = XkbMajorVersion, xkb_minor_version = XkbMinorVersion;
  have_xkb_ext =
      XkbQueryExtension(display, &xkb_opcode, &xkb_event_base, &xkb_error_base,
                        &xkb_major_version, &xkb_minor_version);
#endif

  if (!GetHostName(hostname, sizeof(hostname))) {
    return 1;
  }
  if (!GetUserName(username, sizeof(username))) {
    return 1;
  }

  main_window = ReadWindowID();
  if (main_window == None) {
    Log("Invalid/no window ID in XSCREENSAVER_WINDOW");
    return 1;
  }
  Window unused_root;
  Window *unused_children = NULL;
  unsigned int unused_nchildren;
  XQueryTree(display, main_window, &unused_root, &parent_window,
             &unused_children, &unused_nchildren);
  XFree(unused_children);

  Colormap colormap = DefaultColormap(display, DefaultScreen(display));

  XColor dummy;
  XAllocNamedColor(
      display, DefaultColormap(display, DefaultScreen(display)),
      GetStringSetting("XSECURELOCK_AUTH_BACKGROUND_COLOR", "black"),
      &xcolor_background, &dummy);
  XAllocNamedColor(
      display, DefaultColormap(display, DefaultScreen(display)),
      GetStringSetting("XSECURELOCK_AUTH_FOREGROUND_COLOR", "white"),
      &xcolor_foreground, &dummy);
  XAllocNamedColor(display, DefaultColormap(display, DefaultScreen(display)),
                   GetStringSetting("XSECURELOCK_AUTH_WARNING_COLOR", "red"),
                   &xcolor_warning, &dummy);

  core_font = NULL;
#ifdef HAVE_XFT_EXT
  xft_font = NULL;
  xft_font_large = NULL;
#endif

  GetPrimaryMonitor(display, parent_window, &main_monitor);
  double scale = main_monitor.ppi/100;
  double font_size = 12 * scale;
  double font_large_size = 20 * scale;

  const char *font_name = GetStringSetting("XSECURELOCK_FONT", "");

  // First try parsing the font name as an X11 core font. We're trying these
  // first as their font name format is more restrictive (usually starts with a
  // dash), except for when font aliases are used.
  int have_font = 0;
  if (font_name[0] != 0) {
    core_font = XLoadQueryFont(display, font_name);
    have_font = (core_font != NULL);
#ifdef HAVE_XFT_EXT
    if (!have_font) {
      xft_font = CreateXftFont(display, DefaultScreen(display), font_name, font_size);
      xft_font_large = CreateXftFont(display, DefaultScreen(display), font_name, font_large_size);
      have_font = (xft_font != NULL);
    }
#endif
  }
  if (!have_font) {
    if (font_name[0] != 0) {
      Log("Could not load the specified font %s - trying a default font",
          font_name);
    }
#ifdef HAVE_XFT_EXT
    xft_font = CreateXftFont(display, DefaultScreen(display), "monospace", font_size);
    xft_font_large = CreateXftFont(display, DefaultScreen(display), "monospace", font_large_size);
    have_font = (xft_font != NULL);
#endif
  }
  if (!have_font) {
    core_font = XLoadQueryFont(display, "fixed");
    have_font = (core_font != NULL);
  }
  if (!have_font) {
    Log("Could not load a mind-bogglingly stupid font");
    return 1;
  }

#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    XRenderColor xrcolor;
    xrcolor.alpha = 65535;

    // Translate the X11 colors to XRender colors.
    xrcolor.red = xcolor_foreground.red;
    xrcolor.green = xcolor_foreground.green;
    xrcolor.blue = xcolor_foreground.blue;
    XftColorAllocValue(display, DefaultVisual(display, DefaultScreen(display)),
                       DefaultColormap(display, DefaultScreen(display)),
                       &xrcolor, &xft_color_foreground);

    xrcolor.red = xcolor_warning.red;
    xrcolor.green = xcolor_warning.green;
    xrcolor.blue = xcolor_warning.blue;
    XftColorAllocValue(display, DefaultVisual(display, DefaultScreen(display)),
                       DefaultColormap(display, DefaultScreen(display)),
                       &xrcolor, &xft_color_warning);
  }
#endif

  SelectMonitorChangeEvents(display, main_window);
  InitWaitPgrp();
  int status = Authenticate();

  // Clear any possible processing message by closing our windows.
  DestroyPerMonitorWindows(0);

#ifdef HAVE_XFT_EXT
  if (xft_font != NULL) {
    XftColorFree(display, DefaultVisual(display, DefaultScreen(display)),
                 DefaultColormap(display, DefaultScreen(display)),
                 &xft_color_warning);
    XftColorFree(display, DefaultVisual(display, DefaultScreen(display)),
                 DefaultColormap(display, DefaultScreen(display)),
                 &xft_color_foreground);
    XftFontClose(display, xft_font);
    XftFontClose(display, xft_font_large);
  }
#endif

  XFreeColors(display, colormap, &xcolor_warning.pixel, 1, 0);
  XFreeColors(display, colormap, &xcolor_foreground.pixel, 1, 0);
  XFreeColors(display, colormap, &xcolor_background.pixel, 1, 0);

  return status;
}
