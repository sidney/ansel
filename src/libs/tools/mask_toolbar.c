#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/masks.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "bauhaus/bauhaus.h"

DT_MODULE(1)

typedef struct dt_lib_tool_mask_t
{
  GtkWidget *mask_lock;
} dt_lib_tool_mask_t;

const char *name(dt_lib_module_t *self)
{
  return _("mask toolbar");
}

const char **views(dt_lib_module_t *self)
{
  /* for now, show in all view due this affects filmroll too

     TODO: Consider to add flag for all views, which prevents
           unloading/loading a module while switching views.

   */
  static const char *v[] = {"darkroom", NULL};
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
  return 1000;
}

static void mask_lock_callback(GtkWidget *widget, gpointer data)
{
  if(darktable.gui->reset) return;
  dt_masks_set_lock_mode(darktable.develop, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  darktable.develop->darkroom_skip_mouse_events = dt_masks_get_lock_mode(darktable.develop);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_tool_mask_t *d = (dt_lib_tool_mask_t *)g_malloc0(sizeof(dt_lib_tool_mask_t));
  self->data = (void *)d;
  self->widget = gtk_box_new(FALSE, 0);
  d->mask_lock = gtk_check_button_new_with_label(_("Lock masks"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->mask_lock, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(d->mask_lock, _("Prevent accidental masks displacement when moving the view"));
  g_signal_connect(G_OBJECT(d->mask_lock), "toggled", G_CALLBACK(mask_lock_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}
