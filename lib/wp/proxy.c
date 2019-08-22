/* WirePlumber
 *
 * Copyright © 2019 Collabora Ltd.
 *    @author Julian Bouzas <julian.bouzas@collabora.com>
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "proxy.h"
#include "error.h"
#include "remote-pipewire.h"
#include "wpenums.h"

#include "proxy-link.h"
#include "proxy-node.h"
#include "proxy-port.h"

#include <pipewire/pipewire.h>
#include <spa/debug/types.h>

typedef struct _WpProxyPrivate WpProxyPrivate;
struct _WpProxyPrivate
{
  /* properties */
  GWeakRef remote;

  guint32 global_id;
  guint32 global_perm;
  WpProperties *global_props;

  guint32 iface_type;
  guint32 iface_version;

  struct pw_proxy *pw_proxy;
  gpointer native_info;
  GDestroyNotify native_info_destroy;

  /* The proxy listener */
  struct spa_hook listener;

  /* augment state */
  WpProxyFeatures ft_ready;
  WpProxyFeatures ft_wanted;
  GTask *task;
};

enum {
  PROP_0,
  PROP_REMOTE,
  PROP_GLOBAL_ID,
  PROP_GLOBAL_PERMISSIONS,
  PROP_GLOBAL_PROPERTIES,
  PROP_INTERFACE_TYPE,
  PROP_INTERFACE_NAME,
  PROP_INTERFACE_QUARK,
  PROP_INTERFACE_VERSION,
  PROP_PW_PROXY,
  PROP_NATIVE_INFO,
  PROP_FEATURES,
};

enum
{
  SIGNAL_PW_PROXY_CREATED,
  SIGNAL_PW_PROXY_DESTROYED,
  LAST_SIGNAL,
};

static guint wp_proxy_signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE_WITH_PRIVATE (WpProxy, wp_proxy, G_TYPE_OBJECT)

G_DEFINE_QUARK (core, wp_proxy_core)
G_DEFINE_QUARK (registry, wp_proxy_registry)
G_DEFINE_QUARK (node, wp_proxy_node)
G_DEFINE_QUARK (port, wp_proxy_port)
G_DEFINE_QUARK (factory, wp_proxy_factory)
G_DEFINE_QUARK (link, wp_proxy_link)
G_DEFINE_QUARK (client, wp_proxy_client)
G_DEFINE_QUARK (module, wp_proxy_module)
G_DEFINE_QUARK (device, wp_proxy_device)
G_DEFINE_QUARK (client-node, wp_proxy_client_node)

static struct {
  /* the pipewire interface type */
  guint32 pw_type;
  /* the minimum interface version that the remote object must support */
  guint32 req_version;
  /* the _get_type() function of the subclass */
  GType (*get_type) (void);
  /* a function returning a quark that identifies the interface */
  GQuark (*get_quark) (void);
} types_assoc[] = {
  { PW_TYPE_INTERFACE_Core, 0, wp_proxy_get_type, wp_proxy_core_quark },
  { PW_TYPE_INTERFACE_Registry, 0, wp_proxy_get_type, wp_proxy_registry_quark },
  { PW_TYPE_INTERFACE_Node, 0, wp_proxy_node_get_type, wp_proxy_node_quark },
  { PW_TYPE_INTERFACE_Port, 0, wp_proxy_port_get_type, wp_proxy_port_quark },
  { PW_TYPE_INTERFACE_Factory, 0, wp_proxy_get_type, wp_proxy_factory_quark },
  { PW_TYPE_INTERFACE_Link, 0, wp_proxy_link_get_type, wp_proxy_link_quark },
  { PW_TYPE_INTERFACE_Client, 0, wp_proxy_get_type, wp_proxy_client_quark },
  { PW_TYPE_INTERFACE_Module, 0, wp_proxy_get_type, wp_proxy_module_quark },
  { PW_TYPE_INTERFACE_Device, 0, wp_proxy_get_type, wp_proxy_device_quark },
  { PW_TYPE_INTERFACE_ClientNode, 0, wp_proxy_get_type, wp_proxy_client_node_quark },
};

static inline GType
wp_proxy_find_instance_type (guint32 type, guint32 version)
{
  for (gint i = 0; i < SPA_N_ELEMENTS (types_assoc); i++) {
    if (types_assoc[i].pw_type == type &&
        types_assoc[i].req_version <= version)
      return types_assoc[i].get_type ();
  }

  return WP_TYPE_PROXY;
}

static inline GQuark
wp_proxy_find_quark_for_type (guint32 type)
{
  for (gint i = 0; i < SPA_N_ELEMENTS (types_assoc); i++) {
    if (types_assoc[i].pw_type == type)
      return types_assoc[i].get_quark ();
  }

  return 0;
}

static void
proxy_event_destroy (void *data)
{
  WpProxy *self = WP_PROXY (data);
  WpProxyPrivate *priv = wp_proxy_get_instance_private (self);

  priv->pw_proxy = NULL;

  g_signal_emit (self, wp_proxy_signals[SIGNAL_PW_PROXY_DESTROYED], 0);

  /* Return error if the pw_proxy destruction happened while the async
   * init or augment of this proxy object was in progress */
  if (priv->task) {
    g_task_return_new_error (priv->task, WP_DOMAIN_LIBRARY,
        WP_LIBRARY_ERROR_OPERATION_FAILED,
        "pipewire node proxy destroyed before finishing");
    g_clear_object (&priv->task);
  }
}

static const struct pw_proxy_events proxy_events = {
  PW_VERSION_PROXY_EVENTS,
  .destroy = proxy_event_destroy,
};

static void
wp_proxy_init (WpProxy * self)
{
  WpProxyPrivate *priv = wp_proxy_get_instance_private (self);
  g_weak_ref_init (&priv->remote, NULL);
}

static void
wp_proxy_constructed (GObject * object)
{
  WpProxy *self = WP_PROXY (object);
  WpProxyPrivate *priv = wp_proxy_get_instance_private (self);

  /* native proxy was passed in the constructor, declare it as ready */
  if (priv->pw_proxy) {
    priv->ft_ready |= WP_PROXY_FEATURE_PW_PROXY;

    /* and inform the subclasses */
    g_signal_emit (self, wp_proxy_signals[SIGNAL_PW_PROXY_CREATED], 0,
        priv->pw_proxy);
  }
}

static void
wp_proxy_finalize (GObject * object)
{
  WpProxyPrivate *priv = wp_proxy_get_instance_private (WP_PROXY(object));

  g_debug ("%s:%p destroyed (global %u; pw_proxy %p)", G_OBJECT_TYPE_NAME (object),
      object, priv->global_id, priv->pw_proxy);

  g_clear_object (&priv->task);
  g_clear_pointer (&priv->global_props, wp_properties_unref);
  g_clear_pointer (&priv->native_info, priv->native_info_destroy);
  g_clear_pointer (&priv->pw_proxy, pw_proxy_destroy);
  g_weak_ref_clear (&priv->remote);

  G_OBJECT_CLASS (wp_proxy_parent_class)->finalize (object);
}

static void
wp_proxy_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  WpProxyPrivate *priv = wp_proxy_get_instance_private (WP_PROXY(object));

  switch (property_id) {
  case PROP_REMOTE:
    g_weak_ref_set (&priv->remote, g_value_get_object (value));
    break;
  case PROP_GLOBAL_ID:
    priv->global_id = g_value_get_uint (value);
    break;
  case PROP_GLOBAL_PERMISSIONS:
    priv->global_perm = g_value_get_uint (value);
    break;
  case PROP_GLOBAL_PROPERTIES:
    priv->global_props = g_value_dup_boxed (value);
    break;
  case PROP_INTERFACE_TYPE:
    priv->iface_type = g_value_get_uint (value);
    break;
  case PROP_INTERFACE_VERSION:
    priv->iface_version = g_value_get_uint (value);
    break;
  case PROP_PW_PROXY:
    priv->pw_proxy = g_value_get_pointer (value);
    break;
  case PROP_NATIVE_INFO:
    priv->native_info = g_value_get_pointer (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
wp_proxy_get_property (GObject * object, guint property_id, GValue * value,
    GParamSpec * pspec)
{
  WpProxyPrivate *priv = wp_proxy_get_instance_private (WP_PROXY(object));

  switch (property_id) {
  case PROP_REMOTE:
    g_value_take_object (value, g_weak_ref_get (&priv->remote));
    break;
  case PROP_GLOBAL_ID:
    g_value_set_uint (value, priv->global_id);
    break;
  case PROP_GLOBAL_PERMISSIONS:
    g_value_set_uint (value, priv->global_perm);
    break;
  case PROP_GLOBAL_PROPERTIES:
    g_value_set_boxed (value, priv->global_props);
    break;
  case PROP_INTERFACE_TYPE:
    g_value_set_uint (value, priv->iface_type);
    break;
  case PROP_INTERFACE_NAME:
    g_value_set_static_string (value,
        spa_debug_type_find_name (pw_type_info(), priv->iface_type));
    break;
  case PROP_INTERFACE_QUARK:
    g_value_set_uint (value, wp_proxy_find_quark_for_type (priv->iface_type));
    break;
  case PROP_INTERFACE_VERSION:
    g_value_set_uint (value, priv->iface_version);
    break;
  case PROP_PW_PROXY:
    g_value_set_pointer (value, priv->pw_proxy);
    break;
  case PROP_NATIVE_INFO:
    g_value_set_pointer (value, priv->native_info);
    break;
  case PROP_FEATURES:
    g_value_set_flags (value, priv->ft_ready);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static void
wp_proxy_default_augment (WpProxy * self, WpProxyFeatures features)
{
  WpProxyPrivate *priv = wp_proxy_get_instance_private (self);

  /* ensure we have a pw_proxy, as we can't have
   * any other feature without first having that */
  if (!priv->pw_proxy && features != 0)
    features |= WP_PROXY_FEATURE_PW_PROXY;

  /* if we don't have a pw_proxy, we have to assume that this WpProxy
   * represents a global object from the registry; we have no other way
   * to get a pw_proxy */
  if (features & WP_PROXY_FEATURE_PW_PROXY) {
    if (!wp_proxy_is_global (self)) {
      wp_proxy_augment_error (self, g_error_new (WP_DOMAIN_LIBRARY,
            WP_LIBRARY_ERROR_INVALID_ARGUMENT,
            "No global id specified; cannot bind pw_proxy"));
      return;
    }

    wp_proxy_bind_global (self);
  }
}

static void
wp_proxy_class_init (WpProxyClass * klass)
{
  GObjectClass *object_class = (GObjectClass *) klass;

  object_class->constructed = wp_proxy_constructed;
  object_class->finalize = wp_proxy_finalize;
  object_class->get_property = wp_proxy_get_property;
  object_class->set_property = wp_proxy_set_property;

  klass->augment = wp_proxy_default_augment;

  /* Install the properties */

  g_object_class_install_property (object_class, PROP_REMOTE,
      g_param_spec_object ("remote", "remote",
          "The pipewire connection WpRemote", WP_TYPE_REMOTE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_GLOBAL_ID,
      g_param_spec_uint ("global-id", "global-id",
          "The pipewire global id", 0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_GLOBAL_PERMISSIONS,
      g_param_spec_uint ("global-permissions", "global-permissions",
          "The pipewire global permissions", 0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_GLOBAL_PROPERTIES,
      g_param_spec_boxed ("global-properties", "global-properties",
          "The pipewire global properties", WP_TYPE_PROPERTIES,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_INTERFACE_TYPE,
      g_param_spec_uint ("interface-type", "interface-type",
          "The pipewire interface type", 0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_INTERFACE_NAME,
      g_param_spec_string ("interface-name", "interface-name",
          "The name of the pipewire interface", NULL,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_INTERFACE_QUARK,
      g_param_spec_uint ("interface-quark", "interface-quark",
          "A quark identifying the pipewire interface", 0, G_MAXUINT, 0,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_INTERFACE_VERSION,
      g_param_spec_uint ("interface-version", "interface-version",
          "The pipewire interface version", 0, G_MAXUINT, 0,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_PW_PROXY,
      g_param_spec_pointer ("pw-proxy", "pw-proxy", "The struct pw_proxy *",
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_NATIVE_INFO,
      g_param_spec_pointer ("native-info", "native-info",
          "The struct pw_XXXX_info *",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_FEATURES,
      g_param_spec_flags ("features", "features",
          "The ready WpProxyFeatures on this proxy", WP_TYPE_PROXY_FEATURES, 0,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /* Signals */
  wp_proxy_signals[SIGNAL_PW_PROXY_CREATED] = g_signal_new (
      "pw-proxy-created", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_FIRST,
      G_STRUCT_OFFSET (WpProxyClass, pw_proxy_created), NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_POINTER);

  wp_proxy_signals[SIGNAL_PW_PROXY_DESTROYED] = g_signal_new (
      "pw-proxy-destroyed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_FIRST,
      G_STRUCT_OFFSET (WpProxyClass, pw_proxy_destroyed), NULL, NULL, NULL,
      G_TYPE_NONE, 0);
}

WpProxy *
wp_proxy_new_global (WpRemote * remote,
    guint32 id, guint32 permissions, WpProperties * properties,
    guint32 type, guint32 version)
{
  GType gtype = wp_proxy_find_instance_type (type, version);
  return g_object_new (gtype,
      "remote", remote,
      "global-id", id,
      "global-permissions", permissions,
      "global-properties", properties,
      "interface-type", type,
      "interface-version", version,
      NULL);
}

WpProxy *
wp_proxy_new_wrap (WpRemote * remote,
    struct pw_proxy * proxy, guint32 type, guint32 version)
{
  GType gtype = wp_proxy_find_instance_type (type, version);
  return g_object_new (gtype,
      "remote", remote,
      "pw-proxy", proxy,
      "interface-type", type,
      "interface-version", version,
      NULL);
}

void
wp_proxy_augment (WpProxy * self,
    WpProxyFeatures ft_wanted, GCancellable * cancellable,
    GAsyncReadyCallback callback, gpointer user_data)
{
  WpProxyPrivate *priv;
  WpProxyFeatures missing = 0;

  g_return_if_fail (WP_IS_PROXY (self));
  g_return_if_fail (WP_PROXY_GET_CLASS (self)->augment);

  priv = wp_proxy_get_instance_private (self);
  priv->task = g_task_new (self, cancellable, callback, user_data);

  /* we don't simply assign here, we keep all the previous wanted features;
   * it is not allowed to remove features */
  priv->ft_wanted |= ft_wanted;

  /* find which features are wanted but missing from the "ready" set */
  missing = (priv->ft_ready ^ priv->ft_wanted) & priv->ft_wanted;

  /* if the features are not ready, call augment(),
   * otherwise signal the callback directly */
  if (missing != 0) {
    WP_PROXY_GET_CLASS (self)->augment (self, missing);
  } else {
    g_task_return_boolean (priv->task, TRUE);
    g_clear_object (&priv->task);
  }
}

gboolean
wp_proxy_augment_finish (WpProxy * self, GAsyncResult * res,
    GError ** error)
{
  g_return_val_if_fail (WP_IS_PROXY (self), FALSE);
  g_return_val_if_fail (g_task_is_valid (res, self), FALSE);

  return g_task_propagate_boolean (G_TASK (res), error);
}

void
wp_proxy_set_feature_ready (WpProxy * self, WpProxyFeatures feature)
{
  WpProxyPrivate *priv;

  g_return_if_fail (WP_IS_PROXY (self));

  priv = wp_proxy_get_instance_private (self);

  /* feature already marked as ready */
  if (priv->ft_ready & feature)
    return;

  priv->ft_ready |= feature;

  g_object_notify (G_OBJECT (self), "features");

  /* return from the task if all the wanted features are now ready */
  if (priv->task &&
      (priv->ft_ready & priv->ft_wanted) == priv->ft_wanted) {
    g_task_return_boolean (priv->task, TRUE);
    g_clear_object (&priv->task);
  }
}

/**
 * wp_proxy_augment_error:
 * @self: the proxy
 * @error: (transfer full): the error
 *
 * Reports an error that occured during the augment process
 */
void
wp_proxy_augment_error (WpProxy * self, GError * error)
{
  WpProxyPrivate *priv;

  g_return_if_fail (WP_IS_PROXY (self));

  priv = wp_proxy_get_instance_private (self);

  if (priv->task)
    g_task_return_error (priv->task, error);
  else
    g_error_free (error);

  g_clear_object (&priv->task);
}

gboolean
wp_proxy_bind_global (WpProxy * self)
{
  WpProxyPrivate *priv;
  g_autoptr (WpRemote) remote = NULL;

  g_return_val_if_fail (wp_proxy_is_global (self), FALSE);

  priv = wp_proxy_get_instance_private (self);

  if (priv->pw_proxy)
    return FALSE;

  remote = g_weak_ref_get (&priv->remote);
  g_return_val_if_fail (remote != NULL, FALSE);

  /* bind */
  priv->pw_proxy  = wp_remote_pipewire_proxy_bind (
      WP_REMOTE_PIPEWIRE (remote),
      priv->global_id, priv->iface_type);
  pw_proxy_add_listener (priv->pw_proxy, &priv->listener, &proxy_events,
      self);

  /* inform subclasses and listeners */
  g_signal_emit (self, wp_proxy_signals[SIGNAL_PW_PROXY_CREATED], 0,
      priv->pw_proxy);

  /* declare the feature as ready */
  wp_proxy_set_feature_ready (self, WP_PROXY_FEATURE_PW_PROXY);

  return TRUE;
}

WpProxyFeatures
wp_proxy_get_features (WpProxy * self)
{
  WpProxyPrivate *priv;

  g_return_val_if_fail (WP_IS_PROXY (self), 0);

  priv = wp_proxy_get_instance_private (self);
  return priv->ft_ready;
}

/**
 * wp_proxy_get_remote:
 * @self: the proxy
 *
 * Returns: (transfer full): the remote that created this proxy
 */
WpRemote *
wp_proxy_get_remote (WpProxy * self)
{
  WpProxyPrivate *priv;

  g_return_val_if_fail (WP_IS_PROXY (self), NULL);

  priv = wp_proxy_get_instance_private (self);
  return g_weak_ref_get (&priv->remote);
}

gboolean
wp_proxy_is_global (WpProxy * self)
{
  return wp_proxy_get_global_id (self) != 0;
}

guint32
wp_proxy_get_global_id (WpProxy * self)
{
  WpProxyPrivate *priv;

  g_return_val_if_fail (WP_IS_PROXY (self), 0);

  priv = wp_proxy_get_instance_private (self);
  return priv->global_id;
}

guint32
wp_proxy_get_global_permissions (WpProxy * self)
{
  WpProxyPrivate *priv;

  g_return_val_if_fail (WP_IS_PROXY (self), 0);

  priv = wp_proxy_get_instance_private (self);
  return priv->global_perm;
}

/**
 * wp_proxy_get_global_properties:
 *
 * Returns: (transfer full): the global properties of the proxy
 */
WpProperties *
wp_proxy_get_global_properties (WpProxy * self)
{
  WpProxyPrivate *priv;

  g_return_val_if_fail (WP_IS_PROXY (self), NULL);

  priv = wp_proxy_get_instance_private (self);
  return priv->global_props ? wp_properties_ref (priv->global_props) : NULL;
}

guint32
wp_proxy_get_interface_type (WpProxy * self)
{
  WpProxyPrivate *priv;

  g_return_val_if_fail (WP_IS_PROXY (self), 0);

  priv = wp_proxy_get_instance_private (self);
  return priv->iface_type;
}

const gchar *
wp_proxy_get_interface_name (WpProxy * self)
{
  const gchar *name = NULL;

  g_return_val_if_fail (WP_IS_PROXY (self), NULL);

  g_object_get (self, "interface-name", &name, NULL);
  return name;
}

GQuark
wp_proxy_get_interface_quark (WpProxy * self)
{
  GQuark q = 0;

  g_return_val_if_fail (WP_IS_PROXY (self), 0);

  g_object_get (self, "interface-quark", &q, NULL);
  return q;
}

guint32
wp_proxy_get_interface_version (WpProxy * self)
{
  WpProxyPrivate *priv;

  g_return_val_if_fail (WP_IS_PROXY (self), 0);

  priv = wp_proxy_get_instance_private (self);
  return priv->iface_version;
}

struct pw_proxy *
wp_proxy_get_pw_proxy (WpProxy * self)
{
  WpProxyPrivate *priv;

  g_return_val_if_fail (WP_IS_PROXY (self), NULL);

  priv = wp_proxy_get_instance_private (self);
  return priv->pw_proxy;
}

gconstpointer
wp_proxy_get_native_info (WpProxy * self)
{
  WpProxyPrivate *priv;

  g_return_val_if_fail (WP_IS_PROXY (self), NULL);

  priv = wp_proxy_get_instance_private (self);
  return priv->native_info;
}

void
wp_proxy_update_native_info (WpProxy * self, gconstpointer info,
    WpProxyNativeInfoUpdate update, GDestroyNotify destroy)
{
  WpProxyPrivate *priv;

  g_return_if_fail (WP_IS_PROXY (self));
  g_return_if_fail (destroy != NULL);

  priv = wp_proxy_get_instance_private (self);

  if (update) {
    priv->native_info = update (priv->native_info, info);
  } else {
    g_clear_pointer (&priv->native_info, priv->native_info_destroy);
    priv->native_info = (gpointer) info;
  }

  priv->native_info_destroy = destroy;

  g_object_notify (G_OBJECT (self), "native-info");
}
