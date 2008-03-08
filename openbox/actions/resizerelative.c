#include "openbox/actions.h"
#include "openbox/client.h"
#include "openbox/screen.h"
#include "openbox/frame.h"
#include <stdlib.h> /* for atoi */

typedef struct {
    gint left;
    gint right;
    gint top;
    gint bottom;
} Options;

static gpointer setup_func(xmlNodePtr node);
static gboolean run_func(ObActionsData *data, gpointer options);

void action_resizerelative_startup(void)
{
    actions_register("ResizeRelative", setup_func, g_free, run_func,
                     NULL, NULL);
}

static gpointer setup_func(xmlNodePtr node)
{
    xmlNodePtr n;
    Options *o;

    o = g_new0(Options, 1);

    if ((n = obt_parse_find_node(node, "left")))
        o->left = obt_parse_node_int(n);
    if ((n = obt_parse_find_node(node, "right")))
        o->right = obt_parse_node_int(n);
    if ((n = obt_parse_find_node(node, "top")) ||
        (n = obt_parse_find_node(node, "up")))
        o->top = obt_parse_node_int(n);
    if ((n = obt_parse_find_node(node, "bottom")) ||
        (n = obt_parse_find_node(node, "down")))
        o->bottom = obt_parse_node_int(n);

    return o;
}

/* Always return FALSE because its not interactive */
static gboolean run_func(ObActionsData *data, gpointer options)
{
    Options *o = options;

    if (!actions_client_locked(data)) {
        ObClient *c = data->client;
        gint x, y, ow, xoff, nw, oh, yoff, nh, lw, lh;

        x = c->area.x;
        y = c->area.y;
        ow = c->area.width;
        xoff = -o->left * c->size_inc.width;
        nw = ow + o->right * c->size_inc.width
            + o->left * c->size_inc.width;
        oh = c->area.height;
        yoff = -o->top * c->size_inc.height;
        nh = oh + o->bottom * c->size_inc.height
            + o->top * c->size_inc.height;

        client_try_configure(c, &x, &y, &nw, &nh, &lw, &lh, TRUE);
        xoff = xoff == 0 ? 0 :
            (xoff < 0 ? MAX(xoff, ow-nw) : MIN(xoff, ow-nw));
        yoff = yoff == 0 ? 0 :
            (yoff < 0 ? MAX(yoff, oh-nh) : MIN(yoff, oh-nh));

        actions_client_move(data, TRUE);
        client_move_resize(c, x + xoff, y + yoff, nw, nh);
        actions_client_move(data, FALSE);
    }

    return FALSE;
}
