/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   obt/parse.c for the Openbox window manager
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

#include "obt/parse.h"
#include "obt/paths.h"

#include <glib.h>

#ifdef HAVE_STDLIB_H
#  include <stdlib.h>
#endif
#ifdef HAVE_SYS_STAT_H
#  include <sys/stat.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif

struct Callback {
    gchar *tag;
    ObtParseCallback func;
    gpointer data;
};

struct _ObtParseInst {
    gint ref;
    ObtPaths *xdg_paths;
    GHashTable *callbacks;
    xmlDocPtr doc;
    xmlNodePtr root;
    gchar *path;
};

static void destfunc(struct Callback *c)
{
    g_free(c->tag);
    g_free(c);
}

ObtParseInst* obt_parse_instance_new(void)
{
    ObtParseInst *i = g_new(ObtParseInst, 1);
    i->ref = 1;
    i->xdg_paths = obt_paths_new();
    i->callbacks = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                         (GDestroyNotify)destfunc);
    i->doc = NULL;
    i->root = NULL;
    i->path = NULL;
    return i;
}

void obt_parse_instance_ref(ObtParseInst *i)
{
    ++i->ref;
}

void obt_parse_instance_unref(ObtParseInst *i)
{
    if (i && --i->ref == 0) {
        obt_paths_unref(i->xdg_paths);
        g_hash_table_destroy(i->callbacks);
        g_free(i);
    }
}

xmlDocPtr obt_parse_doc(ObtParseInst *i)
{
    g_assert(i->doc); /* a doc is open? */
    return i->doc;
}

xmlNodePtr obt_parse_root(ObtParseInst *i)
{
    g_assert(i->doc); /* a doc is open? */
    return i->root;
}

void obt_parse_register(ObtParseInst *i, const gchar *tag,
                        ObtParseCallback func, gpointer data)
{
    struct Callback *c;

    if (g_hash_table_lookup(i->callbacks, tag)) {
        g_error("Tag '%s' already registered", tag);
        return;
    }

    c = g_new(struct Callback, 1);
    c->tag = g_strdup(tag);
    c->func = func;
    c->data = data;
    g_hash_table_insert(i->callbacks, c->tag, c);
}

static gboolean load_file(ObtParseInst *i,
                          const gchar *domain,
                          const gchar *filename,
                          const gchar *root_node,
                          GSList *paths)
{
    GSList *it;
    gboolean r = FALSE;

    g_assert(i->doc == NULL); /* another doc isn't open already? */

    for (it = paths; !r && it; it = g_slist_next(it)) {
        gchar *path;
        struct stat s;

        if (!domain && !filename) /* given a full path to the file */
            path = g_strdup(it->data);
        else
            path = g_build_filename(it->data, domain, filename, NULL);

        if (stat(path, &s) >= 0) {
            /* XML_PARSE_BLANKS is needed apparently, or the tree can end up
               with extra nodes in it. */
            i->doc = xmlReadFile(path, NULL, (XML_PARSE_NOBLANKS |
                                              XML_PARSE_RECOVER));
            if (i->doc) {
                i->root = xmlDocGetRootElement(i->doc);
                if (!i->root) {
                    xmlFreeDoc(i->doc);
                    i->doc = NULL;
                    g_message("%s is an empty XML document", path);
                }
                else if (xmlStrcmp(i->root->name,
                                   (const xmlChar*)root_node)) {
                    xmlFreeDoc(i->doc);
                    i->doc = NULL;
                    i->root = NULL;
                    g_message("XML document %s is of wrong type. Root "
                              "node is not '%s'", path, root_node);
                }
                else {
                    i->path = g_strdup(path);
                    r = TRUE; /* ok! */
                }
            }
        }

        g_free(path);
    }

    return r;
}

gboolean obt_parse_load_file(ObtParseInst *i,
                             const gchar *path,
                             const gchar *root_node)
{
    GSList *paths;
    gboolean r;

    paths = g_slist_append(NULL, g_strdup(path));

    r = load_file(i, NULL, NULL, root_node, paths);

    while (paths) {
        g_free(paths->data);
        paths = g_slist_delete_link(paths, paths);
    }
    return r;
}

gboolean obt_parse_load_config_file(ObtParseInst *i,
                                    const gchar *domain,
                                    const gchar *filename,
                                    const gchar *root_node)
{
    GSList *it, *paths = NULL;
    gboolean r;

    for (it = obt_paths_config_dirs(i->xdg_paths); it; it = g_slist_next(it))
        paths = g_slist_append(paths, g_strdup(it->data));

    r = load_file(i, domain, filename, root_node, paths);

    while (paths) {
        g_free(paths->data);
        paths = g_slist_delete_link(paths, paths);
    }
    return r;
}

gboolean obt_parse_load_data_file(ObtParseInst *i,
                                  const gchar *domain,
                                  const gchar *filename,
                                  const gchar *root_node)
{
    GSList *it, *paths = NULL;
    gboolean r;

    for (it = obt_paths_data_dirs(i->xdg_paths); it; it = g_slist_next(it))
        paths = g_slist_append(paths, g_strdup(it->data));

    r = load_file(i, domain, filename, root_node, paths);

    while (paths) {
        g_free(paths->data);
        paths = g_slist_delete_link(paths, paths);
    }
    return r;
}

gboolean obt_parse_load_theme_file(ObtParseInst *i,
                                   const gchar *theme,
                                   const gchar *domain,
                                   const gchar *filename,
                                   const gchar *root_node)
{
    GSList *it, *paths = NULL;
    gboolean r;

    /* use ~/.themes for backwards compatibility */
    paths = g_slist_append
        (paths, g_build_filename(g_get_home_dir(), ".themes", theme, NULL));

    for (it = obt_paths_data_dirs(i->xdg_paths); it; it = g_slist_next(it))
        paths = g_slist_append
            (paths, g_build_filename(it->data, "themes", theme, NULL));

    r = load_file(i, domain, filename, root_node, paths);

    while (paths) {
        g_free(paths->data);
        paths = g_slist_delete_link(paths, paths);
    }
    return r;
}


gboolean obt_parse_load_mem(ObtParseInst *i,
                            gpointer data, guint len, const gchar *root_node)
{
    gboolean r = FALSE;

    g_assert(i->doc == NULL); /* another doc isn't open already? */

    i->doc = xmlParseMemory(data, len);
    if (i) {
        i->root = xmlDocGetRootElement(i->doc);
        if (!i->root) {
            xmlFreeDoc(i->doc);
            i->doc = NULL;
            g_message("Given memory is an empty document");
        }
        else if (xmlStrcmp(i->root->name, (const xmlChar*)root_node)) {
            xmlFreeDoc(i->doc);
            i->doc = NULL;
            i->root = NULL;
            g_message("XML Document in given memory is of wrong "
                      "type. Root node is not '%s'\n", root_node);
        }
        else
            r = TRUE; /* ok ! */
    }
    return r;
}

void obt_parse_close(ObtParseInst *i)
{
    if (i && i->doc) {
        xmlFreeDoc(i->doc);
        g_free(i->path);
        i->doc = NULL;
        i->root = NULL;
        i->path = NULL;
    }
}

void obt_parse_tree(ObtParseInst *i, xmlNodePtr node)
{
    g_assert(i->doc); /* a doc is open? */

    while (node) {
        struct Callback *c = g_hash_table_lookup(i->callbacks, node->name);
        if (c) c->func(node, c->data);
        node = node->next;
    }
}

void obt_parse_tree_from_root(ObtParseInst *i)
{
    obt_parse_tree(i, i->root->children);
}

gchar *obt_parse_node_string(xmlNodePtr node)
{
    xmlChar *c = xmlNodeGetContent(node);
    gchar *s = g_strdup(c ? (gchar*)c : "");
    xmlFree(c);
    return s;
}

gint obt_parse_node_int(xmlNodePtr node)
{
    xmlChar *c = xmlNodeGetContent(node);
    gint i = c ? atoi((gchar*)c) : 0;
    xmlFree(c);
    return i;
}

gboolean obt_parse_node_bool(xmlNodePtr node)
{
    xmlChar *c = xmlNodeGetContent(node);
    gboolean b = FALSE;
    if (c && !xmlStrcasecmp(c, (const xmlChar*) "true"))
        b = TRUE;
    else if (c && !xmlStrcasecmp(c, (const xmlChar*) "yes"))
        b = TRUE;
    else if (c && !xmlStrcasecmp(c, (const xmlChar*) "on"))
        b = TRUE;
    xmlFree(c);
    return b;
}

gboolean obt_parse_node_contains(xmlNodePtr node, const gchar *val)
{
    xmlChar *c = xmlNodeGetContent(node);
    gboolean r;
    r = !xmlStrcasecmp(c, (const xmlChar*) val);
    xmlFree(c);
    return r;
}

xmlNodePtr obt_parse_find_node(xmlNodePtr node, const gchar *tag)
{
    while (node) {
        if (!xmlStrcmp(node->name, (const xmlChar*) tag))
            return node;
        node = node->next;
    }
    return NULL;
}

gboolean obt_parse_attr_bool(xmlNodePtr node, const gchar *name,
                             gboolean *value)
{
    xmlChar *c = xmlGetProp(node, (const xmlChar*) name);
    gboolean r = FALSE;
    if (c) {
        if (!xmlStrcasecmp(c, (const xmlChar*) "true"))
            *value = TRUE, r = TRUE;
        else if (!xmlStrcasecmp(c, (const xmlChar*) "yes"))
            *value = TRUE, r = TRUE;
        else if (!xmlStrcasecmp(c, (const xmlChar*) "on"))
            *value = TRUE, r = TRUE;
        else if (!xmlStrcasecmp(c, (const xmlChar*) "false"))
            *value = FALSE, r = TRUE;
        else if (!xmlStrcasecmp(c, (const xmlChar*) "no"))
            *value = FALSE, r = TRUE;
        else if (!xmlStrcasecmp(c, (const xmlChar*) "off"))
            *value = FALSE, r = TRUE;
    }
    xmlFree(c);
    return r;
}

gboolean obt_parse_attr_int(xmlNodePtr node, const gchar *name, gint *value)
{
    xmlChar *c = xmlGetProp(node, (const xmlChar*) name);
    gboolean r = FALSE;
    if (c) {
        *value = atoi((gchar*)c);
        r = TRUE;
    }
    xmlFree(c);
    return r;
}

gboolean obt_parse_attr_string(xmlNodePtr node, const gchar *name,
                               gchar **value)
{
    xmlChar *c = xmlGetProp(node, (const xmlChar*) name);
    gboolean r = FALSE;
    if (c) {
        *value = g_strdup((gchar*)c);
        r = TRUE;
    }
    xmlFree(c);
    return r;
}

gboolean obt_parse_attr_contains(xmlNodePtr node, const gchar *name,
                                 const gchar *val)
{
    xmlChar *c = xmlGetProp(node, (const xmlChar*) name);
    gboolean r = FALSE;
    if (c)
        r = !xmlStrcasecmp(c, (const xmlChar*) val);
    xmlFree(c);
    return r;
}
