#include "gui/actions/menu.h"


void append_file(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  dt_action_t *pnl = dt_action_section(&darktable.control->actions_global, N_("File"));
  dt_action_t *ac;

  add_sub_menu_entry(menus, lists, _("Copy files on disk…"), index, NULL, dt_control_copy_images, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Copy files on disk"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_copy_images, 0, 0);

  add_sub_menu_entry(menus, lists, _("Move files on disk…"), index, NULL, dt_control_move_images, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Move files on disk"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_move_images, 0, 0);

  add_sub_menu_entry(menus, lists, _("Create a blended HDR"), index, NULL, dt_control_merge_hdr, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Create a blended HDR"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_merge_hdr, 0, 0);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Copy distant images locally"), index, NULL, dt_control_set_local_copy_images, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Copy distant images locally"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_set_local_copy_images, 0, 0);

  add_sub_menu_entry(menus, lists, _("Resynchronize distant images"), index, NULL, dt_control_reset_local_copy_images, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Resynchronize distant images"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_reset_local_copy_images, 0, 0);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Remove from library"), index, NULL, (void *)dt_control_remove_images, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Remove files from library"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, (void *)dt_control_remove_images, GDK_KEY_Delete, 0);

  add_sub_menu_entry(menus, lists, _("Delete on disk"), index, NULL, dt_control_delete_images, NULL, NULL,
                     sensitive_if_selected);
  ac = dt_action_define(pnl, NULL, N_("Delete files on disk"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_delete_images, GDK_KEY_Delete, GDK_SHIFT_MASK);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Quit"), index, NULL, dt_control_quit, NULL, NULL, NULL);
  ac = dt_action_define(pnl, NULL, N_("Quit"), get_last_widget(lists), NULL);
  dt_action_register(ac, NULL, dt_control_quit, GDK_KEY_q, GDK_CONTROL_MASK);
}
