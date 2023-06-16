#include "gui/actions/menu.h"
#include "common/grouping.h"


void rotate_counterclockwise_callback()
{
  dt_control_flip_images(1);
}

void rotate_clockwise_callback()
{
  dt_control_flip_images(0);
}

void reset_rotation_callback()
{
  dt_control_flip_images(2);
}

/** merges all the selected images into a single group.
 * if there is an expanded group, then they will be joined there, otherwise a new one will be created. */
void group_images_callback()
{
  int new_group_id = darktable.gui->expanded_group_id;
  GList *imgs = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1,
                              &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    if(new_group_id == -1) new_group_id = id;
    dt_grouping_add_to_group(new_group_id, id);
    imgs = g_list_prepend(imgs, GINT_TO_POINTER(id));
  }
  imgs = g_list_reverse(imgs); // list was built in reverse order, so un-reverse it
  sqlite3_finalize(stmt);
  if(darktable.gui->grouping)
    darktable.gui->expanded_group_id = new_group_id;
  else
    darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_GROUPING, imgs);
  dt_control_queue_redraw_center();
}

/** removes the selected images from their current group. */
void ungroup_images_callback()
{
  GList *imgs = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1,
                              &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    const int new_group_id = dt_grouping_remove_from_group(id);
    if(new_group_id != -1)
    {
      // new_group_id == -1 if image to be ungrouped was a single image and no change to any group was made
      imgs = g_list_prepend(imgs, GINT_TO_POINTER(id));
    }
  }
  sqlite3_finalize(stmt);
  if(imgs != NULL)
  {
    darktable.gui->expanded_group_id = -1;
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_GROUPING,
                               g_list_reverse(imgs));
    dt_control_queue_redraw_center();
  }
}


void append_image(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  dt_action_t *pnl = dt_action_section(&darktable.control->actions_global, N_("Image"));
  dt_action_t *ac;

  add_top_submenu_entry(menus, lists, _("Rotate"), index);
  GtkWidget *parent = get_last_widget(lists);

  add_sub_sub_menu_entry(parent, lists, _("90° counter-clockwise"), index, NULL,
                         rotate_counterclockwise_callback, NULL, NULL, sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Rotate 90° counter-clockwise"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, rotate_counterclockwise_callback, 0, 0);

  add_sub_sub_menu_entry(parent, lists, _("90° clockwise"), index, NULL,
                         rotate_clockwise_callback, NULL, NULL, sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Rotate 90° clockwise"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, rotate_clockwise_callback, 0, 0);

  add_sub_menu_separator(parent);

  add_sub_sub_menu_entry(parent, lists, _("Reset rotation"), index, NULL,
                         reset_rotation_callback, NULL, NULL, sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Reset rotation"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, reset_rotation_callback, 0, 0);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Reload EXIF from file"), index, NULL, dt_control_refresh_exif, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Reload EXIF from file"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_refresh_exif, 0, 0);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Group images"), index, NULL, group_images_callback, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Group images"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, group_images_callback, GDK_KEY_g, GDK_CONTROL_MASK);

  add_sub_menu_entry(menus, lists, _("Ungroup images"), index, NULL, ungroup_images_callback, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Ungroup images"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, ungroup_images_callback, GDK_KEY_g, GDK_CONTROL_MASK);
}
