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

#include <X11/X.h>                  // for Atom, Success, None, GCBackground
#include <X11/Xlib.h>               // for XDrawString, XTextWidth, XFontStruct
#include <X11/extensions/XKB.h>     // for XkbUseCoreKbd, XkbGroupNamesMask
#include <X11/extensions/XKBstr.h>  // for XkbStateRec, _XkbDesc, _XkbNamesRec
#include <locale.h>                 // for NULL, setlocale, LC_CTYPE
#include <pwd.h>                    // for getpwuid, passwd
#include <security/_pam_types.h>    // for PAM_SUCCESS, pam_strerror, pam_re...
#include <security/pam_appl.h>      // for pam_end, pam_start, pam_acct_mgmt
#include <stdio.h>                  // for fprintf, stderr, LogErrno, NULL
#include <stdlib.h>                 // for mblen, exit, free, calloc, getenv
#include <string.h>                 // for memcpy, strlen, memset
#include <sys/select.h>             // for timeval, select, FD_SET, FD_ZERO
#include <time.h>                   // for time
#include <unistd.h>                 // for gethostname, getuid, read, ssize_t

#ifdef HAVE_XKB
#include <X11/XKBlib.h>  // for XkbFreeClientMap, XkbGetIndicator...
#endif

#include "../env_settings.h"      // for GetStringSetting
#include "../logging.h"           // for Log, LogErrno
#include "../mlock_page.h"        // for MLOCK_PAGE
#include "../xscreensaver_api.h"  // for ReadWindowID
#include "monitors.h"             // for Monitor, GetMonitors, IsMonitorCh...

//! The blinking interval in microseconds.
#define BLINK_INTERVAL (250 * 1000)

//! The maximum time to wait at a prompt for user input in microseconds.
#define PROMPT_TIMEOUT (5 * 60 * 1000 * 1000)

//! Do not reveal the length of the password.
#define PARANOID_PASSWORD

//! Length of the "paranoid password display".
#define PARANOID_PASSWORD_LENGTH 32

//! Minimum distance the cursor shall move on keypress.
#define PARANOID_PASSWORD_MIN_CHANGE 4

//! The X11 display.
Display *display;

//! The X11 window to draw in. Provided from $XSCREENSAVER_WINDOW.
Window window;

//! The X11 graphics context to draw with.
GC gc;

//! The font for the PAM messages.
XFontStruct *font;

//! The Black color (used as background).
unsigned long Black;

//! The White color (used as foreground).
unsigned long White;

//! Set if a conversation error has happened during the last PAM call.
static int conv_error = 0;

//! The cursor character displayed at the end of the masked password input.
static const char cursor[] = "_";

#define MAX_MONITORS 16
static int num_monitors;
static Monitor monitors[MAX_MONITORS];

#ifdef HAVE_XKB
/*! \brief Check which modifiers are active.
 *
 * \return The current modifier mask as a string.
 */
const char *get_indicators() {
  static char buf[128];  // Flawfinder: ignore
  char *p;

  XkbDescPtr xkb;
  xkb = XkbGetMap(display, 0, XkbUseCoreKbd);
  if (XkbGetNames(display, XkbIndicatorNamesMask | XkbGroupNamesMask, xkb) !=
      Success) {
    Log("XkbGetNames failed");
    XkbFreeClientMap(xkb, 0, True);
    return "";
  }
  XkbStateRec state;
  if (XkbGetState(display, XkbUseCoreKbd, &state) != Success) {
    Log("XkbGetState failed");
    XkbFreeClientMap(xkb, 0, True);
    return "";
  }
  unsigned int istate;
  if (XkbGetIndicatorState(display, XkbUseCoreKbd, &istate) != Success) {
    Log("XkbGetIndicatorState failed");
    XkbFreeClientMap(xkb, 0, True);
    return "";
  }

  p = buf;

  const char *word = "Keyboard: ";
  size_t n = strlen(word);  // Flawfinder: ignore
  if (n >= sizeof(buf) - (p - buf)) {
    Log("Not enough space to store intro '%s'", word);
    return "";
  }
  memcpy(p, word, n);  // Flawfinder: ignore
  p += n;

  word = XGetAtomName(display, xkb->names->groups[state.group]);
  n = strlen(word);  // Flawfinder: ignore
  if (n >= sizeof(buf) - (p - buf)) {
    Log("Not enough space to store group name '%s'", word);
    return "";
  }
  memcpy(p, word, n);  // Flawfinder: ignore
  p += n;

  int i;
  for (i = 0; i < XkbNumIndicators; i++) {
    if (!(istate & (1 << i))) {
      continue;
    }
    Atom namea = xkb->names->indicators[i];
    if (namea == None) {
      continue;
    }
    const char *word = XGetAtomName(display, namea);
    size_t n = strlen(word);  // Flawfinder: ignore
    if (n + 2 >= sizeof(buf) - (p - buf)) {
      Log("Not enough space to store modifier name '%s'", word);
      continue;
    }
    memcpy(p, ", ", 2);      // Flawfinder: ignore
    memcpy(p + 2, word, n);  // Flawfinder: ignore
    p += n + 2;
  }
  *p = 0;
  return buf;
}
#endif

/*! \brief Display a string in the window.
 *
 * The given title and message will be displayed on all screens. In case caps
 * lock is enabled, the string's case will be inverted.
 *
 * \param title The title of the message.
 * \param str The message itself.
 */
void display_string(const char *title, const char *str) {
  static int region_x;
  static int region_y;
  static int region_w = 0;
  static int region_h = 0;

  int th = font->max_bounds.ascent + font->max_bounds.descent + 4;
  int to = font->max_bounds.ascent + 2;  // Text at to has bbox from 0 to th.

  int len_title = strlen(title);  // Flawfinder: ignore
  int tw_title = XTextWidth(font, title, len_title);

  int len_str = strlen(str);  // Flawfinder: ignore
  int tw_str = XTextWidth(font, str, len_str);

  int tw_cursor =
      XTextWidth(font, cursor, strlen(cursor));  // Flawfinder: ignore

#ifdef HAVE_XKB
  const char *indicators = get_indicators();
  int len_indicators = strlen(indicators);  // Flawfinder: ignore
  int tw_indicators = XTextWidth(font, indicators, len_indicators);
#endif

  if (region_w == 0 || region_h == 0) {
    XClearWindow(display, window);
  }

  int i;
  for (i = 0; i < num_monitors; ++i) {
    int cx = monitors[i].x + monitors[i].width / 2;
    int cy = monitors[i].y + monitors[i].height / 2;
    int sy = cy + to - th * 2;

    // Clip all following output to the bounds of this monitor.
    XRectangle rect;
    rect.x = monitors[i].x;
    rect.y = monitors[i].y;
    rect.width = monitors[i].width;
    rect.height = monitors[i].height;
    XSetClipRectangles(display, gc, 0, 0, &rect, 1, YXBanded);

    // Clear the region last written to.
    if (region_w != 0 && region_h != 0) {
      XClearArea(display, window, cx + region_x, cy + region_y, region_w,
                 region_h, False);
    }

    XDrawString(display, window, gc, cx - tw_title / 2, sy, title, len_title);

    XDrawString(display, window, gc, cx - tw_str / 2, sy + th * 2, str,
                len_str);

#ifdef HAVE_XKB
    XDrawString(display, window, gc, cx - tw_indicators / 2, sy + th * 3,
                indicators, len_indicators);
#endif

    // Disable clipping again.
    XSetClipMask(display, gc, None);
  }

  // Remember the region we just wrote to, relative to cx and cy.
  region_w = tw_title;
  if (tw_str > region_w) {
    region_w = tw_str;
  }
#ifdef HAVE_XKB
  if (tw_indicators > region_w) {
    region_w = tw_indicators;
  }
#endif
  region_w += tw_cursor;
  region_x = -region_w / 2;
#ifdef HAVE_XKB
  region_h = 4 * th;
#else
  region_h = 3 * th;
#endif
  region_y = -region_h / 2;

  // Make the things just drawn appear on the screen as soon as possible.
  XFlush(display);
}

/*! \brief Show a message to the user.
 *
 * \param msg The message.
 * \param is_error If true, the message is assumed to be an error.
 */
void alert(const char *msg, int is_error) {
  // Display message.
  display_string(is_error ? "Error" : "PAM says", msg);

  // Sleep for up to 1 second _or_ a key press.
  struct timeval timeout;
  timeout.tv_sec = 1;
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
 * \return PAM_SUCCESS if successful, anything else otherwise.
 */
int prompt(const char *msg, char **response, int echo) {
  // Ask something. Return strdup'd string.
  struct {
    // The received X11 event.
    XEvent ev;

    // Input buffer. Not NUL-terminated.
    char pwbuf[PWBUF_SIZE];  // Flawfinder: ignore
    // Current input length.
    size_t pwlen;

    // Display buffer. If echo is 0, this will only contain asterisks, a
    // possible cursor, and be NUL-terminated.
    char displaybuf[DISPLAYBUF_SIZE];  // Flawfinder: ignore
    // Display buffer length.
    size_t displaylen;

#ifdef PARANOID_PASSWORD
    // The display marker changes on every input action to a value from 0 to
    // PARANOID_PASSWORD-1. It indicates where to display the "cursor".
    size_t displaymarker;
#endif

    // Character read buffer.
    char inputbuf;

    // Temporary position variables that might leak properties about the
    // password and thus are in the private struct too.
    size_t prevpos;
    size_t pos;
    int len;
  } priv;
  int blinks = 0;

  if (!echo && MLOCK_PAGE(&priv, sizeof(priv)) < 0) {
    LogErrno("mlock");
    // We continue anyway, as the user being unable to unlock the screen is
    // worse. But let's alert the user.
    alert("Password will not be stored securely.", 1);
  }

  priv.pwlen = 0;
  priv.displaymarker = rand() % PARANOID_PASSWORD_LENGTH;

  int max_blinks = PROMPT_TIMEOUT / BLINK_INTERVAL;

  // Unfortunately we may have to break out of multiple loops at once here but
  // still do common cleanup work. So we have to track the return value in a
  // variable.
  int status = PAM_CONV_ERR;
  int done = 0;

  while (!done) {
    if (echo) {
      if (priv.pwlen != 0) {
        memcpy(priv.displaybuf, priv.pwbuf, priv.pwlen);  // Flawfinder: ignore
      }
      priv.displaylen = priv.pwlen;
    } else {
#ifdef PARANOID_PASSWORD
      priv.displaylen = PARANOID_PASSWORD_LENGTH;
      if (priv.pwlen != 0) {
        memset(priv.displaybuf, '*', priv.displaylen);
        priv.displaybuf[priv.displaymarker] = '|';
      } else {
        memset(priv.displaybuf, '_', priv.displaylen);
      }
#else
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
#endif
    }
    // Note that priv.pwlen <= sizeof(priv.pwbuf) and thus
    // priv.pwlen + 2 <= sizeof(priv.displaybuf).
    priv.displaybuf[priv.displaylen] = (blinks % 2) ? ' ' : *cursor;
    priv.displaybuf[priv.displaylen + 1] = 0;
    display_string(msg, priv.displaybuf);

    // Blink the cursor.
    ++blinks;
    if (blinks > max_blinks) {
      done = 1;
      break;
    }

    struct timeval timeout;
    timeout.tv_sec = BLINK_INTERVAL / 1000000;
    timeout.tv_usec = BLINK_INTERVAL % 1000000;

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
      if (nfds == 0) {
        // Blink...
        break;
      }

      // From now on, only do nonblocking selects so we update the screen ASAP.
      timeout.tv_usec = 0;

      // Force the cursor to be in visible state while typing. This also resets
      // the prompt timeout.
      blinks = 0;

      ssize_t nread = read(0, &priv.inputbuf, 1);  // Flawfinder: ignore
      if (nread <= 0) {
        Log("EOF on password input - bailing out");
        done = 1;
        break;
      }
      switch (priv.inputbuf) {
        case '\b':
        case '\177': {
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
#ifdef PARANOID_PASSWORD
          if (priv.prevpos != priv.pwlen) {
            priv.displaymarker =
                (priv.displaymarker + PARANOID_PASSWORD_MIN_CHANGE +
                 rand() % (PARANOID_PASSWORD_LENGTH -
                           2 * PARANOID_PASSWORD_MIN_CHANGE + 1)) %
                PARANOID_PASSWORD_LENGTH;
          }
#endif
          priv.pwlen = priv.prevpos;
          break;
        }
        case 0:
        case '\033':
          done = 1;
          break;
        case '\r':
        case '\n':
          *response = malloc(priv.pwlen + 1);
          if (!echo && MLOCK_PAGE(*response, priv.pwlen + 1) < 0) {
            LogErrno("mlock");
            // We continue anyway, as the user being unable to unlock the screen
            // is worse. But let's alert the user of this.
            alert("Password has not been stored securely.", 1);
          }
          if (priv.pwlen != 0) {
            memcpy(*response, priv.pwbuf, priv.pwlen);  // Flawfinder: ignore
          }
          (*response)[priv.pwlen] = 0;
          status = PAM_SUCCESS;
          done = 1;
          break;
        default:
          if (priv.pwlen < sizeof(priv.pwbuf)) {
            priv.pwbuf[priv.pwlen] = priv.inputbuf;
            ++priv.pwlen;
#ifdef PARANOID_PASSWORD
            priv.displaymarker =
                (priv.displaymarker + PARANOID_PASSWORD_MIN_CHANGE +
                 rand() % (PARANOID_PASSWORD_LENGTH -
                           2 * PARANOID_PASSWORD_MIN_CHANGE + 1)) %
                PARANOID_PASSWORD_LENGTH;
#endif
          } else {
            Log("Password entered is too long - bailing out");
            done = 1;
            break;
          }
          break;
      }
    }

    // Handle X11 events that queued up.
    while (!done && XPending(display) && (XNextEvent(display, &priv.ev), 1)) {
      if (IsMonitorChangeEvent(display, priv.ev.type)) {
        num_monitors = GetMonitors(display, window, monitors, MAX_MONITORS);
        XClearWindow(display, window);
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

/*! \brief Perform a single PAM conversation step.
 *
 * \param msg The PAM message.
 * \param resp The PAM response to store the output in.
 * \return The PAM status (PAM_SUCCESS in case of success, or anything else in
 *   case of error).
 */
int converse_one(const struct pam_message *msg, struct pam_response *resp) {
  resp->resp_retcode = 0;  // Unused but should be set to zero.
  switch (msg->msg_style) {
    case PAM_PROMPT_ECHO_OFF:
      return prompt(msg->msg, &resp->resp, 0);
    case PAM_PROMPT_ECHO_ON:
      return prompt(msg->msg, &resp->resp, 1);
    case PAM_ERROR_MSG:
      alert(msg->msg, 1);
      return PAM_SUCCESS;
    case PAM_TEXT_INFO:
      alert(msg->msg, 0);
      return PAM_SUCCESS;
    default:
      return PAM_CONV_ERR;
  }
}

/*! \brief Perform a PAM conversation.
 *
 * \param num_msg The number of conversation steps to execute.
 * \param msg The PAM messages.
 * \param resp The PAM responses to store the output in.
 * \param appdata_ptr Unused.
 * \return The PAM status (PAM_SUCCESS in case of success, or anything else in
 *   case of error).
 */
int converse(int num_msg, const struct pam_message **msg,
             struct pam_response **resp, void *appdata_ptr) {
  (void)appdata_ptr;

  if (conv_error) {
    Log("converse() got called again with %d messages (first: %s) after "
        "having failed before - this is very likely a bug in the PAM "
        "module having made the call. Bailing out",
        num_msg, num_msg <= 0 ? "(none)" : msg[0]->msg);
    exit(1);
  }

  *resp = calloc(num_msg, sizeof(struct pam_response));

  int i;
  for (i = 0; i < num_msg; ++i) {
    int status = converse_one(msg[i], &(*resp)[i]);
    if (status != PAM_SUCCESS) {
      for (i = 0; i < num_msg; ++i) {
        free((*resp)[i].resp);
      }
      free(*resp);
      *resp = NULL;
      conv_error = 1;
      return status;
    }
  }

  return PAM_SUCCESS;
}

/*! \brief Perform a single PAM operation with retrying logic.
 */
int call_pam_with_retries(int (*pam_call)(pam_handle_t *, int),
                          pam_handle_t *pam, int flags) {
  int attempt = 0;
  for (;;) {
    conv_error = 0;
    int status = pam_call(pam, flags);
    if (conv_error) {  // Timeout or escape.
      return status;
    }
    switch (status) {
      // Never retry these:
      case PAM_ABORT:             // This is fine.
      case PAM_MAXTRIES:          // D'oh.
      case PAM_NEW_AUTHTOK_REQD:  // hunter2 no longer good enough.
      case PAM_SUCCESS:           // Duh.
        return status;
      default:
        // Let's try again then.
        ++attempt;
        if (attempt >= 3) {
          return status;
        }
        break;
    }
  }
}

/*! \brief Perform PAM authentication.
 *
 * \param username The user name to authenticate as.
 * \param hostname The host name to authenticate on.
 * \param conv The PAM conversation handler.
 * \param pam The PAM handle will be returned here.
 * \return The PAM status (PAM_SUCCESS after successful authentication, or
 *   anything else in case of error).
 */
int authenticate(const char *username, const char *hostname,
                 struct pam_conv *conv, pam_handle_t **pam) {
  const char *service_name =
      GetStringSetting("XSECURELOCK_PAM_SERVICE", PAM_SERVICE_NAME);
  int status = pam_start(service_name, username, conv, pam);
  if (status != PAM_SUCCESS) {
    Log("pam_start: %d",
        status);  // Or can one call pam_strerror on a NULL handle?
    return status;
  }

  status = pam_set_item(*pam, PAM_RHOST, hostname);
  if (status != PAM_SUCCESS) {
    Log("pam_set_item: %s", pam_strerror(*pam, status));
    return status;
  }
  status = pam_set_item(*pam, PAM_RUSER, username);
  if (status != PAM_SUCCESS) {
    Log("pam_set_item: %s", pam_strerror(*pam, status));
    return status;
  }
  const char *display = getenv("DISPLAY");  // Flawfinder: ignore
  status = pam_set_item(*pam, PAM_TTY, display);
  if (status != PAM_SUCCESS) {
    Log("pam_set_item: %s", pam_strerror(*pam, status));
    return status;
  }

  status = call_pam_with_retries(pam_authenticate, *pam, 0);
  if (status != PAM_SUCCESS) {
    if (!conv_error) {
      Log("pam_authenticate: %s", pam_strerror(*pam, status));
    }
    return status;
  }

  int status2 = call_pam_with_retries(pam_acct_mgmt, *pam, 0);
  if (status2 == PAM_NEW_AUTHTOK_REQD) {
    status2 =
        call_pam_with_retries(pam_chauthtok, *pam, PAM_CHANGE_EXPIRED_AUTHTOK);
#ifdef PAM_CHECK_ACCOUNT_TYPE
    if (status2 != PAM_SUCCESS) {
      if (!conv_error) {
        Log("pam_chauthtok: %s", pam_strerror(*pam, status2));
      }
      return status2;
    }
#else
    (void)status2;
#endif
  }

#ifdef PAM_CHECK_ACCOUNT_TYPE
  if (status2 != PAM_SUCCESS) {
    // If this one is true, it must be coming from pam_acct_mgmt, as
    // pam_chauthtok's result already has been checked against PAM_SUCCESS.
    if (!conv_error) {
      Log("pam_acct_mgmt: %s", pam_strerror(*pam, status2));
    }
    return status2;
  }
#endif

  return status;
}

/*! \brief The main program.
 *
 * Usage: XSCREENSAVER_WINDOW=window_id ./auth_pam_x11; status=$?
 *
 * \return 0 if authentication successful, anything else otherwise.
 */
int main() {
  setlocale(LC_CTYPE, "");

#ifdef PARANOID_PASSWORD
  // This is used by displaymarker only (no security relevance of the RNG).
  srand(time(NULL));  // Flawfinder: ignore
#endif

  if ((display = XOpenDisplay(NULL)) == NULL) {
    Log("Could not connect to $DISPLAY");
    return 1;
  }

  char hostname[256];  // Flawfinder: ignore
  if (gethostname(hostname, sizeof(hostname))) {
    LogErrno("gethostname");
    return 1;
  }
  hostname[sizeof(hostname) - 1] = 0;

  struct passwd *pwd = NULL;
  struct passwd pwd_storage;
  char *pwd_buf;
  long pwd_bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (pwd_bufsize < 0) {
    pwd_bufsize = 1 << 20;
  }
  pwd_buf = malloc((size_t)pwd_bufsize);
  if (!pwd_buf) {
    LogErrno("malloc(pwd_bufsize)");
    return 1;
  }
  getpwuid_r(getuid(), &pwd_storage, pwd_buf, (size_t)pwd_bufsize, &pwd);
  if (!pwd) {
    LogErrno("getpwuid_r");
    free(pwd_buf);
    return 1;
  }

  window = ReadWindowID();
  if (window == None) {
    Log("Invalid/no window ID in XSCREENSAVER_WINDOW");
    free(pwd_buf);
    return 1;
  }

  Black = BlackPixel(display, DefaultScreen(display));
  White = WhitePixel(display, DefaultScreen(display));

  font = NULL;
  const char *font_name = GetStringSetting("XSECURELOCK_FONT", "");
  if (font_name[0] != 0) {
    font = XLoadQueryFont(display, font_name);
    if (font == NULL) {
      Log("Could not load the specified font %s - trying to fall back to "
          "fixed",
          font_name);
    }
  }
  if (font == NULL) {
    font = XLoadQueryFont(display, "fixed");
  }
  if (font == NULL) {
    Log("Could not load a mind-bogglingly stupid font");
    exit(1);
  }

  XGCValues gcattrs;
  gcattrs.function = GXcopy;
  gcattrs.foreground = White;
  gcattrs.background = Black;
  gcattrs.font = font->fid;
  gc = XCreateGC(display, window,
                 GCFunction | GCForeground | GCBackground | GCFont, &gcattrs);
  XSetWindowBackground(display, window, Black);

  SelectMonitorChangeEvents(display, window);
  num_monitors = GetMonitors(display, window, monitors, MAX_MONITORS);

  struct pam_conv conv;
  conv.conv = converse;
  conv.appdata_ptr = NULL;

  pam_handle_t *pam;
  int status = authenticate(pwd->pw_name, hostname, &conv, &pam);
  int status2 = pam_end(pam, status);

  // Done with PAM, so we can free the getpwuid_r buffer now.
  free(pwd_buf);

  if (status != PAM_SUCCESS) {
    // The caller already displayed an error.
    return 1;
  }
  if (status2 != PAM_SUCCESS) {
    Log("pam_end: %s", pam_strerror(pam, status2));
    return 1;
  }

  return 0;
}
