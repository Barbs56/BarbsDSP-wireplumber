/* WirePlumber
 *
 * Copyright © 2019-2021 Collabora Ltd.
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <wp/wp.h>
#include <glib-unix.h>
#include <pipewire/keys.h>

static gchar * config_file = NULL;

static GOptionEntry entries[] =
{
  { "config-file", 'c', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &config_file,
    "The configuration file to load components from", NULL },
  { NULL }
};

#define WP_DOMAIN_DAEMON (wp_domain_daemon_quark ())
static G_DEFINE_QUARK (wireplumber-daemon, wp_domain_daemon);

enum WpExitCode
{
  WP_CODE_DISCONNECTED = 0,
  WP_CODE_INTERRUPTED,
  WP_CODE_OPERATION_FAILED,
  WP_CODE_INVALID_ARGUMENT,
};

/*** WpInitTransition ***/

struct _WpInitTransition
{
  WpTransition parent;
  WpObjectManager *om;
  guint pending_plugins;
};

enum {
  STEP_LOAD_MODULE = WP_TRANSITION_STEP_CUSTOM_START,
  STEP_LOAD_CONFIG,
  STEP_CONNECT,
  STEP_ACTIVATE_PLUGINS,
  STEP_ACTIVATE_SCRIPTS,
};

G_DECLARE_FINAL_TYPE (WpInitTransition, wp_init_transition,
                      WP, INIT_TRANSITION, WpTransition)
G_DEFINE_TYPE (WpInitTransition, wp_init_transition, WP_TYPE_TRANSITION)

static void
wp_init_transition_init (WpInitTransition * self)
{
}

static guint
wp_init_transition_get_next_step (WpTransition * transition, guint step)
{
  switch (step) {
  case WP_TRANSITION_STEP_NONE: return STEP_LOAD_MODULE;
  case STEP_LOAD_MODULE:        return STEP_LOAD_CONFIG;
  case STEP_LOAD_CONFIG:        return STEP_CONNECT;
  case STEP_CONNECT:            return STEP_ACTIVATE_PLUGINS;
  case STEP_ACTIVATE_PLUGINS: {
    WpInitTransition *self = WP_INIT_TRANSITION (transition);
    if (self->pending_plugins == 0)
      return STEP_ACTIVATE_SCRIPTS;
    else
      return STEP_ACTIVATE_PLUGINS;
  }
  case STEP_ACTIVATE_SCRIPTS:   return WP_TRANSITION_STEP_NONE;
  default:
    g_return_val_if_reached (WP_TRANSITION_STEP_ERROR);
  }
}

static void
on_plugin_activated (WpObject * p, GAsyncResult * res, WpInitTransition *self)
{
  GError *error = NULL;

  if (!wp_object_activate_finish (p, res, &error)) {
    wp_transition_return_error (WP_TRANSITION (self), error);
    return;
  }

  --self->pending_plugins;
  wp_transition_advance (WP_TRANSITION (self));
}

static void
on_plugin_added (WpObjectManager * om, WpObject * p, WpInitTransition *self)
{
  self->pending_plugins++;
  wp_object_activate (p, WP_PLUGIN_FEATURE_ENABLED, NULL,
      (GAsyncReadyCallback) on_plugin_activated, self);
}

static void
wp_init_transition_execute_step (WpTransition * transition, guint step)
{
  WpInitTransition *self = WP_INIT_TRANSITION (transition);
  WpCore *core = wp_transition_get_source_object (transition);
  GError *error = NULL;

  switch (step) {
  case STEP_LOAD_MODULE:
    if (!wp_core_load_component (core, "libwireplumber-module-lua-scripting",
            "module", NULL, &error)) {
      wp_transition_return_error (transition, error);
      return;
    }
    wp_transition_advance (transition);
    break;

  case STEP_LOAD_CONFIG: {
    const gchar *f = config_file ? config_file : "config.lua";

    if (!wp_core_load_component (core, f, "config/lua", NULL, &error)) {
      wp_transition_return_error (transition, error);
      return;
    }
    wp_transition_advance (transition);
    break;
  }

  case STEP_CONNECT:
    g_signal_connect_object (core, "connected",
        G_CALLBACK (wp_transition_advance), transition, G_CONNECT_SWAPPED);

    if (!wp_core_connect (core)) {
      wp_transition_return_error (transition, g_error_new (WP_DOMAIN_DAEMON,
          WP_CODE_OPERATION_FAILED, "Failed to connect to PipeWire"));
      return;
    }
    break;

  case STEP_ACTIVATE_PLUGINS:
    wp_info_object (self, "Activating plugins...");

    self->om = wp_object_manager_new ();
    wp_object_manager_add_interest (self->om, WP_TYPE_PLUGIN,
        WP_CONSTRAINT_TYPE_G_PROPERTY, "name", "!s", "lua-scripting",
        NULL);
    g_signal_connect_object (self->om, "object-added",
        G_CALLBACK (on_plugin_added), self, 0);
    g_signal_connect_object (self->om, "installed",
        G_CALLBACK (wp_transition_advance), transition, G_CONNECT_SWAPPED);
    wp_core_install_object_manager (core, self->om);
    break;

  case STEP_ACTIVATE_SCRIPTS: {
    g_clear_object (&self->om);

    wp_info_object (self, "Executing scripts...");

    WpPlugin *p = wp_plugin_find (core, "lua-scripting");
    wp_object_activate (WP_OBJECT (p), WP_PLUGIN_FEATURE_ENABLED, NULL,
        (GAsyncReadyCallback) on_plugin_activated, self);
    break;
  }

  case WP_TRANSITION_STEP_ERROR:
    g_clear_object (&self->om);
    break;

  default:
    g_assert_not_reached ();
  }
}

static void
wp_init_transition_class_init (WpInitTransitionClass * klass)
{
  WpTransitionClass * transition_class = (WpTransitionClass *) klass;

  transition_class->get_next_step = wp_init_transition_get_next_step;
  transition_class->execute_step = wp_init_transition_execute_step;
}

/*** WpDaemon ***/

typedef struct
{
  WpCore *core;
  GMainLoop *loop;

  gint exit_code;
  gchar *exit_message;
  GDestroyNotify free_message;
} WpDaemon;

static void
daemon_clear (WpDaemon * self)
{
  if (self->free_message) {
    g_clear_pointer (&self->exit_message, self->free_message);
    self->free_message = NULL;
  }
  g_clear_pointer (&self->loop, g_main_loop_unref);
  g_clear_object (&self->core);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WpDaemon, daemon_clear)

static G_GNUC_PRINTF (3, 4) void
daemon_exit (WpDaemon * d, gint code, const gchar *format, ...)
{
  va_list args;
  va_start (args, format);
  d->exit_code = code;
  d->exit_message = g_strdup_vprintf (format, args);
  d->free_message = g_free;
  va_end (args);
  g_main_loop_quit (d->loop);
}

static void
daemon_exit_static_str (WpDaemon * d, gint code, const gchar *str)
{
  d->exit_code = code;
  d->exit_message = (gchar *) str;
  d->free_message = NULL;
  g_main_loop_quit (d->loop);
}

static void
on_disconnected (WpCore *core, WpDaemon * d)
{
  /* something else triggered the exit; let's not change the message */
  if (d->exit_message)
    return;

  daemon_exit_static_str (d, WP_CODE_DISCONNECTED,
      "disconnected from pipewire");
}

static gboolean
signal_handler (gpointer data)
{
  WpDaemon *d = data;
  daemon_exit_static_str (d, WP_CODE_INTERRUPTED, "interrupted by signal");
  return G_SOURCE_CONTINUE;
}

static gboolean
init_start (WpTransition * transition)
{
  wp_transition_advance (transition);
  return G_SOURCE_REMOVE;
}

static void
init_done (WpCore * core, GAsyncResult * res, WpDaemon * d)
{
  g_autoptr (GError) error = NULL;
  if (!wp_transition_finish (res, &error))
    daemon_exit (d, WP_CODE_OPERATION_FAILED, "%s", error->message);
}

gint
main (gint argc, gchar **argv)
{
  g_auto (WpDaemon) d = {0};
  g_autoptr (GOptionContext) context = NULL;
  g_autoptr (GError) error = NULL;

  wp_init (WP_INIT_ALL);

  context = g_option_context_new ("- PipeWire Session/Policy Manager");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    wp_message ("%s", error->message);
    return WP_CODE_INVALID_ARGUMENT;
  }

  /* init wireplumber daemon */
  d.loop = g_main_loop_new (NULL, FALSE);
  d.core = wp_core_new (NULL, wp_properties_new (
          PW_KEY_APP_NAME, "WirePlumber",
          NULL));
  g_signal_connect (d.core, "disconnected", G_CALLBACK (on_disconnected), &d);

  /* watch for exit signals */
  g_unix_signal_add (SIGINT, signal_handler, &d);
  g_unix_signal_add (SIGTERM, signal_handler, &d);
  g_unix_signal_add (SIGHUP, signal_handler, &d);

  /* initialization transition */
  g_idle_add ((GSourceFunc) init_start,
      wp_transition_new (wp_init_transition_get_type (), d.core,
          NULL, (GAsyncReadyCallback) init_done, &d));

  /* run */
  g_main_loop_run (d.loop);
  wp_core_disconnect (d.core);

  if (d.exit_message)
    wp_message ("%s", d.exit_message);
  return d.exit_code;
}
