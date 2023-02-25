/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/collection.h"
#include "common/darktable.h"
#include "common/l10n.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/thumbtable.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/preferences.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

typedef struct dt_lib_tool_preferences_t
{
  GtkWidget *preferences_button, *help_button, *keymap_button;
} dt_lib_tool_preferences_t;

/* callback for preference button */
static void _lib_preferences_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for help button */
static void _lib_help_button_clicked(GtkWidget *widget, gpointer user_data);
/* callbacks for key mapping button */
static void _lib_keymap_button_clicked(GtkWidget *widget, gpointer user_data);
static gboolean _lib_keymap_button_press_release(GtkWidget *button, GdkEventButton *event, gpointer user_data);

const char *name(dt_lib_module_t *self)
{
  return _("preferences");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}


void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)g_malloc0(sizeof(dt_lib_tool_preferences_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  /* create the widget help button */
  d->help_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_help, 0, NULL);
  dt_action_define(&darktable.control->actions_global, NULL, N_("help"), d->help_button, &dt_action_def_toggle);
  gtk_box_pack_start(GTK_BOX(self->widget), d->help_button, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(d->help_button, _("enable this, then click on a control element to see its online help"));
  g_signal_connect(G_OBJECT(d->help_button), "clicked", G_CALLBACK(_lib_help_button_clicked), d);

  /* create the shortcuts button */
  d->keymap_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_shortcut, 0, NULL);
  dt_action_define(&darktable.control->actions_global, NULL, N_("shortcuts"), d->keymap_button, &dt_action_def_toggle);
  gtk_box_pack_start(GTK_BOX(self->widget), d->keymap_button, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(d->keymap_button, _("define shortcuts\n"
                                                  "ctrl+click to switch off overwrite confirmations\n\n"
                                                  "hover over a widget and press keys with mouse click and scroll or move combinations\n"
                                                  "repeat same combination again to delete mapping\n"
                                                  "click on a widget, module or screen area to open the dialog for further configuration"));
  g_signal_connect(G_OBJECT(d->keymap_button), "clicked", G_CALLBACK(_lib_keymap_button_clicked), d);
  g_signal_connect(G_OBJECT(d->keymap_button), "button-press-event", G_CALLBACK(_lib_keymap_button_press_release), d);
  g_signal_connect(G_OBJECT(d->keymap_button), "button-release-event", G_CALLBACK(_lib_keymap_button_press_release), d);

  // the rest of these is added in reverse order as they are always put at the end of the container.
  // that's done so that buttons added via Lua will come first.

  /* create the preference button */
  d->preferences_button = dtgtk_button_new(dtgtk_cairo_paint_preferences, 0, NULL);
  dt_action_define(&darktable.control->actions_global, NULL, N_("preferences"), d->preferences_button, &dt_action_def_button);
  gtk_box_pack_end(GTK_BOX(self->widget), d->preferences_button, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(d->preferences_button, _("show global preferences"));
  g_signal_connect(G_OBJECT(d->preferences_button), "clicked", G_CALLBACK(_lib_preferences_button_clicked),
                   NULL);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

void _lib_preferences_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_gui_preferences_show();
}


// TODO: this doesn't work for all widgets. the reason being that the GtkEventBox we put libs/iops into catches events.
static char *get_help_url(GtkWidget *widget)
{
  while(widget)
  {
    // if the widget doesn't have a help url set go up the widget hierarchy to find a parent that has an url
    gchar *help_url = g_object_get_data(G_OBJECT(widget), "dt-help-url");

    if(help_url)
      return help_url;

    // TODO: shall we cross from libs/iops to the core gui? if not, here is the place to break out of the loop

    widget = gtk_widget_get_parent(widget);
  }

  return NULL;
}


static void _main_do_event_help(GdkEvent *event, gpointer data)
{
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)data;

  gboolean handled = FALSE;

  switch(event->type)
  {
    case GDK_BUTTON_PRESS:
    {
      GtkWidget *event_widget = gtk_get_event_widget(event);
      if(event_widget)
      {
        // if clicking on help button again process normally to switch off mode
        if(event_widget == d->help_button)
          break;

        // TODO: When the widget doesn't have a help url set we should probably look at the parent(s)
        // Note : help url contains the fully-formed URL adapted to current language
        gchar *help_url = get_help_url(event_widget);
        if(help_url && *help_url)
        {
          GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
          dt_print(DT_DEBUG_CONTROL, "[context help] opening `%s'\n", help_url);

          // ask the user if the website may be accessed
          GtkWidget *dialog = gtk_message_dialog_new
            (GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
              GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
              _("do you want to access ansel.photos ?"));
#ifdef GDK_WINDOWING_QUARTZ
            dt_osx_disallow_fullscreen(dialog);
#endif

          gtk_window_set_title(GTK_WINDOW(dialog), _("access the online usermanual?"));
          const gint res = gtk_dialog_run(GTK_DIALOG(dialog));
          const gboolean open = (res == GTK_RESPONSE_YES);
          gtk_widget_destroy(dialog);

          if(open)
          {
            // TODO: call the web browser directly so that file:// style base for local installs works
            GError *error = NULL;
            const gboolean uri_success = gtk_show_uri_on_window(GTK_WINDOW(win), help_url, gtk_get_current_event_time(), &error);

            if(uri_success)
            {
              dt_control_log(_("help url opened in web browser"));
            }
            else
            {
              dt_control_log(_("error while opening help url in web browser"));
              if (error != NULL) // uri_success being FALSE should guarantee that
              {
                fprintf (stderr, "unable to read file: %s\n", error->message);
                g_error_free (error);
              }
            }
          }
        }
        else
        {
          dt_control_log(_("there is no help available for this element"));
        }
      }
      handled = TRUE;
      break;
    }

    case GDK_BUTTON_RELEASE:
    {
      // reset GTK to normal behaviour
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->help_button), FALSE);

      handled = TRUE;
    }
    break;

    case GDK_ENTER_NOTIFY:
    case GDK_LEAVE_NOTIFY:
    {
      GtkWidget *event_widget = gtk_get_event_widget(event);
      if(event_widget)
      {
        gchar *help_url = get_help_url(event_widget);
        if(help_url)
        {
          // TODO: find a better way to tell the user that the hovered widget has a help link
          dt_cursor_t cursor = event->type == GDK_ENTER_NOTIFY ? GDK_QUESTION_ARROW : GDK_X_CURSOR;
          dt_control_allow_change_cursor();
          dt_control_change_cursor(cursor);
          dt_control_forbid_change_cursor();
        }
      }
      break;
    }
    default:
      break;
  }

  if(!handled) gtk_main_do_event(event);
}

// Don't save across sessions (window managers role)
static struct { gint x, y, w, h; } _shortcuts_dialog_posize = {};

static gboolean _resize_shortcuts_dialog(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_window_get_position(GTK_WINDOW(widget), &_shortcuts_dialog_posize.x, &_shortcuts_dialog_posize.y);
  gtk_window_get_size(GTK_WINDOW(widget), &_shortcuts_dialog_posize.w, &_shortcuts_dialog_posize.h);

  dt_conf_set_int("ui_last/shortcuts_dialog_width", _shortcuts_dialog_posize.w);
  dt_conf_set_int("ui_last/shortcuts_dialog_height", _shortcuts_dialog_posize.h);

  return FALSE;
}

static void _set_mapping_mode_cursor(GtkWidget *widget)
{
  GdkCursorType cursor = GDK_DIAMOND_CROSS;

  if(GTK_IS_EVENT_BOX(widget)) widget = gtk_bin_get_child(GTK_BIN(widget));

  if(widget && !strcmp(gtk_widget_get_name(widget), "module-header"))
    cursor = GDK_BASED_ARROW_DOWN;
  else if(g_hash_table_lookup(darktable.control->widgets, darktable.control->mapping_widget))
    cursor = GDK_BOX_SPIRAL;

  dt_control_allow_change_cursor();
  dt_control_change_cursor(cursor);
  dt_control_forbid_change_cursor();
}

static void _show_shortcuts_prefs(GtkWidget *w)
{
  GtkWidget *shortcuts_dialog = gtk_dialog_new_with_buttons(_("shortcuts"), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                                            GTK_DIALOG_DESTROY_WITH_PARENT, NULL, NULL);
  if(!_shortcuts_dialog_posize.w)
    gtk_window_set_default_size(GTK_WINDOW(shortcuts_dialog),
                                DT_PIXEL_APPLY_DPI(dt_conf_get_int("ui_last/shortcuts_dialog_width")),
                                DT_PIXEL_APPLY_DPI(dt_conf_get_int("ui_last/shortcuts_dialog_height")));
  else
  {
    gtk_window_move(GTK_WINDOW(shortcuts_dialog), _shortcuts_dialog_posize.x, _shortcuts_dialog_posize.y);
    gtk_window_resize(GTK_WINDOW(shortcuts_dialog), _shortcuts_dialog_posize.w, _shortcuts_dialog_posize.h);
  }
  g_signal_connect(G_OBJECT(shortcuts_dialog), "configure-event", G_CALLBACK(_resize_shortcuts_dialog), NULL);

  //grab the content area of the dialog
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(shortcuts_dialog));
  gtk_box_pack_start(GTK_BOX(content), dt_shortcuts_prefs(w), TRUE, TRUE, 0);

  gtk_widget_show_all(shortcuts_dialog);
  gtk_dialog_run(GTK_DIALOG(shortcuts_dialog));
  gtk_widget_destroy(shortcuts_dialog);
}

static void _main_do_event_keymap(GdkEvent *event, gpointer data)
{
  GtkWidget *event_widget = gtk_get_event_widget(event);

  switch(event->type)
  {
  case GDK_LEAVE_NOTIFY:
  case GDK_ENTER_NOTIFY:
    if(darktable.control->mapping_widget
       && event->crossing.mode == GDK_CROSSING_UNGRAB)
      break;
  case GDK_GRAB_BROKEN:
  case GDK_FOCUS_CHANGE:
    darktable.control->mapping_widget = event_widget;
    _set_mapping_mode_cursor(event_widget);
    break;
  case GDK_BUTTON_PRESS:
    if(gdk_display_device_is_grabbed(gdk_window_get_display(event->button.window), event->button.device))
      break;

    GtkWidget *main_window = dt_ui_main_window(darktable.gui->ui);
    if(gtk_widget_get_toplevel(event_widget) != main_window)
      break;

    if(!gtk_window_is_active(GTK_WINDOW(main_window)))
      break;

    dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)data;
    if(event_widget == d->keymap_button)
      break;

    if(GTK_IS_ENTRY(event_widget))
      break;

    if(event->button.button == GDK_BUTTON_SECONDARY)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->keymap_button), FALSE);
    else if(event->button.button == GDK_BUTTON_MIDDLE)
      dt_shortcut_dispatcher(event_widget, event, data);
    else if(event->button.button > 7)
      break;
    else if(dt_modifier_is(event->button.state, GDK_CONTROL_MASK))
    {
      _set_mapping_mode_cursor(event_widget);
    }
    else
    {
      // allow opening modules to map widgets inside
      if(GTK_IS_EVENT_BOX(event_widget)) event_widget = gtk_bin_get_child(GTK_BIN(event_widget));
      if(event_widget && !strcmp(gtk_widget_get_name(event_widget), "module-header"))
        break;

      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->keymap_button), FALSE);
      _show_shortcuts_prefs(event_widget);
    }

    return;
  default:
    break;
  }

  gtk_main_do_event(event);
}

static void _lib_help_button_clicked(GtkWidget *widget, gpointer user_data)
{
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)))
  {
    dt_control_change_cursor(GDK_X_CURSOR);
    dt_control_forbid_change_cursor();
    gdk_event_handler_set(_main_do_event_help, user_data, NULL);
  }
  else
  {
    dt_control_allow_change_cursor();
    dt_control_change_cursor(GDK_LEFT_PTR);
    gdk_event_handler_set((GdkEventFunc)gtk_main_do_event, NULL, NULL);
  }
}

static void _lib_keymap_button_clicked(GtkWidget *widget, gpointer user_data)
{
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)))
  {
    gdk_event_handler_set(_main_do_event_keymap, user_data, NULL);
  }
  else
  {
    darktable.control->mapping_widget = NULL;
    dt_control_allow_change_cursor();
    dt_control_change_cursor(GDK_LEFT_PTR);
    gdk_event_handler_set((GdkEventFunc)gtk_main_do_event, NULL, NULL);
  }
}

static gboolean _lib_keymap_button_press_release(GtkWidget *button, GdkEventButton *event, gpointer user_data)
{
  static guint start_time = 0;

  darktable.control->confirm_mapping = !dt_modifier_is(event->state, GDK_CONTROL_MASK);

  int delay = 0;
  g_object_get(gtk_settings_get_default(), "gtk-long-press-time", &delay, NULL);

  if((event->type == GDK_BUTTON_PRESS && event->button == 3) ||
     (event->type == GDK_BUTTON_RELEASE && event->time - start_time > delay))
  {
    _show_shortcuts_prefs(NULL);
    return TRUE;
  }
  else
  {
    start_time = event->time;
    return FALSE;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
