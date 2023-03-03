#include "gui/actions/menu.h"
#include "gui/preferences.h"


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

static void _reset_cursor_and_keymap()
{
  darktable.control->mapping_widget = NULL;
  dt_control_allow_change_cursor();
  dt_control_change_cursor(GDK_LEFT_PTR);
  gdk_event_handler_set((GdkEventFunc)gtk_main_do_event, NULL, NULL);
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

static gboolean _cancel_key_mapping(GdkEvent *event, GtkWidget *event_widget, GtkWidget *main_window)
{
  // Check all conditions that would discard key shortcut mapping
  const gboolean cond1
      = gdk_display_device_is_grabbed(gdk_window_get_display(event->button.window), event->button.device);
  const gboolean cond2 = (gtk_widget_get_toplevel(event_widget) != main_window);
  const gboolean cond3 = !gtk_window_is_active(GTK_WINDOW(main_window));
  const gboolean cond4 = GTK_IS_ENTRY(event_widget);
  return (cond1 || cond2 || cond3 || cond4);
}

static void _main_do_event_keymap(GdkEvent *event, gpointer data)
{
  GtkWidget *event_widget = gtk_get_event_widget(event);

  switch(event->type)
  {
  case GDK_LEAVE_NOTIFY:
  case GDK_ENTER_NOTIFY:
  {
    if(darktable.control->mapping_widget
       && event->crossing.mode == GDK_CROSSING_UNGRAB)
      break;
  }
  case GDK_GRAB_BROKEN:
  case GDK_FOCUS_CHANGE:
  {
    darktable.control->mapping_widget = event_widget;
    _set_mapping_mode_cursor(event_widget);
    break;
  }
  case GDK_BUTTON_PRESS:
  {
    GtkWidget *main_window = dt_ui_main_window(darktable.gui->ui);
    if(_cancel_key_mapping(event, event_widget, main_window))
    {
      _reset_cursor_and_keymap();
      break;
    }

    if(event->button.button == GDK_BUTTON_SECONDARY)
      _reset_cursor_and_keymap();
    else if(event->button.button == GDK_BUTTON_MIDDLE)
      dt_shortcut_dispatcher(event_widget, event, data);
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
      _reset_cursor_and_keymap();
      _show_shortcuts_prefs(event_widget);
    }
    return;
  }
  default:
    break;
  }

  gtk_main_do_event(event);
}

static void define_keymap_callback(GtkWidget *widget)
{
  gdk_event_handler_set(_main_do_event_keymap, NULL, NULL);
}

static void preferences_callback(GtkWidget *widget)
{
  dt_gui_preferences_show();
}

void append_edit(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  add_sub_menu_entry(menus, lists, _("Key shortcuts"), index, NULL, define_keymap_callback, NULL, NULL, NULL);
  add_sub_menu_entry(menus, lists, _("Preferences"), index, NULL, preferences_callback, NULL, NULL, NULL);
}
