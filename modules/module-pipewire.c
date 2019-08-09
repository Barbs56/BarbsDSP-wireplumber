/* WirePlumber
 *
 * Copyright © 2019 Collabora Ltd.
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * module-pipewire provides basic integration between wireplumber and pipewire.
 * It provides the pipewire core and remote, connects to pipewire and provides
 * the most primitive implementations of WpEndpoint and WpEndpointLink
 */

#include <wp/wp.h>
#include <pipewire/pipewire.h>

void simple_endpoint_factory (WpFactory * factory, GType type,
    GVariant * properties, GAsyncReadyCallback ready, gpointer user_data);
void simple_endpoint_link_factory (WpFactory * factory, GType type,
    GVariant * properties, GAsyncReadyCallback ready, gpointer user_data);

void
wireplumber__module_init (WpModule * module, WpCore * core, GVariant * args)
{
  WpRemotePipewire *rp;

  /* Get the remote pipewire */
  rp = wp_core_get_global (core, WP_GLOBAL_REMOTE_PIPEWIRE);
  if (!rp) {
    g_critical ("module-pipewire cannot be loaded without a registered "
        "WpRemotePipewire object");
    return;
  }

  /* Load the client-device and adapter modules */
  wp_remote_pipewire_module_load(rp, "libpipewire-module-client-device", NULL,
      NULL, NULL, NULL);
  wp_remote_pipewire_module_load(rp, "libpipewire-module-adapter", NULL, NULL,
      NULL, NULL);

  /* Register simple-endpoint and simple-endpoint-link */
  wp_factory_new (core, "pipewire-simple-endpoint",
      simple_endpoint_factory);
  wp_factory_new (core, "pipewire-simple-endpoint-link",
      simple_endpoint_link_factory);
}
