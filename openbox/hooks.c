#include "hooks.h"
#include "actions.h"
#include "client.h"
#include "focus.h"
#include "debug.h"
#include "obt/display.h"

#include <glib.h>

static GSList *hooks[OB_NUM_HOOKS];
static const gchar *names[OB_NUM_HOOKS];

typedef struct {
    ObHook hook;
    struct _ObClient *client;
} ObHookQueue;

#define QUEUE_SIZE 20
ObHookQueue run_queue[QUEUE_SIZE];
gint        queue_size;

void hooks_startup(gboolean reconfig)
{
    gint i;

    for (i = 0; i < OB_NUM_HOOKS; ++i)
        hooks[i] = NULL;

    queue_size = 0;

    names[OB_HOOK_WIN_NEW] = "WindowNew";
    names[OB_HOOK_WIN_CLOSE] = "WindowClosed";
    names[OB_HOOK_WIN_VISIBLE] = "WindowVisible";
    names[OB_HOOK_WIN_INVISIBLE] = "WindowInvisible";
    names[OB_HOOK_WIN_ICONIC] = "WindowIconified";
    names[OB_HOOK_WIN_UNICONIC] = "WindowUniconified";
    names[OB_HOOK_WIN_MAX] = "WindowMaximized";
    names[OB_HOOK_WIN_UNMAX] = "WindowUnmaximized";
    names[OB_HOOK_WIN_SHADE] = "WindowShaded";
    names[OB_HOOK_WIN_UNSHADE] = "WindowUnshaded";
    names[OB_HOOK_WIN_FOCUS] = "WindowFocused";
    names[OB_HOOK_WIN_UNFOCUS] = "WindowUnfocused";
    names[OB_HOOK_WIN_DESK_CHANGE] = "WindowOnNewDesktop";
    names[OB_HOOK_WIN_DECORATED] = "WindowDecorated";
    names[OB_HOOK_WIN_UNDECORATED] = "WindowUndecorated";
    names[OB_HOOK_SCREEN_DESK_CHANGE] = "DesktopChanged";
}

void hooks_shutdown(gboolean reconfig)
{
    gint i;

    for (i = 0; i < OB_NUM_HOOKS; ++i)
        while (hooks[i]) {
            actions_act_unref(hooks[i]->data);
            hooks[i] = g_slist_delete_link(hooks[i], hooks[i]);
        }
}

ObHook hooks_hook_from_name(const gchar *n)
{
    gint i;

    for (i = 1; i < OB_NUM_HOOKS; ++i)
        if (!g_ascii_strcasecmp(n, names[i]))
            return (ObHook)i;
    return OB_HOOK_INVALID;
}

void hooks_queue(ObHook hook, struct _ObClient *client)
{
    ObHookQueue *q;

    g_assert(hook < OB_NUM_HOOKS && hook > OB_HOOK_INVALID);

    //ob_debug("Queing hook %s for client 0x%x", names[hook],
    //         (client ? client->window : 0));
    q = &run_queue[queue_size++];
    q->hook = hook;
    q->client = client;

    if (queue_size == QUEUE_SIZE)
        /* queue is full */
        hooks_run_queue();
}

void hooks_run(ObHook hook, struct _ObClient *c)
{
    hooks_queue(hook, c);
    hooks_run_queue();
}

void hooks_add(ObHook hook, struct _ObActionsAct *act)
{
    g_assert(hook < OB_NUM_HOOKS && hook > OB_HOOK_INVALID);

    /* append so they are executed in the same order as they appear in the
       config file */
    hooks[hook] = g_slist_append(hooks[hook], act);
}

void hooks_run_queue(void)
{
    gint i;

    for (i = 0; i < queue_size; ++i) {
        const ObHookQueue *q = &run_queue[i];

        //ob_debug("Running hook %s for client 0x%x", names[q->hook],
        //         (q->client ? q->client->window : 0));
        actions_run_acts(hooks[q->hook],
                         OB_USER_ACTION_HOOK,
                         0, -1, -1, 0,
                         OB_FRAME_CONTEXT_NONE,
                         q->client);
    }
    queue_size = 0;
}
