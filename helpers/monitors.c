/*
Copyright 2018 Google Inc. All rights reserved.

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

#include "monitors.h"

#include <X11/Xlib.h>  // for XWindowAttributes, Display, XGetW...
#include <stdlib.h>    // for qsort
#include <string.h>    // for memcmp, memset
#include <math.h>      // for math functions

#include <X11/extensions/Xrandr.h>  // for XRRMonitorInfo, XRRCrtcInfo, XRRO...
#include <X11/extensions/randr.h>   // for RANDR_MAJOR, RRNotify, RANDR_MINOR

#include "../env_settings.h"  // for GetIntSetting
#include "../logging.h"       // for Log

static int event_base;
static int error_base;

#define CLAMP(x, mi, ma) ((x) < (mi) ? (mi) : (x) > (ma) ? (ma) : (x))

static double ComputePpi(int w, int h, int mw, int mh) {
  double diagonal_px = sqrt(pow(w, 2) + pow(h, 2));

  double iw = mw/10/2.54;
  double ih = mh/10/2.54;
  double diagonal_in = sqrt(pow(iw, 2) + pow(ih, 2));

  if (diagonal_px == 0 || diagonal_in == 0) {
    return 100;
  }

  return diagonal_px / diagonal_in;
}

static void QueryXRandR(Display* dpy, Window window, const XWindowAttributes* xwa, Monitor* monitor) {
  // Translate to absolute coordinates so we can compare them to XRandR data.
  int wx, wy;
  Window child;

  if (!XTranslateCoordinates(dpy, window, DefaultRootWindow(dpy), xwa->x, xwa->y, &wx, &wy, &child)) {
    Log("XTranslateCoordinates failed");
    wx = xwa->x;
    wy = xwa->y;
  }

  int ww = xwa->width;
  int wh = xwa->height;

  int num_rrmonitors;
  XRRMonitorInfo* rrmonitors = XRRGetMonitors(dpy, window, 1, &num_rrmonitors);

  if (rrmonitors == NULL) {
    return;
  }

  for (int i = num_rrmonitors - 1; i >= 0; --i) {
    XRRMonitorInfo* info = &rrmonitors[i];

    int x = CLAMP(info->x, wx, wx + ww) - wx;
    int y = CLAMP(info->y, wy, wy + wh) - wy;
    int w = CLAMP(info->x + info->width, wx + x, wx + ww) - (wx + x);
    int h = CLAMP(info->y + info->height, wy + y, wy + wh) - (wy + y);
    int mw = (int) info->mwidth;
    int mh = (int) info->mheight;
    double ppi = ComputePpi(info->width, info->height, info->mwidth, info->mheight);
    int is_primary = (info->primary) ? 1 : 0;

    if (w < 0 || h < 0) {
      continue;
    }

    monitor->x = x;
    monitor->y = y;
    monitor->width = w;
    monitor->height = h;
    monitor->mwidth = mw;
    monitor->mheight = mh;
    monitor->ppi = ppi;
    monitor->is_primary = is_primary;

    if (info->primary) {
      break;
    }
  }
  
  XRRFreeMonitors(rrmonitors);
}

void GetPrimaryMonitor(Display* dpy, Window window, Monitor* monitor) {
  XWindowAttributes xwa;
  XGetWindowAttributes(dpy, window, &xwa);
  QueryXRandR(dpy, window, &xwa, monitor);
}

void SelectMonitorChangeEvents(Display* dpy, Window window) {
  XRRQueryExtension(dpy, &event_base, &error_base);
  XRRSelectInput(dpy, window,
                 RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
}

int IsMonitorChangeEvent(Display* dpy, int type) {
  XRRQueryExtension(dpy, &event_base, &error_base);
  switch (type - event_base) {
    case RRScreenChangeNotify:
    case RRNotify + RRNotify_CrtcChange:
    case RRNotify + RRNotify_OutputChange:
      return 1;
    default:
      return 0;
  }
}
