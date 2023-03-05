#include "gui/actions/menu.h"
#include "control/crawler.h"

static void clear_caches_callback(GtkWidget *widget)
{
  dt_dev_reprocess_all(darktable.develop);
}

static void optimize_database_callback(GtkWidget *widget)
{
  dt_database_perform_maintenance(darktable.db);
}

static void backup_database_callback(GtkWidget *widget)
{
  dt_database_snapshot(darktable.db);
}

static void crawl_xmp_changes(GtkWidget *widget)
{
  GList *changed_xmp_files = dt_control_crawler_run();
  dt_control_crawler_show_image_list(changed_xmp_files);
}

void append_run(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  add_sub_menu_entry(menus, lists, _("Clear all pipeline caches"), index, NULL, clear_caches_callback, NULL, NULL, NULL);
  add_menu_separator(menus[index]);
  add_sub_menu_entry(menus, lists, _("Defragment the library"), index, NULL, optimize_database_callback, NULL, NULL, NULL);
  add_sub_menu_entry(menus, lists, _("Backup the library"), index, NULL, backup_database_callback, NULL, NULL, NULL);
  add_sub_menu_entry(menus, lists, _("Resynchronize library and XMP"), index, NULL, crawl_xmp_changes, NULL, NULL, NULL);
}
