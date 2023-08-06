# XSecureLock

The xsecurelock is an X11 screen lock utility designed with the primary goal of security. Screen lock utilities are widespread. However, in the past they often had security issues regarding authentication bypass (a crashing screen locker would unlock the screen), information disclosure (notifications may appear on top of the screen saver), or sometimes even worse. In xsecurelock, security is achieved using a modular design to avoid the usual pitfalls of screen locking utility design on X11.

## Requirements and dependencies

The following packages need to be installed:

*   autotools-dev
*   autoconf (for Ubuntu 18.04 and newer)
*   binutils
*   gcc
*   libc6-dev
*   libpam0g-dev (for Ubuntu 18.04 and newer)
*   libpam-dev (for the `authproto_pam` module)
*   libx11-dev
*   libxcomposite-dev
*   libxext-dev
*   libxfixes-dev
*   libxft-dev
*   libxmuu-dev
*   libxrandr-dev
*   libxss-dev
*   make
*   pkg-config
*   x11proto-core-dev
*   python: python-pam (for the `authproto_pypam` module)
*   python: screeninfo (for the `saver_clock` module)

## How to install

Execute the following commands to clone the source code, build and install the binary files.

```
git clone https://github.com/tzeikob/xsecurelock.git
cd xsecurelock
sh autogen.sh
./configure --with-pam-service-name=SERVICE_NAME
make
sudo make install
```

>NOTE: Please replace SERVICE-NAME by the name of an appropriate and existing file in `/etc/pam.d`. Good choices are `system-auth` or `common-auth` both should work fine. This will be used as default and can be overridden with [`XSECURELOCK_PAM_SERVICE`](#options).

>CAUTION: Configuring a broken or missing SERVICE-NAME will render unlocking the screen impossible! If this should happen to you, switch to another terminal with `Ctrl-Alt-F1`, log in there and run `killall xsecurelock` to force unlocking of the screen.

## How to run

Tell your desktop environment to run xsecurelock by using a command line such as one of the following:

```
xsecurelock

env XSECURELOCK_FONT='PixelMix' \
  XSECURELOCK_SAVER="saver_clock" \
  XSECURELOCK_NO_COMPOSITE=1 \
  XSECURELOCK_BLANK_TIMEOUT=-1 \
  xsecurelock
```

>IMPORTANT: Make sure your desktop environment does not launch any other locker, be it via autostart file or its own configuration, as multiple screen lockers may interfere with each other.

To have the authentication process start up without a keypress when the system exits suspend/hibernate, arrange for the system to send the `SIGUSR2` signal to the xsecurelock process. For example, you can copy the following script to the file `/usr/lib/systemd/system-sleep/locker`:

```
#!/bin/bash

# Delay suspend to give some time to the locker to take action
if [[ "${1}" == "pre" ]] && [[ "${2}" == "suspend" ]]; then
  sleep 2
fi

# On resume prompt for authentication
if [[ "${1}" == "post" ]]; then
  pkill -x -USR2 xsecurelock
fi

exit 0
```

Don't forget to make the script executable.

## Options

Options to xsecurelock can be passed by environment variables:

<!-- ENV VARIABLES START -->
 `XSECURELOCK_AUTH`: The desired authentication module:<br>
  &ensp;`auth_x11`: An X11 authentication prompt dialog, set as default.<br>
 
 `XSECURELOCK_AUTHPROTO`: The desired authentication protocol module:<br>
  &ensp;`authproto_pypam`: A no retry PAM authentication controller, set as default.<br>
  &ensp;`authproto_pam`: The regular PAM authentication controller.<br>
 
 `XSECURELOCK_AUTH_TIMEOUT`: The secs to wait for response to auth before giving up, default is `30`.<br>
  
 `XSECURELOCK_PAM_SERVICE`: The name of an existing pam service in `/etc/pam.d`, default is `system-auth`.<br>
 
 `XSECURELOCK_NO_PAM_RHOST`: Do not set PAM_RHOST to localhost, despite [recommendation](http://www.linux-pam.org/Linux-PAM-html/adg-security-user-identity.html) to do so by the Linux-PAM Application Developers' Guide. This may work around bugs in third-party PAM authentication modules. If this solves a problem for you, please report a bug against said PAM module.<br>
  &ensp;`0`: Set PAM_RHOST to localhost.<br>
  &ensp;`1`: Do not set PAM_RHOST to localhost, set as default.<br>
 
 `XSECURELOCK_PASSWORD_PROMPT`: The desired password prompt text mode:<br>
  &ensp;`asterisks`: Shows asterisks like classic password prompts, set as default.<br>
  &ensp;`hidden`: Hides the password with no feedback for keypresses.<br>
 
 `XSECURELOCK_AUTH_SOUNDS`: Whether to play sounds during authentication to indicate status:<br>
  &ensp;`0`: Do not play sounds.<br>
  &ensp;`1`: Play sounds, set as default.<br>
 
 `XSECURELOCK_DISCARD_FIRST_KEYPRESS`: The key pressed to stop the screen saver and spawn the auth child is sent to the auth child (and thus becomes part of the password entry). By default we always discard the key press that started the authentication flow, to prevent users from getting used to type their password on a blank screen (which could be just powered off and have a chat client behind or similar).<br>
  &ensp;`0`: Do not discard first keypress.<br>
  &ensp;`1`: Discard the first keypress, set as default.<br>
 
 `XSECURELOCK_BACKGROUND_COLOR`: An `#RRGGBB` Hex X11 color for the background.<br>
 
 `XSECURELOCK_FOREGROUND_COLOR`: An `#RRGGBB` Hex X11 color for foreground texts.<br>
 
 `XSECURELOCK_WARNING_COLOR`: An `#RRGGBB` Hex X11 color for warning texts.<br>
 
 `XSECURELOCK_FONT`: An X11 or FontConfig font name to be used for texts.<br>
 
 `XSECURELOCK_DATE_FORMAT`: The format to be used for the date in saver modules.<br>
 
 `XSECURELOCK_TIME_FORMAT`: The format to be used for the time in saver modules.<br>
 
 `XSECURELOCK_NO_COMPOSITE`: Switches to a more traditional way of locking, but may allow desktop notifications to be visible on top of the screen lock:<br>
  &ensp;`0`: Covering the composite overlay window, set as default.<br>
  &ensp;`1`: Disables covering the composite overlay window.<br>
 
 `XSECURELOCK_COMPOSITE_OBSCURER`: Create a second full-screen window to obscure window content in case a running compositor unmaps its own window. Helps with some instances of bad compositor behavior (such as compositor crashes/restarts, but also compton has been caught at drawing notification icons above the screen locker when not using the GLX backend), should prevent compositors from unredirecting as it's 1 pixel smaller than the screen from every side, and should otherwise be harmless, so it's enabled by default.<br>
  &ensp;`0`: Do not create a second full-screen window to obscure window content.<br>
  &ensp;`1`: Create a second full-screen window to obscure window content, set as default.<br>
 
 `XSECURELOCK_SAVER`: The desired screen saver module:<br>
  &ensp;`saver_blank`: Leaves the screen plain blank, set as default.<br>
  &ensp;`saver_clock`: Shows the current date and time in digital form.<br>
 
 `XSECURELOCK_SAVER_DELAY_MS`: The milliseconds to wait after starting children process and before mapping windows to let children be ready to display and reduce the black flash, defautl set to `0`.<br>
 
 `XSECURELOCK_SAVER_RESET_ON_AUTH_CLOSE`: Specifies whether to reset the saver module when the auth dialog closes. Resetting is done by sending `SIGUSR1` to the saver, which may either just terminate, or handle this specifically to do a cheaper reset.<br>
  &ensp;`0`: Do not reset the saver module, set as default.<br>
  &ensp;`1`: Reset the saver module.<br>
 
 `XSECURELOCK_SAVER_STOP_ON_BLANK`: Specifies if saver is stopped when screen is blanked (DPMS or XSS):<br>
  &ensp;`0`: Do not stop saver when screen is blanked.<br>
  &ensp;`1`: Stop saver when screen is blanked, set as default.<br>
 
 `XSECURELOCK_GLOBAL_SAVER`: Specifies the desired global screen saver module (by default this is a multiplexer that runs `XSECURELOCK_SAVER` on each screen).<br>
 
 `XSECURELOCK_BLANK_TIMEOUT`: The time in seconds before telling X11 to fully blank the screen; a negative value disables X11 blanking. The time is measured since the closing of the auth window or xsecurelock startup. Setting this to 0 is rather nonsensical, as key-release events (e.g. from the keystroke to launch xsecurelock or from pressing escape to close the auth dialog) always wake up the screen, default set to `600`.<br>
 
 `XSECURELOCK_BLANK_DPMS_STATE`: Specifies which DPMS state to put the screen in when blanking, `standby`, `suspend`, `off` or `on` where `on` means to not invoke DPMS at all, default set to `off`.<br>
 
 `XSECURELOCK_KEY_%s_COMMAND`: Where `%s` is the name of an X11 keysym (find using `xev`), a shell command to execute when the specified key is pressed. Useful e.g. for media player control. Beware: be cautious about what you run with this, as it may yield attackers control over your computer.<br>
 
 `XSECURELOCK_FORCE_GRAB`: When grabbing fails, try stealing the grab from other windows, This works only sometimes and is incompatible with many window managers, so use with care:<br>
  &ensp;`0`: Do not force grabbibg, set as default.<br>
  &ensp;`1`: Only steals from client windows.<br>
  &ensp;`2`: Steals from all descendants of the root window.<br>
 
 `XSECURELOCK_DEBUG_WINDOW_INFO`: When complaining about another window misbehaving, print not just the window ID but also some info about it:<br>
  &ensp;`0`: Do not debug window info, set as default.<br>
  &ensp;`1`: Debug window info.<br>
 
 `XSECURELOCK_DEBUG_ALLOW_LOCKING_IF_INEFFECTIVE`: Normally we don't allow locking sessions that are likely not any useful to lock, such as the X11 part of a Wayland session (one could still use Wayland applicatione when locked) or VNC sessions (as it'd only lock the server side session while users will likely think they locked the client, allowing for an easy escape). These checks can be bypassed by setting this variable to 1. Not recommended other than for debugging xsecurelock itself via such connections:<br>
  &ensp;`0`: Do not allow locking when ineffective, set as default.<br>
  &ensp;`1`: Do not allow locking when ineffective.<br>
<!-- ENV VARIABLES END -->

Additionally, command line arguments following a "--" argument will be executed via `execvp` once locking is successful; this can be used to notify a calling process of successful locking.

## Security Design

In order to achieve maximum possible security against screen lock bypass exploits, the following measures are taken:

*   Authentication dialog, authentication checking and screen saving are done using separate processes. Therefore a crash of these processes will not unlock the screen, which means that these processes are allowed to do "possibly dangerous" things.
*   This also means that on operating systems where authentication checking requires special privileges (such as FreeBSD), only that module can be set to run at elevated privileges, unlike most other screen lockers which in this scenario also run graphical user interface code as root.
*   The main process is kept minimal and only uses C, POSIX and X11 APIs. This limits the possible influence from bugs in external libraries, and allows for easy auditing.
*   The main process regularly refreshes the screen grabs in case they get lost for whatever reason.
*   The main process regularly brings its window to the front, to avoid leaking information from notification messages that are OverrideRedirect.
*   The main process resizes its window to the size of the root window, should the root window size change, to avoid leaking information by attaching a secondary display.
*   The main processes uses only a single buffer - to hold a single keystroke. Therefore it is impossible to exploit a buffer overrun in the main process by e.g. an overlong password entry.
*   The only exit conditions of the program is the Authentication Module returning with exit status zero, on which xsecurelock itself will return with status zero; therefore especially security-conscious users might want to run it as `sh -c "xsecurelock ... || kill -9 -1"` :)

## Known Security Issues

*   Locking the screen will fail while other applications already have a keyboard or pointer grab open (for example while running a fullscreen game, or after opening a context menu). This will be noticeable as the screen will not turn black and should thus usually not be an issue - however when relying on automatic locking via `xss-lock`, this could leave a workstation open for days. Above `... || kill -9 -1` workaround would mitigate this issue too by simply killing the entire session if locking it fails.
*   As xsecurelock relies on an event notification after a screen configuration change, window content may be visible for a short time after attaching a monitor. No usual interaction with applications should be possible though. On desktop systems where monitors are usually not hotplugged, I'd recommend [turning off automatic screen reconfiguration](http://tech.draiser.net/2015/07/14/ignoring-hotplug-monitor-events-on-arch-linux/).
*   The xsecurelock relies on a keyboard and pointer grab in order to prevent other applications from receiving keyboard events (and thus an unauthorized user from controlling the machine). However, there are various other ways for applications - in particular games - to receive input:
    *   Polling current keyboard status (`XQueryKeymap`).
    *   Polling current mouse position (`XQueryPointer`).
    *   Receiving input out-of-band (`/dev/input`), including other input devices than keyboard and mouse, such as gamepads or joysticks.

Most these issues are inherent with X11 and can only really be fixed by migrating to an alternative such as Wayland; some of the issues (in particular the gamepad input issue) will probably persist even with Wayland.

## Forcing Grabs

As a workaround to the issue of another window already holding a grab, we offer an `XSECURELOCK_FORCE_GRAB` option.

This adds a last measure attempt to force grabbing by iterating through all subwindows of the root window, unmapping them (which closes down their grabs), then taking the grab and mapping them again.

This has the following known issues:

*   Grabs owned by the root window cannot be closed down this way. However, only screen lockers and fullscreen games should be doing that.
*   If the grab was owned by a full screen window (e.g. a game using `OverrideRedirect` to gain fullscreen mode), the window will become unresponsive, as your actions will be interpreted by another window - which you can't see - instead. Alt-Tabbing around may often work around this.
*   If the grab was owned by a context menu, it may become impossible to close the menu other than by selecting an item in it.
*   It will also likely confuse window managers:
    *   Probably all window managers will rearrange the windows in response to this.
    *   Cinnamon (and probably other GNOME-derived WMs) may become unresponsive and needs to be restarted.
        *   As a mitigation we try to hit only client windows - but then we lose the ability of closing down window manager owned grabs.
*   Negative side effects as described are still likely to happen in case the measure fails.

## Known Compatibility Issues

*   There is an open issue with the NVidia graphics driver in conjunction with some compositors. Workarounds include switching to the `nouveau` graphics driver, using a compositor that uses the Composite Overlay Window (e.g.
    `compton` with the flags `--backend glx --paint-on-overlay`) or passing `XSECURELOCK_NO_COMPOSITE=1` to xsecurelock (which however may make notifications appear on top of the screen lock).

*   The xsecurelock is incompatible with the compositor built into `metacity` (a GNOME component) because it draws on the Compositor Overlay Window with `IncludeInferiors` set (i.e. it explicitly requests to draw on top of programs like xsecurelock). It likely does this because the same is necessary when drawing on top of the root window, which it had done in the past but no longer does. Workarounds include disabling its compositor with `gsettings set org.gnome.metacity compositing-manager false` or passing `XSECURELOCK_NO_COMPOSITE=1` to xsecurelock.

*   Picom doesn't remove windows in the required order causing a window with the text "INCOMPATIBLE COMPOSITOR, PLEASE FIX!" to be displayed. To fix this you can disable composite obscurer with `XSECURELOCK_COMPOSITE_OBSCURER=0` to stop the window from being drawn all together.

*   In general, most compositor issues will become visible in form of a text "INCOMPATIBLE COMPOSITOR, PLEASE FIX!" being displayed. A known good compositor is `compton --backend glx --paint-on-overlay`. In worst case you can turn off our workaround for transparent windows by setting `XSECURELOCK_NO_COMPOSITE=1`.

## License

The code is released under the Apache 2.0 license. See the LICENSE file for more details.

This project is not an official Google project. It is not supported by Google and Google specifically disclaims all warranties as to its quality, merchantability, or fitness for a particular purpose.
