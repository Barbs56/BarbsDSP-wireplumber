/* WirePlumber
 *
 * Copyright © 2019 Collabora Ltd.
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __WIREPLUMBER_REMOTE_PIPEWIRE_H__
#define __WIREPLUMBER_REMOTE_PIPEWIRE_H__

#include "remote.h"

G_BEGIN_DECLS

#define WP_TYPE_REMOTE_PIPEWIRE (wp_remote_pipewire_get_type ())
G_DECLARE_FINAL_TYPE (WpRemotePipewire, wp_remote_pipewire,
    WP, REMOTE_PIPEWIRE, WpRemote)

WpRemote *wp_remote_pipewire_new (WpCore *core, GMainContext *context);

gpointer wp_remote_pipewire_proxy_bind (WpRemotePipewire *self, guint global_id,
    guint global_type);
gpointer wp_remote_pipewire_find_factory (WpRemotePipewire *self,
    const char *factory_name);
gpointer wp_remote_pipewire_create_object (WpRemotePipewire *self,
    const char *factory_name, guint global_type, gconstpointer props);
void wp_remote_pipewire_add_spa_lib (WpRemotePipewire *self,
    const char *factory_regexp, const char *lib);
gpointer wp_remote_pipewire_load_spa_handle(WpRemotePipewire *self,
    const char *factory_name, gconstpointer info);
gpointer wp_remote_pipewire_export (WpRemotePipewire *self, guint type,
    gpointer props, gpointer object, size_t user_data_size);
gpointer wp_remote_pipewire_module_load (WpRemotePipewire *self,
    const char *name, const char *args, gpointer properties);

G_END_DECLS

#endif
