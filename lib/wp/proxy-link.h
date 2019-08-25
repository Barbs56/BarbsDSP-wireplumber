/* WirePlumber
 *
 * Copyright © 2019 Collabora Ltd.
 *    @author Julian Bouzas <julian.bouzas@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __WIREPLUMBER_PROXY_LINK_H__
#define __WIREPLUMBER_PROXY_LINK_H__

#include "proxy.h"

G_BEGIN_DECLS

#define WP_TYPE_PROXY_LINK (wp_proxy_link_get_type ())
G_DECLARE_FINAL_TYPE (WpProxyLink, wp_proxy_link, WP, PROXY_LINK, WpProxy)

static inline const struct pw_link_info *
wp_proxy_link_get_info (WpProxyLink * self)
{
  return (const struct pw_link_info *)
      wp_proxy_get_native_info (WP_PROXY (self));
}

G_END_DECLS

#endif
