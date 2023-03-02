/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

#include "common/darktable.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#include "gui/actions/global.h"
#include "gui/actions/views.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

typedef struct dt_lib_menu_t
{
  GtkWidget *menu_bar;
  GtkWidget *menus[DT_MENU_LAST];
  GList *item_lists;
} dt_lib_menu_t;

const char *name(dt_lib_module_t *self)
{
  return _("Main menu");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_TOP_LEFT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_menu_t *d = (dt_lib_menu_t *)g_malloc0(sizeof(dt_lib_menu_t));
  self->data = (void *)d;
  d->item_lists = NULL;

  /* Init container widget */
  self->widget = gtk_box_new(FALSE, 0);
  d->menu_bar = gtk_menu_bar_new();
  gtk_widget_set_name(d->menu_bar, "menu-bar");

  /* Init top-level menus */
  gchar *labels [DT_MENU_LAST] = { _("_File"), _("_Edit"), _("_Selection"), _("_Display"), _("_Ateliers"), _("_Help") };
  for(int i = 0; i < DT_MENU_LAST; i++)
  {
    add_top_menu_entry(d->menu_bar, d->menus, &d->item_lists, i, labels[i]);
  }

  gtk_box_pack_start(GTK_BOX(self->widget), d->menu_bar, FALSE, FALSE, 0);

  /* Populate file menu */
  add_sub_menu_entry(d->menus, &d->item_lists, _("Quit"), DT_MENU_FILE, NULL, quit_callback, NULL, NULL, NULL);

  /* Populate ateliers menu */
  append_views(d->menus, &d->item_lists, DT_MENU_ATELIERS);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_menu_t *d = (dt_lib_menu_t *)self->data;
  // Free all the dt_menu_entry_t elements from the GList that stores them
  g_list_free_full(d->item_lists, g_free);
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
