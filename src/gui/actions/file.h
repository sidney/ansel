#include "gui/actions/menu.h"


static void quit_callback(GtkWidget *widget)
{
  dt_control_quit();
}

void append_file(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  add_sub_menu_entry(menus, lists, _("Quit"), index, NULL, quit_callback, NULL, NULL, NULL);
}
