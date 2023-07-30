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

static int CompareMonitorsByPrimary(const void* a, const void* b) {
  Monitor *monitorA = (Monitor *) a;
  Monitor *monitorB = (Monitor *) b;

  return monitorB->is_primary - monitorA->is_primary;
}

static int IntervalsOverlap(int astart, int asize, int bstart, int bsize) {
  // Compute exclusive bounds.
  int aend = astart + asize;
  int bend = bstart + bsize;

  // If one interval starts at or after the other, there's no overlap.
  if (astart >= bend || bstart >= aend) {
    return 0;
  }

  // Otherwise, there must be an overlap.
  return 1;
}

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

static void AddMonitor(Monitor* monitors, size_t* num,
                       int x, int y, int w, int h, int mw, int mh, double ppi, int is_primary) {
  // Skip empty "monitors".
  if (w <= 0 || h <= 0) {
    return;
  }

  // Skip overlapping monitors
  for (size_t i = 0; i < *num; ++i) {
    if (IntervalsOverlap(x, w, monitors[i].x, monitors[i].width) &&
        IntervalsOverlap(y, h, monitors[i].y, monitors[i].height)) {
      return;
    }
  }

  monitors[*num].x = x;
  monitors[*num].y = y;
  monitors[*num].width = w;
  monitors[*num].height = h;
  monitors[*num].mwidth = mw;
  monitors[*num].mheight = mh;
  monitors[*num].ppi = ppi;
  monitors[*num].is_primary = is_primary;

  ++*num;
}

static void GetMonitorsXRandR(Display* dpy, Window window, const XWindowAttributes* xwa,
                             Monitor* monitors, size_t* num) {
  // Translate to absolute coordinates so we can compare them to XRandR data.
  int wx, wy;
  Window child;

  if (!XTranslateCoordinates(dpy, window, DefaultRootWindow(dpy),
                             xwa->x, xwa->y, &wx, &wy, &child)) {
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

  for (int i = 0; i < num_rrmonitors; ++i) {
    XRRMonitorInfo* info = &rrmonitors[i];

    int x = CLAMP(info->x, wx, wx + ww) - wx;
    int y = CLAMP(info->y, wy, wy + wh) - wy;
    int w = CLAMP(info->x + info->width, wx + x, wx + ww) - (wx + x);
    int h = CLAMP(info->y + info->height, wy + y, wy + wh) - (wy + y);
    int mw = (int) info->mwidth;
    int mh = (int) info->mheight;
    double ppi = ComputePpi(info->width, info->height, info->mwidth, info->mheight);
    int is_primary = 0;

    if (info->primary) {
      is_primary = 1;
    }

    AddMonitor(monitors, num, x, y, w, h, mw, mh, ppi, is_primary);
  }
  
  XRRFreeMonitors(rrmonitors);
}

size_t GetMonitors(Display* dpy, Window window, Monitor* monitors) {
  // As outputs will be relative to the window, we have to query its attributes.
  XWindowAttributes xwa;
  XGetWindowAttributes(dpy, window, &xwa);

  size_t num = 0;
  GetMonitorsXRandR(dpy, window, &xwa, monitors, &num);

  // Sort the monitors in some deterministic order.
  qsort(monitors, num, sizeof(*monitors), CompareMonitorsByPrimary);

  return num;
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
