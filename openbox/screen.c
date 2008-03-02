/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   screen.c for the Openbox window manager
   Copyright (c) 2006        Mikael Magnusson
   Copyright (c) 2003-2007   Dana Jansens

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   See the COPYING file for a copy of the GNU General Public License.
*/

#include "debug.h"
#include "openbox.h"
#include "dock.h"
#include "grab.h"
#include "startupnotify.h"
#include "moveresize.h"
#include "config.h"
#include "screen.h"
#include "client.h"
#include "session.h"
#include "frame.h"
#include "event.h"
#include "focus.h"
#include "popup.h"
#include "hooks.h"
#include "render/render.h"
#include "gettext.h"
#include "obt/display.h"
#include "obt/prop.h"
#include "obt/mainloop.h"

#include <X11/Xlib.h>
#ifdef HAVE_UNISTD_H
#  include <sys/types.h>
#  include <unistd.h>
#endif
#include <assert.h>

/*! The event mask to grab on the root window */
#define ROOT_EVENTMASK (StructureNotifyMask | PropertyChangeMask | \
                        EnterWindowMask | LeaveWindowMask | \
                        SubstructureRedirectMask | FocusChangeMask | \
                        ButtonPressMask | ButtonReleaseMask)

static gboolean screen_validate_layout(ObDesktopLayout *l);
static gboolean replace_wm(void);
static void     screen_tell_ksplash(void);
static void     screen_fallback_focus(void);

guint           screen_num_desktops;
guint           screen_num_monitors;
guint           screen_desktop;
guint           screen_last_desktop;
gboolean        screen_showing_desktop;
ObDesktopLayout screen_desktop_layout;
gchar         **screen_desktop_names;
Window          screen_support_win;
Time            screen_desktop_user_time = CurrentTime;

static Size     screen_physical_size;
static guint    screen_old_desktop;
static gboolean screen_desktop_timeout = TRUE;
/*! An array of desktops, holding an array of areas per monitor */
static Rect  *monitor_area = NULL;
/*! An array of desktops, holding an array of struts */
static GSList *struts_top = NULL;
static GSList *struts_left = NULL;
static GSList *struts_right = NULL;
static GSList *struts_bottom = NULL;

static ObPagerPopup *desktop_popup;

/*! The number of microseconds that you need to be on a desktop before it will
  replace the remembered "last desktop" */
#define REMEMBER_LAST_DESKTOP_TIME 750000

static gboolean replace_wm(void)
{
    gchar *wm_sn;
    Atom wm_sn_atom;
    Window current_wm_sn_owner;
    Time timestamp;

    wm_sn = g_strdup_printf("WM_S%d", ob_screen);
    wm_sn_atom = XInternAtom(obt_display, wm_sn, FALSE);
    g_free(wm_sn);

    current_wm_sn_owner = XGetSelectionOwner(obt_display, wm_sn_atom);
    if (current_wm_sn_owner == screen_support_win)
        current_wm_sn_owner = None;
    if (current_wm_sn_owner) {
        if (!ob_replace_wm) {
            g_message(_("A window manager is already running on screen %d"),
                      ob_screen);
            return FALSE;
        }
        obt_display_ignore_errors(TRUE);

        /* We want to find out when the current selection owner dies */
        XSelectInput(obt_display, current_wm_sn_owner, StructureNotifyMask);
        XSync(obt_display, FALSE);

        obt_display_ignore_errors(FALSE);
        if (obt_display_error_occured)
            current_wm_sn_owner = None;
    }

    timestamp = event_get_server_time();

    XSetSelectionOwner(obt_display, wm_sn_atom, screen_support_win,
                       timestamp);

    if (XGetSelectionOwner(obt_display, wm_sn_atom) != screen_support_win) {
        g_message(_("Could not acquire window manager selection on screen %d"),
                  ob_screen);
        return FALSE;
    }

    /* Wait for old window manager to go away */
    if (current_wm_sn_owner) {
      XEvent event;
      gulong wait = 0;
      const gulong timeout = G_USEC_PER_SEC * 15; /* wait for 15s max */

      while (wait < timeout) {
          if (XCheckWindowEvent(obt_display, current_wm_sn_owner,
                                StructureNotifyMask, &event) &&
              event.type == DestroyNotify)
              break;
          g_usleep(G_USEC_PER_SEC / 10);
          wait += G_USEC_PER_SEC / 10;
      }

      if (wait >= timeout) {
          g_message(_("The WM on screen %d is not exiting"), ob_screen);
          return FALSE;
      }
    }

    /* Send client message indicating that we are now the WM */
    obt_prop_message(ob_screen, obt_root(ob_screen), OBT_PROP_ATOM(MANAGER),
                     timestamp, wm_sn_atom, screen_support_win, 0, 0,
                     SubstructureNotifyMask);

    return TRUE;
}

gboolean screen_annex(void)
{
    XSetWindowAttributes attrib;
    pid_t pid;
    gint i, num_support;
    gulong *supported;

    /* create the netwm support window */
    attrib.override_redirect = TRUE;
    attrib.event_mask = PropertyChangeMask;
    screen_support_win = XCreateWindow(obt_display, obt_root(ob_screen),
                                       -100, -100, 1, 1, 0,
                                       CopyFromParent, InputOutput,
                                       CopyFromParent,
                                       CWEventMask | CWOverrideRedirect,
                                       &attrib);
    XMapWindow(obt_display, screen_support_win);
    XLowerWindow(obt_display, screen_support_win);

    if (!replace_wm()) {
        XDestroyWindow(obt_display, screen_support_win);
        return FALSE;
    }

    obt_display_ignore_errors(TRUE);
    XSelectInput(obt_display, obt_root(ob_screen), ROOT_EVENTMASK);
    obt_display_ignore_errors(FALSE);
    if (obt_display_error_occured) {
        g_message(_("A window manager is already running on screen %d"),
                  ob_screen);

        XDestroyWindow(obt_display, screen_support_win);
        return FALSE;
    }

    screen_set_root_cursor();

    /* set the OPENBOX_PID hint */
    pid = getpid();
    OBT_PROP_SET32(obt_root(ob_screen), OPENBOX_PID, CARDINAL, pid);

    /* set supporting window */
    OBT_PROP_SET32(obt_root(ob_screen),
                   NET_SUPPORTING_WM_CHECK, WINDOW, screen_support_win);

    /* set properties on the supporting window */
    OBT_PROP_SETS(screen_support_win, NET_WM_NAME, utf8, "Openbox");
    OBT_PROP_SET32(screen_support_win, NET_SUPPORTING_WM_CHECK,
                   WINDOW, screen_support_win);

    /* set the _NET_SUPPORTED_ATOMS hint */

    /* this is all the atoms after NET_SUPPORTED in the ObtPropAtoms enum */
    num_support = OBT_PROP_NUM_ATOMS - OBT_PROP_NET_SUPPORTED - 1;
    i = 0;
    supported = g_new(gulong, num_support);
    supported[i++] = OBT_PROP_ATOM(NET_SUPPORTING_WM_CHECK);
    supported[i++] = OBT_PROP_ATOM(NET_WM_FULL_PLACEMENT);
    supported[i++] = OBT_PROP_ATOM(NET_CURRENT_DESKTOP);
    supported[i++] = OBT_PROP_ATOM(NET_NUMBER_OF_DESKTOPS);
    supported[i++] = OBT_PROP_ATOM(NET_DESKTOP_GEOMETRY);
    supported[i++] = OBT_PROP_ATOM(NET_DESKTOP_VIEWPORT);
    supported[i++] = OBT_PROP_ATOM(NET_ACTIVE_WINDOW);
    supported[i++] = OBT_PROP_ATOM(NET_WORKAREA);
    supported[i++] = OBT_PROP_ATOM(NET_CLIENT_LIST);
    supported[i++] = OBT_PROP_ATOM(NET_CLIENT_LIST_STACKING);
    supported[i++] = OBT_PROP_ATOM(NET_DESKTOP_NAMES);
    supported[i++] = OBT_PROP_ATOM(NET_CLOSE_WINDOW);
    supported[i++] = OBT_PROP_ATOM(NET_DESKTOP_LAYOUT);
    supported[i++] = OBT_PROP_ATOM(NET_SHOWING_DESKTOP);
    supported[i++] = OBT_PROP_ATOM(NET_WM_NAME);
    supported[i++] = OBT_PROP_ATOM(NET_WM_VISIBLE_NAME);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ICON_NAME);
    supported[i++] = OBT_PROP_ATOM(NET_WM_VISIBLE_ICON_NAME);
    supported[i++] = OBT_PROP_ATOM(NET_WM_DESKTOP);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STRUT);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STRUT_PARTIAL);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ICON);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ICON_GEOMETRY);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_DESKTOP);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_DOCK);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_TOOLBAR);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_MENU);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_UTILITY);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_SPLASH);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_DIALOG);
    supported[i++] = OBT_PROP_ATOM(NET_WM_WINDOW_TYPE_NORMAL);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ALLOWED_ACTIONS);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_MOVE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_RESIZE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_MINIMIZE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_SHADE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_MAXIMIZE_HORZ);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_MAXIMIZE_VERT);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_FULLSCREEN);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_CHANGE_DESKTOP);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_CLOSE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_ABOVE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_ACTION_BELOW);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_MODAL);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_MAXIMIZED_VERT);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_MAXIMIZED_HORZ);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_SHADED);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_SKIP_TASKBAR);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_SKIP_PAGER);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_HIDDEN);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_FULLSCREEN);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_ABOVE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_BELOW);
    supported[i++] = OBT_PROP_ATOM(NET_WM_STATE_DEMANDS_ATTENTION);
    supported[i++] = OBT_PROP_ATOM(NET_MOVERESIZE_WINDOW);
    supported[i++] = OBT_PROP_ATOM(NET_WM_MOVERESIZE);
    supported[i++] = OBT_PROP_ATOM(NET_WM_USER_TIME);
/*
    supported[i++] = OBT_PROP_ATOM(NET_WM_USER_TIME_WINDOW);
*/
    supported[i++] = OBT_PROP_ATOM(NET_FRAME_EXTENTS);
    supported[i++] = OBT_PROP_ATOM(NET_REQUEST_FRAME_EXTENTS);
    supported[i++] = OBT_PROP_ATOM(NET_RESTACK_WINDOW);
    supported[i++] = OBT_PROP_ATOM(NET_STARTUP_ID);
#ifdef SYNC
    supported[i++] = OBT_PROP_ATOM(NET_WM_SYNC_REQUEST);
    supported[i++] = OBT_PROP_ATOM(NET_WM_SYNC_REQUEST_COUNTER);
#endif
    supported[i++] = OBT_PROP_ATOM(NET_WM_PID);
    supported[i++] = OBT_PROP_ATOM(NET_WM_PING);

    supported[i++] = OBT_PROP_ATOM(KDE_WM_CHANGE_STATE);
    supported[i++] = OBT_PROP_ATOM(KDE_NET_WM_FRAME_STRUT);
    supported[i++] = OBT_PROP_ATOM(KDE_NET_WM_WINDOW_TYPE_OVERRIDE);

    supported[i++] = OBT_PROP_ATOM(OB_WM_ACTION_UNDECORATE);
    supported[i++] = OBT_PROP_ATOM(OB_WM_STATE_UNDECORATED);
    supported[i++] = OBT_PROP_ATOM(OPENBOX_PID);
    supported[i++] = OBT_PROP_ATOM(OB_THEME);
    supported[i++] = OBT_PROP_ATOM(OB_CONFIG_FILE);
    supported[i++] = OBT_PROP_ATOM(OB_CONTROL);
    g_assert(i == num_support);

    OBT_PROP_SETA32(obt_root(ob_screen),
                    NET_SUPPORTED, ATOM, supported, num_support);
    g_free(supported);

    screen_tell_ksplash();

    return TRUE;
}

static void screen_tell_ksplash(void)
{
    XEvent e;
    char **argv;

    argv = g_new(gchar*, 6);
    argv[0] = g_strdup("dcop");
    argv[1] = g_strdup("ksplash");
    argv[2] = g_strdup("ksplash");
    argv[3] = g_strdup("upAndRunning(QString)");
    argv[4] = g_strdup("wm started");
    argv[5] = NULL;

    /* tell ksplash through the dcop server command line interface */
    g_spawn_async(NULL, argv, NULL,
                  G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                  G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_STDOUT_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
    g_strfreev(argv);

    /* i'm not sure why we do this, kwin does it, but ksplash doesn't seem to
       hear it anyways. perhaps it is for old ksplash. or new ksplash. or
       something. oh well. */
    e.xclient.type = ClientMessage;
    e.xclient.display = obt_display;
    e.xclient.window = obt_root(ob_screen);
    e.xclient.message_type =
        XInternAtom(obt_display, "_KDE_SPLASH_PROGRESS", False);
    e.xclient.format = 8;
    strcpy(e.xclient.data.b, "wm started");
    XSendEvent(obt_display, obt_root(ob_screen),
               False, SubstructureNotifyMask, &e);
}

void screen_startup(gboolean reconfig)
{
    gchar **names = NULL;
    guint32 d;
    gboolean namesexist = FALSE;

    desktop_popup = pager_popup_new();
    pager_popup_height(desktop_popup, POPUP_HEIGHT);

    if (reconfig) {
        /* update the pager popup's width */
        pager_popup_text_width_to_strings(desktop_popup,
                                          screen_desktop_names,
                                          screen_num_desktops);
        return;
    }

    /* get the initial size */
    screen_resize();

    /* have names already been set for the desktops? */
    if (OBT_PROP_GETSS(obt_root(ob_screen), NET_DESKTOP_NAMES, utf8, &names)) {
        g_strfreev(names);
        namesexist = TRUE;
    }

    /* if names don't exist and we have session names, set those.
       do this stuff BEFORE setting the number of desktops, because that
       will create default names for them
    */
    if (!namesexist && session_desktop_names != NULL) {
        guint i, numnames;
        GSList *it;

        /* get the desktop names */
        numnames = g_slist_length(session_desktop_names);
        names = g_new(gchar*, numnames + 1);
        names[numnames] = NULL;
        for (i = 0, it = session_desktop_names; it; ++i, it = g_slist_next(it))
            names[i] = g_strdup(it->data);

        /* set the root window property */
        OBT_PROP_SETSS(obt_root(ob_screen),
                       NET_DESKTOP_NAMES, utf8, (const gchar**)names);

        g_strfreev(names);
    }

    /* set the number of desktops, if it's not already set.

       this will also set the default names from the config file up for
       desktops that don't have names yet */
    screen_num_desktops = 0;
    if (OBT_PROP_GET32(obt_root(ob_screen),
                       NET_NUMBER_OF_DESKTOPS, CARDINAL, &d))
    {
        if (d != config_desktops_num) {
            /* TRANSLATORS: If you need to specify a different order of the
               arguments, you can use %1$d for the first one and %2$d for the
               second one. For example,
               "The current session has %2$d desktops, but Openbox is configured for %1$d ..." */
            g_warning(_("Openbox is configured for %d desktops, but the current session has %d.  Overriding the Openbox configuration."),
                      config_desktops_num, d);
        }
        screen_set_num_desktops(d);
    }
    /* restore from session if possible */
    else if (session_num_desktops)
        screen_set_num_desktops(session_num_desktops);
    else
        screen_set_num_desktops(config_desktops_num);

    screen_desktop = screen_num_desktops;  /* something invalid */
    /* start on the current desktop when a wm was already running */
    if (OBT_PROP_GET32(obt_root(ob_screen),
                       NET_CURRENT_DESKTOP, CARDINAL, &d) &&
        d < screen_num_desktops)
    {
        screen_set_desktop(d, FALSE);
    } else if (session_desktop >= 0)
        screen_set_desktop(MIN((guint)session_desktop,
                               screen_num_desktops), FALSE);
    else
        screen_set_desktop(MIN(config_screen_firstdesk,
                               screen_num_desktops) - 1, FALSE);
    screen_last_desktop = screen_desktop;

    /* don't start in showing-desktop mode */
    screen_showing_desktop = FALSE;
    OBT_PROP_SET32(obt_root(ob_screen),
                   NET_SHOWING_DESKTOP, CARDINAL, screen_showing_desktop);

    if (session_desktop_layout_present &&
        screen_validate_layout(&session_desktop_layout))
    {
        screen_desktop_layout = session_desktop_layout;
    }
    else
        screen_update_layout();
}

void screen_shutdown(gboolean reconfig)
{
    pager_popup_free(desktop_popup);

    if (reconfig)
        return;

    XSelectInput(obt_display, obt_root(ob_screen), NoEventMask);

    /* we're not running here no more! */
    OBT_PROP_ERASE(obt_root(ob_screen), OPENBOX_PID);
    /* not without us */
    OBT_PROP_ERASE(obt_root(ob_screen), NET_SUPPORTED);
    /* don't keep this mode */
    OBT_PROP_ERASE(obt_root(ob_screen), NET_SHOWING_DESKTOP);

    XDestroyWindow(obt_display, screen_support_win);

    g_strfreev(screen_desktop_names);
    screen_desktop_names = NULL;
}

void screen_resize(void)
{
    static gint oldw = 0, oldh = 0;
    gint w, h;
    GList *it;
    gulong geometry[2];

    w = WidthOfScreen(ScreenOfDisplay(obt_display, ob_screen));
    h = HeightOfScreen(ScreenOfDisplay(obt_display, ob_screen));

    if (w == oldw && h == oldh) return;

    oldw = w; oldh = h;

    /* Set the _NET_DESKTOP_GEOMETRY hint */
    screen_physical_size.width = geometry[0] = w;
    screen_physical_size.height = geometry[1] = h;
    OBT_PROP_SETA32(obt_root(ob_screen),
                    NET_DESKTOP_GEOMETRY, CARDINAL, geometry, 2);

    if (ob_state() != OB_STATE_RUNNING)
        return;

    screen_update_areas();
    dock_configure();

    for (it = client_list; it; it = g_list_next(it))
        client_move_onscreen(it->data, FALSE);
}

void screen_set_num_desktops(guint num)
{
    guint old;
    gulong *viewport;
    GList *it, *stacking_copy;

    g_assert(num > 0);

    if (screen_num_desktops == num) return;

    old = screen_num_desktops;
    screen_num_desktops = num;
    OBT_PROP_SET32(obt_root(ob_screen), NET_NUMBER_OF_DESKTOPS, CARDINAL, num);

    /* set the viewport hint */
    viewport = g_new0(gulong, num * 2);
    OBT_PROP_SETA32(obt_root(ob_screen),
                    NET_DESKTOP_VIEWPORT, CARDINAL, viewport, num * 2);
    g_free(viewport);

    /* the number of rows/columns will differ */
    screen_update_layout();

    /* move windows on desktops that will no longer exist!
       make a copy of the list cuz we're changing it */
    stacking_copy = g_list_copy(stacking_list);
    for (it = g_list_last(stacking_copy); it; it = g_list_previous(it)) {
        if (WINDOW_IS_CLIENT(it->data)) {
            ObClient *c = it->data;
            if (c->desktop != DESKTOP_ALL && c->desktop >= num)
                client_set_desktop(c, num - 1, FALSE, TRUE);
            /* raise all the windows that are on the current desktop which
               is being merged */
            else if (screen_desktop == num - 1 &&
                     (c->desktop == DESKTOP_ALL ||
                      c->desktop == screen_desktop))
                stacking_raise(CLIENT_AS_WINDOW(c));
        }
    }
    g_list_free(stacking_copy);

    /* change our struts/area to match (after moving windows) */
    screen_update_areas();

    /* may be some unnamed desktops that we need to fill in with names
       (after updating the areas so the popup can resize) */
    screen_update_desktop_names();

    /* change our desktop if we're on one that no longer exists! */
    if (screen_desktop >= screen_num_desktops)
        screen_set_desktop(num - 1, TRUE);
}

static void screen_fallback_focus(void)
{
    ObClient *c;
    gboolean allow_omni;

    /* only allow omnipresent windows to get focus on desktop change if
       an omnipresent window is already focused (it'll keep focus probably, but
       maybe not depending on mouse-focus options) */
    allow_omni = focus_client && (client_normal(focus_client) &&
                                  focus_client->desktop == DESKTOP_ALL);

    /* the client moved there already so don't move focus. prevent flicker
       on sendtodesktop + follow */
    if (focus_client && focus_client->desktop == screen_desktop)
        return;

    /* have to try focus here because when you leave an empty desktop
       there is no focus out to watch for. also, we have different rules
       here. we always allow it to look under the mouse pointer if
       config_focus_last is FALSE

       do this before hiding the windows so if helper windows are coming
       with us, they don't get hidden
    */
    if ((c = focus_fallback(TRUE, !config_focus_last, allow_omni,
                            !allow_omni)))
    {
        /* only do the flicker reducing stuff ahead of time if we are going
           to call xsetinputfocus on the window ourselves. otherwise there is
           no guarantee the window will actually take focus.. */
        if (c->can_focus) {
            /* reduce flicker by hiliting now rather than waiting for the
               server FocusIn event */
            frame_adjust_focus(c->frame, TRUE);
            /* do this here so that if you switch desktops to a window with
               helper windows then the helper windows won't flash */
            client_bring_helper_windows(c);
        }
    }
}

static gboolean last_desktop_func(gpointer data)
{
    screen_desktop_timeout = TRUE;
    return FALSE;
}

void screen_set_desktop(guint num, gboolean dofocus)
{
    GList *it;
    guint previous;
    gulong ignore_start;

    g_assert(num < screen_num_desktops);

    previous = screen_desktop;
    screen_desktop = num;

    if (previous == num) return;

    OBT_PROP_SET32(obt_root(ob_screen), NET_CURRENT_DESKTOP, CARDINAL, num);

    /* This whole thing decides when/how to save the screen_last_desktop so
       that it can be restored later if you want */
    if (screen_desktop_timeout) {
        /* If screen_desktop_timeout is true, then we've been on this desktop
           long enough and we can save it as the last desktop. */

        if (screen_last_desktop == previous)
            /* this is the startup state only */
            screen_old_desktop = screen_desktop;
        else {
            /* save the "last desktop" as the "old desktop" */
            screen_old_desktop = screen_last_desktop;
            /* save the desktop we're coming from as the "last desktop" */
            screen_last_desktop = previous;
        }
    }
    else {
        /* If screen_desktop_timeout is false, then we just got to this desktop
           and we are moving away again. */

        if (screen_desktop == screen_last_desktop) {
            /* If we are moving to the "last desktop" .. */
            if (previous == screen_old_desktop) {
                /* .. from the "old desktop", change the last desktop to
                   be where we are coming from */
                screen_last_desktop = screen_old_desktop;
            }
            else if (screen_last_desktop == screen_old_desktop) {
                /* .. and also to the "old desktop", change the "last
                   desktop" to be where we are coming from */
                screen_last_desktop = previous;
            }
            else {
                /* .. from some other desktop, then set the "last desktop" to
                   be the saved "old desktop", i.e. where we were before the
                   "last desktop" */
                screen_last_desktop = screen_old_desktop;
            }
        }
        else {
            /* If we are moving to any desktop besides the "last desktop"..
               (this is the normal case) */
            if (screen_desktop == screen_old_desktop) {
                /* If moving to the "old desktop", which is not the
                   "last desktop", don't save anything */
            }
            else if (previous == screen_old_desktop) {
                /* If moving from the "old desktop", and not to the
                   "last desktop", don't save anything */
            }
            else if (screen_last_desktop == screen_old_desktop) {
                /* If the "last desktop" is the same as "old desktop" and
                   you're not moving to the "last desktop" then save where
                   we're coming from as the "last desktop" */
                screen_last_desktop = previous;
            }
            else {
                /* If the "last desktop" is different from the "old desktop"
                   and you're not moving to the "last desktop", then don't save
                   anything */
            }
        }
    }
    screen_desktop_timeout = FALSE;
    obt_main_loop_timeout_remove(ob_main_loop, last_desktop_func);
    obt_main_loop_timeout_add(ob_main_loop, REMEMBER_LAST_DESKTOP_TIME,
                              last_desktop_func, NULL, NULL, NULL);

    ob_debug("Moving to desktop %d", num+1);

    if (ob_state() == OB_STATE_RUNNING)
        screen_show_desktop_popup(screen_desktop);

    /* ignore enter events caused by the move */
    ignore_start = event_start_ignore_all_enters();

    if (moveresize_client)
        client_set_desktop(moveresize_client, num, TRUE, FALSE);

    /* show windows before hiding the rest to lessen the enter/leave events */

    /* show windows from top to bottom */
    for (it = stacking_list; it; it = g_list_next(it)) {
        if (WINDOW_IS_CLIENT(it->data)) {
            ObClient *c = it->data;
            client_show(c);
        }
    }

    if (dofocus) screen_fallback_focus();

    /* hide windows from bottom to top */
    for (it = g_list_last(stacking_list); it; it = g_list_previous(it)) {
        if (WINDOW_IS_CLIENT(it->data)) {
            ObClient *c = it->data;
            client_hide(c);
        }
    }

    event_end_ignore_all_enters(ignore_start);

    if (event_curtime != CurrentTime)
        screen_desktop_user_time = event_curtime;

    hooks_queue(OB_HOOK_SCREEN_DESK_CHANGE, NULL);
}

void screen_add_desktop(gboolean current)
{
    gulong ignore_start;

    /* ignore enter events caused by this */
    ignore_start = event_start_ignore_all_enters();

    screen_set_num_desktops(screen_num_desktops+1);

    /* move all the clients over */
    if (current) {
        GList *it;

        for (it = client_list; it; it = g_list_next(it)) {
            ObClient *c = it->data;
            if (c->desktop != DESKTOP_ALL && c->desktop >= screen_desktop &&
                /* don't move direct children, they'll be moved with their
                   parent - which will have to be on the same desktop */
                !client_direct_parent(c))
            {
                ob_debug("moving window %s", c->title);
                client_set_desktop(c, c->desktop+1, FALSE, TRUE);
            }
        }
    }

    event_end_ignore_all_enters(ignore_start);
}

void screen_remove_desktop(gboolean current)
{
    guint rmdesktop, movedesktop;
    GList *it, *stacking_copy;
    gulong ignore_start;

    if (screen_num_desktops <= 1) return;

    /* ignore enter events caused by this */
    ignore_start = event_start_ignore_all_enters();

    /* what desktop are we removing and moving to? */
    if (current)
        rmdesktop = screen_desktop;
    else
        rmdesktop = screen_num_desktops - 1;
    if (rmdesktop < screen_num_desktops - 1)
        movedesktop = rmdesktop + 1;
    else
        movedesktop = rmdesktop;

    /* make a copy of the list cuz we're changing it */
    stacking_copy = g_list_copy(stacking_list);
    for (it = g_list_last(stacking_copy); it; it = g_list_previous(it)) {
        if (WINDOW_IS_CLIENT(it->data)) {
            ObClient *c = it->data;
            guint d = c->desktop;
            if (d != DESKTOP_ALL && d >= movedesktop &&
                /* don't move direct children, they'll be moved with their
                   parent - which will have to be on the same desktop */
                !client_direct_parent(c))
            {
                ob_debug("moving window %s", c->title);
                client_set_desktop(c, c->desktop - 1, TRUE, TRUE);
            }
            /* raise all the windows that are on the current desktop which
               is being merged */
            if ((screen_desktop == rmdesktop - 1 ||
                 screen_desktop == rmdesktop) &&
                (d == DESKTOP_ALL || d == screen_desktop))
            {
                stacking_raise(CLIENT_AS_WINDOW(c));
                ob_debug("raising window %s", c->title);
            }
        }
    }
    g_list_free(stacking_copy);

    /* fallback focus like we're changing desktops */
    if (screen_desktop < screen_num_desktops - 1) {
        screen_fallback_focus();
        ob_debug("fake desktop change");
    }

    screen_set_num_desktops(screen_num_desktops-1);

    event_end_ignore_all_enters(ignore_start);
}

static void get_row_col(guint d, guint *r, guint *c)
{
    switch (screen_desktop_layout.orientation) {
    case OB_ORIENTATION_HORZ:
        switch (screen_desktop_layout.start_corner) {
        case OB_CORNER_TOPLEFT:
            *r = d / screen_desktop_layout.columns;
            *c = d % screen_desktop_layout.columns;
            break;
        case OB_CORNER_BOTTOMLEFT:
            *r = screen_desktop_layout.rows - 1 -
                d / screen_desktop_layout.columns;
            *c = d % screen_desktop_layout.columns;
            break;
        case OB_CORNER_TOPRIGHT:
            *r = d / screen_desktop_layout.columns;
            *c = screen_desktop_layout.columns - 1 -
                d % screen_desktop_layout.columns;
            break;
        case OB_CORNER_BOTTOMRIGHT:
            *r = screen_desktop_layout.rows - 1 -
                d / screen_desktop_layout.columns;
            *c = screen_desktop_layout.columns - 1 -
                d % screen_desktop_layout.columns;
            break;
        }
        break;
    case OB_ORIENTATION_VERT:
        switch (screen_desktop_layout.start_corner) {
        case OB_CORNER_TOPLEFT:
            *r = d % screen_desktop_layout.rows;
            *c = d / screen_desktop_layout.rows;
            break;
        case OB_CORNER_BOTTOMLEFT:
            *r = screen_desktop_layout.rows - 1 -
                d % screen_desktop_layout.rows;
            *c = d / screen_desktop_layout.rows;
            break;
        case OB_CORNER_TOPRIGHT:
            *r = d % screen_desktop_layout.rows;
            *c = screen_desktop_layout.columns - 1 -
                d / screen_desktop_layout.rows;
            break;
        case OB_CORNER_BOTTOMRIGHT:
            *r = screen_desktop_layout.rows - 1 -
                d % screen_desktop_layout.rows;
            *c = screen_desktop_layout.columns - 1 -
                d / screen_desktop_layout.rows;
            break;
        }
        break;
    }
}

static guint translate_row_col(guint r, guint c)
{
    switch (screen_desktop_layout.orientation) {
    case OB_ORIENTATION_HORZ:
        switch (screen_desktop_layout.start_corner) {
        case OB_CORNER_TOPLEFT:
            return r % screen_desktop_layout.rows *
                screen_desktop_layout.columns +
                c % screen_desktop_layout.columns;
        case OB_CORNER_BOTTOMLEFT:
            return (screen_desktop_layout.rows - 1 -
                    r % screen_desktop_layout.rows) *
                screen_desktop_layout.columns +
                c % screen_desktop_layout.columns;
        case OB_CORNER_TOPRIGHT:
            return r % screen_desktop_layout.rows *
                screen_desktop_layout.columns +
                (screen_desktop_layout.columns - 1 -
                 c % screen_desktop_layout.columns);
        case OB_CORNER_BOTTOMRIGHT:
            return (screen_desktop_layout.rows - 1 -
                    r % screen_desktop_layout.rows) *
                screen_desktop_layout.columns +
                (screen_desktop_layout.columns - 1 -
                 c % screen_desktop_layout.columns);
        }
    case OB_ORIENTATION_VERT:
        switch (screen_desktop_layout.start_corner) {
        case OB_CORNER_TOPLEFT:
            return c % screen_desktop_layout.columns *
                screen_desktop_layout.rows +
                r % screen_desktop_layout.rows;
        case OB_CORNER_BOTTOMLEFT:
            return c % screen_desktop_layout.columns *
                screen_desktop_layout.rows +
                (screen_desktop_layout.rows - 1 -
                 r % screen_desktop_layout.rows);
        case OB_CORNER_TOPRIGHT:
            return (screen_desktop_layout.columns - 1 -
                    c % screen_desktop_layout.columns) *
                screen_desktop_layout.rows +
                r % screen_desktop_layout.rows;
        case OB_CORNER_BOTTOMRIGHT:
            return (screen_desktop_layout.columns - 1 -
                    c % screen_desktop_layout.columns) *
                screen_desktop_layout.rows +
                (screen_desktop_layout.rows - 1 -
                 r % screen_desktop_layout.rows);
        }
    }
    g_assert_not_reached();
    return 0;
}

static gboolean hide_desktop_popup_func(gpointer data)
{
    pager_popup_hide(desktop_popup);
    return FALSE; /* don't repeat */
}

void screen_show_desktop_popup(guint d)
{
    Rect *a;

    /* 0 means don't show the popup */
    if (!config_desktop_popup_time) return;

    a = screen_physical_area_active();
    pager_popup_position(desktop_popup, CenterGravity,
                         a->x + a->width / 2, a->y + a->height / 2);
    pager_popup_icon_size_multiplier(desktop_popup,
                                     (screen_desktop_layout.columns /
                                      screen_desktop_layout.rows) / 2,
                                     (screen_desktop_layout.rows/
                                      screen_desktop_layout.columns) / 2);
    pager_popup_max_width(desktop_popup,
                          MAX(a->width/3, POPUP_WIDTH));
    pager_popup_show(desktop_popup, screen_desktop_names[d], d);

    obt_main_loop_timeout_remove(ob_main_loop, hide_desktop_popup_func);
    obt_main_loop_timeout_add(ob_main_loop, config_desktop_popup_time * 1000,
                              hide_desktop_popup_func, NULL, NULL, NULL);
    g_free(a);
}

void screen_hide_desktop_popup(void)
{
    obt_main_loop_timeout_remove(ob_main_loop, hide_desktop_popup_func);
    pager_popup_hide(desktop_popup);
}

guint screen_find_desktop(guint from, ObDirection dir,
                          gboolean wrap, gboolean linear)
{
    guint r, c;
    guint d;

    d = from;
    get_row_col(d, &r, &c);
    if (linear) {
        switch (dir) {
        case OB_DIRECTION_EAST:
            if (d < screen_num_desktops - 1)
                ++d;
            else if (wrap)
                d = 0;
            else
                return from;
            break;
        case OB_DIRECTION_WEST:
            if (d > 0)
                --d;
            else if (wrap)
                d = screen_num_desktops - 1;
            else
                return from;
            break;
        default:
            g_assert_not_reached();
            return from;
        }
    } else {
        switch (dir) {
        case OB_DIRECTION_EAST:
            ++c;
            if (c >= screen_desktop_layout.columns) {
                if (wrap)
                    c = 0;
                else
                    return from;
            }
            d = translate_row_col(r, c);
            if (d >= screen_num_desktops) {
                if (wrap)
                    ++c;
                else
                    return from;
            }
            break;
        case OB_DIRECTION_WEST:
            --c;
            if (c >= screen_desktop_layout.columns) {
                if (wrap)
                    c = screen_desktop_layout.columns - 1;
                else
                    return from;
            }
            d = translate_row_col(r, c);
            if (d >= screen_num_desktops) {
                if (wrap)
                    --c;
                else
                    return from;
            }
            break;
        case OB_DIRECTION_SOUTH:
            ++r;
            if (r >= screen_desktop_layout.rows) {
                if (wrap)
                    r = 0;
                else
                    return from;
            }
            d = translate_row_col(r, c);
            if (d >= screen_num_desktops) {
                if (wrap)
                    ++r;
                else
                    return from;
            }
            break;
        case OB_DIRECTION_NORTH:
            --r;
            if (r >= screen_desktop_layout.rows) {
                if (wrap)
                    r = screen_desktop_layout.rows - 1;
                else
                    return from;
            }
            d = translate_row_col(r, c);
            if (d >= screen_num_desktops) {
                if (wrap)
                    --r;
                else
                    return from;
            }
            break;
        default:
            g_assert_not_reached();
            return from;
        }

        d = translate_row_col(r, c);
    }
    return d;
}

static gboolean screen_validate_layout(ObDesktopLayout *l)
{
    if (l->columns == 0 && l->rows == 0) /* both 0's is bad data.. */
        return FALSE;

    /* fill in a zero rows/columns */
    if (l->columns == 0) {
        l->columns = screen_num_desktops / l->rows;
        if (l->rows * l->columns < screen_num_desktops)
            l->columns++;
        if (l->rows * l->columns >= screen_num_desktops + l->columns)
            l->rows--;
    } else if (l->rows == 0) {
        l->rows = screen_num_desktops / l->columns;
        if (l->columns * l->rows < screen_num_desktops)
            l->rows++;
        if (l->columns * l->rows >= screen_num_desktops + l->rows)
            l->columns--;
    }

    /* bounds checking */
    if (l->orientation == OB_ORIENTATION_HORZ) {
        l->columns = MIN(screen_num_desktops, l->columns);
        l->rows = MIN(l->rows,
                      (screen_num_desktops + l->columns - 1) / l->columns);
        l->columns = screen_num_desktops / l->rows +
            !!(screen_num_desktops % l->rows);
    } else {
        l->rows = MIN(screen_num_desktops, l->rows);
        l->columns = MIN(l->columns,
                         (screen_num_desktops + l->rows - 1) / l->rows);
        l->rows = screen_num_desktops / l->columns +
            !!(screen_num_desktops % l->columns);
    }
    return TRUE;
}

void screen_update_layout(void)

{
    ObDesktopLayout l;
    guint32 *data;
    guint num;

    screen_desktop_layout.orientation = OB_ORIENTATION_HORZ;
    screen_desktop_layout.start_corner = OB_CORNER_TOPLEFT;
    screen_desktop_layout.rows = 1;
    screen_desktop_layout.columns = screen_num_desktops;

    if (OBT_PROP_GETA32(obt_root(ob_screen),
                        NET_DESKTOP_LAYOUT, CARDINAL, &data, &num)) {
        if (num == 3 || num == 4) {

            if (data[0] == OBT_PROP_ATOM(NET_WM_ORIENTATION_VERT))
                l.orientation = OB_ORIENTATION_VERT;
            else if (data[0] == OBT_PROP_ATOM(NET_WM_ORIENTATION_HORZ))
                l.orientation = OB_ORIENTATION_HORZ;
            else
                return;

            if (num < 4)
                l.start_corner = OB_CORNER_TOPLEFT;
            else {
                if (data[3] == OBT_PROP_ATOM(NET_WM_TOPLEFT))
                    l.start_corner = OB_CORNER_TOPLEFT;
                else if (data[3] == OBT_PROP_ATOM(NET_WM_TOPRIGHT))
                    l.start_corner = OB_CORNER_TOPRIGHT;
                else if (data[3] == OBT_PROP_ATOM(NET_WM_BOTTOMRIGHT))
                    l.start_corner = OB_CORNER_BOTTOMRIGHT;
                else if (data[3] == OBT_PROP_ATOM(NET_WM_BOTTOMLEFT))
                    l.start_corner = OB_CORNER_BOTTOMLEFT;
                else
                    return;
            }

            l.columns = data[1];
            l.rows = data[2];

            if (screen_validate_layout(&l))
                screen_desktop_layout = l;

            g_free(data);
        }
    }
}

void screen_update_desktop_names(void)
{
    guint i;

    /* empty the array */
    g_strfreev(screen_desktop_names);
    screen_desktop_names = NULL;

    if (OBT_PROP_GETSS(obt_root(ob_screen),
                       NET_DESKTOP_NAMES, utf8, &screen_desktop_names))
        for (i = 0; screen_desktop_names[i] && i < screen_num_desktops; ++i);
    else
        i = 0;
    if (i < screen_num_desktops) {
        GSList *it;

        screen_desktop_names = g_renew(gchar*, screen_desktop_names,
                                       screen_num_desktops + 1);
        screen_desktop_names[screen_num_desktops] = NULL;

        it = g_slist_nth(config_desktops_names, i);

        for (; i < screen_num_desktops; ++i) {
            if (it && ((char*)it->data)[0]) /* not empty */
                /* use the names from the config file when possible */
                screen_desktop_names[i] = g_strdup(it->data);
            else
                /* make up a nice name if it's not though */
                screen_desktop_names[i] = g_strdup_printf(_("desktop %i"),
                                                          i + 1);
            if (it) it = g_slist_next(it);
        }

        /* if we changed any names, then set the root property so we can
           all agree on the names */
        OBT_PROP_SETSS(obt_root(ob_screen), NET_DESKTOP_NAMES,
                       utf8, (const gchar**)screen_desktop_names);
    }

    /* resize the pager for these names */
    pager_popup_text_width_to_strings(desktop_popup,
                                      screen_desktop_names,
                                      screen_num_desktops);
}

void screen_show_desktop(gboolean show, ObClient *show_only)
{
    GList *it;

    if (show == screen_showing_desktop) return; /* no change */

    screen_showing_desktop = show;

    if (show) {
        /* hide windows bottom to top */
        for (it = g_list_last(stacking_list); it; it = g_list_previous(it)) {
            if (WINDOW_IS_CLIENT(it->data)) {
                ObClient *client = it->data;
                client_showhide(client);
            }
        }
    }
    else {
        /* restore windows top to bottom */
        for (it = stacking_list; it; it = g_list_next(it)) {
            if (WINDOW_IS_CLIENT(it->data)) {
                ObClient *client = it->data;
                if (client_should_show(client)) {
                    if (!show_only || client == show_only)
                        client_show(client);
                    else
                        client_iconify(client, TRUE, FALSE, TRUE);
                }
            }
        }
    }

    if (show) {
        /* focus the desktop */
        for (it = focus_order; it; it = g_list_next(it)) {
            ObClient *c = it->data;
            if (c->type == OB_CLIENT_TYPE_DESKTOP &&
                (c->desktop == screen_desktop || c->desktop == DESKTOP_ALL) &&
                client_focus(it->data))
                break;
        }
    }
    else if (!show_only) {
        ObClient *c;

        if ((c = focus_fallback(TRUE, FALSE, TRUE, FALSE))) {
            /* only do the flicker reducing stuff ahead of time if we are going
               to call xsetinputfocus on the window ourselves. otherwise there
               is no guarantee the window will actually take focus.. */
            if (c->can_focus) {
                /* reduce flicker by hiliting now rather than waiting for the
                   server FocusIn event */
                frame_adjust_focus(c->frame, TRUE);
            }
        }
    }

    show = !!show; /* make it boolean */
    OBT_PROP_SET32(obt_root(ob_screen), NET_SHOWING_DESKTOP, CARDINAL, show);
}

void screen_install_colormap(ObClient *client, gboolean install)
{
    if (client == NULL || client->colormap == None) {
        if (install)
            XInstallColormap(obt_display, RrColormap(ob_rr_inst));
        else
            XUninstallColormap(obt_display, RrColormap(ob_rr_inst));
    } else {
        obt_display_ignore_errors(TRUE);
        if (install)
            XInstallColormap(obt_display, client->colormap);
        else
            XUninstallColormap(obt_display, client->colormap);
        obt_display_ignore_errors(FALSE);
    }
}

#define STRUT_LEFT_ON_MONITOR(s, i) \
    (RANGES_INTERSECT(s->left_start, s->left_end - s->left_start + 1, \
                      monitor_area[i].y, monitor_area[i].height))
#define STRUT_RIGHT_ON_MONITOR(s, i) \
    (RANGES_INTERSECT(s->right_start, s->right_end - s->right_start + 1, \
                      monitor_area[i].y, monitor_area[i].height))
#define STRUT_TOP_ON_MONITOR(s, i) \
    (RANGES_INTERSECT(s->top_start, s->top_end - s->top_start + 1, \
                      monitor_area[i].x, monitor_area[i].width))
#define STRUT_BOTTOM_ON_MONITOR(s, i) \
    (RANGES_INTERSECT(s->bottom_start, s->bottom_end - s->bottom_start + 1, \
                      monitor_area[i].x, monitor_area[i].width))

typedef struct {
    guint desktop;
    StrutPartial *strut;
} ObScreenStrut;

#define RESET_STRUT_LIST(sl) \
    (g_slist_free(sl), sl = NULL)

#define ADD_STRUT_TO_LIST(sl, d, s) \
{ \
    ObScreenStrut *ss = g_new(ObScreenStrut, 1); \
    ss->desktop = d; \
    ss->strut = s;  \
    sl = g_slist_prepend(sl, ss); \
}

#define VALIDATE_STRUTS(sl, side, max) \
{ \
    GSList *it; \
    for (it = sl; it; it = g_slist_next(it)) { \
      ObScreenStrut *ss = it->data; \
      ss->strut->side = MIN(max, ss->strut->side); \
    } \
}

static void get_xinerama_screens(Rect **xin_areas, guint *nxin)
{
    guint i;
    gint l, r, t, b;

    if (ob_debug_xinerama) {
        gint w = WidthOfScreen(ScreenOfDisplay(obt_display, ob_screen));
        gint h = HeightOfScreen(ScreenOfDisplay(obt_display, ob_screen));
        *nxin = 2;
        *xin_areas = g_new(Rect, *nxin + 1);
        RECT_SET((*xin_areas)[0], 0, 0, w/2, h);
        RECT_SET((*xin_areas)[1], w/2, 0, w-(w/2), h);
    }
#ifdef XINERAMA
    else if (obt_display_extension_xinerama) {
        guint i;
        gint n;
        XineramaScreenInfo *info = XineramaQueryScreens(obt_display, &n);
        *nxin = n;
        *xin_areas = g_new(Rect, *nxin + 1);
        for (i = 0; i < *nxin; ++i)
            RECT_SET((*xin_areas)[i], info[i].x_org, info[i].y_org,
                     info[i].width, info[i].height);
        XFree(info);
    }
#endif
    else {
        *nxin = 1;
        *xin_areas = g_new(Rect, *nxin + 1);
        RECT_SET((*xin_areas)[0], 0, 0,
                 WidthOfScreen(ScreenOfDisplay(obt_display, ob_screen)),
                 HeightOfScreen(ScreenOfDisplay(obt_display, ob_screen)));
    }

    /* returns one extra with the total area in it */
    l = (*xin_areas)[0].x;
    t = (*xin_areas)[0].y;
    r = (*xin_areas)[0].x + (*xin_areas)[0].width - 1;
    b = (*xin_areas)[0].y + (*xin_areas)[0].height - 1;
    for (i = 1; i < *nxin; ++i) {
        l = MIN(l, (*xin_areas)[i].x);
        t = MIN(l, (*xin_areas)[i].y);
        r = MAX(r, (*xin_areas)[i].x + (*xin_areas)[i].width - 1);
        b = MAX(b, (*xin_areas)[i].y + (*xin_areas)[i].height - 1);
    }
    RECT_SET((*xin_areas)[*nxin], l, t, r - l + 1, b - t + 1);
}

void screen_update_areas(void)
{
    guint i, j;
    gulong *dims;
    GList *it;
    GSList *sit;

    g_free(monitor_area);
    get_xinerama_screens(&monitor_area, &screen_num_monitors);

    /* set up the user-specified margins */
    config_margins.top_start = RECT_LEFT(monitor_area[screen_num_monitors]);
    config_margins.top_end = RECT_RIGHT(monitor_area[screen_num_monitors]);
    config_margins.bottom_start = RECT_LEFT(monitor_area[screen_num_monitors]);
    config_margins.bottom_end = RECT_RIGHT(monitor_area[screen_num_monitors]);
    config_margins.left_start = RECT_TOP(monitor_area[screen_num_monitors]);
    config_margins.left_end = RECT_BOTTOM(monitor_area[screen_num_monitors]);
    config_margins.right_start = RECT_TOP(monitor_area[screen_num_monitors]);
    config_margins.right_end = RECT_BOTTOM(monitor_area[screen_num_monitors]);

    dims = g_new(gulong, 4 * screen_num_desktops * screen_num_monitors);

    RESET_STRUT_LIST(struts_left);
    RESET_STRUT_LIST(struts_top);
    RESET_STRUT_LIST(struts_right);
    RESET_STRUT_LIST(struts_bottom);

    /* collect the struts */
    for (it = client_list; it; it = g_list_next(it)) {
        ObClient *c = it->data;
        if (c->strut.left)
            ADD_STRUT_TO_LIST(struts_left, c->desktop, &c->strut);
        if (c->strut.top)
            ADD_STRUT_TO_LIST(struts_top, c->desktop, &c->strut);
        if (c->strut.right)
            ADD_STRUT_TO_LIST(struts_right, c->desktop, &c->strut);
        if (c->strut.bottom)
            ADD_STRUT_TO_LIST(struts_bottom, c->desktop, &c->strut);
    }
    if (dock_strut.left)
        ADD_STRUT_TO_LIST(struts_left, DESKTOP_ALL, &dock_strut);
    if (dock_strut.top)
        ADD_STRUT_TO_LIST(struts_top, DESKTOP_ALL, &dock_strut);
    if (dock_strut.right)
        ADD_STRUT_TO_LIST(struts_right, DESKTOP_ALL, &dock_strut);
    if (dock_strut.bottom)
        ADD_STRUT_TO_LIST(struts_bottom, DESKTOP_ALL, &dock_strut);

    if (config_margins.left)
        ADD_STRUT_TO_LIST(struts_left, DESKTOP_ALL, &config_margins);
    if (config_margins.top)
        ADD_STRUT_TO_LIST(struts_top, DESKTOP_ALL, &config_margins);
    if (config_margins.right)
        ADD_STRUT_TO_LIST(struts_right, DESKTOP_ALL, &config_margins);
    if (config_margins.bottom)
        ADD_STRUT_TO_LIST(struts_bottom, DESKTOP_ALL, &config_margins);

    VALIDATE_STRUTS(struts_left, left,
                    monitor_area[screen_num_monitors].width / 2);
    VALIDATE_STRUTS(struts_right, right,
                    monitor_area[screen_num_monitors].width / 2);
    VALIDATE_STRUTS(struts_top, top,
                    monitor_area[screen_num_monitors].height / 2);
    VALIDATE_STRUTS(struts_bottom, bottom,
                    monitor_area[screen_num_monitors].height / 2);

    /* set up the work areas to be full screen */
    for (i = 0; i < screen_num_monitors; ++i)
        for (j = 0; j < screen_num_desktops; ++j) {
            dims[(i * screen_num_desktops + j) * 4+0] = monitor_area[i].x;
            dims[(i * screen_num_desktops + j) * 4+1] = monitor_area[i].y;
            dims[(i * screen_num_desktops + j) * 4+2] = monitor_area[i].width;
            dims[(i * screen_num_desktops + j) * 4+3] = monitor_area[i].height;
        }

    /* calculate the work areas from the struts */
    for (i = 0; i < screen_num_monitors; ++i)
        for (j = 0; j < screen_num_desktops; ++j) {
            gint l = 0, r = 0, t = 0, b = 0;

            /* only add the strut to the area if it touches the monitor */

            for (sit = struts_left; sit; sit = g_slist_next(sit)) {
                ObScreenStrut *s = sit->data;
                if ((s->desktop == j || s->desktop == DESKTOP_ALL) &&
                    STRUT_LEFT_ON_MONITOR(s->strut, i))
                    l = MAX(l, s->strut->left);
            }
            for (sit = struts_top; sit; sit = g_slist_next(sit)) {
                ObScreenStrut *s = sit->data;
                if ((s->desktop == j || s->desktop == DESKTOP_ALL) &&
                    STRUT_TOP_ON_MONITOR(s->strut, i))
                    t = MAX(t, s->strut->top);
            }
            for (sit = struts_right; sit; sit = g_slist_next(sit)) {
                ObScreenStrut *s = sit->data;
                if ((s->desktop == j || s->desktop == DESKTOP_ALL) &&
                    STRUT_RIGHT_ON_MONITOR(s->strut, i))
                    r = MAX(r, s->strut->right);
            }
            for (sit = struts_bottom; sit; sit = g_slist_next(sit)) {
                ObScreenStrut *s = sit->data;
                if ((s->desktop == j || s->desktop == DESKTOP_ALL) &&
                    STRUT_BOTTOM_ON_MONITOR(s->strut, i))
                    b = MAX(b, s->strut->bottom);
            }

            /* based on these margins, set the work area for the
               monitor/desktop */
            dims[(i * screen_num_desktops + j) * 4 + 0] += l;
            dims[(i * screen_num_desktops + j) * 4 + 1] += t;
            dims[(i * screen_num_desktops + j) * 4 + 2] -= l + r;
            dims[(i * screen_num_desktops + j) * 4 + 3] -= t + b;
        }

    /* all the work areas are not used here, only the ones for the first
       monitor are */
    OBT_PROP_SETA32(obt_root(ob_screen), NET_WORKAREA, CARDINAL,
                    dims, 4 * screen_num_desktops);

    /* the area has changed, adjust all the windows if they need it */
    for (it = client_list; it; it = g_list_next(it))
        client_reconfigure(it->data, FALSE);

    g_free(dims);
}

#if 0
Rect* screen_area_all_monitors(guint desktop)
{
    guint i;
    Rect *a;

    a = screen_area_monitor(desktop, 0);

    /* combine all the monitors together */
    for (i = 1; i < screen_num_monitors; ++i) {
        Rect *m = screen_area_monitor(desktop, i);
        gint l, r, t, b;

        l = MIN(RECT_LEFT(*a), RECT_LEFT(*m));
        t = MIN(RECT_TOP(*a), RECT_TOP(*m));
        r = MAX(RECT_RIGHT(*a), RECT_RIGHT(*m));
        b = MAX(RECT_BOTTOM(*a), RECT_BOTTOM(*m));

        RECT_SET(*a, l, t, r - l + 1, b - t + 1);

        g_free(m);
    }

    return a;
}
#endif

#define STRUT_LEFT_IN_SEARCH(s, search) \
    (RANGES_INTERSECT(search->y, search->height, \
                      s->left_start, s->left_end - s->left_start + 1))
#define STRUT_RIGHT_IN_SEARCH(s, search) \
    (RANGES_INTERSECT(search->y, search->height, \
                      s->right_start, s->right_end - s->right_start + 1))
#define STRUT_TOP_IN_SEARCH(s, search) \
    (RANGES_INTERSECT(search->x, search->width, \
                      s->top_start, s->top_end - s->top_start + 1))
#define STRUT_BOTTOM_IN_SEARCH(s, search) \
    (RANGES_INTERSECT(search->x, search->width, \
                      s->bottom_start, s->bottom_end - s->bottom_start + 1))

#define STRUT_LEFT_IGNORE(s, us, search) \
    (head == SCREEN_AREA_ALL_MONITORS && us && \
     RECT_LEFT(monitor_area[i]) + s->left > RECT_LEFT(*search))
#define STRUT_RIGHT_IGNORE(s, us, search) \
    (head == SCREEN_AREA_ALL_MONITORS && us && \
     RECT_RIGHT(monitor_area[i]) - s->right < RECT_RIGHT(*search))
#define STRUT_TOP_IGNORE(s, us, search) \
    (head == SCREEN_AREA_ALL_MONITORS && us && \
     RECT_TOP(monitor_area[i]) + s->top > RECT_TOP(*search))
#define STRUT_BOTTOM_IGNORE(s, us, search) \
    (head == SCREEN_AREA_ALL_MONITORS && us && \
     RECT_BOTTOM(monitor_area[i]) - s->bottom < RECT_BOTTOM(*search))

Rect* screen_area(guint desktop, guint head, Rect *search)
{
    Rect *a;
    GSList *it;
    gint l, r, t, b, al, ar, at, ab;
    guint i, d;
    gboolean us = search != NULL; /* user provided search */

    g_assert(desktop < screen_num_desktops || desktop == DESKTOP_ALL);
    g_assert(head < screen_num_monitors || head == SCREEN_AREA_ONE_MONITOR ||
             head == SCREEN_AREA_ALL_MONITORS);
    g_assert(!(head == SCREEN_AREA_ONE_MONITOR && search == NULL));

    /* find any struts for this monitor
       which will be affecting the search area.
    */

    /* search everything if search is null */
    if (!search) {
        if (head < screen_num_monitors) search = &monitor_area[head];
        else search = &monitor_area[screen_num_monitors];
    }
    if (head == SCREEN_AREA_ONE_MONITOR) head = screen_find_monitor(search);

    /* al is "all left" meaning the furthest left you can get, l is our
       "working left" meaning our current strut edge which we're calculating
    */

    /* only include monitors which the search area lines up with */
    if (RECT_INTERSECTS_RECT(monitor_area[screen_num_monitors], *search)) {
        al = l = RECT_RIGHT(monitor_area[screen_num_monitors]);
        at = t = RECT_BOTTOM(monitor_area[screen_num_monitors]);
        ar = r = RECT_LEFT(monitor_area[screen_num_monitors]);
        ab = b = RECT_TOP(monitor_area[screen_num_monitors]);
        for (i = 0; i < screen_num_monitors; ++i) {
            /* add the monitor if applicable */
            if (RANGES_INTERSECT(search->x, search->width,
                                 monitor_area[i].x, monitor_area[i].width))
            {
                at = t = MIN(t, RECT_TOP(monitor_area[i]));
                ab = b = MAX(b, RECT_BOTTOM(monitor_area[i]));
            }
            if (RANGES_INTERSECT(search->y, search->height,
                                 monitor_area[i].y, monitor_area[i].height))
            {
                al = l = MIN(l, RECT_LEFT(monitor_area[i]));
                ar = r = MAX(r, RECT_RIGHT(monitor_area[i]));
            }
        }
    } else {
        al = l = RECT_LEFT(monitor_area[screen_num_monitors]);
        at = t = RECT_TOP(monitor_area[screen_num_monitors]);
        ar = r = RECT_RIGHT(monitor_area[screen_num_monitors]);
        ab = b = RECT_BOTTOM(monitor_area[screen_num_monitors]);
    }

    for (d = 0; d < screen_num_desktops; ++d) {
        if (d != desktop && desktop != DESKTOP_ALL) continue;

        for (i = 0; i < screen_num_monitors; ++i) {
            if (head != SCREEN_AREA_ALL_MONITORS && head != i) continue;

            for (it = struts_left; it; it = g_slist_next(it)) {
                ObScreenStrut *s = it->data;
                if ((s->desktop == d || s->desktop == DESKTOP_ALL) &&
                    STRUT_LEFT_IN_SEARCH(s->strut, search) &&
                    !STRUT_LEFT_IGNORE(s->strut, us, search))
                    l = MAX(l, al + s->strut->left);
            }
            for (it = struts_top; it; it = g_slist_next(it)) {
                ObScreenStrut *s = it->data;
                if ((s->desktop == d || s->desktop == DESKTOP_ALL) &&
                    STRUT_TOP_IN_SEARCH(s->strut, search) &&
                    !STRUT_TOP_IGNORE(s->strut, us, search))
                    t = MAX(t, at + s->strut->top);
            }
            for (it = struts_right; it; it = g_slist_next(it)) {
                ObScreenStrut *s = it->data;
                if ((s->desktop == d || s->desktop == DESKTOP_ALL) &&
                    STRUT_RIGHT_IN_SEARCH(s->strut, search) &&
                    !STRUT_RIGHT_IGNORE(s->strut, us, search))
                    r = MIN(r, ar - s->strut->right);
            }
            for (it = struts_bottom; it; it = g_slist_next(it)) {
                ObScreenStrut *s = it->data;
                if ((s->desktop == d || s->desktop == DESKTOP_ALL) &&
                    STRUT_BOTTOM_IN_SEARCH(s->strut, search) &&
                    !STRUT_BOTTOM_IGNORE(s->strut, us, search))
                    b = MIN(b, ab - s->strut->bottom);
            }

            /* limit to this monitor */
            if (head == i) {
                l = MAX(l, RECT_LEFT(monitor_area[i]));
                t = MAX(t, RECT_TOP(monitor_area[i]));
                r = MIN(r, RECT_RIGHT(monitor_area[i]));
                b = MIN(b, RECT_BOTTOM(monitor_area[i]));
            }
        }
    }

    a = g_new(Rect, 1);
    a->x = l;
    a->y = t;
    a->width = r - l + 1;
    a->height = b - t + 1;
    return a;
}

guint screen_find_monitor(Rect *search)
{
    guint i;
    guint most = screen_num_monitors;
    guint mostv = 0;

    for (i = 0; i < screen_num_monitors; ++i) {
        Rect *area = screen_physical_area_monitor(i);
        if (RECT_INTERSECTS_RECT(*area, *search)) {
            Rect r;
            guint v;

            RECT_SET_INTERSECTION(r, *area, *search);
            v = r.width * r.height;

            if (v > mostv) {
                mostv = v;
                most = i;
            }
        }
        g_free(area);
    }
    return most;
}

Rect* screen_physical_area_all_monitors(void)
{
    return screen_physical_area_monitor(screen_num_monitors);
}

Rect* screen_physical_area_monitor(guint head)
{
    Rect *a;
    g_assert(head <= screen_num_monitors);

    a = g_new(Rect, 1);
    *a = monitor_area[head];
    return a;
}

gboolean screen_physical_area_monitor_contains(guint head, Rect *search)
{
    g_assert(head <= screen_num_monitors);
    g_assert(search);
    return RECT_INTERSECTS_RECT(monitor_area[head], *search);
}

Rect* screen_physical_area_active(void)
{
    Rect *a;
    gint x, y;

    if (moveresize_client)
        a = screen_physical_area_monitor(client_monitor(focus_client));
    else if (focus_client)
        a = screen_physical_area_monitor(client_monitor(focus_client));
    else {
        Rect mon;
        if (screen_pointer_pos(&x, &y))
            RECT_SET(mon, x, y, 1, 1);
        else
            RECT_SET(mon, 0, 0, 1, 1);
        a = screen_physical_area_monitor(screen_find_monitor(&mon));
    }
    return a;
}

void screen_set_root_cursor(void)
{
    if (sn_app_starting())
        XDefineCursor(obt_display, obt_root(ob_screen),
                      ob_cursor(OB_CURSOR_BUSYPOINTER));
    else
        XDefineCursor(obt_display, obt_root(ob_screen),
                      ob_cursor(OB_CURSOR_POINTER));
}

gboolean screen_pointer_pos(gint *x, gint *y)
{
    Window w;
    gint i;
    guint u;
    gboolean ret;

    ret = !!XQueryPointer(obt_display, obt_root(ob_screen),
                          &w, &w, x, y, &i, &i, &u);
    if (!ret) {
        for (i = 0; i < ScreenCount(obt_display); ++i)
            if (i != ob_screen)
                if (XQueryPointer(obt_display, obt_root(i),
                                  &w, &w, x, y, &i, &i, &u))
                    break;
    }
    return ret;
}
