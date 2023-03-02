
#include "common/darktable.h"
#include "common/debug.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#pragma once

typedef enum dt_menus_t
{
  DT_MENU_FILE = 0,
  DT_MENU_EDIT,
  DT_MENU_SELECTION,
  DT_MENU_DISPLAY,
  DT_MENU_ATELIERS,
  DT_MENU_HELP,
  DT_MENU_LAST
} dt_menus_t;


typedef struct dt_menu_entry_t
{
  GtkWidget *widget;         // Bounding box for the whole menu item
  gboolean has_checkbox;     // Show or hide the checkbox, aka "is the current action setting a boolean ?"
  void (*action_callback)(GtkWidget *widget); // Callback to run when the menu item is clicked, aka the action
  gboolean (*sensitive_callback)(GtkWidget *widget);      // Callback checking some conditions to decide whether the menu item should be made insensitive in current context
  gboolean (*checked_callback)(GtkWidget *widget);        // Callback checking some conditions to determine if the boolean pref set by the item is currently TRUE or FALSE
  gboolean (*active_callback)(GtkWidget *widget);         // Callback checking some conditions to determine if the menu item is the current view
  dt_menus_t menu;           // Index of first-level menu
} dt_menu_entry_t;


/** How to use:
 *  1. write callback functions returning a gboolean that will check the context to decide if
 *  the menu item should be insensitive, checked, active. These should only use the content of
 *  globally accessible structures like `darktable.gui` since they take no arguments.
 *
 *  2. re-use the action callback functions already used for global keyboard shortcuts (actions/accels).
 *  Again, all inputs and internal functions should be globally accessible, for example using proxies.
 *
 *  3. wire everything with the `set_menu_entry` function below. GUI states of the children menu items
 *  will be updated automatically everytime a top-level menu is opened.
 **/

static dt_menu_entry_t * set_menu_entry(GList **items_list, const gchar *label, gchar *shortcut,
                                        dt_menus_t menu_index,
                                        void *data,
                                        void (*action_callback)(GtkWidget *widget),
                                        gboolean (*checked_callback)(GtkWidget *widget),
                                        gboolean (*active_callback)(GtkWidget *widget),
                                        gboolean (*sensitive_callback)(GtkWidget *widget))
{
  // Alloc and set to 0
  dt_menu_entry_t *entry = calloc(1, sizeof(dt_menu_entry_t));

  // Main widget
  if(checked_callback)
    entry->widget = gtk_check_menu_item_new_with_label(label);
  else
    entry->widget = gtk_menu_item_new_with_label(label);

  // Store arbitrary data in the GtkWidget if needed
  if(data)
    g_object_set_data(G_OBJECT(entry->widget), "custom-data", data);

  entry->menu = menu_index;
  entry->action_callback = action_callback;
  entry->checked_callback = checked_callback;
  entry->sensitive_callback = sensitive_callback;
  entry->active_callback = active_callback;

  // Add it to the list of menus items for easy sequential access later
  *items_list = g_list_append(*items_list, entry);

  return entry;
}

void update_entry(dt_menu_entry_t *entry)
{
  // Use the callbacks functions to update the visual properties of the menu entry
  if(entry->checked_callback)
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(entry->widget), entry->checked_callback(entry->widget));

  if(entry->sensitive_callback)
    gtk_widget_set_sensitive(entry->widget, entry->sensitive_callback(entry->widget));

  if(entry->active_callback)
  {
    if(entry->active_callback(entry->widget)) dt_gui_add_class(entry->widget, "menu-active");
    else dt_gui_remove_class(entry->widget, "menu-active");
  }
}

void update_menu_entries(GtkWidget *widget, gpointer user_data)
{
  // Update the graphic state of all sub-menu entries from the current top-level menu
  // but only when it is opened.
  GList **entries = (GList **)user_data;
  if(*entries)
  {
    for(GList *entry_iter = g_list_first(*entries); entry_iter; entry_iter = g_list_next(entry_iter))
    {
      dt_menu_entry_t *entry = (dt_menu_entry_t *)(entry_iter)->data;
      if(entry) update_entry(entry);
    }
  }
}

void add_top_menu_entry(GtkWidget *menu_bar, GtkWidget **menus, GList **lists, const dt_menus_t index, gchar *label)
{
  menus[index] = gtk_menu_new();
  GtkWidget *menu_label = gtk_menu_item_new_with_mnemonic(label);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_label), menus[index]);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), menu_label);
  dt_gui_add_class(menu_label, "top-level-item");
  gtk_menu_item_set_use_underline(GTK_MENU_ITEM(menu_label), TRUE);
  g_signal_connect(G_OBJECT(menu_label), "activate", G_CALLBACK(update_menu_entries), lists);
}

void add_sub_menu_entry(GtkWidget **menus, GList **lists, const gchar *label, const dt_menus_t index,
                        void *data,
                        void (*action_callback)(GtkWidget *widget),
                        gboolean (*checked_callback)(GtkWidget *widget),
                        gboolean (*active_callback)(GtkWidget *widget),
                        gboolean (*sensitive_callback)(GtkWidget *widget))
{
  dt_menu_entry_t *entry = set_menu_entry(lists, label, NULL, index,
                                          data,
                                          action_callback, checked_callback,
                                          active_callback, sensitive_callback);
  update_entry(entry);
  gtk_menu_shell_append(GTK_MENU_SHELL(menus[index]), entry->widget);
  gtk_menu_item_set_reserve_indicator(GTK_MENU_ITEM(entry->widget), TRUE);

  if(action_callback)
    g_signal_connect(G_OBJECT(entry->widget), "activate", G_CALLBACK(entry->action_callback), NULL);
}

void add_menu_separator(GtkWidget *menu)
{
  GtkWidget *sep = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);
}

const char * get_label_text(GtkWidget *widget)
{
  // Helper to get the text label from a menuitem
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(widget));
  return gtk_label_get_label(GTK_LABEL(child));
}

void * get_custom_data(GtkWidget *widget)
{
  // Grab custom data optionnaly passed by pointer to the menuitem widget
  return g_object_get_data(G_OBJECT(widget), "custom-data");
}

GtkWidget * get_last_widget(GList **list)
{
  GList *last_entry = g_list_last(*list);
  GtkWidget *w = NULL;
  if(last_entry)
  {
    dt_menu_entry_t *entry = (dt_menu_entry_t *)(last_entry)->data;
    if(entry && entry->widget) w = entry->widget;
  }
  return w;
}
