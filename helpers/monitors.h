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

#ifndef MONITORS_H
#define MONITORS_H

#include <X11/X.h>     // for Window
#include <X11/Xlib.h>  // for Display
#include <stddef.h>    // for size_t

typedef struct {
  int x, y;
  int width, height;
  int mwidth, mheight;
  double ppi;
  int is_primary;
} Monitor;

/*! \brief Queries the current primary monitor.
 *
 * Note: if no primary monitor is found the first in order monitor
 *       of the configuration will be returned.
 *
 * \param dpy The current display.
 * \param window The window this application intends to draw in.
 * \param monitor A pointer to the primary monitor of the configuration.
 */
void GetPrimaryMonitor(Display* dpy, Window window, Monitor* monitor);

/*! \brief Enable receiving monitor change events for the given display at window.
 */
void SelectMonitorChangeEvents(Display* dpy, Window window);

/*! \brief Returns the event type that indicates a change to the monitor
 *    configuration.
 *
 * \param dpy The current display.
 * \param type The received event type.
 *
 * \returns 1 if the received event is a monitor change event and GetMonitors
 *   should be called, or 0 otherwise.
 */
int IsMonitorChangeEvent(Display* dpy, int type);

#endif
