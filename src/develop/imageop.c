/*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.

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

#include "develop/imageop.h"
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#include "common/exif.h"
#include "common/history.h"
#include "common/imagebuf.h"
#include "common/imageio_rawspeed.h"
#include "common/interpolation.h"
#include "common/module.h"
#include "common/opencl.h"
#include "common/usermanual_url.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/format.h"
#include "develop/masks.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/gradientslider.h"
#include "dtgtk/icon.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "libs/modulegroups.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <assert.h>
#include <gmodule.h>
#include <math.h>
#include <complex.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <time.h>

enum
{
  DT_ACTION_ELEMENT_SHOW = 0,
  DT_ACTION_ELEMENT_ENABLE = 1,
  DT_ACTION_ELEMENT_INSTANCE = 3,
  DT_ACTION_ELEMENT_RESET = 4,
  DT_ACTION_ELEMENT_PRESETS = 5,
};

typedef struct dt_iop_gui_simple_callback_t
{
  dt_iop_module_t *self;
  int index;
} dt_iop_gui_simple_callback_t;

static void _show_module_callback(dt_iop_module_t *module);
static void _enable_module_callback(dt_iop_module_t *module);

void dt_iop_load_default_params(dt_iop_module_t *module)
{
  memcpy(module->params, module->default_params, module->params_size);
  dt_develop_blend_colorspace_t cst = dt_develop_blend_default_module_blend_colorspace(module);
  dt_develop_blend_init_blend_parameters(module->default_blendop_params, cst);
  dt_iop_commit_blend_params(module, module->default_blendop_params);
  dt_iop_gui_blending_reload_defaults(module);
  dt_iop_module_hash(module);
}

static void _iop_modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                               const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
}

static void _iop_modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}

/* default group for modules which do not implement the default_group() function */
static int default_default_group(void)
{
  return IOP_GROUP_TECHNICAL;
}

/* default flags for modules which does not implement the flags() function */
static int default_flags(void)
{
  return 0;
}

/* default operation tags for modules which does not implement the flags() function */
static int default_operation_tags(void)
{
  return 0;
}

/* default operation tags filter for modules which does not implement the flags() function */
static int default_operation_tags_filter(void)
{
  return 0;
}

static const char **default_description(struct dt_iop_module_t *self)
{
  return NULL;
}

static const char *default_aliases(void)
{
  return "";
}

static const char *default_deprecated_msg(void)
{
  return NULL;
}

static void default_commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params,
                                   dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, self->params_size);
}

static void default_init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                              dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1,self->params_size);
  piece->data_size = self->params_size;
}

static void default_cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                                 dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

static void default_gui_cleanup(dt_iop_module_t *self)
{
  IOP_GUI_FREE;
}

static void default_cleanup(dt_iop_module_t *module)
{
  g_free(module->params);
  module->params = NULL;
  free(module->default_params);
  module->default_params = NULL;
}


static int default_distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points,
                                     size_t points_count)
{
  return 1;
}
static int default_distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points,
                                         size_t points_count)
{
  return 1;
}

static void default_process(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                            const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
                            const struct dt_iop_roi_t *const roi_out)
{
  if(roi_in->width <= 1 || roi_in->height <= 1 || roi_out->width <= 1 || roi_out->height <= 1) return;

  if(darktable.codepath.OPENMP_SIMD && self->process_plain)
    self->process_plain(self, piece, i, o, roi_in, roi_out);
#if defined(__SSE__)
  else if(darktable.codepath.SSE2 && self->process_sse2)
    self->process_sse2(self, piece, i, o, roi_in, roi_out);
#endif
  else if(self->process_plain)
    self->process_plain(self, piece, i, o, roi_in, roi_out);
  else
    dt_unreachable_codepath_with_desc(self->op);
}

static dt_introspection_field_t *default_get_introspection_linear(void)
{
  return NULL;
}
static dt_introspection_t *default_get_introspection(void)
{
  return NULL;
}
static void *default_get_p(const void *param, const char *name)
{
  return NULL;
}
static dt_introspection_field_t *default_get_f(const char *name)
{
  return NULL;
}

void dt_iop_default_init(dt_iop_module_t *module)
{
  size_t param_size = module->so->get_introspection()->size;
  module->params_size = param_size;
  module->params = (dt_iop_params_t *)calloc(1, param_size);
  module->default_params = (dt_iop_params_t *)calloc(1, param_size);

  module->default_enabled = 0;
  module->gui_data = NULL;

  dt_introspection_field_t *i = module->so->get_introspection_linear();
  while(i->header.type != DT_INTROSPECTION_TYPE_NONE)
  {
    switch(i->header.type)
    {
    case DT_INTROSPECTION_TYPE_FLOAT:
      *(float*)((uint8_t *)module->default_params + i->header.offset) = i->Float.Default;
      break;
    case DT_INTROSPECTION_TYPE_INT:
      *(int*)((uint8_t *)module->default_params + i->header.offset) = i->Int.Default;
      break;
    case DT_INTROSPECTION_TYPE_UINT:
      *(unsigned int*)((uint8_t *)module->default_params + i->header.offset) = i->UInt.Default;
      break;
    case DT_INTROSPECTION_TYPE_USHORT:
      *(unsigned short*)((uint8_t *)module->default_params + i->header.offset) = i->UShort.Default;
      break;
    case DT_INTROSPECTION_TYPE_ENUM:
      *(int*)((uint8_t *)module->default_params + i->header.offset) = i->Enum.Default;
      break;
    case DT_INTROSPECTION_TYPE_BOOL:
      *(gboolean*)((uint8_t *)module->default_params + i->header.offset) = i->Bool.Default;
      break;
    case DT_INTROSPECTION_TYPE_CHAR:
      *(char*)((uint8_t *)module->default_params + i->header.offset) = i->Char.Default;
      break;
    case DT_INTROSPECTION_TYPE_OPAQUE:
      memset((uint8_t *)module->default_params + i->header.offset, 0, i->header.size);
      break;
    case DT_INTROSPECTION_TYPE_ARRAY:
      {
        if(i->Array.type == DT_INTROSPECTION_TYPE_CHAR) break;

        size_t element_size = i->Array.field->header.size;
        if(element_size % sizeof(int))
        {
          int8_t *p = (int8_t *)module->default_params + i->header.offset;
          for (size_t c = element_size; c < i->header.size; c++, p++)
            p[element_size] = *p;
        }
        else
        {
          element_size /= sizeof(int);
          size_t num_ints = i->header.size / sizeof(int);

          int *p = (int *)((uint8_t *)module->default_params + i->header.offset);
          for (size_t c = element_size; c < num_ints; c++, p++)
            p[element_size] = *p;
        }
      }
      break;
    case DT_INTROSPECTION_TYPE_STRUCT:
      // ignore STRUCT; nothing to do
      break;
    default:
      fprintf(stderr, "unsupported introspection type \"%s\" encountered in dt_iop_default_init (field %s)\n", i->header.type_name, i->header.field_name);
      break;
    }

    i++;
  }
}

int dt_iop_load_module_so(void *m, const char *libname, const char *module_name)
{
  dt_iop_module_so_t *module = (dt_iop_module_so_t *)m;
  g_strlcpy(module->op, module_name, sizeof(module->op));

#define INCLUDE_API_FROM_MODULE_LOAD "iop_load_module"
#include "iop/iop_api.h"

  if(!module->init) module->init = dt_iop_default_init;
  if(!module->modify_roi_in) module->modify_roi_in = _iop_modify_roi_in;
  if(!module->modify_roi_out) module->modify_roi_out = _iop_modify_roi_out;

  #ifdef HAVE_OPENCL
  if(!module->process_tiling_cl) module->process_tiling_cl = darktable.opencl->inited ? default_process_tiling_cl : NULL;
  if(!darktable.opencl->inited) module->process_cl = NULL;
  #endif // HAVE_OPENCL

  module->process_plain = module->process;
  module->process = default_process;

  module->data = NULL;

  // the introspection api
  module->have_introspection = FALSE;
  if(module->introspection_init)
  {
    if(!module->introspection_init(module, DT_INTROSPECTION_VERSION))
    {
      // set the introspection related fields in module
      module->have_introspection = TRUE;

      if(module->get_p == default_get_p ||
         module->get_f == default_get_f ||
         module->get_introspection_linear == default_get_introspection_linear ||
         module->get_introspection == default_get_introspection)
        goto api_h_error;
    }
    else
      fprintf(stderr, "[iop_load_module] failed to initialize introspection for operation `%s'\n", module_name);
  }

  if(module->init_global) module->init_global(module);
  return 0;
}

int dt_iop_load_module_by_so(dt_iop_module_t *module, dt_iop_module_so_t *so, dt_develop_t *dev)
{
  module->actions = DT_ACTION_TYPE_IOP_INSTANCE;
  module->dev = dev;
  module->widget = NULL;
  module->header = NULL;
  module->off = NULL;
  module->hide_enable_button = 0;
  module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  module->request_histogram = DT_REQUEST_ONLY_IN_GUI;
  module->histogram_stats.bins_count = 0;
  module->histogram_stats.pixels = 0;
  module->multi_priority = 0;
  module->iop_order = 0;
  for(int k = 0; k < 3; k++)
  {
    module->picked_color[k] = module->picked_output_color[k] = 0.0f;
    module->picked_color_min[k] = module->picked_output_color_min[k] = 666.0f;
    module->picked_color_max[k] = module->picked_output_color_max[k] = -666.0f;
  }
  module->histogram_cst = IOP_CS_NONE;
  module->histogram = NULL;
  module->histogram_max[0] = module->histogram_max[1] = module->histogram_max[2] = module->histogram_max[3]
      = 0;
  module->histogram_middle_grey = FALSE;
  module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  module->suppress_mask = 0;
  module->bypass_cache = FALSE;
  module->enabled = module->default_enabled = 0; // all modules disabled by default.
  g_strlcpy(module->op, so->op, 20);
  module->raster_mask.source.users = g_hash_table_new(NULL, NULL);
  module->raster_mask.source.masks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  module->raster_mask.sink.source = NULL;
  module->raster_mask.sink.id = 0;

  // only reference cached results of dlopen:
  module->module = so->module;
  module->so = so;

#define INCLUDE_API_FROM_MODULE_LOAD_BY_SO
#include "iop/iop_api.h"

  module->version = so->version;
  module->process_plain = so->process_plain;
  module->have_introspection = so->have_introspection;

  module->reset_button = NULL;
  module->presets_button = NULL;
  module->fusion_slider = NULL;

  module->global_data = so->data;

  // now init the instance:
  module->init(module);
  module->hash = 0;

  /* initialize blendop params and default values */
  module->blend_params = calloc(1, sizeof(dt_develop_blend_params_t));
  module->default_blendop_params = calloc(1, sizeof(dt_develop_blend_params_t));
  dt_develop_blend_colorspace_t cst = dt_develop_blend_default_module_blend_colorspace(module);
  dt_develop_blend_init_blend_parameters(module->default_blendop_params, cst);
  dt_iop_commit_blend_params(module, module->default_blendop_params);

  if(module->params_size == 0)
  {
    fprintf(stderr, "[iop_load_module] `%s' needs to have a params size > 0!\n", so->op);
    return 1; // empty params hurt us in many places, just add a dummy value
  }
  module->enabled = module->default_enabled; // apply (possibly new) default.
  return 0;
}

void dt_iop_init_pipe(struct dt_iop_module_t *module, struct dt_dev_pixelpipe_t *pipe,
                      struct dt_dev_pixelpipe_iop_t *piece)
{
  module->init_pipe(module, pipe, piece);
  piece->blendop_data = calloc(1, sizeof(dt_develop_blend_params_t));
}

static gboolean _header_enter_notify_callback(GtkWidget *eventbox, GdkEventCrossing *event, gpointer user_data)
{
  darktable.control->element = GPOINTER_TO_INT(user_data);
  return FALSE;
}

static gboolean _header_motion_notify_show_callback(GtkWidget *eventbox, GdkEventCrossing *event, dt_iop_module_t *module)
{
  darktable.control->element = DT_ACTION_ELEMENT_SHOW;
  return dt_iop_show_hide_header_buttons(module, event, TRUE, FALSE);
}

static gboolean _header_motion_notify_hide_callback(GtkWidget *eventbox, GdkEventCrossing *event, dt_iop_module_t *module)
{
  return dt_iop_show_hide_header_buttons(module, event, FALSE, FALSE);
}

static gboolean _header_menu_deactivate_callback(GtkMenuShell *menushell, dt_iop_module_t *module)
{
  return dt_iop_show_hide_header_buttons(module, NULL, FALSE, FALSE);
}

static void _gui_delete_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_develop_t *dev = module->dev;

  // we search another module with the same base
  // we want the next module if any or the previous one
  GList *modules = module->dev->iop;
  dt_iop_module_t *next = NULL;
  int find = 0;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
    {
      find = 1;
      if(next) break;
    }
    else if(mod->instance == module->instance)
    {
      next = mod;
      if(find) break;
    }
    modules = g_list_next(modules);
  }
  if(!next) return; // what happened ???

  if(dev->gui_attached)
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE,
                            dt_history_duplicate(darktable.develop->history), dt_dev_get_history_end(darktable.develop),
                            dt_ioppr_iop_order_copy_deep(darktable.develop->iop_order_list));

  // we must pay attention if priority is 0
  const gboolean is_zero = (module->multi_priority == 0);

  // we set the focus to the other instance
  dt_iop_gui_set_expanded(next, TRUE, FALSE);
  dt_iop_request_focus(next);

  ++darktable.gui->reset;

  // we remove the plugin effectively
  if(!dt_iop_is_hidden(module))
  {
    // we just hide the module to avoid lots of gtk critical warnings
    gtk_widget_hide(module->expander);

    // we move the module far away, to avoid problems when reordering instance after that
    // FIXME: ?????
    gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                          module->expander, -1);

    dt_iop_gui_cleanup_module(module);
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
    gtk_widget_destroy(module->widget);
  }

  // we remove all references in the history stack and dev->iop
  // this will inform that a module has been removed from history
  // we do it here so we have the multi_priorities to reconstruct
  // de deleted module if the user undo it
  dt_dev_module_remove(dev, module);

  // if module was priority 0, then we set next to priority 0
  if(is_zero)
  {
    // we want the first one in history
    dt_iop_module_t *first = NULL;
    GList *history = dev->history;
    while(history)
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
      if(hist->module->instance == module->instance && hist->module != module)
      {
        first = hist->module;
        break;
      }
      history = g_list_next(history);
    }
    if(first == NULL) first = next;

    // we set priority of first to 0
    dt_iop_update_multi_priority(first, 0);

    // we change this in the history stack too
    for(history = dev->history; history; history = g_list_next(history))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
      if(hist->module == first) hist->multi_priority = 0;
    }
  }

  // we save the current state of history (with the new multi_priorities)
  if(dev->gui_attached)
  {
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  }

  // rebuild the accelerators (to point to an extant module)
  dt_iop_connect_accels_multi(module->so);

  dt_action_cleanup_instance_iop(module);

  // don't delete the module, a pipe may still need it
  dev->alliop = g_list_append(dev->alliop, module);

  // we update show params for multi-instances for each other instances
  dt_dev_modules_update_multishow(dev);

  /* redraw */
  dt_dev_pixelpipe_rebuild(dev);
  dt_control_queue_redraw_center();
  dt_dev_refresh_ui_images(dev);

  --darktable.gui->reset;
}

gboolean dt_iop_gui_module_is_visible(dt_iop_module_t *module)
{
  GtkWidget *expander = module->expander;
  return (expander && gtk_widget_is_visible(expander) && !dt_iop_is_hidden(module));
}

dt_iop_module_t *dt_iop_gui_get_previous_visible_module(dt_iop_module_t *module)
{
  dt_iop_module_t *prev = NULL;
  for(GList *modules = g_list_first(module->dev->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
      break;
    else if(dt_iop_gui_module_is_visible(mod))
      prev = mod;
  }
  return prev;
}

dt_iop_module_t *dt_iop_gui_get_next_visible_module(dt_iop_module_t *module)
{
  dt_iop_module_t *next = NULL;
  for(GList *modules = g_list_last(module->dev->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
      break;
    else if(dt_iop_gui_module_is_visible(mod))
      next = mod;
  }
  return next;
}

static void _gui_movedown_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_ioppr_check_iop_order(module->dev, 0, "dt_iop_gui_movedown_callback begin");

  // we need to place this module right before the previous
  dt_iop_module_t *prev = dt_iop_gui_get_previous_visible_module(module);
  // dt_ioppr_check_iop_order(module->dev, "dt_iop_gui_movedown_callback 1");
  if(!prev) return;

  const int moved = dt_ioppr_move_iop_before(module->dev, module, prev);
  // dt_ioppr_check_iop_order(module->dev, "dt_iop_gui_movedown_callback 2");
  if(!moved) return;

  // we move the headers
  GValue gv = { 0, { { 0 } } };
  g_value_init(&gv, G_TYPE_INT);
  gtk_container_child_get_property(
      GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)), prev->expander,
      "position", &gv);
  gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                        module->expander, g_value_get_int(&gv));

  // we update the headers
  dt_dev_modules_update_multishow(prev->dev);

  dt_dev_add_history_item(prev->dev, module, TRUE);

  dt_ioppr_check_iop_order(module->dev, 0, "dt_iop_gui_movedown_callback end");

  // rebuild the accelerators
  dt_iop_connect_accels_multi(module->so);

  dt_dev_pixelpipe_rebuild(module->dev);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_MOVED);
}

static void _gui_moveup_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_ioppr_check_iop_order(module->dev, 0, "dt_iop_gui_moveup_callback begin");

  // we need to place this module right after the next one
  dt_iop_module_t *next = dt_iop_gui_get_next_visible_module(module);
  if(!next) return;

  const int moved = dt_ioppr_move_iop_after(module->dev, module, next);
  if(!moved) return;

  // we move the headers
  GValue gv = { 0, { { 0 } } };
  g_value_init(&gv, G_TYPE_INT);
  gtk_container_child_get_property(
      GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)), next->expander,
      "position", &gv);

  gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                        module->expander, g_value_get_int(&gv));

  // we update the headers
  dt_dev_modules_update_multishow(next->dev);

  dt_dev_add_history_item(next->dev, module, TRUE);

  dt_ioppr_check_iop_order(module->dev, 0, "dt_iop_gui_moveup_callback end");

  // rebuild the accelerators
  dt_iop_connect_accels_multi(module->so);

  dt_dev_pixelpipe_rebuild(next->dev);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_MOVED);
}

dt_iop_module_t *dt_iop_gui_duplicate(dt_iop_module_t *base, gboolean copy_params)
{
  // make sure the duplicated module appears in the history
  dt_dev_add_history_item(base->dev, base, FALSE);

  // first we create the new module
  ++darktable.gui->reset;
  dt_iop_module_t *module = dt_dev_module_duplicate(base->dev, base);
  --darktable.gui->reset;
  if(!module) return NULL;

  // what is the position of the module in the pipe ?
  GList *modules = module->dev->iop;
  int pos_module = 0;
  int pos_base = 0;
  int pos = 0;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
      pos_module = pos;
    else if(mod == base)
      pos_base = pos;
    modules = g_list_next(modules);
    pos++;
  }

  // we set the gui part of it
  /* initialize gui if iop have one defined */
  if(!dt_iop_is_hidden(module))
  {
    // make sure gui_init and reload defaults is called safely
    dt_iop_gui_init(module);

    /* add module to right panel */
    dt_iop_gui_set_expander(module);
    GValue gv = { 0, { { 0 } } };
    g_value_init(&gv, G_TYPE_INT);
    gtk_container_child_get_property(
        GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),
        base->expander, "position", &gv);
    gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                          module->expander, g_value_get_int(&gv) + pos_base - pos_module + 1);
    dt_iop_gui_set_expanded(module, TRUE, FALSE);

    darktable.gui->scroll_to[1] = module->expander;

    dt_iop_reload_defaults(module); // some modules like profiled denoise update the gui in reload_defaults

    if(copy_params)
    {
      memcpy(module->params, base->params, module->params_size);
      if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
      {
        dt_iop_commit_blend_params(module, base->blend_params);
        if(base->blend_params->mask_id > 0)
        {
          module->blend_params->mask_id = 0;
          dt_masks_iop_use_same_as(module, base);
        }
      }
    }

    dt_iop_gui_update_blending(module);

    // we save the new instance creation
    dt_dev_add_history_item(module->dev, module, TRUE);
  }

  // we update show params for multi-instances for each other instances
  dt_dev_modules_update_multishow(module->dev);

  // and we refresh the pipe
  dt_iop_request_focus(module);

  if(module->dev->gui_attached)
  {
    dt_dev_pixelpipe_rebuild(module->dev);
  }

  /* update ui to new parameters */
  dt_iop_gui_update(module);

  dt_dev_modulegroups_update_visibility(darktable.develop);

  return module;
}

void dt_iop_gui_rename_module(dt_iop_module_t *module);

static void _gui_copy_callback(GtkButton *button, gpointer user_data)
{
  dt_iop_module_t *module = dt_iop_gui_duplicate(user_data, FALSE);

  /* setup key accelerators */
  dt_iop_connect_accels_multi(((dt_iop_module_t *)user_data)->so);

  if(dt_conf_get_bool("darkroom/ui/rename_new_instance"))
    dt_iop_gui_rename_module(module);
}

static void _gui_duplicate_callback(GtkButton *button, gpointer user_data)
{
  dt_iop_module_t *module = dt_iop_gui_duplicate(user_data, TRUE);

  /* setup key accelerators */
  dt_iop_connect_accels_multi(((dt_iop_module_t *)user_data)->so);

  if(dt_conf_get_bool("darkroom/ui/rename_new_instance"))
    dt_iop_gui_rename_module(module);
}

static gboolean _rename_module_key_press(GtkWidget *entry, GdkEventKey *event, dt_iop_module_t *module)
{
  int ended = 0;

  if(event->type == GDK_FOCUS_CHANGE || event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
  {
    if(gtk_entry_get_text_length(GTK_ENTRY(entry)) > 0)
    {
      // name is not empty, set new multi_name

       const gchar *name = gtk_entry_get_text(GTK_ENTRY(entry));

      // restore saved 1st character of instance name (without it the same name wouls still produce unnecessary copy + add history item)
      module->multi_name[0] = module->multi_name[sizeof(module->multi_name) - 1];
      module->multi_name[sizeof(module->multi_name) - 1] = 0;

      if(g_strcmp0(module->multi_name, name) != 0)
      {
        g_strlcpy(module->multi_name, name, sizeof(module->multi_name));
        dt_dev_add_history_item(module->dev, module, TRUE);
      }
    }
    else
    {
      // clear out multi-name (set 1st char to 0)
      module->multi_name[0] = 0;
      dt_dev_add_history_item(module->dev, module, TRUE);
    }

    ended = 1;
  }
  else if(event->keyval == GDK_KEY_Escape)
  {
    // restore saved 1st character of instance name
    module->multi_name[0] = module->multi_name[sizeof(module->multi_name) - 1];
    module->multi_name[sizeof(module->multi_name) - 1] = 0;

    ended = 1;
  }

  if(ended)
  {
    g_signal_handlers_disconnect_by_func(entry, G_CALLBACK(_rename_module_key_press), module);
    gtk_widget_destroy(entry);
    dt_iop_show_hide_header_buttons(module, NULL, TRUE, FALSE); // after removing entry
    dt_iop_gui_update_header(module);
    dt_masks_group_update_name(module);
    return TRUE;
  }

  return FALSE; /* event not handled */
}

static gboolean _rename_module_resize(GtkWidget *entry, GdkEventKey *event, dt_iop_module_t *module)
{
  int width = 0;
  GtkBorder padding;

  pango_layout_get_pixel_size(gtk_entry_get_layout(GTK_ENTRY(entry)), &width, NULL);
  gtk_style_context_get_padding(gtk_widget_get_style_context (entry),
                                gtk_widget_get_state_flags (entry),
                                &padding);
  gtk_widget_set_size_request(entry, width + padding.left + padding.right + 1, -1);

  return TRUE;
}

void dt_iop_gui_rename_module(dt_iop_module_t *module)
{
  GtkWidget *focused = gtk_container_get_focus_child(GTK_CONTAINER(module->header));
  if(focused && GTK_IS_ENTRY(focused)) return;

  GtkWidget *entry = gtk_entry_new();

  gtk_widget_set_name(entry, "iop-panel-label");
  gtk_entry_set_width_chars(GTK_ENTRY(entry), 0);
  gtk_entry_set_max_length(GTK_ENTRY(entry), sizeof(module->multi_name) - 1);
  gtk_entry_set_text(GTK_ENTRY(entry), module->multi_name);

  // remove instance name but save 1st character in case of escape
  module->multi_name[sizeof(module->multi_name) - 1] = module->multi_name[0];
  module->multi_name[0] = 0;
  dt_iop_gui_update_header(module);

  gtk_widget_add_events(entry, GDK_FOCUS_CHANGE_MASK);
  g_signal_connect(entry, "key-press-event", G_CALLBACK(_rename_module_key_press), module);
  g_signal_connect(entry, "focus-out-event", G_CALLBACK(_rename_module_key_press), module);
  g_signal_connect(entry, "style-updated", G_CALLBACK(_rename_module_resize), module);
  g_signal_connect(entry, "changed", G_CALLBACK(_rename_module_resize), module);
  g_signal_connect(entry, "enter-notify-event", G_CALLBACK(_header_enter_notify_callback),
                   GINT_TO_POINTER(DT_ACTION_ELEMENT_SHOW));

  dt_iop_show_hide_header_buttons(module, NULL, FALSE, TRUE); // before adding entry
  gtk_box_pack_start(GTK_BOX(module->header), entry, TRUE, TRUE, 0);
  gtk_widget_show(entry);
  gtk_widget_grab_focus(entry);
}

static void _gui_rename_callback(GtkButton *button, dt_iop_module_t *module)
{
  dt_iop_gui_rename_module(module);
}

static gboolean _gui_multiinstance_callback(GtkButton *button, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  if(event && event->button == 3)
  {
    if(!(module->flags() & IOP_FLAGS_ONE_INSTANCE)) _gui_copy_callback(button, user_data);
    return TRUE;
  }
  else if(event && event->button == 2)
  {
    return FALSE;
  }

  GtkMenuShell *menu = GTK_MENU_SHELL(gtk_menu_new());
  GtkWidget *item;

  item = gtk_menu_item_new_with_label(_("new instance"));
  // gtk_widget_set_tooltip_text(item, _("add a new instance of this module to the pipe"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_gui_copy_callback), module);
  gtk_widget_set_sensitive(item, module->multi_show_new);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("duplicate instance"));
  // gtk_widget_set_tooltip_text(item, _("add a copy of this instance to the pipe"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_gui_duplicate_callback), module);
  gtk_widget_set_sensitive(item, module->multi_show_new);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("move up"));
  // gtk_widget_set_tooltip_text(item, _("move this instance up"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_gui_moveup_callback), module);
  gtk_widget_set_sensitive(item, module->multi_show_up);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("move down"));
  // gtk_widget_set_tooltip_text(item, _("move this instance down"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_gui_movedown_callback), module);
  gtk_widget_set_sensitive(item, module->multi_show_down);
  gtk_menu_shell_append(menu, item);

  item = gtk_menu_item_new_with_label(_("delete"));
  // gtk_widget_set_tooltip_text(item, _("delete this instance"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_gui_delete_callback), module);
  gtk_widget_set_sensitive(item, module->multi_show_close);
  gtk_menu_shell_append(menu, item);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  item = gtk_menu_item_new_with_label(_("rename"));
  g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_gui_rename_callback), module);
  gtk_menu_shell_append(menu, item);

  g_signal_connect(G_OBJECT(menu), "deactivate", G_CALLBACK(_header_menu_deactivate_callback), module);

  dt_gui_menu_popup(GTK_MENU(menu), GTK_WIDGET(button), GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);

  // make sure the button is deactivated now that the menu is opened
  if(button) dtgtk_button_set_active(DTGTK_BUTTON(button), FALSE);
  return TRUE;
}

static gboolean _gui_off_button_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  if(!darktable.gui->reset && dt_modifier_is(e->state, GDK_CONTROL_MASK))
  {
    dt_iop_request_focus(darktable.develop->gui_module == module ? NULL : module);
    return TRUE;
  }
  return FALSE;
}

static void _gui_off_callback(GtkToggleButton *togglebutton, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  if(!darktable.gui->reset)
  {
    if(gtk_toggle_button_get_active(togglebutton))
    {
      module->enabled = 1;

      darktable.gui->scroll_to[1] = module->expander;

      dt_dev_add_history_item(module->dev, module, FALSE);
    }
    else
    {
      module->enabled = 0;

      //  if current module is set as the CAT instance, remove that setting
      if(module->dev->proxy.chroma_adaptation == module)
        module->dev->proxy.chroma_adaptation = NULL;

      dt_dev_add_history_item(module->dev, module, FALSE);
    }

    const gboolean raster = module->blend_params->mask_mode & DEVELOP_MASK_RASTER;
    // set mask indicator sensitive according to module activation and raster mask
    if(module->mask_indicator)
      gtk_widget_set_sensitive(GTK_WIDGET(module->mask_indicator), !raster && module->enabled);
  }

  char tooltip[512];
  gchar *module_label = dt_history_item_get_name(module);
  snprintf(tooltip, sizeof(tooltip), module->enabled ? _("%s is switched on") : _("%s is switched off"),
           module_label);
  g_free(module_label);
  gtk_widget_set_tooltip_text(GTK_WIDGET(togglebutton), tooltip);
  gtk_widget_queue_draw(GTK_WIDGET(togglebutton));

  // rebuild the accelerators
  dt_iop_connect_accels_multi(module->so);

  if(module->enabled && !gtk_widget_is_visible(module->header))
    dt_dev_modulegroups_update_visibility(darktable.develop);
}

gboolean dt_iop_so_is_hidden(dt_iop_module_so_t *module)
{
  gboolean is_hidden = TRUE;
  if(!(module->flags() & IOP_FLAGS_HIDDEN))
  {
    if(!module->gui_init)
      g_debug("Module '%s' is not hidden and lacks implementation of gui_init()...", module->op);
    else if(!module->gui_cleanup)
      g_debug("Module '%s' is not hidden and lacks implementation of gui_cleanup()...", module->op);
    else
      is_hidden = FALSE;
  }
  return is_hidden;
}

gboolean dt_iop_is_hidden(dt_iop_module_t *module)
{
  return dt_iop_so_is_hidden(module->so);
}

static void _iop_panel_label(dt_iop_module_t *module)
{
  GtkWidget *lab = dt_gui_container_nth_child(GTK_CONTAINER(module->header), IOP_MODULE_LABEL);
  lab = gtk_bin_get_child(GTK_BIN(lab));
  gtk_widget_set_name(lab, "iop-panel-label");
  char *module_name = dt_history_item_get_name_html(module);

  dt_capitalize_label(module_name);
  gtk_label_set_markup(GTK_LABEL(lab), module_name);
  g_free(module_name);

  gtk_label_set_ellipsize(GTK_LABEL(lab), !module->multi_name[0] ? PANGO_ELLIPSIZE_END: PANGO_ELLIPSIZE_MIDDLE);
  g_object_set(G_OBJECT(lab), "xalign", 0.0, (gchar *)0);
}

void dt_iop_gui_update_header(dt_iop_module_t *module)
{
  if (!module->header)                  /* some modules such as overexposed don't actually have a header */
    return;

  // set panel name to display correct multi-instance
  _iop_panel_label(module);
  dt_iop_gui_set_enable_button(module);
}

void dt_iop_gui_set_enable_button_icon(GtkWidget *w, dt_iop_module_t *module)
{
  // set on/off icon
  if(module->default_enabled && module->hide_enable_button)
  {
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(w), dtgtk_cairo_paint_switch_on, 0, module);
  }
  else if(!module->default_enabled && module->hide_enable_button)
  {
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(w), dtgtk_cairo_paint_switch_off, 0, module);
  }
  else
  {
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(w), dtgtk_cairo_paint_switch, 0, module);
  }
}

void dt_iop_gui_set_enable_button(dt_iop_module_t *module)
{
  if(module->off)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), module->enabled);
    if(module->hide_enable_button)
      gtk_widget_set_sensitive(GTK_WIDGET(module->off), FALSE);
    else
      gtk_widget_set_sensitive(GTK_WIDGET(module->off), TRUE);

    dt_iop_gui_set_enable_button_icon(GTK_WIDGET(module->off), module);
  }
}

void dt_iop_gui_init(dt_iop_module_t *module)
{
  ++darktable.gui->reset;
  --darktable.bauhaus->skip_accel;
  module->focused = NULL;
  if(module->gui_init) module->gui_init(module);
  ++darktable.bauhaus->skip_accel;
  --darktable.gui->reset;
}

void dt_iop_reload_defaults(dt_iop_module_t *module)
{
  if(darktable.gui) ++darktable.gui->reset;
  if(module->reload_defaults)
  {
    // report if reload_defaults was called unnecessarily => this should be considered a bug
    // the whole point of reload_defaults is to update defaults _based on current image_
    // any required initialisation should go in init (and not be performed repeatedly here)
    if(module->dev)
    {
      module->reload_defaults(module);
      dt_print(DT_DEBUG_PARAMS, "[params] defaults reloaded for %s\n", module->op);
    }
    else
    {
      fprintf(stderr, "reload_defaults should not be called without image.\n");
    }
  }
  dt_iop_load_default_params(module);

  if(darktable.gui) --darktable.gui->reset;

  if(module->header) dt_iop_gui_update_header(module);
}

void dt_iop_cleanup_histogram(gpointer data, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)data;

  free(module->histogram);
  module->histogram = NULL;
  module->histogram_stats.bins_count = 0;
  module->histogram_stats.pixels = 0;
}

static void _init_presets(dt_iop_module_so_t *module_so)
{
  if(module_so->init_presets) module_so->init_presets(module_so);

  // this seems like a reasonable place to check for and update legacy
  // presets.

  const int32_t module_version = module_so->version();

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT name, op_version, op_params, blendop_version, blendop_params FROM data.presets WHERE operation = ?1",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module_so->op, -1, SQLITE_TRANSIENT);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (char *)sqlite3_column_text(stmt, 0);
    int32_t old_params_version = sqlite3_column_int(stmt, 1);
    const void *old_params = (void *)sqlite3_column_blob(stmt, 2);
    const int32_t old_params_size = sqlite3_column_bytes(stmt, 2);
    const int32_t old_blend_params_version = sqlite3_column_int(stmt, 3);
    const void *old_blend_params = (void *)sqlite3_column_blob(stmt, 4);
    const int32_t old_blend_params_size = sqlite3_column_bytes(stmt, 4);

    if(old_params_version == 0)
    {
      // this preset doesn't have a version.  go digging through the database
      // to find a history entry that matches the preset params, and get
      // the module version from that.

      sqlite3_stmt *stmt2;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT module FROM main.history WHERE operation = ?1 AND op_params = ?2", -1,
                                  &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 1, module_so->op, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt2, 2, old_params, old_params_size, SQLITE_TRANSIENT);

      if(sqlite3_step(stmt2) == SQLITE_ROW)
      {
        old_params_version = sqlite3_column_int(stmt2, 0);
      }
      else
      {
        fprintf(stderr, "[imageop_init_presets] WARNING: Could not find versioning information for '%s' "
                        "preset '%s'\nUntil some is found, the preset will be unavailable.\n(To make it "
                        "return, please load an image that uses the preset.)\n",
                module_so->op, name);
        sqlite3_finalize(stmt2);
        continue;
      }

      sqlite3_finalize(stmt2);

      // we found an old params version.  Update the database with it.

      fprintf(stderr, "[imageop_init_presets] Found version %d for '%s' preset '%s'\n", old_params_version,
              module_so->op, name);

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE data.presets SET op_version=?1 WHERE operation=?2 AND name=?3", -1,
                                  &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, old_params_version);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 2, module_so->op, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 3, name, -1, SQLITE_TRANSIENT);

      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);
    }

    if(module_version > old_params_version && module_so->legacy_params != NULL)
    {
      // we need a dt_iop_module_t for legacy_params()
      dt_iop_module_t *module;
      module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module_by_so(module, module_so, NULL))
      {
        free(module);
        continue;
      }
/*
      module->init(module);
      if(module->params_size == 0)
      {
        dt_iop_cleanup_module(module);
        free(module);
        continue;
      }
      // we call reload_defaults() in case the module defines it
      if(module->reload_defaults) module->reload_defaults(module); // why not call dt_iop_reload_defaults? (if needed at all)
*/

      const int32_t new_params_size = module->params_size;
      void *new_params = calloc(1, new_params_size);

      // convert the old params to new
      if(module->legacy_params(module, old_params, old_params_version, new_params, module_version))
      {
        free(new_params);
        dt_iop_cleanup_module(module);
        free(module);
        continue;
      }

      fprintf(stderr, "[imageop_init_presets] updating '%s' preset '%s' from version %d to version %d\nto:'%s'",
              module_so->op, name, old_params_version, module_version,
              dt_exif_xmp_encode(new_params, new_params_size, NULL));

      // and write the new params back to the database
      sqlite3_stmt *stmt2;
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "UPDATE data.presets "
                                                                 "SET op_version=?1, op_params=?2 "
                                                                 "WHERE operation=?3 AND name=?4",
                                  -1, &stmt2, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, module->version());
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt2, 2, new_params, new_params_size, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 3, module->op, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 4, name, -1, SQLITE_TRANSIENT);

      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);

      free(new_params);
      dt_iop_cleanup_module(module);
      free(module);
    }
    else if(module_version > old_params_version)
    {
      fprintf(stderr, "[imageop_init_presets] Can't upgrade '%s' preset '%s' from version %d to %d, no "
                      "legacy_params() implemented \n",
              module_so->op, name, old_params_version, module_version);
    }

    if(!old_blend_params || dt_develop_blend_version() > old_blend_params_version)
    {
      fprintf(stderr,
              "[imageop_init_presets] updating '%s' preset '%s' from blendop version %d to version %d\n",
              module_so->op, name, old_blend_params_version, dt_develop_blend_version());

      // we need a dt_iop_module_t for dt_develop_blend_legacy_params()
      // using dt_develop_blend_legacy_params_by_so won't help as we need "module" anyway
      dt_iop_module_t *module;
      module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module_by_so(module, module_so, NULL))
      {
        free(module);
        continue;
      }

      if(module->params_size == 0)
      {
        dt_iop_cleanup_module(module);
        free(module);
        continue;
      }
      void *new_blend_params = malloc(sizeof(dt_develop_blend_params_t));

      // convert the old blend params to new
      if(old_blend_params
         && dt_develop_blend_legacy_params(module, old_blend_params, old_blend_params_version,
                                           new_blend_params, dt_develop_blend_version(),
                                           old_blend_params_size) == 0)
      {
        // do nothing
      }
      else
      {
        memcpy(new_blend_params, module->default_blendop_params, sizeof(dt_develop_blend_params_t));
      }

      // and write the new blend params back to the database
      sqlite3_stmt *stmt2;
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "UPDATE data.presets "
                                                                 "SET blendop_version=?1, blendop_params=?2 "
                                                                 "WHERE operation=?3 AND name=?4",
                                  -1, &stmt2, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, dt_develop_blend_version());
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt2, 2, new_blend_params, sizeof(dt_develop_blend_params_t),
                                 SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 3, module->op, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 4, name, -1, SQLITE_TRANSIENT);

      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);

      free(new_blend_params);
      dt_iop_cleanup_module(module);
      free(module);
    }
  }
  sqlite3_finalize(stmt);
}

static void _init_presets_actions(dt_iop_module_so_t *module)
{
  /** load shortcuts for presets **/
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name FROM data.presets WHERE operation=?1 ORDER BY writeprotect DESC, rowid",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, module->op, -1, SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_action_define_preset(&module->actions, (const char *)sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);
}

static void _init_module_so(void *m)
{
  dt_iop_module_so_t *module = (dt_iop_module_so_t *)m;

  _init_presets(module);

  // do not init accelerators if there is no gui
  if(darktable.gui)
  {
    module->actions = (dt_action_t){ DT_ACTION_TYPE_IOP, module->op, module->name(),
                                     .owner = &darktable.control->actions_iops };
    dt_action_insert_sorted(&darktable.control->actions_iops, &module->actions);

    // Calling the accelerator initialization callback, if present
    _init_presets_actions(module);

    // create a gui and have the widgets register their accelerators
    dt_iop_module_t *module_instance = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));

    if(module->gui_init && !dt_iop_load_module_by_so(module_instance, module, NULL))
    {
      darktable.control->accel_initialising = TRUE;
      dt_iop_gui_init(module_instance);

      static gboolean blending_accels_initialized = FALSE;
      if(!blending_accels_initialized)
      {
        dt_iop_colorspace_type_t cst = module->blend_colorspace(module_instance, NULL, NULL);

        if((module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) &&
           !(module->flags() & IOP_FLAGS_NO_MASKS) &&
           (cst == IOP_CS_LAB || cst == IOP_CS_RGB))
        {
          GtkWidget *iopw = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
          dt_iop_gui_init_blending(iopw, module_instance);
          dt_iop_gui_cleanup_blending(module_instance);
          gtk_widget_destroy(iopw);

          blending_accels_initialized = TRUE;
        }
      }

      module->gui_cleanup(module_instance);
      darktable.control->accel_initialising = FALSE;

      dt_iop_cleanup_module(module_instance);
    }

    free(module_instance);
  }
}

void dt_iop_load_modules_so(void)
{
  darktable.iop = dt_module_load_modules("/plugins", sizeof(dt_iop_module_so_t), dt_iop_load_module_so,
                                         _init_module_so, NULL);
}

int dt_iop_load_module(dt_iop_module_t *module, dt_iop_module_so_t *module_so, dt_develop_t *dev)
{
  memset(module, 0, sizeof(dt_iop_module_t));
  if(dt_iop_load_module_by_so(module, module_so, dev))
  {
    free(module);
    return 1;
  }
  return 0;
}

GList *dt_iop_load_modules_ext(dt_develop_t *dev, gboolean no_image)
{
  GList *res = NULL;
  dt_iop_module_t *module;
  dt_iop_module_so_t *module_so;
  dev->iop_instance = 0;
  GList *iop = darktable.iop;
  while(iop)
  {
    module_so = (dt_iop_module_so_t *)iop->data;
    module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
    if(dt_iop_load_module_by_so(module, module_so, dev))
    {
      free(module);
      continue;
    }
    res = g_list_insert_sorted(res, module, dt_sort_iop_by_order);
    module->global_data = module_so->data;
    module->so = module_so;
    iop = g_list_next(iop);
  }

  GList *it = res;
  while(it)
  {
    module = (dt_iop_module_t *)it->data;
    module->instance = dev->iop_instance++;
    module->multi_name[0] = '\0';
    it = g_list_next(it);
  }
  return res;
}

GList *dt_iop_load_modules(dt_develop_t *dev)
{
  return dt_iop_load_modules_ext(dev, FALSE);
}

void dt_iop_cleanup_module(dt_iop_module_t *module)
{
  module->cleanup(module);

  free(module->blend_params);
  module->blend_params = NULL;
  free(module->default_blendop_params);
  module->default_blendop_params = NULL;

  // don't have a picker pointing to a disappeared module
  if(darktable.lib
     && darktable.lib->proxy.colorpicker.picker_proxy
     && darktable.lib->proxy.colorpicker.picker_proxy->module == module)
    darktable.lib->proxy.colorpicker.picker_proxy = NULL;

  free(module->histogram);
  module->histogram = NULL;
  g_hash_table_destroy(module->raster_mask.source.users);
  g_hash_table_destroy(module->raster_mask.source.masks);
  module->raster_mask.source.users = NULL;
  module->raster_mask.source.masks = NULL;
}

void dt_iop_unload_modules_so()
{
  while(darktable.iop)
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)darktable.iop->data;
    if(module->cleanup_global) module->cleanup_global(module);
    if(module->module) g_module_close(module->module);
    free(darktable.iop->data);
    darktable.iop = g_list_delete_link(darktable.iop, darktable.iop);
  }
}

void dt_iop_set_mask_mode(dt_iop_module_t *module, int mask_mode)
{
  static const int key = 0;
  // showing raster masks doesn't make sense, one can use the original source instead. or does it?
  if(mask_mode & DEVELOP_MASK_ENABLED && !(mask_mode & DEVELOP_MASK_RASTER))
  {
    char *modulename = dt_history_item_get_name(module);
    g_hash_table_insert(module->raster_mask.source.masks, GINT_TO_POINTER(key), modulename);
  }
  else
  {
    g_hash_table_remove(module->raster_mask.source.masks, GINT_TO_POINTER(key));
  }
}

// make sure that blend_params are in sync with the iop struct
void dt_iop_commit_blend_params(dt_iop_module_t *module, const dt_develop_blend_params_t *blendop_params)
{
  if(module->raster_mask.sink.source)
    g_hash_table_remove(module->raster_mask.sink.source->raster_mask.source.users, module);

  if(module->blend_params != blendop_params)
    memcpy(module->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));

  if(blendop_params->blend_cst == DEVELOP_BLEND_CS_NONE)
  {
    module->blend_params->blend_cst = dt_develop_blend_default_module_blend_colorspace(module);
  }
  dt_iop_set_mask_mode(module, blendop_params->mask_mode);

  if(module->dev)
  {
    for(GList *iter = module->dev->iop; iter; iter = g_list_next(iter))
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iter->data;
      if(!strcmp(m->op, blendop_params->raster_mask_source))
      {
        if(m->multi_priority == blendop_params->raster_mask_instance)
        {
          g_hash_table_insert(m->raster_mask.source.users, module, GINT_TO_POINTER(blendop_params->raster_mask_id));
          module->raster_mask.sink.source = m;
          module->raster_mask.sink.id = blendop_params->raster_mask_id;
          return;
        }
      }
    }
  }

  module->raster_mask.sink.source = NULL;
  module->raster_mask.sink.id = 0;
}

gboolean _iop_validate_params(dt_introspection_field_t *field, gpointer params, gboolean report)
{
  dt_iop_params_t *p = (dt_iop_params_t *)((uint8_t *)params + field->header.offset);

  gboolean all_ok = TRUE;

  switch(field->header.type)
  {
  case DT_INTROSPECTION_TYPE_STRUCT:
    for(int i = 0; i < field->Struct.entries; i++)
    {
      dt_introspection_field_t *entry = field->Struct.fields[i];

      all_ok &= _iop_validate_params(entry, params, report);
    }
    break;
  case DT_INTROSPECTION_TYPE_UNION:
    all_ok = FALSE;
    for(int i = field->Union.entries - 1; i >= 0 ; i--)
    {
      dt_introspection_field_t *entry = field->Union.fields[i];

      if(_iop_validate_params(entry, params, report && i == 0))
      {
        all_ok = TRUE;
        break;
      }
    }
    break;
  case DT_INTROSPECTION_TYPE_ARRAY:
    if(field->Array.type == DT_INTROSPECTION_TYPE_CHAR)
    {
      if(!memchr(p, '\0', field->Array.count))
      {
        if(report)
          fprintf(stderr, "validation check failed in _iop_validate_params for type \"%s\"; string not null terminated.\n",
                          field->header.type_name);
        all_ok = FALSE;
      }
    }
    else
    {
      for(int i = 0, item_offset = 0; i < field->Array.count; i++, item_offset += field->Array.field->header.size)
      {
        if(!_iop_validate_params(field->Array.field, (uint8_t *)params + item_offset, report))
        {
          if(report)
            fprintf(stderr, "validation check failed in _iop_validate_params for type \"%s\", for array element \"%d\"\n",
                            field->header.type_name, i);
          all_ok = FALSE;
          break;
        }
      }
    }
    break;
  case DT_INTROSPECTION_TYPE_FLOAT:
    all_ok = isnan(*(float*)p) || ((*(float*)p >= field->Float.Min && *(float*)p <= field->Float.Max));
    break;
  case DT_INTROSPECTION_TYPE_INT:
    all_ok = (*(int*)p >= field->Int.Min && *(int*)p <= field->Int.Max);
    break;
  case DT_INTROSPECTION_TYPE_UINT:
    all_ok = (*(unsigned int*)p >= field->UInt.Min && *(unsigned int*)p <= field->UInt.Max);
    break;
  case DT_INTROSPECTION_TYPE_USHORT:
    all_ok = (*(unsigned short int*)p >= field->UShort.Min && *(unsigned short int*)p <= field->UShort.Max);
    break;
  case DT_INTROSPECTION_TYPE_INT8:
    all_ok = (*(uint8_t*)p >= field->Int8.Min && *(uint8_t*)p <= field->Int8.Max);
    break;
  case DT_INTROSPECTION_TYPE_CHAR:
    all_ok = (*(char*)p >= field->Char.Min && *(char*)p <= field->Char.Max);
    break;
  case DT_INTROSPECTION_TYPE_FLOATCOMPLEX:
    all_ok = creal(*(float complex*)p) >= creal(field->FloatComplex.Min) &&
             creal(*(float complex*)p) <= creal(field->FloatComplex.Max) &&
             cimag(*(float complex*)p) >= cimag(field->FloatComplex.Min) &&
             cimag(*(float complex*)p) <= cimag(field->FloatComplex.Max);
    break;
  case DT_INTROSPECTION_TYPE_ENUM:
    all_ok = FALSE;
    for(dt_introspection_type_enum_tuple_t *i = field->Enum.values; i && i->name; i++)
    {
      if(i->value == *(int*)p)
      {
        all_ok = TRUE;
        break;
      }
    }
    break;
  case DT_INTROSPECTION_TYPE_BOOL:
    // *(gboolean*)p
    break;
  case DT_INTROSPECTION_TYPE_OPAQUE:
    // TODO: special case float2
    break;
  default:
    fprintf(stderr, "unsupported introspection type \"%s\" encountered in _iop_validate_params (field %s)\n",
                    field->header.type_name, field->header.name);
    all_ok = FALSE;
    break;
  }

  if(!all_ok && report)
    fprintf(stderr, "validation check failed in _iop_validate_params for type \"%s\"%s%s\n",
                    field->header.type_name, (*field->header.name ? ", field: " : ""), field->header.name);

  return all_ok;
}


gboolean dt_iop_check_modules_equal(dt_iop_module_t *mod_1, dt_iop_module_t *mod_2)
{
  // Use module fingerprints to determine if two instances are actually the same
  return mod_1 == mod_2
          && mod_1->instance == mod_2->instance
          && mod_1->multi_priority == mod_2->multi_priority
          && mod_1->iop_order == mod_2->iop_order;
}

uint64_t dt_iop_module_hash(dt_iop_module_t *module)
{
  // Uniform way of getting the full state hash of user-defined parameters,
  // including masks and blending.
  // WARNING: doesn't take into account parameters dynamically set at runtime.

  uint64_t hash = dt_hash(5381, (char *)module->params, module->params_size);
  hash = dt_hash(hash, (char *)&module->instance, sizeof(int32_t));
  hash = dt_hash(hash, (char *)&module->multi_priority, sizeof(int));
  hash = dt_hash(hash, (char *)&module->iop_order, sizeof(int));

  if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    if(module->dev)
    {
      dt_masks_form_t *grp = dt_masks_get_from_id(module->dev, module->blend_params->mask_id);
      hash = dt_masks_group_get_hash(hash, grp);
    }
    else
    {
      // This should not happen.
      fprintf(stdout, "[dt_iop_module_hash] WARNING: function is called on %s without inited develop.\n", module->op);
    }
  }

  // Blend params are always inited even when module doesn't support blending
  hash = dt_hash(hash, (char *)module->blend_params, sizeof(dt_develop_blend_params_t));

  return hash;
}

void dt_iop_commit_params(dt_iop_module_t *module, dt_iop_params_t *params,
                          dt_develop_blend_params_t *blendop_params, dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece)
{
  assert(piece->pipe == pipe);
  if(!piece->enabled)
  {
    piece->global_hash = piece->hash = 0;
    return;
  }

  // 1. commit params
  memcpy(piece->blendop_data, blendop_params, sizeof(dt_develop_blend_params_t));

#ifdef HAVE_OPENCL
  // assume process_cl is ready, commit_params can overwrite this.
  if(module->process_cl)
    piece->process_cl_ready = 1;
#endif // HAVE_OPENCL

  // register if module allows tiling, commit_params can overwrite this.
  if(module->flags() & IOP_FLAGS_ALLOW_TILING)
    piece->process_tiling_ready = 1;

  if(darktable.unmuted & DT_DEBUG_PARAMS && module->so->get_introspection())
    _iop_validate_params(module->so->get_introspection()->field, params, TRUE);

  module->commit_params(module, params, pipe, piece);

  // 2. compute the hash
  // piece->hash = dt_hash(module->hash, (const char *)piece->data, piece->data_size);
  // That's actually too aggressive and unneeded, fetch only user-params checksum here.
  // We need to take mask display into account too because it's set in various ways from GUI.
  piece->global_hash = piece->hash = dt_hash(module->hash, (const char *)&module->request_mask_display, sizeof(int));

  dt_print(DT_DEBUG_PIPE, "[pipe] commit for %s (%s) in pipe %i with hash %lu\n", module->op, module->multi_name, pipe->type, (long unsigned int)piece->hash);
}

void dt_iop_gui_cleanup_module(dt_iop_module_t *module)
{
  while(g_idle_remove_by_data(module->widget))
    ; // remove multiple delayed gtk_widget_queue_draw triggers
  g_slist_free_full(module->widget_list, g_free);
  module->widget_list = NULL;
  module->gui_cleanup(module);
  dt_iop_gui_cleanup_blending(module);
}

void dt_iop_gui_update(dt_iop_module_t *module)
{
  ++darktable.gui->reset;
  if(!dt_iop_is_hidden(module))
  {
    if(module->gui_data)
    {
      dt_bauhaus_update_module(module);

      if(module->params && module->gui_update)
        module->gui_update(module);

      // Shitty dependency graph ahead !!!
      // dt_bauhaus_update_module is in bauhaus.c
      // 1. it calls dt_bauhaus_slider_set() and dt_bauhaus_combobox_set()
      // 2. those call dt_iop_gui_changed() from here (imageop.c)
      // 3. dt_iop_gui_changed() calls module->gui_changed() if available
      //    BUT only if darktable.gui->reset is 0.
      // So we have to call module->gui_update() which may or may not call module->gui_changed(),
      // depending on modules, to init and reset sliders and comboboxes not linked to module params.
      // Because those widgets directly declared from params introspection are added to the module->bh_widget_list
      // (GSList *), which is updated in dt_bauhaus_update_module();
      // TODO: fix this FUCKING mess:
      // 1. the bauhaus lib should not change its behaviour based on the state of the global variable darktable.gui->reset,
      //    instead the global variable should be checked upstream before dispatching bauhaus events (like set,
      //    reset, update). Nesting dependencies across layers of libs to the state of a global variable is super unreliable and prone
      //    to race conditions.
      // 2. the bauhaus lib should be unaware of imageop.c lib (modules). If modules need to perform operations,
      //    they should connect callbacks to the `value-changed` signal. The bauhaus lib should be treated as an
      //    an extension of Gtk, implementation-agnostic.
      // 3. Ideally, all sliders and comboboxes should be updated through an unified method to prevent
      //    programmer errors in the future because, again, WE DON'T HAVE DEV DOC, so API need to be dummy-proof.

      dt_iop_gui_update_blending(module);
      dt_iop_gui_update_expanded(module);
    }
    dt_iop_gui_update_header(module);
    dt_iop_show_hide_header_buttons(module, NULL, FALSE, FALSE);
  }
  --darktable.gui->reset;
}

void dt_iop_gui_reset(dt_iop_module_t *module)
{
  ++darktable.gui->reset;
  if(module->gui_reset && !dt_iop_is_hidden(module)) module->gui_reset(module);
  --darktable.gui->reset;
}

static void _gui_reset_callback(GtkButton *button, GdkEventButton *event, dt_iop_module_t *module)
{
  // never use the callback if module is always disabled
  const gboolean disabled = !module->default_enabled && module->hide_enable_button;
  if(disabled) return;

  //Ctrl is used to apply any auto-presets to the current module
  //If Ctrl was not pressed, or no auto-presets were applied, reset the module parameters
  // FIXME: can we stop with all the easter-eggs key modifiers doing undocumented stuff all along ?
  if(!(event && dt_modifier_is(event->state, GDK_CONTROL_MASK)) || !dt_gui_presets_autoapply_for_module(module))
  {
    // if a drawn mask is set, remove it from the list
    if(module->blend_params->mask_id > 0)
    {
      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
      if(grp) dt_masks_form_remove(module, NULL, grp);
    }
    /* reset to default params */
    dt_iop_reload_defaults(module);
    dt_iop_commit_blend_params(module, module->default_blendop_params);

    /* reset ui to its defaults */
    dt_iop_gui_reset(module);

    /* update ui to default params*/
    dt_iop_gui_update(module);

    dt_dev_add_history_item(module->dev, module, TRUE);
  }

  // rebuild the accelerators
  dt_iop_connect_accels_multi(module->so);
}

static void _presets_popup_callback(GtkButton *button, dt_iop_module_t *module)
{
  const gboolean disabled = !module->default_enabled && module->hide_enable_button;
  if(disabled) return;

  dt_gui_presets_popup_menu_show_for_module(module);

  g_signal_connect(G_OBJECT(darktable.gui->presets_popup_menu), "deactivate", G_CALLBACK(_header_menu_deactivate_callback), module);

  dt_gui_menu_popup(darktable.gui->presets_popup_menu, GTK_WIDGET(button), GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);
}

void dt_iop_request_focus(dt_iop_module_t *module)
{
  dt_iop_module_t *out_focus_module = darktable.develop->gui_module;

  if(darktable.gui->reset || (out_focus_module == module)) return;

  darktable.develop->gui_module = module;

  /* lets lose the focus of previous focus module*/
  if(out_focus_module)
  {
    if(out_focus_module->gui_focus)
      out_focus_module->gui_focus(out_focus_module, FALSE);

    dt_iop_color_picker_reset(out_focus_module, TRUE);
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
    out_focus_module->focused = NULL;

    gtk_widget_set_state_flags(dt_iop_gui_get_pluginui(out_focus_module), GTK_STATE_FLAG_NORMAL, TRUE);


    dt_iop_connect_accels_multi(out_focus_module->so);

    /* reset mask view */
    dt_masks_reset_form_gui();

    /* do stuff needed in the blending gui */
    dt_iop_gui_blending_lose_focus(out_focus_module);

    /* redraw the expander */
    gtk_widget_queue_draw(out_focus_module->expander);

    /* and finally collection restore hinter messages */
    dt_collection_hint_message(darktable.collection);

    // we also remove the focus css class
    GtkWidget *iop_w = gtk_widget_get_parent(dt_iop_gui_get_pluginui(out_focus_module));
    dt_gui_remove_class(iop_w, "dt_module_focus");

    // if the module change the image size, we update the final sizes
    if(out_focus_module->modify_roi_out) dt_image_update_final_size(darktable.develop->preview_pipe->output_imgid);
  }

  /* set the focus on module */
  if(module)
  {
    gtk_widget_set_state_flags(dt_iop_gui_get_pluginui(module), GTK_STATE_FLAG_SELECTED, TRUE);

    dt_iop_connect_accels_multi(module->so);

    if(module->gui_focus) module->gui_focus(module, TRUE);

    /* redraw the expander */
    gtk_widget_queue_draw(module->expander);
    gtk_widget_grab_focus(module->expander);
    module->focused = NULL;

    // we also add the focus css class
    // FIXME: focused CSS node have a :focus pseudo-class already, this is redundant
    GtkWidget *iop_w = gtk_widget_get_parent(dt_iop_gui_get_pluginui(darktable.develop->gui_module));
    dt_gui_add_class(iop_w, "dt_module_focus");
  }

  /* update sticky accels window */
  if(darktable.view_manager->accels_window.window && darktable.view_manager->accels_window.sticky)
    dt_view_accels_refresh(darktable.view_manager);

  dt_control_change_cursor(GDK_LEFT_PTR);
  dt_control_queue_redraw_center();
}

/*
 * NEW EXPANDER
 */

static void _gui_set_single_expanded(dt_iop_module_t *module, gboolean expanded)
{
  if(!module->expander) return;

  /* update expander arrow state */
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(module->expander), expanded);

  /* store expanded state of module.
   * we do that first, so update_expanded won't think it should be visible
   * and undo our changes right away. */
  module->expanded = expanded;

  /* show / hide plugin widget */
  if(expanded)
  {
    /* set this module to receive focus / draw events*/
    dt_iop_request_focus(module);

    /* focus the current module */
    for(int k = 0; k < DT_UI_CONTAINER_SIZE; k++)
      dt_ui_container_focus_widget(darktable.gui->ui, k, module->expander);

    /* redraw center, iop might have post expose */
    dt_control_queue_redraw_center();
  }
  else
  {
    if(module->dev->gui_module == module)
    {
      dt_iop_request_focus(NULL);
      dt_control_queue_redraw_center();
    }
  }

  char var[1024];
  snprintf(var, sizeof(var), "plugins/darkroom/%s/expanded", module->op);
  dt_conf_set_bool(var, expanded);
}

void dt_iop_gui_set_expanded(dt_iop_module_t *module, gboolean expanded, gboolean collapse_others)
{
  if(!module->expander) return;
  /* handle shiftclick on expander, hide all except this */
  if(collapse_others)
  {
    const int current_group = dt_dev_modulegroups_get(module->dev);

    GList *iop = module->dev->iop;
    gboolean all_other_closed = TRUE;
    while(iop)
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iop->data;
      if(m != module && (is_module_in_group(m, current_group) || is_module_group_global(current_group)))
      {
        all_other_closed = all_other_closed && !m->expanded;
        _gui_set_single_expanded(m, FALSE);
      }

      iop = g_list_next(iop);
    }
    if(all_other_closed)
      _gui_set_single_expanded(module, expanded);
    else
      _gui_set_single_expanded(module, TRUE);
  }
  else
  {
    /* else just toggle */
    _gui_set_single_expanded(module, expanded);
  }
}

void dt_iop_gui_update_expanded(dt_iop_module_t *module)
{
  if(!module->expander) return;

  const gboolean expanded = module->expanded;

  dtgtk_expander_set_expanded(DTGTK_EXPANDER(module->expander), expanded);
}

static gboolean _iop_plugin_body_button_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  /* Reset the scrolling focus. If the click happened on any bauhaus element,
   * its internal button_press method will set it for itself */
  darktable.gui->has_scroll_focus = NULL;

  gboolean handled = FALSE;

  if(e->button == 1)
  {
    dt_iop_request_focus(module);
    handled = TRUE;
  }
  else if(e->button == 3)
  {
    _presets_popup_callback(NULL, module);
    handled = TRUE;
  }
  return handled;
}

static gboolean _iop_plugin_header_button_press(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  if(e->type == GDK_2BUTTON_PRESS || e->type == GDK_3BUTTON_PRESS) return TRUE;

  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  /* Reset the scrolling focus. If the click happened on any bauhaus element,
   * its internal button_press method will set it for itself */
  darktable.gui->has_scroll_focus = NULL;

  if(e->button == 1)
  {
    if(dt_modifier_is(e->state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
    {
      GtkBox *container = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
      g_object_set_data(G_OBJECT(container), "source_data", user_data);
      return FALSE;
    }
    else if(dt_modifier_is(e->state, GDK_CONTROL_MASK))
    {
      dt_iop_gui_rename_module(module);
      return TRUE;
    }
    else
    {
      // make gtk scroll to the module once it updated its allocation size
      darktable.gui->scroll_to[1] = module->expander;

      dt_iop_gui_set_expanded(module, !module->expanded, dt_modifier_is(e->state, GDK_SHIFT_MASK));

      // rebuild the accelerators
      dt_iop_connect_accels_multi(module->so);

      //used to take focus away from module search text input box when module selected
      gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

      return TRUE;
    }
  }
  else if(e->button == 3)
  {
    _presets_popup_callback(NULL, module);

    return TRUE;
  }
  return FALSE;
}

static void _header_size_callback(GtkWidget *widget, GdkRectangle *allocation, GtkWidget *header)
{
  GList *children = gtk_container_get_children(GTK_CONTAINER(header));
  GList *button = children;
  GtkRequisition button_size;
  gtk_widget_show(GTK_WIDGET(button->data));
  gtk_widget_get_preferred_size(GTK_WIDGET(button->data), &button_size, NULL);

  gboolean hide_all = (allocation->width == 1);
  int num_to_unhide = (allocation->width - 2) / button_size.width;
  double opacity_leftmost = num_to_unhide > 0
    ? 1.0
    : (double) allocation->width / button_size.width;

  double opacity_others = 1.0;
  GList *prev_button = NULL;

  for(button = g_list_last(children);
      button && GTK_IS_BUTTON(button->data);
      button = g_list_previous(button))
  {
    GtkWidget *b = GTK_WIDGET(button->data);

    if(!gtk_widget_get_visible(b))
    {
      if(num_to_unhide == 0) break;
      --num_to_unhide;
    }

    gtk_widget_set_visible(b, !hide_all);
    gtk_widget_set_opacity(b, opacity_others);

    prev_button = button;
  }
  if(prev_button && num_to_unhide == 0)
    gtk_widget_set_opacity(GTK_WIDGET(prev_button->data), opacity_leftmost);

  g_list_free(children);

  GtkAllocation header_allocation;
  gtk_widget_get_allocation(header, &header_allocation);
  if(header_allocation.width > 1) gtk_widget_size_allocate(header, &header_allocation);
}

gboolean dt_iop_show_hide_header_buttons(dt_iop_module_t *module, GdkEventCrossing *event, gboolean show_buttons, gboolean always_hide)
{
  // check if Entry widget for module name edit exists
  GtkWidget *header = module->header;
  GtkWidget *focused = gtk_container_get_focus_child(GTK_CONTAINER(header));
  if(focused && GTK_IS_ENTRY(focused)) return TRUE;

  if(event && (darktable.develop->darkroom_skip_mouse_events ||
     event->detail == GDK_NOTIFY_INFERIOR ||
     event->mode != GDK_CROSSING_NORMAL)) return TRUE;

  const char *config = dt_conf_get_string_const("darkroom/ui/hide_header_buttons");

  gboolean dynamic = FALSE;
  double opacity = 1.0;
  if(!g_strcmp0(config, "always"))
  {
    show_buttons = TRUE;
  }
  else if(!g_strcmp0(config, "dim"))
  {
    if(!show_buttons) opacity = 0.3;
    show_buttons = TRUE;
  }
  else if(!g_strcmp0(config, "active"))
    ;
  else
    dynamic = TRUE;

  const gboolean disabled = !module->default_enabled && module->hide_enable_button;

  GList *children = gtk_container_get_children(GTK_CONTAINER(header));

  GList *button;
  for(button = g_list_last(children);
      button && GTK_IS_BUTTON(button->data);
      button = g_list_previous(button))
  {
    gtk_widget_set_no_show_all(GTK_WIDGET(button->data), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(button->data), show_buttons && !always_hide && !disabled);
    gtk_widget_set_opacity(GTK_WIDGET(button->data), opacity);
  }
  if(GTK_IS_DRAWING_AREA(button->data))
  {
    // temporarily or permanently (de)activate width trigger widget
    if(dynamic)
      gtk_widget_set_visible(GTK_WIDGET(button->data), !show_buttons && !always_hide);
    else
      gtk_widget_destroy(GTK_WIDGET(button->data));
  }
  else
  {
    if(dynamic)
    {
      GtkWidget *space = gtk_drawing_area_new();
      gtk_box_pack_end(GTK_BOX(header), space, TRUE, TRUE, 0);
      gtk_widget_show(space);
      g_signal_connect(G_OBJECT(space), "size-allocate", G_CALLBACK(_header_size_callback), header);
    }
  }

  g_list_free(children);

  if(dynamic && !show_buttons && !always_hide)
  {
    GdkRectangle fake_allocation = {.width = UINT16_MAX};
    _header_size_callback(NULL, &fake_allocation, header);
  }

  return TRUE;
}

static void _display_mask_indicator_callback(GtkToggleButton *bt, dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;

  const gboolean is_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(bt));
  const dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  module->request_mask_display &= ~DT_DEV_PIXELPIPE_DISPLAY_MASK;
  module->request_mask_display |= (is_active ? DT_DEV_PIXELPIPE_DISPLAY_MASK : 0);

  dt_iop_set_cache_bypass(module, module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE);

  // set the module show mask button too
  if(bd->showmask)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask), is_active);

  dt_iop_request_focus(module);
  dt_iop_refresh_center(module);
}

static gboolean _mask_indicator_tooltip(GtkWidget *treeview, gint x, gint y, gboolean kb_mode,
      GtkTooltip* tooltip, dt_iop_module_t *module)
{
  gboolean res = FALSE;
  const gboolean raster = module->blend_params->mask_mode & DEVELOP_MASK_RASTER;
  if(module->mask_indicator)
  {
    gchar *type = _("unknown mask");
    gchar *text;
    const uint32_t mm = module->blend_params->mask_mode;
    if((mm & DEVELOP_MASK_MASK) && (mm & DEVELOP_MASK_CONDITIONAL))
      type=_("drawn + parametric mask");
    else if(mm & DEVELOP_MASK_MASK)
      type=_("drawn mask");
    else if(mm & DEVELOP_MASK_CONDITIONAL)
      type=_("parametric mask");
    else if(mm & DEVELOP_MASK_RASTER)
      type=_("raster mask");
    else
      fprintf(stderr, "unknown mask mode '%d' in module '%s'\n", mm, module->op);
    gchar *part1 = g_strdup_printf(_("this module has a `%s'"), type);
    gchar *part2 = NULL;
    if(raster && module->raster_mask.sink.source)
    {
      gchar *source = dt_history_item_get_name(module->raster_mask.sink.source);
      part2 = g_strdup_printf(_("taken from module %s"), source);
      g_free(source);
    }

    if(!raster && !part2)
      part2 = g_strdup(_("click to display (module must be activated first)"));

    if(part2)
      text = g_strconcat(part1, "\n", part2, NULL);
    else
      text = g_strdup(part1);

    gtk_tooltip_set_text(tooltip, text);
    res = TRUE;
    g_free(part1);
    g_free(part2);
    g_free(text);
  }
  return res;
}

void add_remove_mask_indicator(dt_iop_module_t *module, gboolean add)
{
  const gboolean raster = module->blend_params->mask_mode & DEVELOP_MASK_RASTER;

  if(module->mask_indicator)
  {
    if(!add)
    {
      gtk_widget_destroy(module->mask_indicator);
      module->mask_indicator = NULL;
      dt_iop_show_hide_header_buttons(module, NULL, FALSE, FALSE);
    }
    else
      gtk_widget_set_sensitive(GTK_WIDGET(module->mask_indicator), !raster && module->enabled);
  }
  else if(add)
  {
    module->mask_indicator = dtgtk_togglebutton_new(dtgtk_cairo_paint_showmask, 0, NULL);
    dt_gui_add_class(module->mask_indicator, "dt_transparent_background");
    g_signal_connect(G_OBJECT(module->mask_indicator), "toggled",
                     G_CALLBACK(_display_mask_indicator_callback), module);
    g_signal_connect(G_OBJECT(module->mask_indicator), "query-tooltip",
                     G_CALLBACK(_mask_indicator_tooltip), module);
    gtk_widget_set_has_tooltip(module->mask_indicator, TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(module->mask_indicator), !raster && module->enabled);
    gtk_box_pack_end(GTK_BOX(module->header), module->mask_indicator, FALSE, FALSE, 0);

    // in dynamic modes, we need to put the mask indicator after the drawing area
    GList *children = gtk_container_get_children(GTK_CONTAINER(module->header));
    GList *child;

    for(child = g_list_last(children); child && GTK_IS_BUTTON(child->data); child = g_list_previous(child));

    if(GTK_IS_DRAWING_AREA(child->data))
    {
      GValue position = G_VALUE_INIT;
      g_value_init (&position, G_TYPE_INT);
      gtk_container_child_get_property(GTK_CONTAINER(module->header), child->data ,"position", &position);
      gtk_box_reorder_child(GTK_BOX(module->header), module->mask_indicator, g_value_get_int(&position));
    }
    g_list_free(children);

    dt_iop_show_hide_header_buttons(module, NULL, FALSE, FALSE);
  }
}

gboolean _iop_tooltip_callback(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                               GtkTooltip *tooltip, gpointer user_data)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;

  const char **des = module->description(module);

  if(!des) return FALSE;

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), FALSE);
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));
  gtk_widget_set_hexpand(grid, FALSE);

  GtkWidget *label = gtk_label_new(des[0] ? des[0] : "");
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
  // if there is no more description, do not add a separator
  if(des[1]) dt_gui_add_class(label, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

  gtk_widget_set_size_request(label, DT_PIXEL_APPLY_DPI(300), -1);
  gtk_widget_set_size_request(grid, DT_PIXEL_APPLY_DPI(300), -1);
  gtk_widget_set_size_request(vbox, DT_PIXEL_APPLY_DPI(300), -1);

  const char *icon_purpose = "\342\237\263";
  const char *icon_input   = "\342\207\245";
  const char *icon_process = "\342\237\264";
  const char *icon_output  = "\342\206\246";

  const char *icons[4] = {icon_purpose, icon_input, icon_process, icon_output};
  const char *ilabs[4] = {_("Purpose"), _("Input"), _("Process"), _("Output")};

  for(int k=1; k<5; k++)
  {
    if(des[k])
    {
      label = gtk_label_new(icons[k-1]);
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), label, 0, k, 1, 1);
      gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);

      label = gtk_label_new(ilabs[k-1]);
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), label, 1, k, 1, 1);
      gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);

      label = gtk_label_new(":");
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), label, 2, k, 1, 1);
      gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);

      label = gtk_label_new(des[k]);
      gtk_widget_set_halign(label, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), label, 3, k, 1, 1);
      gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    }
  }

  gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);
  gtk_widget_show_all(vbox);
  gtk_tooltip_set_custom(tooltip, vbox);

  return TRUE;
}

void dt_iop_gui_set_expander(dt_iop_module_t *module)
{
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(GTK_WIDGET(header), "module-header");

  GtkWidget *iopw = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *expander = dtgtk_expander_new(header, iopw);
  dt_gui_add_class(expander, "dt_module_frame");

  GtkWidget *header_evb = dtgtk_expander_get_header_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *body_evb = dtgtk_expander_get_body_event_box(DTGTK_EXPANDER(expander));
  GtkWidget *pluginui_frame = dtgtk_expander_get_frame(DTGTK_EXPANDER(expander));

  dt_gui_add_class(pluginui_frame, "dt_plugin_ui");

  module->header = header;

  /* setup the header box */
  g_signal_connect(G_OBJECT(header_evb), "button-press-event", G_CALLBACK(_iop_plugin_header_button_press), module);
  gtk_widget_add_events(header_evb, GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(header_evb), "enter-notify-event", G_CALLBACK(_header_motion_notify_show_callback), module);
  g_signal_connect(G_OBJECT(header_evb), "leave-notify-event", G_CALLBACK(_header_motion_notify_hide_callback), module);

  /* connect mouse button callbacks for focus and presets */
  g_signal_connect(G_OBJECT(body_evb), "button-press-event", G_CALLBACK(_iop_plugin_body_button_press), module);
  gtk_widget_add_events(body_evb, GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(body_evb), "enter-notify-event", G_CALLBACK(_header_motion_notify_show_callback), module);
  g_signal_connect(G_OBJECT(body_evb), "leave-notify-event", G_CALLBACK(_header_motion_notify_hide_callback), module);

  /*
   * initialize the header widgets
   */
  GtkWidget *hw[IOP_MODULE_LAST] = { NULL };

  /* init empty place for icon, this is then set in CSS if needed */
  char w_name[256] = { 0 };
  snprintf(w_name, sizeof(w_name), "iop-panel-icon-%s", module->op);
  hw[IOP_MODULE_ICON] = gtk_label_new("");
  gtk_widget_set_name(GTK_WIDGET(hw[IOP_MODULE_ICON]), w_name);
  gtk_widget_set_valign(GTK_WIDGET(hw[IOP_MODULE_ICON]), GTK_ALIGN_CENTER);

  /* add module label */
  hw[IOP_MODULE_LABEL] = gtk_event_box_new();
  GtkWidget *lab = hw[IOP_MODULE_LABEL];
  gtk_container_add(GTK_CONTAINER(lab), gtk_label_new(""));
  if((module->flags() & IOP_FLAGS_DEPRECATED) && module->deprecated_msg())
    gtk_widget_set_tooltip_text(lab, module->deprecated_msg());
  else
  {
    gtk_widget_set_name(lab, "iop_description");
    g_signal_connect(lab, "query-tooltip", G_CALLBACK(_iop_tooltip_callback), module);
  }
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_LABEL]), "enter-notify-event", G_CALLBACK(_header_enter_notify_callback),
                   GINT_TO_POINTER(DT_ACTION_ELEMENT_SHOW));

  /* add multi instances menu button */
  hw[IOP_MODULE_INSTANCE] = dtgtk_button_new(dtgtk_cairo_paint_multiinstance, 0, NULL);
  module->multimenu_button = GTK_WIDGET(hw[IOP_MODULE_INSTANCE]);
  gtk_widget_set_tooltip_text(GTK_WIDGET(hw[IOP_MODULE_INSTANCE]),
                              _("multiple instance actions\nright-click creates new instance"));
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_INSTANCE]), "button-press-event", G_CALLBACK(_gui_multiinstance_callback),
                   module);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_INSTANCE]), "enter-notify-event", G_CALLBACK(_header_enter_notify_callback),
                   GINT_TO_POINTER(DT_ACTION_ELEMENT_INSTANCE));

  dt_gui_add_help_link(expander, dt_get_help_url(module->op));

  /* add reset button */
  hw[IOP_MODULE_RESET] = dtgtk_button_new(dtgtk_cairo_paint_reset, 0, NULL);
  module->reset_button = GTK_WIDGET(hw[IOP_MODULE_RESET]);
  gtk_widget_set_tooltip_text(GTK_WIDGET(hw[IOP_MODULE_RESET]), _("reset parameters\nctrl+click to reapply any automatic presets"));
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_RESET]), "button-press-event", G_CALLBACK(_gui_reset_callback), module);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_RESET]), "enter-notify-event", G_CALLBACK(_header_enter_notify_callback),
                   GINT_TO_POINTER(DT_ACTION_ELEMENT_RESET));

  /* add preset button if module has implementation */
  hw[IOP_MODULE_PRESETS] = dtgtk_button_new(dtgtk_cairo_paint_presets, 0, NULL);
  module->presets_button = GTK_WIDGET(hw[IOP_MODULE_PRESETS]);
  if(!(module->flags() & IOP_FLAGS_ONE_INSTANCE))
    gtk_widget_set_tooltip_text(GTK_WIDGET(hw[IOP_MODULE_PRESETS]), _("presets\nright-click to apply on new instance"));
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_PRESETS]), "clicked", G_CALLBACK(_presets_popup_callback), module);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_PRESETS]), "enter-notify-event", G_CALLBACK(_header_enter_notify_callback),
                   GINT_TO_POINTER(DT_ACTION_ELEMENT_PRESETS));

  /* add enabled button */
  hw[IOP_MODULE_SWITCH] = dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, 0, module);
  dt_gui_add_class(hw[IOP_MODULE_SWITCH], "dt_transparent_background");
  dt_iop_gui_set_enable_button_icon(hw[IOP_MODULE_SWITCH], module);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hw[IOP_MODULE_SWITCH]), module->enabled);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_SWITCH]), "toggled", G_CALLBACK(_gui_off_callback), module);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_SWITCH]), "button-press-event", G_CALLBACK(_gui_off_button_press), module);
  g_signal_connect(G_OBJECT(hw[IOP_MODULE_SWITCH]), "enter-notify-event", G_CALLBACK(_header_enter_notify_callback), GINT_TO_POINTER(DT_ACTION_ELEMENT_ENABLE));

  module->off = DTGTK_TOGGLEBUTTON(hw[IOP_MODULE_SWITCH]);
  gtk_widget_set_sensitive(GTK_WIDGET(hw[IOP_MODULE_SWITCH]), !module->hide_enable_button);

  /* reorder header, for now, iop are always in the right panel */
  for(int i = 0; i <= IOP_MODULE_LABEL; i++)
    if(hw[i]) gtk_box_pack_start(GTK_BOX(header), hw[i], FALSE, FALSE, 0);
  for(int i = IOP_MODULE_LAST - 1; i > IOP_MODULE_LABEL; i--)
    if(hw[i]) gtk_box_pack_end(GTK_BOX(header), hw[i], FALSE, FALSE, 0);
  for(int i = 0; i < IOP_MODULE_LAST; i++)
    if(hw[i]) dt_action_define(&module->so->actions, NULL, NULL, hw[i], NULL);

  dt_gui_add_help_link(header, dt_get_help_url("module_header"));
  // for the module label, point to module specific help page
  dt_gui_add_help_link(hw[IOP_MODULE_LABEL], dt_get_help_url(module->op));

  gtk_widget_set_halign(hw[IOP_MODULE_LABEL], GTK_ALIGN_START);
  gtk_widget_set_halign(hw[IOP_MODULE_INSTANCE], GTK_ALIGN_END);

  // show deprecated message if any
  if(module->deprecated_msg())
  {
    GtkWidget *lb = gtk_label_new(module->deprecated_msg());
    gtk_label_set_line_wrap(GTK_LABEL(lb), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
    dt_gui_add_class(lb, "dt_warning");
    gtk_box_pack_start(GTK_BOX(iopw), lb, TRUE, TRUE, 0);
    gtk_widget_show(lb);
  }

  /* add the blending ui if supported */
  gtk_box_pack_start(GTK_BOX(iopw), module->widget, TRUE, TRUE, 0);
  dt_iop_gui_init_blending(iopw, module);
  dt_gui_add_class(module->widget, "dt_plugin_ui_main");
  dt_gui_add_help_link(module->widget, dt_get_help_url(module->op));
  gtk_widget_hide(iopw);

  module->expander = expander;

  /* update header */
  dt_iop_gui_update_header(module);

  gtk_widget_set_hexpand(module->widget, FALSE);
  gtk_widget_set_vexpand(module->widget, FALSE);

  dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);
  dt_iop_show_hide_header_buttons(module, NULL, FALSE, FALSE);
}

GtkWidget *dt_iop_gui_get_widget(dt_iop_module_t *module)
{
  return dtgtk_expander_get_body(DTGTK_EXPANDER(module->expander));
}

GtkWidget *dt_iop_gui_get_pluginui(dt_iop_module_t *module)
{
  // return gtkframe (pluginui_frame)
  return dtgtk_expander_get_frame(DTGTK_EXPANDER(module->expander));
}

void dt_iop_nap(int32_t usec)
{
  if(usec <= 0) return;

  // relinquish processor
  sched_yield();

  // additionally wait the given amount of time
  g_usleep(usec);
}

gboolean dt_iop_get_cache_bypass(dt_iop_module_t *module)
{
  return module->bypass_cache;
}

void dt_iop_set_cache_bypass(dt_iop_module_t *module, gboolean state)
{
  module->bypass_cache = state;

  if(state && module->dev)
  {
    // Disable other modules bypass if set.
    for(GList *iop = g_list_last(module->dev->iop);
        iop;
        iop = g_list_previous(iop))
    {
      dt_iop_module_t *current = (dt_iop_module_t *)iop->data;
      if(current != module && current->bypass_cache) current->bypass_cache = FALSE;
    }
  }
}


dt_iop_module_t *dt_iop_get_colorout_module(void)
{
  return dt_iop_get_module_from_list(darktable.develop->iop, "colorout");
}

dt_iop_module_t *dt_iop_get_module_from_list(GList *iop_list, const char *op)
{
  dt_iop_module_t *result = NULL;

  for(GList *modules = iop_list; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(strcmp(mod->op, op) == 0)
    {
      result = mod;
      break;
    }
  }

  return result;
}

dt_iop_module_t *dt_iop_get_module(const char *op)
{
  return dt_iop_get_module_from_list(darktable.develop->iop, op);
}

int dt_iop_get_module_flags(const char *op)
{
  GList *modules = darktable.iop;
  while(modules)
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)modules->data;
    if(!strcmp(module->op, op)) return module->flags();
    modules = g_list_next(modules);
  }
  return 0;
}

static void _show_module_callback(dt_iop_module_t *module)
{
  // Showing the module, if it isn't already visible
  const uint32_t current_group = dt_dev_modulegroups_get(module->dev);

  if(module->default_group() != current_group)
  {
    dt_dev_modulegroups_switch(darktable.develop, module);
  }
  else
  {
    dt_dev_modulegroups_set(darktable.develop, current_group);
  }

  dt_iop_gui_set_expanded(module, TRUE, TRUE);
  dt_iop_request_focus(module);
  darktable.gui->scroll_to[1] = module->expander;

  dt_iop_connect_accels_multi(module->so);
}

static void _enable_module_callback(dt_iop_module_t *module)
{
  //cannot toggle module if there's no enable button
  if(module->hide_enable_button) return;

  gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(module->off));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), !active);
}

// to be called before issuing any query based on memory.darktable_iop_names
void dt_iop_set_darktable_iop_table()
{
  sqlite3_stmt *stmt;
  gchar *module_list = NULL;
  for(GList *iop = darktable.iop; iop; iop = g_list_next(iop))
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)iop->data;
    module_list = dt_util_dstrcat(module_list, "(\"%s\",\"%s\"),", module->op, module->name());
  }

  if(module_list)
  {
    module_list[strlen(module_list) - 1] = '\0';
    gchar *query = g_strdup_printf("INSERT INTO memory.darktable_iop_names (operation, name) VALUES %s", module_list);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(query);
    g_free(module_list);
  }
}

const gchar *dt_iop_get_localized_name(const gchar *op)
{
  // Prepare mapping op -> localized name
  static GHashTable *module_names = NULL;
  if(module_names == NULL)
  {
    module_names = g_hash_table_new(g_str_hash, g_str_equal);
    for(GList *iop = darktable.iop; iop; iop = g_list_next(iop))
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)iop->data;
      g_hash_table_insert(module_names, module->op, g_strdup(module->name()));
    }
  }
  if(op != NULL)
  {
    return (gchar *)g_hash_table_lookup(module_names, op);
  }
  else {
    return _("ERROR");
  }
}

const gchar *dt_iop_get_localized_aliases(const gchar *op)
{
  // Prepare mapping op -> localized name
  static GHashTable *module_aliases = NULL;
  if(module_aliases == NULL)
  {
    module_aliases = g_hash_table_new(g_str_hash, g_str_equal);
    for(GList *iop = darktable.iop; iop; iop = g_list_next(iop))
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)iop->data;
      g_hash_table_insert(module_aliases, module->op, g_strdup(module->aliases()));
    }
  }
  if(op != NULL)
  {
    return (gchar *)g_hash_table_lookup(module_aliases, op);
  }
  else {
    return _("ERROR");
  }
}

void dt_iop_update_multi_priority(dt_iop_module_t *module, int new_priority)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, module->raster_mask.source.users);
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    dt_iop_module_t *sink_module = (dt_iop_module_t *)key;

    sink_module->blend_params->raster_mask_instance = new_priority;

    // also fix history entries
    for(GList *hiter = module->dev->history; hiter; hiter = g_list_next(hiter))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)hiter->data;
      if(hist->module == sink_module)
        hist->blend_params->raster_mask_instance = new_priority;
    }
  }

  module->multi_priority = new_priority;
}

gboolean dt_iop_is_raster_mask_used(dt_iop_module_t *module, int id)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, module->raster_mask.source.users);
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    if(GPOINTER_TO_INT(value) == id)
      return TRUE;
  }
  return FALSE;
}

dt_iop_module_t *dt_iop_get_module_by_op_priority(GList *modules, const char *operation, const int multi_priority)
{
  dt_iop_module_t *mod_ret = NULL;

  for(GList *m = modules; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)m->data;

    if(strcmp(mod->op, operation) == 0
       && (mod->multi_priority == multi_priority || multi_priority == -1))
    {
      mod_ret = mod;
      break;
    }
  }
  return mod_ret;
}

dt_iop_module_t *dt_iop_get_module_preferred_instance(dt_iop_module_so_t *module)
{
  /*
   decide which module instance keyboard shortcuts will be applied to based on user preferences, as follows
    - Use the focused module, if it is an instance of this module type and the appropriate preference is checked. Otherwise
    - prefer expanded instances (when selected and instances of the module are expanded on the RHS of the screen, collapsed instances will be ignored)
    - prefer enabled instances (when selected, after applying the above rule, if instances of the module are active, inactive instances will be ignored)
    - prefer unmasked instances (when selected, after applying the above rules, if instances of the module are unmasked, masked instances will be ignored)
    - selection order (after applying the above rules, apply the shortcut to the first or last instance remaining)
  */
  const gboolean prefer_focused = dt_conf_get_bool("accel/prefer_focused");
  const int prefer_expanded = dt_conf_get_bool("accel/prefer_expanded") ? 8 : 0;
  const int prefer_enabled = dt_conf_get_bool("accel/prefer_enabled") ? 4 : 0;
  const int prefer_unmasked = dt_conf_get_bool("accel/prefer_unmasked") ? 2 : 0;
  const int prefer_first = dt_conf_is_equal("accel/select_order", "first instance") ? 1 : 0;

  dt_iop_module_t *accel_mod = NULL;  // The module to which accelerators are to be attached

  // if any instance has focus, use that one
  if(prefer_focused && darktable.develop->gui_module && darktable.develop->gui_module->so == module)
    accel_mod = darktable.develop->gui_module;
  else
  {
    int best_score = -1;

    for(GList *iop_mods = g_list_last(darktable.develop->iop);
        iop_mods;
        iop_mods = g_list_previous(iop_mods))
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)iop_mods->data;

      if(mod->so == module && mod->iop_order != INT_MAX)
      {
        const int score = (mod->expanded ? prefer_expanded : 0)
                        + (mod->enabled ? prefer_enabled : 0)
                        + (mod->blend_params->mask_mode == DEVELOP_MASK_DISABLED
                           || mod->blend_params->mask_mode == DEVELOP_MASK_ENABLED ? prefer_unmasked : 0);

        if(score + prefer_first > best_score)
        {
          best_score = score;
          accel_mod = mod;
        }
      }
    }
  }

  return accel_mod;
}

/** adds keyboard accels to the first module in the pipe to handle where there are multiple instances */
void dt_iop_connect_accels_multi(dt_iop_module_so_t *module)
{
  if(darktable.develop->gui_attached)
  {
    dt_iop_module_t *accel_mod_new = dt_iop_get_module_preferred_instance(module);

    // switch accelerators to new module
    if(accel_mod_new)
    {
      dt_accel_connect_instance_iop(accel_mod_new);

      if(!strcmp(accel_mod_new->op, "exposure"))
        darktable.develop->proxy.exposure.module = accel_mod_new;
    }
  }
}

void dt_iop_connect_accels_all()
{
  for(const GList *iop_mods = g_list_last(darktable.develop->iop); iop_mods; iop_mods = g_list_previous(iop_mods))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)iop_mods->data;
    dt_iop_connect_accels_multi(mod->so);
  }
}

dt_iop_module_t *dt_iop_get_module_by_instance_name(GList *modules, const char *operation, const char *multi_name)
{
  dt_iop_module_t *mod_ret = NULL;

  for(GList *m = modules; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)m->data;

    if((strcmp(mod->op, operation) == 0)
       && ((multi_name == NULL) || (strcmp(mod->multi_name, multi_name) == 0)))
    {
      mod_ret = mod;
      break;
    }
  }
  return mod_ret;
}

/** count instances of a module **/
int dt_iop_count_instances(dt_iop_module_so_t *module)
{
  int inst_count = 0;

  for(const GList *iop_mods = g_list_last(darktable.develop->iop); iop_mods; iop_mods = g_list_previous(iop_mods))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)iop_mods->data;
    if(mod->so == module && mod->iop_order != INT_MAX)
    {
      inst_count++;
    }
  }
  return inst_count;
}

gboolean dt_iop_is_first_instance(GList *modules, dt_iop_module_t *module)
{
  gboolean is_first = TRUE;
  GList *iop = modules;
  while(iop)
  {
    dt_iop_module_t *m = (dt_iop_module_t *)iop->data;
    if(!strcmp(m->op, module->op))
    {
      is_first = (m == module);
      break;
    }
    iop = g_list_next(iop);
  }

  return is_first;
}

void dt_iop_refresh_center(dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_develop_t *dev = module->dev;
  if (dev && dev->gui_attached)
  {
    dt_dev_invalidate(dev);
    dt_dev_refresh_ui_images(dev);
  }
}

void dt_iop_refresh_preview(dt_iop_module_t *module)
{
  if(darktable.gui->reset) return;
  dt_develop_t *dev = module->dev;
  if (dev && dev->gui_attached)
  {
    dt_dev_invalidate_preview(dev);
    dt_dev_refresh_ui_images(dev);
  }
}

void dt_iop_refresh_all(dt_iop_module_t *module)
{
  dt_iop_refresh_preview(module);
  dt_iop_refresh_center(module);
}

static gboolean _postponed_history_update(gpointer data)
{
  dt_iop_module_t *self = (dt_iop_module_t*)data;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  self->timeout_handle = 0;
  return FALSE; //cancel the timer
}

/** queue a delayed call of the add_history function after user interaction, to capture parameter updates (but not */
/** too often). */
void dt_iop_queue_history_update(dt_iop_module_t *module, gboolean extend_prior)
{
  if (module->timeout_handle && extend_prior)
  {
    // we already queued an update, but we don't want to have the update happen until the timeout expires
    // without any activity, so cancel the queued callback
    g_source_remove(module->timeout_handle);
  }
  if (!module->timeout_handle || extend_prior)
  {
    // adaptively set the timeout to 150% of the average time the past several pixelpipe runs took, clamped
    //   to keep updates from appearing to be too sluggish (though early iops such as rawdenoise may have
    //   multiple very slow iops following them, leading to >1000ms processing times)
    const int delay = CLAMP(darktable.develop->average_delay * 3 / 2, 10, 1200);
    module->timeout_handle = g_timeout_add(delay, _postponed_history_update, module);
  }
}

void dt_iop_cancel_history_update(dt_iop_module_t *module)
{
  if (module->timeout_handle)
  {
    g_source_remove(module->timeout_handle);
    module->timeout_handle = 0;
  }
}

const char **dt_iop_set_description(dt_iop_module_t *module, const char *main_text, const char *purpose, const char *input, const char *process,
                             const char *output)
{
  static const char *str_out[5] = {NULL, NULL, NULL, NULL, NULL};

  str_out[0] = main_text;
  str_out[1] = purpose;
  str_out[2] = input;
  str_out[3] = process;
  str_out[4] = output;

  return (const char **)str_out;
}

gboolean dt_iop_have_required_input_format(const int req_ch, struct dt_iop_module_t *const module, const int ch,
                                           const void *const restrict ivoid, void *const restrict ovoid,
                                           const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(ch == req_ch)
  {
    return TRUE;
  }
  else
  {
    // copy the input buffer to the output
    dt_iop_copy_image_roi(ovoid, ivoid, ch, roi_in, roi_out, TRUE);
    // and set the module's trouble message
    if (module)
      fprintf(stdout, "you have placed the module %s at a position in the pipeline where"
                      "the data format does not match its requirements.", module->name());
    else
    {
      //TODO: pop up a toast message?
    }
    return FALSE;
  }
}

// WARNING: this is called in bauhaus.c too when slider & comboboxes values are changed.
// Mind that if any change is done here.
void dt_iop_gui_changed(dt_action_t *action, GtkWidget *widget, gpointer data)
{
  if(!action || action->type != DT_ACTION_TYPE_IOP_INSTANCE) return;
  dt_iop_module_t *module = (dt_iop_module_t *)action;

  if(module->gui_changed) module->gui_changed(module, widget, data);

  dt_iop_color_picker_reset(module, TRUE);

  dt_dev_add_history_item(darktable.develop, module, TRUE);

  dt_iop_gui_set_enable_button(module);
}

enum
{
  // Multi-instance
//DT_ACTION_EFFECT_SHOW,
//DT_ACTION_EFFECT_UP,
//DT_ACTION_EFFECT_DOWN,
  DT_ACTION_EFFECT_NEW = 3,
//DT_ACTION_EFFECT_DELETE = 4,
  DT_ACTION_EFFECT_RENAME = 5,
  DT_ACTION_EFFECT_DUPLICATE = 6,
};

static float _action_process(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  dt_iop_module_t *module = target;

  if(!isnan(move_size))
  {
    switch(element)
    {
    case DT_ACTION_ELEMENT_ENABLE:
      _enable_module_callback(module);
      break;
    case DT_ACTION_ELEMENT_SHOW:
      _show_module_callback(module);
      break;
    case DT_ACTION_ELEMENT_INSTANCE:
      if     (effect == DT_ACTION_EFFECT_NEW       && module->multi_show_new  )
        _gui_copy_callback     (NULL, module);
      else if(effect == DT_ACTION_EFFECT_DUPLICATE && module->multi_show_new  )
        _gui_duplicate_callback(NULL, module);
      else if(effect == DT_ACTION_EFFECT_UP        && module->multi_show_up   )
        _gui_moveup_callback   (NULL, module);
      else if(effect == DT_ACTION_EFFECT_DOWN      && module->multi_show_down )
        _gui_movedown_callback (NULL, module);
      else if(effect == DT_ACTION_EFFECT_DELETE    && module->multi_show_close)
        _gui_delete_callback   (NULL, module);
      else if(effect == DT_ACTION_EFFECT_RENAME                               )
        _gui_rename_callback   (NULL, module);
      else _gui_multiinstance_callback(NULL, NULL, module);
      break;
    case DT_ACTION_ELEMENT_RESET:
      {
        GdkEventButton event = { .state = (effect == DT_ACTION_EFFECT_ACTIVATE_CTRL ? GDK_CONTROL_MASK : 0) };
        _gui_reset_callback(NULL, &event, module);
      }
      break;
    case DT_ACTION_ELEMENT_PRESETS:
      if(module->presets_button) _presets_popup_callback(NULL, module);
      break;
    }

    gchar *text = g_strdup_printf("%s, %s", dt_action_def_iop.elements[element].name,
                                  dt_action_def_iop.elements[element].effects[effect]);
    dt_action_widget_toast(target, NULL, text);
    g_free(text);
  }

  return element == DT_ACTION_ELEMENT_ENABLE ? module->off &&
                                               gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(module->off))
       : element == DT_ACTION_ELEMENT_SHOW ? module->expanded
       : 0;
}

const gchar *dt_action_effect_instance[]
  = { N_("show"),
      N_("move up"),
      N_("move down"),
      N_("new"),
      N_("delete"),
      N_("rename"),
      N_("duplicate"),
      NULL };

static const dt_action_element_def_t _action_elements[]
  = { { N_("show"), dt_action_effect_toggle },
      { N_("enable"), dt_action_effect_toggle },
      { N_("instance"), dt_action_effect_instance },
      { N_("reset"), dt_action_effect_activate },
      { N_("presets"), dt_action_effect_presets },
      { NULL } };

static const dt_shortcut_fallback_t _action_fallbacks[]
  = { { .element = DT_ACTION_ELEMENT_SHOW, .button = DT_SHORTCUT_LEFT, .click = DT_SHORTCUT_LONG },
      { .element = DT_ACTION_ELEMENT_ENABLE, .button = DT_SHORTCUT_LEFT },
      { .element = DT_ACTION_ELEMENT_INSTANCE, .button = DT_SHORTCUT_RIGHT, .click = DT_SHORTCUT_DOUBLE },
      { .element = DT_ACTION_ELEMENT_RESET, .button = DT_SHORTCUT_LEFT, .click = DT_SHORTCUT_DOUBLE },
      { .element = DT_ACTION_ELEMENT_PRESETS, .button = DT_SHORTCUT_RIGHT },
      { } };

const dt_action_def_t dt_action_def_iop
  = { N_("processing module"),
      _action_process,
      _action_elements,
      _action_fallbacks };

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
