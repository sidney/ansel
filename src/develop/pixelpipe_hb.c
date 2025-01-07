/*
    This file was part of darktable,
    This file is part of Ansel,
    Copyright (C) 2009-2021 darktable developers.
    Copyright (C) 2023 Aurélien Pierre.

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
#include "common/color_picker.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/histogram.h"
#include "common/imageio.h"
#include "common/opencl.h"
#include "common/iop_order.h"
#include "control/control.h"
#include "control/conf.h"
#include "control/signal.h"
#include "develop/blend.h"
#include "develop/format.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe.h"
#include "develop/tiling.h"
#include "develop/masks.h"
#include "gui/gtk.h"
#include "libs/colorpicker.h"
#include "libs/lib.h"
#include "gui/color_picker_proxy.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef enum dt_pixelpipe_flow_t
{
  PIXELPIPE_FLOW_NONE = 0,
  PIXELPIPE_FLOW_HISTOGRAM_NONE = 1 << 0,
  PIXELPIPE_FLOW_HISTOGRAM_ON_CPU = 1 << 1,
  PIXELPIPE_FLOW_HISTOGRAM_ON_GPU = 1 << 2,
  PIXELPIPE_FLOW_PROCESSED_ON_CPU = 1 << 3,
  PIXELPIPE_FLOW_PROCESSED_ON_GPU = 1 << 4,
  PIXELPIPE_FLOW_PROCESSED_WITH_TILING = 1 << 5,
  PIXELPIPE_FLOW_BLENDED_ON_CPU = 1 << 6,
  PIXELPIPE_FLOW_BLENDED_ON_GPU = 1 << 7
} dt_pixelpipe_flow_t;

typedef enum dt_pixelpipe_picker_source_t
{
  PIXELPIPE_PICKER_INPUT = 0,
  PIXELPIPE_PICKER_OUTPUT = 1
} dt_pixelpipe_picker_source_t;

#include "develop/pixelpipe_cache.c"

static void get_output_format(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                              dt_develop_t *dev, dt_iop_buffer_dsc_t *dsc);

static char *_pipe_type_to_str(int pipe_type)
{
  char *r = NULL;

  switch(pipe_type & DT_DEV_PIXELPIPE_ANY)
  {
    case DT_DEV_PIXELPIPE_PREVIEW:
      r = "preview";
      break;
    case DT_DEV_PIXELPIPE_FULL:
      r = "full";
      break;
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      r = "thumbnail";
      break;
    case DT_DEV_PIXELPIPE_EXPORT:
      r = "export";
      break;
    default:
      r = "unknown";
  }
  return r;
}

inline static void _copy_buffer(const char *const input, char *const output,
                           const size_t height, const size_t o_width, const size_t i_width,
                           const size_t x_offset, const size_t y_offset,
                           const size_t stride, const size_t bpp)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
          dt_omp_firstprivate(input, output, bpp, o_width, i_width, height, x_offset, y_offset, stride) \
          schedule(static)
#endif
  for(size_t j = 0; j < height; j++)
    memcpy(output + bpp * j * o_width,
           input + bpp * (x_offset + (y_offset + j) * i_width),
           stride);
}


inline static void _uint8_to_float(const uint8_t *const input, float *const output,
                                   const size_t width, const size_t height, const size_t chan)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
        aligned(input, output: 64) \
        dt_omp_firstprivate(input, output, width, height, chan) \
        schedule(static)
#endif
  for(size_t k = 0; k < height * width; k++)
  {
    const size_t index = k * chan;
    // Warning: we take BGRa and put it back into RGBa
    output[index + 0] = (float)input[index + 2] / 255.f;
    output[index + 1] = (float)input[index + 1] / 255.f;
    output[index + 2] = (float)input[index + 0] / 255.f;
    output[index + 3] = 0.f;
  }
}


int dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height, int levels,
                                 gboolean store_masks)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe, sizeof(float) * 4 * width * height, 2);
  pipe->type = DT_DEV_PIXELPIPE_EXPORT;
  pipe->levels = levels;
  pipe->store_all_raster_masks = store_masks;
  return res;
}

int dt_dev_pixelpipe_init_thumbnail(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe, sizeof(float) * 4 * width * height, 2);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  return res;
}

int dt_dev_pixelpipe_init_dummy(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe, sizeof(float) * 4 * width * height, 0);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  return res;
}

int dt_dev_pixelpipe_init_preview(dt_dev_pixelpipe_t *pipe)
{
  // Init with the size of MIPMAP_F
  int32_t cachelines = MAX(dt_conf_get_int("cachelines"), 8);
  const int res = dt_dev_pixelpipe_init_cached(pipe, sizeof(float) * 4 * 720 * 450, cachelines);
  pipe->type = DT_DEV_PIXELPIPE_PREVIEW;
  return res;
}

int dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe)
{
  // Init with the max size of a screen.
  int32_t cachelines = MAX(dt_conf_get_int("cachelines"), 8);
  gint width = 1920;
  gint height = 1080;

  if(darktable.gui)
  {
    gtk_window_get_size(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), &width, &height);
    width *= darktable.gui->ppd;
    height *= darktable.gui->ppd;
  }

  const int res = dt_dev_pixelpipe_init_cached(pipe, sizeof(float) * 4 * width * height, cachelines);
  pipe->type = DT_DEV_PIXELPIPE_FULL;
  return res;
}

int dt_dev_pixelpipe_init_cached(dt_dev_pixelpipe_t *pipe, size_t size, int32_t entries)
{
  pipe->devid = -1;
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  pipe->processed_width = pipe->backbuf_width = pipe->iwidth = 0;
  pipe->processed_height = pipe->backbuf_height = pipe->iheight = 0;
  pipe->nodes = NULL;
  pipe->backbuf_size = size;
  if(!dt_dev_pixelpipe_cache_init(&(pipe->cache), entries, pipe->backbuf_size)) return 0;
  pipe->backbuf = NULL;
  pipe->backbuf_scale = 0.0f;
  pipe->backbuf_zoom_x = 0.0f;
  pipe->backbuf_zoom_y = 0.0f;

  pipe->output_backbuf = NULL;
  pipe->output_backbuf_width = 0;
  pipe->output_backbuf_height = 0;
  pipe->output_imgid = 0;

  pipe->rawdetail_mask_data = NULL;
  pipe->want_detail_mask = DT_DEV_DETAIL_MASK_NONE;

  pipe->processing = 0;
  dt_atomic_set_int(&pipe->shutdown,FALSE);
  pipe->opencl_error = 0;
  pipe->tiling = 0;
  pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  pipe->bypass_blendif = 0;
  pipe->input_timestamp = 0;
  pipe->levels = IMAGEIO_RGB | IMAGEIO_INT8;
  dt_pthread_mutex_init(&(pipe->backbuf_mutex), NULL);
  dt_pthread_mutex_init(&(pipe->busy_mutex), NULL);
  pipe->icc_type = DT_COLORSPACE_NONE;
  pipe->icc_filename = NULL;
  pipe->icc_intent = DT_INTENT_LAST;
  pipe->iop = NULL;
  pipe->iop_order_list = NULL;
  pipe->forms = NULL;
  pipe->store_all_raster_masks = FALSE;
  pipe->work_profile_info = NULL;
  pipe->input_profile_info = NULL;
  pipe->output_profile_info = NULL;

  return 1;
}

void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, float *input, int width, int height,
                                float iscale)
{
  pipe->iwidth = width;
  pipe->iheight = height;
  pipe->iscale = iscale;
  pipe->input = input;
  pipe->image = dev->image_storage;
  get_output_format(NULL, pipe, NULL, dev, &pipe->dsc);
}

void dt_dev_pixelpipe_set_icc(dt_dev_pixelpipe_t *pipe, dt_colorspaces_color_profile_type_t icc_type,
                              const gchar *icc_filename, dt_iop_color_intent_t icc_intent)
{
  pipe->icc_type = icc_type;
  g_free(pipe->icc_filename);
  pipe->icc_filename = g_strdup(icc_filename ? icc_filename : "");
  pipe->icc_intent = icc_intent;
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf = NULL;
  // blocks while busy and sets shutdown bit:
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  // so now it's safe to clean up cache:
  dt_dev_pixelpipe_cache_cleanup(&(pipe->cache));
  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);
  dt_pthread_mutex_destroy(&(pipe->backbuf_mutex));
  dt_pthread_mutex_destroy(&(pipe->busy_mutex));
  pipe->icc_type = DT_COLORSPACE_NONE;
  g_free(pipe->icc_filename);
  pipe->icc_filename = NULL;

  g_free(pipe->output_backbuf);
  pipe->output_backbuf = NULL;
  pipe->output_backbuf_width = 0;
  pipe->output_backbuf_height = 0;
  pipe->output_imgid = 0;

  dt_dev_clear_rawdetail_mask(pipe);

  if(pipe->forms)
  {
    g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);
    pipe->forms = NULL;
  }
}

void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe)
{
  // FIXME: either this or all process() -> gdk mutices have to be changed!
  //        (this is a circular dependency on busy_mutex and the gdk mutex)
  // [[does the above still apply?]]
  dt_pthread_mutex_lock(&pipe->busy_mutex); // block until the pipe has shut down
  // destroy all nodes
  for(GList *nodes = pipe->nodes; nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    // printf("cleanup module `%s'\n", piece->module->name());
    piece->module->cleanup_pipe(piece->module, pipe, piece);
    free(piece->blendop_data);
    piece->blendop_data = NULL;
    free(piece->histogram);
    piece->histogram = NULL;
    g_hash_table_destroy(piece->raster_masks);
    piece->raster_masks = NULL;
    free(piece);
  }
  g_list_free(pipe->nodes);
  pipe->nodes = NULL;
  // also cleanup iop here
  if(pipe->iop)
  {
    g_list_free(pipe->iop);
    pipe->iop = NULL;
  }
  // and iop order
  g_list_free_full(pipe->iop_order_list, free);
  pipe->iop_order_list = NULL;
  dt_pthread_mutex_unlock(&pipe->busy_mutex);	// safe for others to mess with the pipe now
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex); // block until pipe is idle
  // check that the pipe was actually properly cleaned up after the last run
  g_assert(pipe->nodes == NULL);
  g_assert(pipe->iop == NULL);
  g_assert(pipe->iop_order_list == NULL);
  pipe->iop_order_list = dt_ioppr_iop_order_copy_deep(dev->iop_order_list);

  // for all modules in dev:
  // TODO: don't add deprecated modules that are not enabled are not added to pipeline.
  // currently, that loads 84 modules of which a solid third are not used anymore.
  // if(module->flags() & IOP_FLAGS_DEPRECATED && !(module->enabled)) continue;
  pipe->iop = g_list_copy(dev->iop);
  for(GList *modules = pipe->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)calloc(1, sizeof(dt_dev_pixelpipe_iop_t));
    piece->enabled = module->enabled;
    piece->request_histogram = DT_REQUEST_ONLY_IN_GUI;
    piece->histogram_params.roi = NULL;
    piece->histogram_params.bins_count = 256;
    piece->histogram_stats.bins_count = 0;
    piece->histogram_stats.pixels = 0;
    piece->colors
        = ((module->default_colorspace(module, pipe, NULL) == IOP_CS_RAW) && (dt_image_is_raw(&pipe->image)))
              ? 1
              : 4;
    piece->iscale = pipe->iscale;
    piece->iwidth = pipe->iwidth;
    piece->iheight = pipe->iheight;
    piece->module = module;
    piece->pipe = pipe;
    piece->data = NULL;
    piece->hash = 0;
    piece->global_hash = 0;
    piece->bypass_cache = FALSE;
    piece->process_cl_ready = 0;
    piece->process_tiling_ready = 0;
    piece->raster_masks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, dt_free_align_ptr);
    memset(&piece->processed_roi_in, 0, sizeof(piece->processed_roi_in));
    memset(&piece->processed_roi_out, 0, sizeof(piece->processed_roi_out));
    dt_iop_init_pipe(piece->module, pipe, piece);
    pipe->nodes = g_list_append(pipe->nodes, piece);
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex); // safe for others to use/mess with the pipe now
}

static uint64_t _default_pipe_hash(dt_dev_pixelpipe_t *pipe)
{
  // Start with a hash that is unique, image-wise and duplicate-wise.
  uint64_t hash = dt_hash(5381, (const char *)&pipe->image.id, sizeof(uint32_t));
  hash = dt_hash(hash, (const char *)&pipe->image.version, sizeof(uint32_t));
  hash = dt_hash(hash, (const char *)&pipe->image.film_id, sizeof(uint32_t));
  return hash;
}

static uint64_t _node_hash(dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, const int pos)
{
  // to be called at runtime, not at pipe init.

  // Only at the first step of pipe, we don't have a module because we init the base buffer.
  if(piece)
    return piece->global_hash;
  else
  {
    // This is used for the first step of the pipe, before modules, when initing base buffer
    // We need to take care of the ROI manually
    uint64_t hash = _default_pipe_hash(pipe);
    hash = dt_hash(hash, (const char *)roi_out, sizeof(dt_iop_roi_t));
    return dt_hash(hash, (const char *)&pos, sizeof(int));
  }
}


void dt_pixelpipe_get_global_hash(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  /* Traverse the pipeline node by node and compute the cumulative (global) hash of each module.
  *  This hash takes into account the hashes of the previous modules and the size of the current ROI.
  *  It is used to map pipeline cache states to current parameters.
  *  It represents the state of internal modules params as well as their position in the pipe and their output size.
  *  It is to be called at pipe init, not at runtime.
  */

  // bernstein hash (djb2)
  uint64_t hash = _default_pipe_hash(pipe);

  // Bypassing cache contaminates downstream modules.
  gboolean bypass_cache = FALSE;

  for(GList *node = g_list_first(pipe->nodes); node; node = g_list_next(node))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)node->data;

    // Combine with the previous bypass states
    bypass_cache |= piece->module->bypass_cache;
    piece->bypass_cache = bypass_cache;

    if(piece->enabled)
    {
      // Combine with the previous modules hashes
      uint64_t local_hash = piece->hash;

      // Panning and zooming change the ROI. Some GUI modes (crop in editing mode) too.
      // dt_dev_get_roi_in() should have run before
      local_hash = dt_hash(local_hash, (const char *)&piece->planned_roi_in, sizeof(dt_iop_roi_t));
      local_hash = dt_hash(local_hash, (const char *)&piece->planned_roi_out, sizeof(dt_iop_roi_t));
      hash = dt_hash(hash, (const char *)&local_hash, sizeof(uint64_t));

      dt_print(DT_DEBUG_PIPE, "[pipe] global hash for %s in pipe %i with hash %lu\n", piece->module->op, pipe->type, (long unsigned int)hash);
    }

    piece->global_hash = hash;
  }
}

// helper
void dt_dev_pixelpipe_synch(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, GList *history)
{
  /*
  * WARNING: this function is called to traverse the pipeline in the order of history items (touches),
  * not in the order of nodes application in pipeline.
  */
  dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
  // find piece in nodes list
  dt_dev_pixelpipe_iop_t *piece = NULL;

  // FIXME: this is not the place to handle masks.
  // Shitty darktable code as usual: unable to do one thing at one time.
  const dt_image_t *img = &pipe->image;
  const gboolean rawprep_img = dt_image_is_rawprepare_supported(img);
  const gboolean raw_img     = dt_image_is_raw(img);

  pipe->want_detail_mask &= DT_DEV_DETAIL_MASK_REQUIRED;
  if(raw_img)          pipe->want_detail_mask |= DT_DEV_DETAIL_MASK_DEMOSAIC;
  else if(rawprep_img)
    pipe->want_detail_mask |= DT_DEV_DETAIL_MASK_RAWPREPARE;

  for(GList *nodes = g_list_first(pipe->nodes); nodes; nodes = g_list_next(nodes))
  {
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;

    if(piece->module == hist->module)
    {
      piece->enabled = hist->enabled;
      dt_iop_commit_params(hist->module, hist->params, hist->blend_params, pipe, piece);

      if(piece->blendop_data)
      {
        const dt_develop_blend_params_t *const bp = (const dt_develop_blend_params_t *)piece->blendop_data;
        if(bp->details != 0.0f)
          pipe->want_detail_mask |= DT_DEV_DETAIL_MASK_REQUIRED;
      }
    }
  }
}

void dt_dev_pixelpipe_synch_all_real(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, const char *caller_func)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);

  dt_print(DT_DEBUG_DEV, "[pixelpipe] synch all modules with defaults_params for pipe %i called from %s\n", pipe->type, caller_func);

  // call reset_params on all pieces first. This is mandatory to init utility modules that don't have an history stack
  for(GList *nodes = g_list_first(pipe->nodes); nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    piece->hash = 0;
    piece->global_hash = 0;
    piece->enabled = piece->module->default_enabled;
    dt_iop_commit_params(piece->module, piece->module->default_params, piece->module->default_blendop_params,
                         pipe, piece);
  }

  dt_print(DT_DEBUG_DEV, "[pixelpipe] synch all modules with history for pipe %i called from %s\n", pipe->type, caller_func);

  // go through all history items and adjust params
  // note that we don't necessarily process the whole history
  const uint32_t history_end = dt_dev_get_history_end(dev);
  // because history_end is shifted by 1, it is actually the step after the last one we want
  uint32_t k = 0;

  for(GList *history = g_list_first(dev->history);
      history && k < history_end;
      history = g_list_next(history))
  {
    dt_dev_pixelpipe_synch(pipe, dev, history);
    ++k;
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  GList *history = g_list_nth(dev->history, dt_dev_get_history_end(dev) - 1);
  if(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
    dt_print(DT_DEBUG_DEV, "[pixelpipe] synch top history module `%s` for pipe %i\n", hist->module->op, pipe->type);
    dt_dev_pixelpipe_synch(pipe, dev, history);
  }
  else
  {
    dt_print(DT_DEBUG_DEV, "[pixelpipe] synch top history module missing error for pipe %i\n", pipe->type);
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_change(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev)
{
  dt_times_t start;
  dt_get_times(&start);

  dt_pthread_mutex_lock(&dev->history_mutex);
  dt_atomic_set_int(&pipe->shutdown, FALSE);

  // mask display off as a starting point
  pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  // and blendif active
  pipe->bypass_blendif = 0;

  dt_print(DT_DEBUG_DEV, "[dt_dev_pixelpipe_change] pipeline state changing for pipe %i, flag %i\n", pipe->type, pipe->changed);

  // case DT_DEV_PIPE_UNCHANGED: case DT_DEV_PIPE_ZOOMED:
  if(pipe->changed & DT_DEV_PIPE_REMOVE)
  {
    // modules have been added in between or removed. need to rebuild the whole pipeline.
    dt_dev_pixelpipe_cleanup_nodes(pipe);
    dt_dev_pixelpipe_create_nodes(pipe, dev);
    dt_dev_pixelpipe_synch_all(pipe, dev);
  }
  else if(pipe->changed & DT_DEV_PIPE_SYNCH)
  {
    // pipeline topology remains intact, only change all params.
    dt_dev_pixelpipe_synch_all(pipe, dev);
  }
  else if(pipe->changed & DT_DEV_PIPE_TOP_CHANGED)
  {
    // only top history item changed.
    // FIXME: this seems to never be called.
    dt_dev_pixelpipe_synch_top(pipe, dev);
  }

  pipe->changed = DT_DEV_PIPE_UNCHANGED;

  // Get the final output size of the pipe, for GUI coordinates mapping between image buffer and window
  dt_dev_pixelpipe_get_roi_out(pipe, dev, pipe->iwidth, pipe->iheight, &pipe->processed_width,
                                  &pipe->processed_height);

  // TODO: if DT_DEV_PIPE_TOP_CHANGED, compute global hash only for the top ?
  dt_pthread_mutex_unlock(&dev->history_mutex);

  dt_show_times(&start, "[dt_dev_pixelpipe_change] pipeline resync on the current modules stack");
}

// TODO:
void dt_dev_pixelpipe_add_node(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int n)
{
}
// TODO:
void dt_dev_pixelpipe_remove_node(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int n)
{
}

static void get_output_format(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                              dt_develop_t *dev, dt_iop_buffer_dsc_t *dsc)
{
  if(module) return module->output_format(module, pipe, piece, dsc);

  // first input.
  *dsc = pipe->image.buf_dsc;

  if(!(dt_image_is_raw(&pipe->image)))
  {
    // image max is normalized before
    for(int k = 0; k < 4; k++) dsc->processed_maximum[k] = 1.0f;
  }
}


// helper to get per module histogram
static void histogram_collect(dt_dev_pixelpipe_iop_t *piece, const void *pixel, const dt_iop_roi_t *roi,
                              uint32_t **histogram, uint32_t *histogram_max)
{
  dt_dev_histogram_collection_params_t histogram_params = piece->histogram_params;

  dt_histogram_roi_t histogram_roi;

  // if the current module does did not specified its own ROI, use the full ROI
  if(histogram_params.roi == NULL)
  {
    histogram_roi = (dt_histogram_roi_t){
      .width = roi->width, .height = roi->height, .crop_x = 0, .crop_y = 0, .crop_width = 0, .crop_height = 0
    };

    histogram_params.roi = &histogram_roi;
  }

  const dt_iop_colorspace_type_t cst = piece->module->input_colorspace(piece->module, piece->pipe, piece);

  dt_histogram_helper(&histogram_params, &piece->histogram_stats, cst, piece->module->histogram_cst, pixel, histogram,
      piece->module->histogram_middle_grey, dt_ioppr_get_pipe_work_profile_info(piece->pipe));
  dt_histogram_max_helper(&piece->histogram_stats, cst, piece->module->histogram_cst, histogram, histogram_max);
}

#ifdef HAVE_OPENCL
// helper to get per module histogram for OpenCL
//
// this algorithm is inefficient as hell when it comes to larger images. it's only acceptable
// as long as we work on small image sizes like in image preview
static void histogram_collect_cl(int devid, dt_dev_pixelpipe_iop_t *piece, cl_mem img,
                                 const dt_iop_roi_t *roi, uint32_t **histogram, uint32_t *histogram_max,
                                 float *buffer, size_t bufsize)
{
  float *tmpbuf = NULL;
  float *pixel = NULL;

  // if buffer is supplied and if size fits let's use it
  if(buffer && bufsize >= (size_t)roi->width * roi->height * 4 * sizeof(float))
    pixel = buffer;
  else
    pixel = tmpbuf = dt_alloc_align_float((size_t)4 * roi->width * roi->height);

  if(!pixel) return;

  cl_int err = dt_opencl_copy_device_to_host(devid, pixel, img, roi->width, roi->height, sizeof(float) * 4);
  if(err != CL_SUCCESS)
  {
    if(tmpbuf) dt_free_align(tmpbuf);
    return;
  }

  dt_dev_histogram_collection_params_t histogram_params = piece->histogram_params;

  dt_histogram_roi_t histogram_roi;

  // if the current module does did not specified its own ROI, use the full ROI
  if(histogram_params.roi == NULL)
  {
    histogram_roi = (dt_histogram_roi_t){
      .width = roi->width, .height = roi->height, .crop_x = 0, .crop_y = 0, .crop_width = 0, .crop_height = 0
    };

    histogram_params.roi = &histogram_roi;
  }

  const dt_iop_colorspace_type_t cst = piece->module->input_colorspace(piece->module, piece->pipe, piece);

  dt_histogram_helper(&histogram_params, &piece->histogram_stats, cst, piece->module->histogram_cst, pixel, histogram,
      piece->module->histogram_middle_grey, dt_ioppr_get_pipe_work_profile_info(piece->pipe));
  dt_histogram_max_helper(&piece->histogram_stats, cst, piece->module->histogram_cst, histogram, histogram_max);

  if(tmpbuf) dt_free_align(tmpbuf);
}
#endif


dt_backbuf_t * _get_backuf(dt_develop_t *dev, const char *op)
{
  if(!strcmp(op, "demosaic"))
    return &dev->raw_histogram;
  else if(!strcmp(op, "colorout"))
    return &dev->output_histogram;
  else if(!strcmp(op, "gamma"))
    return &dev->display_histogram;
  else
    return NULL;
}

static void pixelpipe_get_histogram_backbuf(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                            void *output, void *cl_mem_output,
                                            dt_iop_buffer_dsc_t *out_format, const dt_iop_roi_t *roi,
                                            dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                            const uint64_t hash, const size_t bpp)
{
  // Runs only on full image but downscaled for perf, aka preview pipe
  if(((pipe->type & DT_DEV_PIXELPIPE_PREVIEW) != DT_DEV_PIXELPIPE_PREVIEW) || piece == NULL || !piece->enabled) return;

  // Not an RGBa float buffer ?
  if(!((bpp == 4 * sizeof(float)) || (bpp == 4 * sizeof(uint8_t)))) return;

  dt_backbuf_t *backbuf = _get_backuf(dev, module->op);
  if(backbuf == NULL) return; // This module is not wired to global histograms
  if(backbuf->hash == hash) return; // Hash didn't change, nothing to update.

  // Prepare the buffer if needed
  if(backbuf->buffer == NULL)
  {
    // Buffer uninited
    backbuf->buffer = dt_alloc_align(roi->width * roi->height * bpp);
    backbuf->height = roi->height;
    backbuf->width = roi->width;
    backbuf->bpp = bpp;
  }
  else if((backbuf->height != roi->height) || (backbuf->width != roi->width) || (backbuf->bpp != bpp))
  {
    // Cached buffer size doesn't match current one.
    // There is no reason yet why this should happen because the preview pipe doesn't change size during its lifetime.
    // But let's future-proof it in case someone gets creative.
    dt_free_align(backbuf->buffer); // maybe write a dt_realloc_align routine ?
    backbuf->buffer = dt_alloc_align(roi->width * roi->height * bpp);
    backbuf->height = roi->height;
    backbuf->width = roi->width;
    backbuf->bpp = bpp;
  }

  if(backbuf->buffer == NULL)
  {
    // Out of memory to allocate. Notify histogram
    backbuf->hash = -1;
    return;
  }

  // Integrity hash, mixing interal module params state, and params states of previous modules in pipe.
  backbuf->hash = hash;

  // Copy to histogram cache
  dt_times_t start;
  dt_get_times(&start);

#ifdef HAVE_OPENCL
  if(cl_mem_output && module->process_cl && piece->process_cl_ready)
  {
    cl_int err = dt_opencl_copy_device_to_host(pipe->devid, backbuf->buffer, cl_mem_output, roi->width, roi->height, bpp);

    // Notify the histogram that the backbuf is unusable
    if(err != CL_SUCCESS) backbuf->hash = -1;
  }
  else if(output)
  {
    _copy_buffer(output, (char *)backbuf->buffer, roi->height, roi->width, roi->width, 0, 0, roi->width * bpp, bpp);
  }
#else
  if(output)
    _copy_buffer(output, (char *)backbuf->buffer, roi->height, roi->width, roi->width, 0, 0, roi->width * bpp, bpp);
#endif

  // gamma outputs uint8, but its bpp count is still 16 like the modules outputting float32
  if(!strcmp(module->op, "gamma") || bpp == 4 * sizeof(uint8_t))
  {
    // We got 8 bits data, we need to convert it back to float32 for uniform handling
    float *new_buffer = dt_alloc_align(roi->width * roi->height * 4 * sizeof(float));
    if(new_buffer == NULL) return;

    uint8_t *old_buffer = (uint8_t *)backbuf->buffer;
    _uint8_to_float(old_buffer, new_buffer, roi->width, roi->height, 4);
    backbuf->buffer = (void *)new_buffer;
    dt_free_align(old_buffer);
  }

  dt_show_times_f(&start, "[dev_pixelpipe]", "copying global histogram for %s", module->op);

  // That's all. From there, histogram catches the "preview pipeline finished recomputing" signal and redraws if needed.
  // We don't manage thread locks because there is only one writing point and one reading point, synchronized
  // through signal & callback.

  // Note that we don't compute the histogram here because, depending on the type of scope requested in GUI,
  // intermediate color conversions might be needed (vectorscope) or various pixel binnings required (waveform).
  // Color conversions and binning are deferred to the GUI thread, prior to drawing update.
}


// helper for per-module color picking
static int pixelpipe_picker_helper(dt_iop_module_t *module, const dt_iop_roi_t *roi, dt_aligned_pixel_t picked_color,
                                   dt_aligned_pixel_t picked_color_min, dt_aligned_pixel_t picked_color_max,
                                   dt_pixelpipe_picker_source_t picker_source, int *box)
{
  const float wd = darktable.develop->preview_pipe->backbuf_width;
  const float ht = darktable.develop->preview_pipe->backbuf_height;
  const int width = roi->width;
  const int height = roi->height;
  const dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;

  dt_boundingbox_t fbox = { 0.0f };

  // get absolute pixel coordinates in final preview image
  if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
  {
    for(int k = 0; k < 4; k += 2) fbox[k] = sample->box[k] * wd;
    for(int k = 1; k < 4; k += 2) fbox[k] = sample->box[k] * ht;
  }
  else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
  {
    fbox[0] = fbox[2] = sample->point[0] * wd;
    fbox[1] = fbox[3] = sample->point[1] * ht;
  }

  // transform back to current module coordinates
  dt_dev_distort_backtransform_plus(darktable.develop, darktable.develop->preview_pipe, module->iop_order,
                               ((picker_source == PIXELPIPE_PICKER_INPUT) ? DT_DEV_TRANSFORM_DIR_FORW_INCL
                               : DT_DEV_TRANSFORM_DIR_FORW_EXCL),fbox, 2);

  fbox[0] -= roi->x;
  fbox[1] -= roi->y;
  fbox[2] -= roi->x;
  fbox[3] -= roi->y;

  // re-order edges of bounding box
  box[0] = fminf(fbox[0], fbox[2]);
  box[1] = fminf(fbox[1], fbox[3]);
  box[2] = fmaxf(fbox[0], fbox[2]);
  box[3] = fmaxf(fbox[1], fbox[3]);

  if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
  {
    // if we are sampling one point, make sure that we actually sample it.
    for(int k = 2; k < 4; k++) box[k] += 1;
  }

  // do not continue if box is completely outside of roi
  if(box[0] >= width || box[1] >= height || box[2] < 0 || box[3] < 0) return 1;

  // clamp bounding box to roi
  for(int k = 0; k < 4; k += 2) box[k] = MIN(width - 1, MAX(0, box[k]));
  for(int k = 1; k < 4; k += 2) box[k] = MIN(height - 1, MAX(0, box[k]));

  // safety check: area needs to have minimum 1 pixel width and height
  if(box[2] - box[0] < 1 || box[3] - box[1] < 1) return 1;

  return 0;
}

static void pixelpipe_picker(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_iop_buffer_dsc_t *dsc,
                             const float *pixel, const dt_iop_roi_t *roi, float *picked_color,
                             float *picked_color_min, float *picked_color_max,
                             const dt_iop_colorspace_type_t image_cst, dt_pixelpipe_picker_source_t picker_source)
{
  int box[4] = { 0 };

  if(pixelpipe_picker_helper(module, roi, picked_color, picked_color_min, picked_color_max, picker_source, box))
  {
    for(int k = 0; k < 4; k++)
    {
      picked_color_min[k] = INFINITY;
      picked_color_max[k] = -INFINITY;
      picked_color[k] = 0.0f;
    }

    return;
  }

  dt_aligned_pixel_t min, max, avg;
  for(int k = 0; k < 4; k++)
  {
    min[k] = INFINITY;
    max[k] = -INFINITY;
    avg[k] = 0.0f;
  }

  const dt_iop_order_iccprofile_info_t *const profile = dt_ioppr_get_pipe_current_profile_info(module, piece->pipe);
  dt_color_picker_helper(dsc, pixel, roi, box, avg, min, max, image_cst,
                         dt_iop_color_picker_get_active_cst(module), profile);

  for(int k = 0; k < 4; k++)
  {
    picked_color_min[k] = min[k];
    picked_color_max[k] = max[k];
    picked_color[k] = avg[k];
  }
}


#ifdef HAVE_OPENCL
// helper for OpenCL color picking
//
// this algorithm is inefficient as hell when it comes to larger images. it's only acceptable
// as long as we work on small image sizes like in image preview
static void pixelpipe_picker_cl(int devid, dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                dt_iop_buffer_dsc_t *dsc, cl_mem img, const dt_iop_roi_t *roi,
                                float *picked_color, float *picked_color_min, float *picked_color_max,
                                float *buffer, size_t bufsize, const dt_iop_colorspace_type_t image_cst,
                                dt_pixelpipe_picker_source_t picker_source)
{
  int box[4] = { 0 };

  if(pixelpipe_picker_helper(module, roi, picked_color, picked_color_min, picked_color_max, picker_source, box))
  {
    for(int k = 0; k < 4; k++)
    {
      picked_color_min[k] = INFINITY;
      picked_color_max[k] = -INFINITY;
      picked_color[k] = 0.0f;
    }

    return;
  }

  const size_t origin[3] = { box[0], box[1], 0 };
  const size_t region[3] = { box[2] - box[0], box[3] - box[1], 1 };

  float *pixel = NULL;
  float *tmpbuf = NULL;

  const size_t size = region[0] * region[1];

  const size_t bpp = dt_iop_buffer_dsc_to_bpp(dsc);

  // if a buffer is supplied and if size fits let's use it
  if(buffer && bufsize >= size * bpp)
    pixel = buffer;
  else
    pixel = tmpbuf = dt_alloc_align(size * bpp);

  if(pixel == NULL) return;

  // get the required part of the image from opencl device
  cl_int err = dt_opencl_read_host_from_device_raw(devid, pixel, img, origin, region, region[0] * bpp, CL_TRUE);

  if(err != CL_SUCCESS) goto error;

  dt_iop_roi_t roi_copy = (dt_iop_roi_t){.x = roi->x + box[0], .y = roi->y + box[1], .width = region[0], .height = region[1] };

  box[0] = 0;
  box[1] = 0;
  box[2] = region[0];
  box[3] = region[1];

  dt_aligned_pixel_t min, max, avg;
  for(int k = 0; k < 4; k++)
  {
    min[k] = INFINITY;
    max[k] = -INFINITY;
    avg[k] = 0.0f;
  }

  const dt_iop_order_iccprofile_info_t *const profile = dt_ioppr_get_pipe_current_profile_info(module, piece->pipe);
  dt_color_picker_helper(dsc, pixel, &roi_copy, box, avg, min, max, image_cst,
                         dt_iop_color_picker_get_active_cst(module), profile);

  for(int k = 0; k < 4; k++)
  {
    picked_color_min[k] = min[k];
    picked_color_max[k] = max[k];
    picked_color[k] = avg[k];
  }

error:
  dt_free_align(tmpbuf);
}
#endif

static void _pixelpipe_pick_from_image(dt_iop_module_t *module,
                                       const float *const pixel, const dt_iop_roi_t *roi_in,
                                       const dt_iop_order_iccprofile_info_t *const display_profile,
                                       dt_colorpicker_sample_t *const sample)
{
  if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
  {
    const int box[4] = {
      MIN(roi_in->width - 1,  MAX(0, sample->box[0] * roi_in->width)),
      MIN(roi_in->height - 1, MAX(0, sample->box[1] * roi_in->height)),
      MIN(roi_in->width - 1,  MAX(0, sample->box[2] * roi_in->width)),
      MIN(roi_in->height - 1, MAX(0, sample->box[3] * roi_in->height))
    };
    const int box_pixels = (box[3] - box[1] + 1) * (box[2] - box[0] + 1);
    lib_colorpicker_sample_statistics picked_rgb = { { 0.0f },
                                                     { FLT_MAX, FLT_MAX, FLT_MAX },
                                                     { FLT_MIN, FLT_MIN, FLT_MIN } };
    dt_aligned_pixel_t acc = { 0.0f };

    for(int j = box[1]; j <= box[3]; j++)
      for(int i = box[0]; i <= box[2]; i++)
      {
        for_each_channel(ch, aligned(picked_rgb, acc) aligned(pixel:64))
        {
          const float v = pixel[4 * (roi_in->width * j + i) + ch];
          picked_rgb[DT_LIB_COLORPICKER_STATISTIC_MIN][ch]
              = MIN(picked_rgb[DT_LIB_COLORPICKER_STATISTIC_MIN][ch], v);
          picked_rgb[DT_LIB_COLORPICKER_STATISTIC_MAX][ch]
              = MAX(picked_rgb[DT_LIB_COLORPICKER_STATISTIC_MAX][ch], v);
          acc[ch] += v;
        }
      }
    for_each_channel(ch, aligned(picked_rgb, acc:16))
      picked_rgb[DT_LIB_COLORPICKER_STATISTIC_MEAN][ch] = acc[ch] / box_pixels;

    // convenient to have pixels in display profile, which makes them easy to display
    memcpy(sample->display[0], picked_rgb[0], sizeof(lib_colorpicker_sample_statistics));

    // NOTE: conversions assume that dt_aligned_pixel_t[x] has no
    // padding, e.g. is equivalent to float[x*4], and that on failure
    // it's OK not to touch output
    int converted_cst;
    dt_ioppr_transform_image_colorspace(module, picked_rgb[0], sample->lab[0], 3, 1, IOP_CS_RGB, IOP_CS_LAB,
                                        &converted_cst, display_profile);

    // NOTE: Previously, the scope member is used to convert display profile
    // color into histogram profile color, but we dropped histogram profile in
    // Ansel and just use display profile for histogram. So assign the same
    // value here.
    memcpy(sample->scope[0], sample->display[0], sizeof(lib_colorpicker_sample_statistics));
  }
  else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
  {
    const int x = MIN(roi_in->width - 1, MAX(0, sample->point[0] * roi_in->width));
    const int y = MIN(roi_in->height - 1, MAX(0, sample->point[1] * roi_in->height));
    int converted_cst;
    // mean = min = max == pixel sample, so only need to do colorspace work on a single point
    memcpy(sample->display[0], pixel + 4 * (roi_in->width * y + x), sizeof(dt_aligned_pixel_t));
    dt_ioppr_transform_image_colorspace(module, sample->display[0], sample->lab[0], 1, 1, IOP_CS_RGB, IOP_CS_LAB,
                                        &converted_cst, display_profile);
    // NOTE: Previously, the scope member is used to convert display profile
    // color into histogram profile color, but we dropped histogram profile in
    // Ansel and just use display profile for histogram. So assign the same
    // value here.
    memcpy(sample->scope[0], sample->display[0], sizeof(dt_aligned_pixel_t));

    for(dt_lib_colorpicker_statistic_t stat = 1; stat < DT_LIB_COLORPICKER_STATISTIC_N; stat++)
    {
      memcpy(sample->display[stat], sample->display[0], sizeof(dt_aligned_pixel_t));
      memcpy(sample->lab[stat], sample->lab[0], sizeof(dt_aligned_pixel_t));
      memcpy(sample->scope[stat], sample->scope[0], sizeof(dt_aligned_pixel_t));
    }
  }
}

static void _pixelpipe_pick_samples(dt_develop_t *dev, dt_iop_module_t *module,
                                    const float *const input, const dt_iop_roi_t *roi_in)
{
  const dt_iop_order_iccprofile_info_t *const display_profile
    = dt_ioppr_add_profile_info_to_list(dev, darktable.color_profiles->display_type,
                                        darktable.color_profiles->display_filename, INTENT_RELATIVE_COLORIMETRIC);

  GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
  while(samples)
  {
    dt_colorpicker_sample_t *sample = samples->data;
    if(!sample->locked)
      _pixelpipe_pick_from_image(module, input, roi_in, display_profile, sample);
    samples = g_slist_next(samples);
  }

  if(darktable.lib->proxy.colorpicker.picker_proxy)
    _pixelpipe_pick_from_image(module, input, roi_in, display_profile,
                               darktable.lib->proxy.colorpicker.primary_sample);
}

// returns 1 if blend process need the module default colorspace
static gboolean _transform_for_blend(const dt_iop_module_t *const self, const dt_dev_pixelpipe_iop_t *const piece)
{
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *)piece->blendop_data;
  if(d)
  {
    // check only if blend is active
    if((self->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && (d->mask_mode != DEVELOP_MASK_DISABLED))
    {
      return TRUE;
    }
  }
  return FALSE;
}

static dt_iop_colorspace_type_t _transform_for_picker(dt_iop_module_t *self, const dt_iop_colorspace_type_t cst)
{
  const dt_iop_colorspace_type_t picker_cst =
    dt_iop_color_picker_get_active_cst(self);

  switch(picker_cst)
  {
    case IOP_CS_RAW:
      return IOP_CS_RAW;
    case IOP_CS_LAB:
    case IOP_CS_LCH:
      return IOP_CS_LAB;
    case IOP_CS_RGB:
    case IOP_CS_HSL:
    case IOP_CS_JZCZHZ:
      return IOP_CS_RGB;
    case IOP_CS_NONE:
      // IOP_CS_NONE is used by temperature.c as it may work in RAW or RGB
      // return the pipe color space to avoid any additional conversions
      return cst;
    default:
      return picker_cst;
  }
}

static gboolean _request_color_pick(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, dt_iop_module_t *module)
{
  // Does the current active module need a picker?
  return
    // pick from preview pipe to get pixels outside the viewport
    dev->gui_attached && pipe == dev->preview_pipe
    // there is an active picker widget
    && darktable.lib->proxy.colorpicker.picker_proxy
    // only modules with focus can pick
    && module == dev->gui_module
    // and they are enabled
    && dev->gui_module->enabled
    // and they want to pick ;)
    && module->request_color_pick != DT_REQUEST_COLORPICK_OFF;
}

static void collect_histogram_on_CPU(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                     float *input, const dt_iop_roi_t *roi_in,
                                     dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                     dt_pixelpipe_flow_t *pixelpipe_flow)
{
  // histogram collection for module
  if((dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
     && (piece->request_histogram & DT_REQUEST_ON))
  {
    histogram_collect(piece, input, roi_in, &(piece->histogram), piece->histogram_max);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_HISTOGRAM_ON_CPU);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_HISTOGRAM_NONE | PIXELPIPE_FLOW_HISTOGRAM_ON_GPU);

    if(piece->histogram && (module->request_histogram & DT_REQUEST_ON)
       && (pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW)
    {
      const size_t buf_size = 4 * piece->histogram_stats.bins_count * sizeof(uint32_t);
      module->histogram = realloc(module->histogram, buf_size);
      memcpy(module->histogram, piece->histogram, buf_size);
      module->histogram_stats = piece->histogram_stats;
      memcpy(module->histogram_max, piece->histogram_max, sizeof(piece->histogram_max));
      if(module->widget)
        dt_control_queue_redraw_widget(module->widget);
    }
  }
  return;
}

#define KILL_SWITCH_ABORT                                             \
  if(dt_atomic_get_int(&pipe->shutdown))                              \
  {                                                                   \
    if (*cl_mem_output != NULL)                                       \
    {                                                                 \
      dt_opencl_release_mem_object(*cl_mem_output);                   \
      *cl_mem_output = NULL;                                          \
    }                                                                 \
    return 1;                                                         \
  }

// Once we have a cache, stopping computation before full completion
// has good chances of leaving it corrupted. So we invalidate it.
#define KILL_SWITCH_AND_FLUSH_CACHE                                     \
  if(dt_atomic_get_int(&pipe->shutdown))                                \
  {                                                                     \
    dt_dev_pixelpipe_cache_invalidate(&(pipe->cache), input);           \
    dt_dev_pixelpipe_cache_invalidate(&(pipe->cache), *output);         \
    if (*cl_mem_output != NULL)                                         \
    {                                                                   \
      dt_opencl_release_mem_object(*cl_mem_output);                     \
      *cl_mem_output = NULL;                                            \
    }                                                                   \
    return 1;                                                           \
  }                                                                     \

static int pixelpipe_process_on_CPU(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                    float *input, dt_iop_buffer_dsc_t *input_format, const dt_iop_roi_t *roi_in,
                                    void **output, dt_iop_buffer_dsc_t **out_format, const dt_iop_roi_t *roi_out,
                                    dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                    dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow)
{
  // Fetch RGB working profile
  // if input is RAW, we can't color convert because RAW is not in a color space
  // so we send NULL to by-pass
  const dt_iop_order_iccprofile_info_t *const work_profile
      = (input_format->cst != IOP_CS_RAW) ? dt_ioppr_get_pipe_work_profile_info(pipe) : NULL;

  // transform to module input colorspace
  dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height, input_format->cst,
                                      module->input_colorspace(module, pipe, piece), &input_format->cst,
                                      work_profile);

  //fprintf(stdout, "input color space for %s : %i\n", module->op, module->input_colorspace(module, pipe, piece));

  collect_histogram_on_CPU(pipe, dev, input, roi_in, module, piece, pixelpipe_flow);

  const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);
  const size_t bpp = dt_iop_buffer_dsc_to_bpp(*out_format);

  const gboolean fitting = dt_tiling_piece_fits_host_memory(MAX(roi_in->width, roi_out->width),
                                       MAX(roi_in->height, roi_out->height), MAX(in_bpp, bpp),
                                          tiling->factor, tiling->overhead);

  /* process module on cpu. use tiling if needed and possible. */
  if(!fitting && piece->process_tiling_ready)
  {
    module->process_tiling(module, piece, input, *output, roi_in, roi_out, in_bpp);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU);
  }
  else
  {
    if(!fitting)
      fprintf(stderr, "[pixelpipe_process_on_CPU] Warning: processes `%s' even if memory requirements are not met\n", module->op);

    module->process(module, piece, input, *output, roi_in, roi_out);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
  }

  // and save the output colorspace
  pipe->dsc.cst = module->output_colorspace(module, pipe, piece);

  // Lab color picking for module
  if(_request_color_pick(pipe, dev, module))
  {
    // ensure that we are using the right color space
    dt_iop_colorspace_type_t picker_cst = _transform_for_picker(module, pipe->dsc.cst);
    dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height,
                                        input_format->cst, picker_cst, &input_format->cst,
                                        work_profile);
    dt_ioppr_transform_image_colorspace(module, *output, *output, roi_out->width, roi_out->height,
                                        pipe->dsc.cst, picker_cst, &pipe->dsc.cst,
                                        work_profile);

    pixelpipe_picker(module, piece, &piece->dsc_in, (float *)input, roi_in, module->picked_color,
                     module->picked_color_min, module->picked_color_max, input_format->cst, PIXELPIPE_PICKER_INPUT);
    pixelpipe_picker(module, piece, &pipe->dsc, (float *)(*output), roi_out, module->picked_output_color,
                     module->picked_output_color_min, module->picked_output_color_max,
                     pipe->dsc.cst, PIXELPIPE_PICKER_OUTPUT);

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PICKERDATA_READY, module, piece);
  }

  // blend needs input/output images with default colorspace
  if(_transform_for_blend(module, piece))
  {
    dt_iop_colorspace_type_t blend_cst = dt_develop_blend_colorspace(piece, pipe->dsc.cst);
    dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height,
                                        input_format->cst, blend_cst, &input_format->cst,
                                        work_profile);
    dt_ioppr_transform_image_colorspace(module, *output, *output, roi_out->width, roi_out->height,
                                        pipe->dsc.cst, blend_cst, &pipe->dsc.cst,
                                        work_profile);
  }

  /* process blending on CPU */
  dt_develop_blend_process(module, piece, input, *output, roi_in, roi_out);
  *pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);
  *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);

  return 0; //no errors
}

static dt_dev_pixelpipe_iop_t *_last_node_in_pipe(dt_dev_pixelpipe_t *pipe)
{
  for(GList *node = g_list_last(pipe->nodes); node; node = g_list_previous(node))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)node->data;
    if(piece->enabled) return piece;
  }

  return NULL;
}

#ifdef HAVE_OPENCL
static int pixelpipe_process_on_GPU(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                    float *input, void *cl_mem_input, dt_iop_buffer_dsc_t *input_format, const dt_iop_roi_t *roi_in,
                                    void **output, void **cl_mem_output, dt_iop_buffer_dsc_t **out_format, const dt_iop_roi_t *roi_out,
                                    dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                    dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow,
                                    const size_t in_bpp, const size_t bpp)
{
  // Fetch RGB working profile
  // if input is RAW, we can't color convert because RAW is not in a color space
  // so we send NULL to by-pass
  const dt_iop_order_iccprofile_info_t *const work_profile
      = (input_format->cst != IOP_CS_RAW) ? dt_ioppr_get_pipe_work_profile_info(pipe) : NULL;

  /* do we have opencl at all? did user tell us to use it? did we get a resource? */
  if(dt_opencl_is_inited() && pipe->opencl_enabled && pipe->devid >= 0)
  {
    gboolean success_opencl = TRUE;
    dt_iop_colorspace_type_t input_cst_cl = input_format->cst;

    /* if input is on gpu memory only, remember this fact to later take appropriate action */
    gboolean valid_input_on_gpu_only = (cl_mem_input != NULL);

    const float required_factor_cl = fmaxf(1.0f, (valid_input_on_gpu_only) ? tiling->factor_cl - 1.0f : tiling->factor_cl);
    /* pre-check if there is enough space on device for non-tiled processing */
    const gboolean fits_on_device = dt_opencl_image_fits_device(pipe->devid, MAX(roi_in->width, roi_out->width),
                                                                MAX(roi_in->height, roi_out->height), MAX(in_bpp, bpp),
                                                                required_factor_cl, tiling->overhead);

    /* general remark: in case of opencl errors within modules or out-of-memory on GPU, we transparently
       fall back to the respective cpu module and continue in pixelpipe. If we encounter errors we set
       pipe->opencl_error=1, return this function with value 1, and leave appropriate action to the calling
       function, which normally would restart pixelpipe without opencl.
       Late errors are sometimes detected when trying to get back data from device into host memory and
       are treated in the same manner. */

    /* test for a possible opencl path after checking some module specific pre-requisites */
    gboolean possible_cl = (module->process_cl && piece->process_cl_ready
       && !(((pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW
             || (pipe->type & DT_DEV_PIXELPIPE_PREVIEW2) == DT_DEV_PIXELPIPE_PREVIEW2)
            && (module->flags() & IOP_FLAGS_PREVIEW_NON_OPENCL))
       && (fits_on_device || piece->process_tiling_ready));

    if(possible_cl && !fits_on_device)
    {
      const float cl_px = dt_opencl_get_device_available(pipe->devid) / (sizeof(float) * MAX(in_bpp, bpp) * ceilf(required_factor_cl));
      const float dx = MAX(roi_in->width, roi_out->width);
      const float dy = MAX(roi_in->height, roi_out->height);
      const float border = tiling->overlap + 1;
      /* tests for required gpu mem reflects the different tiling stategies.
         simple tiles over whole height or width or inside rectangles where we need at last the overlapping area.
      */
      const gboolean possible = (cl_px > dx * border) || (cl_px > dy * border) || (cl_px > border * border);
      if(!possible)
      {
        dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING, "[dt_dev_pixelpipe_process_rec] CL: tiling impossible in module `%s'. avail=%.1fM, requ=%.1fM (%ix%i). overlap=%i\n",
            module->op, cl_px / 1e6f, dx*dy / 1e6f, (int)dx, (int)dy, (int)tiling->overlap);
        possible_cl = FALSE;
      }
    }

    if(possible_cl)
    {
      if(fits_on_device)
      {
        /* image is small enough -> try to directly process entire image with opencl */
        /* input is not on gpu memory -> copy it there */
        if(cl_mem_input == NULL)
        {
          cl_mem_input = dt_opencl_alloc_device(pipe->devid, roi_in->width, roi_in->height, in_bpp);
          if(cl_mem_input == NULL)
          {
            dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't generate input buffer for module %s\n",
                     module->op);
            success_opencl = FALSE;
          }

          if(success_opencl)
          {
            cl_int err = dt_opencl_write_host_to_device(pipe->devid, input, cl_mem_input,
                                                                     roi_in->width, roi_in->height, in_bpp);
            if(err != CL_SUCCESS)
            {
              dt_print(DT_DEBUG_OPENCL,
                       "[opencl_pixelpipe] couldn't copy image to opencl device for module %s\n",
                       module->op);
              success_opencl = FALSE;
            }
          }
        }

        /* try to allocate GPU memory for output */
        if(success_opencl)
        {
          *cl_mem_output = dt_opencl_alloc_device(pipe->devid, roi_out->width, roi_out->height, bpp);
          if(*cl_mem_output == NULL)
          {
            dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't allocate output buffer for module %s\n",
                     module->op);
            success_opencl = FALSE;
          }
        }

        // fprintf(stderr, "[opencl_pixelpipe 2] for module `%s', have bufs %p and %p \n", module->op,
        // cl_mem_input, *cl_mem_output);

        // indirectly give gpu some air to breathe (and to do display related stuff)
        dt_iop_nap(dt_opencl_micro_nap(pipe->devid));

        // transform to input colorspace
        if(success_opencl)
        {
          success_opencl = dt_ioppr_transform_image_colorspace_cl(
              module, piece->pipe->devid, cl_mem_input, cl_mem_input, roi_in->width, roi_in->height, input_cst_cl,
              module->input_colorspace(module, pipe, piece), &input_cst_cl,
              work_profile);
        }

        // histogram collection for module
        if(success_opencl && (dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
           && (piece->request_histogram & DT_REQUEST_ON))
        {
          // we abuse the empty output buffer on host for intermediate storage of data in
          // histogram_collect_cl()
          size_t outbufsize = bpp * roi_out->width * roi_out->height;

          histogram_collect_cl(pipe->devid, piece, cl_mem_input, roi_in, &(piece->histogram),
                               piece->histogram_max, *output, outbufsize);
          *pixelpipe_flow |= (PIXELPIPE_FLOW_HISTOGRAM_ON_GPU);
          *pixelpipe_flow &= ~(PIXELPIPE_FLOW_HISTOGRAM_NONE | PIXELPIPE_FLOW_HISTOGRAM_ON_CPU);

          if(piece->histogram && (module->request_histogram & DT_REQUEST_ON)
             && (pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW)
          {
            const size_t buf_size = sizeof(uint32_t) * 4 * piece->histogram_stats.bins_count;
            module->histogram = realloc(module->histogram, buf_size);
            memcpy(module->histogram, piece->histogram, buf_size);
            module->histogram_stats = piece->histogram_stats;
            memcpy(module->histogram_max, piece->histogram_max, sizeof(piece->histogram_max));

            if(module->widget) dt_control_queue_redraw_widget(module->widget);
          }
        }

        /* now call process_cl of module; module should emit meaningful messages in case of error */
        if(success_opencl)
        {
          success_opencl
              = module->process_cl(module, piece, cl_mem_input, *cl_mem_output, roi_in, roi_out);
          *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_GPU);
          *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);

          // and save the output colorspace
          pipe->dsc.cst = module->output_colorspace(module, pipe, piece);
        }

        // Lab color picking for module
        if(success_opencl && _request_color_pick(pipe, dev, module))
        {
          // ensure that we are using the right color space
          dt_iop_colorspace_type_t picker_cst = _transform_for_picker(module, pipe->dsc.cst);
          success_opencl = dt_ioppr_transform_image_colorspace_cl(
              module, piece->pipe->devid, cl_mem_input, cl_mem_input, roi_in->width, roi_in->height,
              input_cst_cl, picker_cst, &input_cst_cl, work_profile);
          success_opencl &= dt_ioppr_transform_image_colorspace_cl(
              module, piece->pipe->devid, *cl_mem_output, *cl_mem_output, roi_out->width, roi_out->height,
              pipe->dsc.cst, picker_cst, &pipe->dsc.cst, work_profile);

          // we abuse the empty output buffer on host for intermediate storage of data in
          // pixelpipe_picker_cl()
          const size_t outbufsize = bpp * roi_out->width * roi_out->height;

          pixelpipe_picker_cl(pipe->devid, module, piece, &piece->dsc_in, cl_mem_input, roi_in,
                              module->picked_color, module->picked_color_min, module->picked_color_max,
                              *output, outbufsize, input_cst_cl, PIXELPIPE_PICKER_INPUT);
          pixelpipe_picker_cl(pipe->devid, module, piece, &pipe->dsc, (*cl_mem_output), roi_out,
                              module->picked_output_color, module->picked_output_color_min,
                              module->picked_output_color_max, *output, outbufsize, pipe->dsc.cst,
                              PIXELPIPE_PICKER_OUTPUT);

          DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PICKERDATA_READY, module, piece);
        }

        // blend needs input/output images with default colorspace
        if(success_opencl && _transform_for_blend(module, piece))
        {
          dt_iop_colorspace_type_t blend_cst = dt_develop_blend_colorspace(piece, pipe->dsc.cst);
          success_opencl = dt_ioppr_transform_image_colorspace_cl(
              module, piece->pipe->devid, cl_mem_input, cl_mem_input, roi_in->width, roi_in->height,
              input_cst_cl, blend_cst, &input_cst_cl, work_profile);
          success_opencl &= dt_ioppr_transform_image_colorspace_cl(
              module, piece->pipe->devid, *cl_mem_output, *cl_mem_output, roi_out->width, roi_out->height,
              pipe->dsc.cst, blend_cst, &pipe->dsc.cst, work_profile);
        }

        /* process blending */
        if(success_opencl)
        {
          success_opencl
              = dt_develop_blend_process_cl(module, piece, cl_mem_input, *cl_mem_output, roi_in, roi_out);
          *pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_GPU);
          *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_CPU);
        }

        /* synchronization point for opencl pipe */
        if(success_opencl)
          success_opencl = dt_opencl_finish_sync_pipe(pipe->devid, pipe->type);

      }
      else if(piece->process_tiling_ready)
      {
        /* image is too big for direct opencl processing -> try to process image via tiling */

        // fprintf(stderr, "[opencl_pixelpipe 3] module '%s' tiling with process_tiling_cl\n", module->op);

        /* we might need to copy back valid image from device to host */
        if(cl_mem_input != NULL)
        {
          /* copy back to CPU buffer, then clean unneeded buffer */
          cl_int err = dt_opencl_copy_device_to_host(pipe->devid, input, cl_mem_input, roi_in->width, roi_in->height,
                                              in_bpp);
          if(err != CL_SUCCESS)
          {
            /* late opencl error */
            dt_print(
                DT_DEBUG_OPENCL,
                "[opencl_pixelpipe (a)] late opencl error detected while copying back to cpu buffer: %s\n", cl_errstr(err));
            dt_opencl_release_mem_object(cl_mem_input);
            pipe->opencl_error = 1;
            return 1;
          }
          else
            input_format->cst = input_cst_cl;
          dt_opencl_release_mem_object(cl_mem_input);
          cl_mem_input = NULL;
          valid_input_on_gpu_only = FALSE;
        }

        // indirectly give gpu some air to breathe (and to do display related stuff)
        dt_iop_nap(dt_opencl_micro_nap(pipe->devid));

        // transform to module input colorspace
        if(success_opencl)
        {
          dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height,
                                              input_format->cst, module->input_colorspace(module, pipe, piece),
                                              &input_format->cst, work_profile);
        }

        // histogram collection for module
        if (success_opencl)
        {
          collect_histogram_on_CPU(pipe, dev, input, roi_in, module, piece, pixelpipe_flow);
        }

        /* now call process_tiling_cl of module; module should emit meaningful messages in case of error */
        if(success_opencl)
        {
          success_opencl
              = module->process_tiling_cl(module, piece, input, *output, roi_in, roi_out, in_bpp);
          *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
          *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_CPU);

          // and save the output colorspace
          pipe->dsc.cst = module->output_colorspace(module, pipe, piece);
        }

        // Lab color picking for module
        if(success_opencl && _request_color_pick(pipe, dev, module))
        {
          // ensure that we are using the right color space
          dt_iop_colorspace_type_t picker_cst = _transform_for_picker(module, pipe->dsc.cst);
          // FIXME: don't need to transform entire image colorspace when just picking a point
          dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height,
                                              input_format->cst, picker_cst, &input_format->cst,
                                              work_profile);
          dt_ioppr_transform_image_colorspace(module, *output, *output, roi_out->width, roi_out->height,
                                              pipe->dsc.cst, picker_cst, &pipe->dsc.cst,
                                              work_profile);

          pixelpipe_picker(module, piece, &piece->dsc_in, (float *)input, roi_in, module->picked_color,
                           module->picked_color_min, module->picked_color_max, input_format->cst,
                           PIXELPIPE_PICKER_INPUT);
          pixelpipe_picker(module, piece, &pipe->dsc, (float *)(*output), roi_out, module->picked_output_color,
                           module->picked_output_color_min, module->picked_output_color_max,
                           pipe->dsc.cst, PIXELPIPE_PICKER_OUTPUT);

          DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PICKERDATA_READY, module, piece);
        }

        // blend needs input/output images with default colorspace
        if(success_opencl && _transform_for_blend(module, piece))
        {
          dt_iop_colorspace_type_t blend_cst = dt_develop_blend_colorspace(piece, pipe->dsc.cst);
          dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height,
                                              input_format->cst, blend_cst, &input_format->cst,
                                              work_profile);
          dt_ioppr_transform_image_colorspace(module, *output, *output, roi_out->width, roi_out->height,
                                              pipe->dsc.cst, blend_cst, &pipe->dsc.cst,
                                              work_profile);
        }

        /* do process blending on cpu (this is anyhow fast enough) */
        if(success_opencl)
        {
          dt_develop_blend_process(module, piece, input, *output, roi_in, roi_out);
          *pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);
          *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);
        }

        /* synchronization point for opencl pipe */
        if(success_opencl)
          success_opencl = dt_opencl_finish_sync_pipe(pipe->devid, pipe->type);

      }
      else
      {
        /* image is too big for direct opencl and tiling is not allowed -> no opencl processing for this
         * module */
        success_opencl = FALSE;
      }

      // if (rand() % 20 == 0) success_opencl = FALSE; // Test code: simulate spurious failures

      /* finally check, if we were successful */
      if(success_opencl)
      {
        /* Nice, everything went fine */

        /* OLD COMMENT:
           this is reasonable on slow GPUs only, where it's more expensive to reprocess the whole pixelpipe
           than regularly copying device buffers back to host. This would slow down fast GPUs considerably.
           NEW COMMENT:
           did you ever met diffuse and sharpen ?
           Also, not caching GPU buffers results in broken mask previews, that darktable handles
           by invalidating the cache completely, which requires a full pipeline computation each time.
           Also, since the cache actually works and can use a lot more memory, caching GPU output
           enables to bypass a serious number of modules, so the memory I/O cost is a good overall investment.
        */
        /* write back input into cache for faster re-usal (not for export or thumbnails) */
        if(cl_mem_input != NULL
            && (pipe->type & DT_DEV_PIXELPIPE_EXPORT) != DT_DEV_PIXELPIPE_EXPORT
            && (pipe->type & DT_DEV_PIXELPIPE_THUMBNAIL) != DT_DEV_PIXELPIPE_THUMBNAIL)
        {
          /* copy input to host memory, so we can find it in cache */
          cl_int err = dt_opencl_copy_device_to_host(pipe->devid, input, cl_mem_input, roi_in->width,
                                              roi_in->height, in_bpp);
          if(err != CL_SUCCESS)
          {
            /* late opencl error, not likely to happen here */
            dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe (e)] late opencl error detected while copying "
                                      "back to cpu buffer: %s\n", cl_errstr(err));
            /* that's all we do here, we later make sure to invalidate cache line */
          }
          else
          {
            /* success: cache line is valid now, so we will not need to invalidate it later */
            valid_input_on_gpu_only = FALSE;

            input_format->cst = input_cst_cl;
            // TODO: check if we need to wait for finished opencl pipe before we release cl_mem_input
            // dt_dev_finish(pipe->devid);
          }
        }

        /* we can now release cl_mem_input */
        dt_opencl_release_mem_object(cl_mem_input);
        cl_mem_input = NULL;
        // we speculate on the next plug-in to possibly copy back cl_mem_output to output,
        // so we're not just yet invalidating the (empty) output cache line.
      }
      else
      {
        /* Bad luck, opencl failed. Let's clean up and fall back to cpu module */
        dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] could not run module '%s' on gpu. falling back to cpu path\n",
                 module->op);

        // fprintf(stderr, "[opencl_pixelpipe 4] module '%s' running on cpu\n", module->op);

        /* we might need to free unused output buffer */
        if(*cl_mem_output != NULL)
        {
          dt_opencl_release_mem_object(*cl_mem_output);
          *cl_mem_output = NULL;
        }

        /* check where our input buffer is located */
        if(cl_mem_input != NULL)
        {
          /* copy back to host memory, then clean no longer needed opencl buffer.
             important info: in order to make this possible, opencl modules must
             not spoil their input buffer, even in case of errors. */
          cl_int err = dt_opencl_copy_device_to_host(pipe->devid, input, cl_mem_input, roi_in->width, roi_in->height,
                                              in_bpp);
          if(err != CL_SUCCESS)
          {
            /* late opencl error */
            dt_print(
                DT_DEBUG_OPENCL,
                "[opencl_pixelpipe (b)] late opencl error detected while copying back to cpu buffer: %s\n", cl_errstr(err));
            dt_opencl_release_mem_object(cl_mem_input);
            pipe->opencl_error = 1;
            return 1;
          }
          else
            input_format->cst = input_cst_cl;

          /* this is a good place to release event handles as we anyhow need to move from gpu to cpu here */
          dt_opencl_finish(pipe->devid);
          dt_opencl_release_mem_object(cl_mem_input);
          valid_input_on_gpu_only = FALSE;
        }
        if (pixelpipe_process_on_CPU(pipe, dev, input, input_format, roi_in, output, out_format, roi_out,
                                     module, piece, tiling, pixelpipe_flow))
          return 1;
      }
    }
    else
    {
      /* we are not allowed to use opencl for this module */

      // fprintf(stderr, "[opencl_pixelpipe 3] for module `%s', have bufs %p and %p \n", module->op,
      // cl_mem_input, *cl_mem_output);

      *cl_mem_output = NULL;

      /* cleanup unneeded opencl buffer, and copy back to CPU buffer */
      if(cl_mem_input != NULL)
      {
        cl_int err = dt_opencl_copy_device_to_host(pipe->devid, input, cl_mem_input, roi_in->width, roi_in->height,
                                            in_bpp);
        // if (rand() % 5 == 0) err = !CL_SUCCESS; // Test code: simulate spurious failures
        if(err != CL_SUCCESS)
        {
          /* late opencl error */
          dt_print(
              DT_DEBUG_OPENCL,
              "[opencl_pixelpipe (c)] late opencl error detected while copying back to cpu buffer: %s\n", cl_errstr(err));
          dt_opencl_release_mem_object(cl_mem_input);
          pipe->opencl_error = 1;
          return 1;
        }
        else
          input_format->cst = input_cst_cl;

        /* this is a good place to release event handles as we anyhow need to move from gpu to cpu here */
        dt_opencl_finish(pipe->devid);
        dt_opencl_release_mem_object(cl_mem_input);
        valid_input_on_gpu_only = FALSE;
      }

      if (pixelpipe_process_on_CPU(pipe, dev, input, input_format, roi_in, output, out_format, roi_out,
                                   module, piece, tiling, pixelpipe_flow))
        return 1;
    }

    /* input is still only on GPU? Let's invalidate CPU input buffer then */
    if(valid_input_on_gpu_only) dt_dev_pixelpipe_cache_invalidate(&(pipe->cache), input);
  }
  else
  {
    /* opencl is not inited or not enabled or we got no resource/device -> everything runs on cpu */
    if (pixelpipe_process_on_CPU(pipe, dev, input, input_format, roi_in, output, out_format, roi_out,
                                 module, piece, tiling, pixelpipe_flow))
      return 1;
  }

  return 0;
}
#endif


static void _print_perf_debug(dt_dev_pixelpipe_t *pipe, const dt_pixelpipe_flow_t pixelpipe_flow, dt_dev_pixelpipe_iop_t *piece, dt_iop_module_t *module, dt_times_t *start)
{
  char histogram_log[32] = "";
  if(!(pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_NONE))
  {
    snprintf(histogram_log, sizeof(histogram_log), ", collected histogram on %s",
             (pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_ON_GPU
                  ? "GPU"
                  : pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_ON_CPU ? "CPU" : ""));
  }

  gchar *module_label = dt_history_item_get_name(module);
  dt_show_times_f(
      start, "[dev_pixelpipe]", "processed `%s' on %s%s%s, blended on %s [%s]", module_label,
      pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_ON_GPU
          ? "GPU"
          : pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_ON_CPU ? "CPU" : "",
      pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_WITH_TILING ? " with tiling" : "",
      (!(pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_NONE) && (piece->request_histogram & DT_REQUEST_ON))
          ? histogram_log
          : "",
      pixelpipe_flow & PIXELPIPE_FLOW_BLENDED_ON_GPU
          ? "GPU"
          : pixelpipe_flow & PIXELPIPE_FLOW_BLENDED_ON_CPU ? "CPU" : "",
      _pipe_type_to_str(pipe->type));
  g_free(module_label);
}


static void _print_nan_debug(dt_dev_pixelpipe_t *pipe, void *cl_mem_output, void *output, const dt_iop_roi_t *roi_out, dt_iop_buffer_dsc_t *out_format, dt_iop_module_t *module, const size_t bpp)
{
  if((darktable.unmuted & DT_DEBUG_NAN) && strcmp(module->op, "gamma") != 0)
  {

#ifdef HAVE_OPENCL
    if(cl_mem_output != NULL)
      dt_opencl_copy_device_to_host(pipe->devid, output, cl_mem_output, roi_out->width, roi_out->height, bpp);
#endif

    gchar *module_label = dt_history_item_get_name(module);

    if(out_format->datatype == TYPE_FLOAT && out_format->channels == 4)
    {
      int hasinf = 0, hasnan = 0;
      dt_aligned_pixel_t min = { FLT_MAX };
      dt_aligned_pixel_t max = { FLT_MIN };

      for(int k = 0; k < 4 * roi_out->width * roi_out->height; k++)
      {
        if((k & 3) < 3)
        {
          float f = ((float *)(output))[k];
          if(isnan(f))
            hasnan = 1;
          else if(isinf(f))
            hasinf = 1;
          else
          {
            min[k & 3] = fmin(f, min[k & 3]);
            max[k & 3] = fmax(f, max[k & 3]);
          }
        }
      }
      if(hasnan)
        fprintf(stderr, "[dev_pixelpipe] module `%s' outputs NaNs! [%s]\n", module_label,
                _pipe_type_to_str(pipe->type));
      if(hasinf)
        fprintf(stderr, "[dev_pixelpipe] module `%s' outputs non-finite floats! [%s]\n", module_label,
                _pipe_type_to_str(pipe->type));
      fprintf(stderr, "[dev_pixelpipe] module `%s' min: (%f; %f; %f) max: (%f; %f; %f) [%s]\n", module_label,
              min[0], min[1], min[2], max[0], max[1], max[2], _pipe_type_to_str(pipe->type));
    }
    else if(out_format->datatype == TYPE_FLOAT && out_format->channels == 1)
    {
      int hasinf = 0, hasnan = 0;
      float min = FLT_MAX;
      float max = FLT_MIN;

      for(int k = 0; k < roi_out->width * roi_out->height; k++)
      {
        float f = ((float *)(output))[k];
        if(isnan(f))
          hasnan = 1;
        else if(isinf(f))
          hasinf = 1;
        else
        {
          min = fmin(f, min);
          max = fmax(f, max);
        }
      }
      if(hasnan)
        fprintf(stderr, "[dev_pixelpipe] module `%s' outputs NaNs! [%s]\n", module_label,
                _pipe_type_to_str(pipe->type));
      if(hasinf)
        fprintf(stderr, "[dev_pixelpipe] module `%s' outputs non-finite floats! [%s]\n", module_label,
                _pipe_type_to_str(pipe->type));
      fprintf(stderr, "[dev_pixelpipe] module `%s' min: (%f) max: (%f) [%s]\n", module_label, min, max,
              _pipe_type_to_str(pipe->type));
    }

    g_free(module_label);
  }
}


static int _init_base_buffer(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output,
                             void **cl_mem_output, dt_iop_buffer_dsc_t **out_format,
                             dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                             const uint64_t hash,
                             const gboolean bypass_cache,
                             const size_t bufsize, const size_t bpp)
{
// we're looking for the full buffer
  if(roi_out->scale == 1.0 && roi_out->x == 0 && roi_out->y == 0 && pipe->iwidth == roi_out->width
      && pipe->iheight == roi_out->height)
  {
    *output = pipe->input;
    return 0;
  }
  else if(bypass_cache || dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, bufsize, output, out_format))
  {
    if(roi_in->scale == 1.0f)
    {
      // fast branch for 1:1 pixel copies.
      // last minute clamping to catch potential out-of-bounds in roi_in and roi_out
      const int in_x = MAX(roi_in->x, 0);
      const int in_y = MAX(roi_in->y, 0);
      const int cp_width = MAX(0, MIN(roi_out->width, pipe->iwidth - in_x));
      const int cp_height = MIN(roi_out->height, pipe->iheight - in_y);

      if(cp_width > 0 && cp_height > 0)
      {
        _copy_buffer((const char *const)pipe->input, (char *const)*output, cp_height, roi_out->width,
                      pipe->iwidth, in_x, in_y, bpp * cp_width, bpp);
        return 0;
      }
      else
      {
        // Invalid dimensions
        return 1;
      }
    }
    else if(bpp == 16)
    {
      // dt_iop_clip_and_zoom() expects 4 * float 32 only
      roi_in->x /= roi_out->scale;
      roi_in->y /= roi_out->scale;
      roi_in->width = pipe->iwidth;
      roi_in->height = pipe->iheight;
      roi_in->scale = 1.0f;
      dt_iop_clip_and_zoom(*output, pipe->input, roi_out, roi_in, roi_out->width, pipe->iwidth);
      return 0;
    }
    else
    {
      fprintf(stdout,
                "Base buffer init: scale %f != 1.0 but the input has %li bytes per pixel. This case is not "
                "covered by the pipeline, please report the bug.\n",
                roi_out->scale, bpp);
      return 1;
    }
  }
  // else found in cache.
  return 0;
}

static int _process_masks_preview(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, dt_dev_pixelpipe_iop_t *piece,
                                  void *input, void **output,
                                  void *cl_mem_input, void **cl_mem_output, dt_iop_buffer_dsc_t **out_format,
                                  const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                                  const size_t in_bpp, const size_t out_bpp, dt_iop_module_t *module)
{
  // special case: user requests to see channel data in the parametric mask of a module, or the blending
  // mask. In that case we skip all modules manipulating pixel content and only process image distorting
  // modules. Finally "gamma" is responsible for displaying channel/mask data accordingly.
  if(strcmp(module->op, "gamma") != 0
     && (pipe->mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE)
     && !(module->operation_tags() & IOP_TAG_DISTORT)
     && (in_bpp == out_bpp) && !memcmp(roi_in, roi_out, sizeof(struct dt_iop_roi_t)))
  {
    // since we're not actually running the module, the output format is the same as the input format
    **out_format = pipe->dsc = piece->dsc_out = piece->dsc_in;

#ifdef HAVE_OPENCL
    if(dt_opencl_is_inited() && pipe->opencl_enabled && pipe->devid >= 0 && (cl_mem_input != NULL))
      *cl_mem_output = cl_mem_input;
    else
      *output = input;
#else
    *output = input;
#endif
    /* Previously: used full buffer copy on CPU:
    _copy_buffer((const char *const)input, (char *const)*output, roi_out->height, roi_out->width, roi_in.width,
                   0, 0, in_bpp * roi_in.width, in_bpp); */

    return 0;
  }

  return 1;
}


// recursive helper for process:
static int dt_dev_pixelpipe_process_rec(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output,
                                        void **cl_mem_output, dt_iop_buffer_dsc_t **out_format,
                                        const dt_iop_roi_t *roi_out, GList *modules, GList *pieces, int pos)
{
  // The pipeline is executed recursively, from the end. For each module n, starting from the end,
  // if output is cached, take it, else if input is cached, take it, process it and output,
  // else recurse to the previous module n-1 to get a an input.
  KILL_SWITCH_ABORT;

  dt_iop_roi_t roi_in = *roi_out;

  char module_name[256] = { 0 };
  void *input = NULL;
  void *cl_mem_input = NULL;
  *cl_mem_output = NULL;
  dt_iop_module_t *module = NULL;
  dt_dev_pixelpipe_iop_t *piece = NULL;

  if(modules)
  {
    module = (dt_iop_module_t *)modules->data;
    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    // skip this module?
    if(!piece->enabled)
      return dt_dev_pixelpipe_process_rec(pipe, dev, output, cl_mem_output, out_format, &roi_in,
                                          g_list_previous(modules), g_list_previous(pieces), pos - 1);
  }

  KILL_SWITCH_ABORT;

  if(module) g_strlcpy(module_name, module->op, MIN(sizeof(module_name), sizeof(module->op)));
  get_output_format(module, pipe, piece, dev, *out_format);
  const size_t bpp = dt_iop_buffer_dsc_to_bpp(*out_format);
  const size_t bufsize = (size_t)bpp * roi_out->width * roi_out->height;

  // 1) if cached buffer is still available, return data.
  uint64_t hash = _node_hash(pipe, piece, roi_out, pos);
  const gboolean bypass_cache = (module) ? piece->bypass_cache : FALSE;
  if(!bypass_cache && dt_dev_pixelpipe_cache_available(&(pipe->cache), hash))
  {
    if(module)
      dt_print(DT_DEBUG_PIPE, "[pixelpipe] dt_dev_pixelpipe_process_rec, cache available for pipe %i and module %s with hash %llu\n",
             pipe->type, module->op, (long long unsigned int)hash);

    (void)dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, bufsize, output, out_format);

    // Get the pipe-global histograms. We want float32 buffers, so we take all outputs
    // except for gamma which outputs uint8 so we need to deal with that internally
    pixelpipe_get_histogram_backbuf(pipe, dev, *output, NULL, *out_format, roi_out, module, piece, hash, bpp);

    KILL_SWITCH_AND_FLUSH_CACHE;
    return 0;
  }

  // 2) if history changed or exit event, abort processing?
  KILL_SWITCH_ABORT;

  // 3) input -> output
  if(!modules)
  {
    // If no modules, we are at the step 0 of the pipe:
    // fetching input buffer.
    // 3a) import input array with given scale and roi
    dt_times_t start;
    dt_get_times(&start);

    if(_init_base_buffer(pipe, dev, output, cl_mem_output, out_format, &roi_in, roi_out, hash, bypass_cache, bufsize,
                      bpp))
      return 1;

    dt_show_times_f(&start, "[dev_pixelpipe]", "initing base buffer [%s]", _pipe_type_to_str(pipe->type));
    return 0;
  }

  // 3b) recurse and obtain output array in &input

  // get region of interest which is needed in input
  // This is already computed ahead of running at init time in _get_roi_in()
  memcpy(&roi_in, &piece->planned_roi_in, sizeof(dt_iop_roi_t));
  // Otherwise, run this:
  // module->modify_roi_in(module, piece, roi_out, &roi_in);

  // recurse to get actual data of input buffer
  dt_iop_buffer_dsc_t _input_format = { 0 };
  dt_iop_buffer_dsc_t *input_format = &_input_format;

  piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
  piece->processed_roi_in = roi_in;
  piece->processed_roi_out = *roi_out;

  if(dt_dev_pixelpipe_process_rec(pipe, dev, &input, &cl_mem_input, &input_format, &roi_in,
                                  g_list_previous(modules), g_list_previous(pieces), pos - 1))
    return 1;

  KILL_SWITCH_ABORT;

  const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);
  piece->dsc_out = piece->dsc_in = *input_format;
  module->output_format(module, pipe, piece, &piece->dsc_out);
  **out_format = pipe->dsc = piece->dsc_out;
  const size_t out_bpp = dt_iop_buffer_dsc_to_bpp(*out_format);

  // reserve new cache line: output
  (void)dt_dev_pixelpipe_cache_get(&(pipe->cache), hash, bufsize, output, out_format);

  dt_times_t start;
  dt_get_times(&start);

  dt_pixelpipe_flow_t pixelpipe_flow = (PIXELPIPE_FLOW_NONE | PIXELPIPE_FLOW_HISTOGRAM_NONE);

  // Bypass pixel filtering and return early if we only want the pipe to display mask previews
  if(!_process_masks_preview(pipe, dev, piece, input, output, cl_mem_input, cl_mem_output, out_format, &roi_in, roi_out,
                         in_bpp, out_bpp, module))
    return 0;

  /* get tiling requirement of module */
  dt_develop_tiling_t tiling = { 0 };
  tiling.factor_cl = tiling.maxbuf_cl = -1;	// set sentinel value to detect whether callback set sizes
  module->tiling_callback(module, piece, &roi_in, roi_out, &tiling);
  if (tiling.factor_cl < 0) tiling.factor_cl = tiling.factor; // default to CPU size if callback didn't set GPU
  if (tiling.maxbuf_cl < 0) tiling.maxbuf_cl = tiling.maxbuf;

  /* does this module involve blending? */
  if(piece->blendop_data && ((dt_develop_blend_params_t *)piece->blendop_data)->mask_mode != DEVELOP_MASK_DISABLED)
  {
    /* get specific memory requirement for blending */
    dt_develop_tiling_t tiling_blendop = { 0 };
    tiling_callback_blendop(module, piece, &roi_in, roi_out, &tiling_blendop);

    /* aggregate in structure tiling */
    tiling.factor = fmax(tiling.factor, tiling_blendop.factor);
    tiling.factor_cl = fmax(tiling.factor_cl, tiling_blendop.factor);
    tiling.maxbuf = fmax(tiling.maxbuf, tiling_blendop.maxbuf);
    tiling.maxbuf_cl = fmax(tiling.maxbuf_cl, tiling_blendop.maxbuf);
    tiling.overhead = fmax(tiling.overhead, tiling_blendop.overhead);
  }

  /* remark: we do not do tiling for blendop step, neither in opencl nor on cpu. if overall tiling
     requirements (maximum of module and blendop) require tiling for opencl path, then following blend
     step is anyhow done on cpu. we assume that blending itself will never require tiling in cpu path,
     because memory requirements will still be low enough. */

  assert(tiling.factor > 0.0f);
  assert(tiling.factor_cl > 0.0f);

  // Actual pixel processing for this module
#ifdef HAVE_OPENCL
  if (pixelpipe_process_on_GPU(pipe, dev, input, cl_mem_input, input_format, &roi_in, output, cl_mem_output, out_format, roi_out,
                               module, piece, &tiling, &pixelpipe_flow, in_bpp, bpp))
    return 1;
#else
  if (pixelpipe_process_on_CPU(pipe, dev, input, input_format, &roi_in, output, out_format, roi_out,
                               module, piece, &tiling, &pixelpipe_flow))
    return 1;
#endif

  // Get the pipe-global histograms. We want float32 buffers, so we take all outputs
  // except for gamma which outputs uint8 so we need to deal with that internally
  pixelpipe_get_histogram_backbuf(pipe, dev, *output, *cl_mem_output, *out_format, roi_out, module, piece, hash, bpp);

  // Don't cache outputs if we requested to bypass the cache
  if(bypass_cache) dt_dev_pixelpipe_cache_invalidate(&(pipe->cache), *output);

  KILL_SWITCH_AND_FLUSH_CACHE;

  _print_perf_debug(pipe, pixelpipe_flow, piece, module, &start);

  // in case we get this buffer from the cache in the future, cache some stuff:
  **out_format = piece->dsc_out = pipe->dsc;

  _print_nan_debug(pipe, *cl_mem_output, *output, roi_out, *out_format, module, bpp);

  // 4) colorpicker and scopes:
  if(dev->gui_attached
     && pipe == dev->preview_pipe
     && (strcmp(module->op, "gamma") == 0)) // only gamma provides meaningful RGB data
  {
    // Pick RGB/Lab for the primary colorpicker and live samples
    if(darktable.lib->proxy.colorpicker.picker_proxy || darktable.lib->proxy.colorpicker.live_samples)
      _pixelpipe_pick_samples(dev, module, (const float *const )input, &roi_in);
  }

  KILL_SWITCH_AND_FLUSH_CACHE;
  return 0;
}


int dt_dev_pixelpipe_process_no_gamma(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width,
                                      int height, float scale)
{
  // temporarily disable gamma mapping.
  GList *gammap = g_list_last(pipe->nodes);
  dt_dev_pixelpipe_iop_t *gamma = (dt_dev_pixelpipe_iop_t *)gammap->data;
  while(strcmp(gamma->module->op, "gamma"))
  {
    gamma = NULL;
    gammap = g_list_previous(gammap);
    if(!gammap) break;
    gamma = (dt_dev_pixelpipe_iop_t *)gammap->data;
  }
  if(gamma) gamma->enabled = 0;
  const int ret = dt_dev_pixelpipe_process(pipe, dev, x, y, width, height, scale);
  if(gamma) gamma->enabled = 1;
  return ret;
}

void dt_dev_pixelpipe_disable_after(dt_dev_pixelpipe_t *pipe, const char *op)
{
  GList *nodes = g_list_last(pipe->nodes);
  dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  while(strcmp(piece->module->op, op))
  {
    piece->enabled = 0;
    piece = NULL;
    nodes = g_list_previous(nodes);
    if(!nodes) break;
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  }
}

void dt_dev_pixelpipe_disable_before(dt_dev_pixelpipe_t *pipe, const char *op)
{
  GList *nodes = pipe->nodes;
  dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  while(strcmp(piece->module->op, op))
  {
    piece->enabled = 0;
    piece = NULL;
    nodes = g_list_next(nodes);
    if(!nodes) break;
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  }
}

static int dt_dev_pixelpipe_process_rec_and_backcopy(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output,
                                                     void **cl_mem_output, dt_iop_buffer_dsc_t **out_format,
                                                     const dt_iop_roi_t *roi_out, GList *modules, GList *pieces,
                                                     int pos)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  darktable.dtresources.group = 4 * darktable.dtresources.level;
#ifdef HAVE_OPENCL
  dt_opencl_check_tuning(pipe->devid);
#endif
  int ret = dt_dev_pixelpipe_process_rec(pipe, dev, output, cl_mem_output, out_format, roi_out, modules, pieces, pos);
#ifdef HAVE_OPENCL
  // copy back final opencl buffer (if any) to CPU
  if(ret)
  {
    dt_opencl_release_mem_object(*cl_mem_output);
    *cl_mem_output = NULL;
  }
  else
  {
    if(*cl_mem_output != NULL)
    {
      cl_int err = dt_opencl_copy_device_to_host(pipe->devid, *output, *cl_mem_output, roi_out->width, roi_out->height,
                                          dt_iop_buffer_dsc_to_bpp(*out_format));
      dt_opencl_release_mem_object(*cl_mem_output);
      *cl_mem_output = NULL;

      if(err != CL_SUCCESS)
      {
        /* this indicates a opencl problem earlier in the pipeline */
        dt_print(DT_DEBUG_OPENCL,
                 "[dt_dev_pixelpipe_process_rec_and_backcopy] late opencl error detected while copying back to cpu buffer: %s\n", cl_errstr(err));
        pipe->opencl_error = 1;
        ret = 1;
      }
    }
  }
#endif
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
  return ret;
}


int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width, int height,
                             float scale)
{
  pipe->processing = 1;
  pipe->opencl_enabled = dt_opencl_update_settings(); // update enabled flag and profile from preferences
  pipe->devid = (pipe->opencl_enabled) ? dt_opencl_lock_device(pipe->type)
                                       : -1; // try to get/lock opencl resource

  dt_print(DT_DEBUG_OPENCL, "[pixelpipe_process] [%s] using device %d\n", _pipe_type_to_str(pipe->type),
           pipe->devid);

  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] before pixelpipe process\n");
    dt_print_mem_usage();
  }

  if(pipe->devid >= 0) dt_opencl_events_reset(pipe->devid);

  dt_iop_roi_t roi = (dt_iop_roi_t){ x, y, width, height, scale };
  // printf("pixelpipe homebrew process start\n");
  if(darktable.unmuted & DT_DEBUG_DEV) dt_dev_pixelpipe_cache_print(&pipe->cache);

  // get a snapshot of mask list
  if(pipe->forms) g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);
  pipe->forms = dt_masks_dup_forms_deep(dev->forms, NULL);

  //  go through list of modules from the end:
  const guint pos = g_list_length(pipe->iop);
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);

// re-entry point: in case of late opencl errors we start all over again with opencl-support disabled
restart:;
  void *buf = NULL;
  void *cl_mem_out = NULL;

  dt_iop_buffer_dsc_t _out_format = { 0 };
  dt_iop_buffer_dsc_t *out_format = &_out_format;

  // Get the roi_out hash
  // Get the previous output size of the module, for cache invalidation.
  dt_dev_pixelpipe_get_roi_in(pipe, dev, roi);
  dt_pixelpipe_get_global_hash(pipe, dev);

  // run pixelpipe recursively and get error status
  const int err =
    dt_dev_pixelpipe_process_rec_and_backcopy(pipe, dev, &buf, &cl_mem_out, &out_format, &roi, modules,
                                              pieces, pos);

  // get status summary of opencl queue by checking the eventlist
  const int oclerr = (pipe->devid >= 0) ? (dt_opencl_events_flush(pipe->devid, 1) != 0) : 0;

  // Check if we had opencl errors ....
  // remark: opencl errors can come in two ways: pipe->opencl_error is TRUE (and err is TRUE) OR oclerr is
  // TRUE
  if(oclerr || (err && pipe->opencl_error))
  {
    // Well, there were errors -> we might need to free an invalid opencl memory object
    dt_opencl_release_mem_object(cl_mem_out);
    dt_opencl_unlock_device(pipe->devid); // release opencl resource
    dt_pthread_mutex_lock(&pipe->busy_mutex);
    pipe->opencl_enabled = 0; // disable opencl for this pipe
    pipe->opencl_error = 0;   // reset error status
    pipe->devid = -1;
    dt_pthread_mutex_unlock(&pipe->busy_mutex);

    darktable.opencl->error_count++; // increase error count
    if(darktable.opencl->error_count >= DT_OPENCL_MAX_ERRORS)
    {
      // too frequent opencl errors encountered: this is a clear sign of a broken setup. give up on opencl
      // during this session.
      darktable.opencl->stopped = 1;
      dt_print(DT_DEBUG_OPENCL,
               "[opencl] frequent opencl errors encountered; disabling opencl for this session!\n");
      dt_control_log(
          _("ansel discovered problems with your OpenCL setup; disabling OpenCL for this session!"));
      // also remove "opencl" from capabilities so that the preference entry is greyed out
      dt_capabilities_remove("opencl");
    }

    dt_dev_pixelpipe_flush_caches(pipe);
    dt_dev_pixelpipe_change(pipe, dev);
    dt_dev_pixelpipe_get_roi_in(pipe, dev, roi);
    dt_pixelpipe_get_global_hash(pipe, dev);

    dt_print(DT_DEBUG_OPENCL, "[pixelpipe_process] [%s] falling back to cpu path\n",
             _pipe_type_to_str(pipe->type));
    goto restart; // try again (this time without opencl)
  }

  // release resources:
  if (pipe->forms)
  {
    g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);
    pipe->forms = NULL;
  }
  if(pipe->devid >= 0)
  {
    dt_opencl_unlock_device(pipe->devid);
    pipe->devid = -1;
  }
  // ... and in case of other errors ...
  if(err)
  {
    pipe->processing = 0;
    return 1;
  }

  // terminate
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);
  const dt_dev_pixelpipe_iop_t *last_module = _last_node_in_pipe(pipe);
  pipe->backbuf_hash = _node_hash(pipe, last_module, &roi, pos);
  pipe->backbuf = buf;
  pipe->backbuf_width = width;
  pipe->backbuf_height = height;

  if((pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW
     || (pipe->type & DT_DEV_PIXELPIPE_FULL) == DT_DEV_PIXELPIPE_FULL
     || (pipe->type & DT_DEV_PIXELPIPE_PREVIEW2) == DT_DEV_PIXELPIPE_PREVIEW2)
  {
    if(pipe->output_backbuf == NULL || pipe->output_backbuf_width != pipe->backbuf_width || pipe->output_backbuf_height != pipe->backbuf_height)
    {
      g_free(pipe->output_backbuf);
      pipe->output_backbuf_width = pipe->backbuf_width;
      pipe->output_backbuf_height = pipe->backbuf_height;
      pipe->output_backbuf = g_malloc0(sizeof(uint8_t) * 4 * pipe->output_backbuf_width * pipe->output_backbuf_height);
    }

    if(pipe->output_backbuf)
      memcpy(pipe->output_backbuf, pipe->backbuf, sizeof(uint8_t) * 4 * pipe->output_backbuf_width * pipe->output_backbuf_height);
    pipe->output_imgid = pipe->image.id;
  }
  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);

  // printf("pixelpipe homebrew process end\n");
  pipe->processing = 0;
  return 0;
}

void dt_dev_pixelpipe_flush_caches(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_cache_flush(&pipe->cache);
}

void dt_dev_pixelpipe_get_roi_out(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int width_in,
                                     int height_in, int *width, int *height)
{
  dt_pthread_mutex_lock(&pipe->busy_mutex);
  dt_iop_roi_t roi_in = (dt_iop_roi_t){ 0, 0, width_in, height_in, 1.0 };
  dt_iop_roi_t roi_out;
  GList *modules = g_list_first(pipe->iop);
  GList *pieces = g_list_first(pipe->nodes);
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;

    piece->buf_in = roi_in;

    // skip this module?
    if(piece->enabled
       && !(dev->gui_module && dev->gui_module != module
            && dev->gui_module->operation_tags_filter() & module->operation_tags()))
    {
      module->modify_roi_out(module, piece, &roi_out, &roi_in);
    }
    else
    {
      // pass through regions of interest for gui post expose events
      roi_out = roi_in;
    }

    piece->buf_out = roi_out;
    roi_in = roi_out;

    modules = g_list_next(modules);
    pieces = g_list_next(pieces);
  }
  *width = roi_out.width;
  *height = roi_out.height;
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

void dt_dev_pixelpipe_get_roi_in(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, const struct dt_iop_roi_t roi_out)
{
  // while module->modify_roi_out describes how the current module will change the size of
  // the output buffer depending on its parameters (pretty intuitive),
  // module->modify_roi_in describes "how much material" the current module needs from the previous one,
  // because some modules (lens correction) need a padding on their input.
  // The tricky part is therefore that the effect of the current module->modify_roi_in() needs to be repercuted
  // upstream in the pipeline for proper pipeline cache invalidation, so we need to browse the pipeline
  // backwards.

  dt_pthread_mutex_lock(&pipe->busy_mutex);
  dt_iop_roi_t roi_out_temp = roi_out;
  dt_iop_roi_t roi_in;
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;

    piece->planned_roi_out = roi_out_temp;

    // skip this module?
    if(piece->enabled && !(dev->gui_module && dev->gui_module != module
            && dev->gui_module->operation_tags_filter() & module->operation_tags()))
    {
      module->modify_roi_in(module, piece, &roi_out_temp, &roi_in);

      /*
      if(dev)
        fprintf(stdout, "ROI IN in for %s on pipe %i: %i × %i at (%i, %i) @ %f\n", module->op, dev->pipe->type, roi_in.width, roi_in.height, roi_in.x, roi_in.y, roi_in.scale);
      */
    }
    else
    {
      // pass through regions of interest for gui post expose events
      roi_in = roi_out_temp;
    }

    piece->planned_roi_in = roi_in;
    roi_out_temp = roi_in;

    modules = g_list_previous(modules);
    pieces = g_list_previous(pieces);
  }
  dt_pthread_mutex_unlock(&pipe->busy_mutex);
}

float *dt_dev_get_raster_mask(const dt_dev_pixelpipe_t *pipe, const dt_iop_module_t *raster_mask_source,
                              const int raster_mask_id, const dt_iop_module_t *target_module,
                              gboolean *free_mask)
{
  if(!raster_mask_source)
    return NULL;

  *free_mask = FALSE;
  float *raster_mask = NULL;

  GList *source_iter;
  for(source_iter = pipe->nodes; source_iter; source_iter = g_list_next(source_iter))
  {
    const dt_dev_pixelpipe_iop_t *candidate = (dt_dev_pixelpipe_iop_t *)source_iter->data;
    if(candidate->module == raster_mask_source)
      break;
  }

  if(source_iter)
  {
    const dt_dev_pixelpipe_iop_t *source_piece = (dt_dev_pixelpipe_iop_t *)source_iter->data;
    if(source_piece && source_piece->enabled) // there might be stale masks from disabled modules left over. don't use those!
    {
      raster_mask = g_hash_table_lookup(source_piece->raster_masks, GINT_TO_POINTER(raster_mask_id));
      if(raster_mask)
      {
        for(GList *iter = g_list_next(source_iter); iter; iter = g_list_next(iter))
        {
          dt_dev_pixelpipe_iop_t *module = (dt_dev_pixelpipe_iop_t *)iter->data;

          if(module->enabled
             && !(module->module->dev->gui_module
                  && (module->module->dev->gui_module->operation_tags_filter() & module->module->operation_tags())))
          {
            if(module->module->distort_mask
              && !(!strcmp(module->module->op, "finalscale") // hack against pipes not using finalscale
                    && module->processed_roi_in.width == 0
                    && module->processed_roi_in.height == 0))
            {
              float *transformed_mask = dt_alloc_align_float((size_t)module->processed_roi_out.width
                                                              * module->processed_roi_out.height);
              module->module->distort_mask(module->module,
                                          module,
                                          raster_mask,
                                          transformed_mask,
                                          &module->processed_roi_in,
                                          &module->processed_roi_out);
              if(*free_mask) dt_free_align(raster_mask);
              *free_mask = TRUE;
              raster_mask = transformed_mask;
            }
            else if(!module->module->distort_mask &&
                    (module->processed_roi_in.width != module->processed_roi_out.width ||
                     module->processed_roi_in.height != module->processed_roi_out.height ||
                     module->processed_roi_in.x != module->processed_roi_out.x ||
                     module->processed_roi_in.y != module->processed_roi_out.y))
              fprintf(stderr, "FIXME: module `%s' changed the roi from %d x %d @ %d / %d to %d x %d | %d / %d but doesn't have "
                     "distort_mask() implemented!\n", module->module->op, module->processed_roi_in.width,
                     module->processed_roi_in.height, module->processed_roi_in.x, module->processed_roi_in.y,
                     module->processed_roi_out.width, module->processed_roi_out.height, module->processed_roi_out.x,
                     module->processed_roi_out.y);
          }

          if(module->module == target_module)
            break;
        }
      }
    }
  }

  return raster_mask;
}

void dt_dev_clear_rawdetail_mask(dt_dev_pixelpipe_t *pipe)
{
  if(pipe->rawdetail_mask_data) dt_free_align(pipe->rawdetail_mask_data);
  pipe->rawdetail_mask_data = NULL;
}

gboolean dt_dev_write_rawdetail_mask(dt_dev_pixelpipe_iop_t *piece, float *const rgb, const dt_iop_roi_t *const roi_in, const int mode)
{
  dt_dev_pixelpipe_t *p = piece->pipe;
  if((p->want_detail_mask & DT_DEV_DETAIL_MASK_REQUIRED) == 0)
  {
    if(p->rawdetail_mask_data)
      dt_dev_clear_rawdetail_mask(p);
    return FALSE;
  }
  if((p->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED) != mode) return FALSE;

  dt_dev_clear_rawdetail_mask(p);

  const int width = roi_in->width;
  const int height = roi_in->height;
  float *mask = dt_alloc_align_float((size_t)width * height);
  float *tmp = dt_alloc_align_float((size_t)width * height);
  if((mask == NULL) || (tmp == NULL)) goto error;

  p->rawdetail_mask_data = mask;
  memcpy(&p->rawdetail_mask_roi, roi_in, sizeof(dt_iop_roi_t));

  dt_aligned_pixel_t wb = { piece->pipe->dsc.temperature.coeffs[0],
                            piece->pipe->dsc.temperature.coeffs[1],
                            piece->pipe->dsc.temperature.coeffs[2] };
  if((p->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED) == DT_DEV_DETAIL_MASK_RAWPREPARE)
  {
    wb[0] = wb[1] = wb[2] = 1.0f;
  }
  dt_masks_calc_rawdetail_mask(rgb, mask, tmp, width, height, wb);
  dt_free_align(tmp);
  dt_print(DT_DEBUG_MASKS, "[dt_dev_write_rawdetail_mask] %i (%ix%i)\n", mode, roi_in->width, roi_in->height);
  return FALSE;

  error:
  fprintf(stderr, "[dt_dev_write_rawdetail_mask] couldn't write detail mask\n");
  dt_free_align(mask);
  dt_free_align(tmp);
  return TRUE;
}

#ifdef HAVE_OPENCL
gboolean dt_dev_write_rawdetail_mask_cl(dt_dev_pixelpipe_iop_t *piece, cl_mem in, const dt_iop_roi_t *const roi_in, const int mode)
{
  dt_dev_pixelpipe_t *p = piece->pipe;
  if((p->want_detail_mask & DT_DEV_DETAIL_MASK_REQUIRED) == 0)
  {
    if(p->rawdetail_mask_data)
      dt_dev_clear_rawdetail_mask(p);
    return FALSE;
  }

  if((p->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED) != mode) return FALSE;

  dt_dev_clear_rawdetail_mask(p);

  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_mem out = NULL;
  cl_mem tmp = NULL;
  float *mask = NULL;
  const int devid = p->devid;

  cl_int err = CL_SUCCESS;
  mask = dt_alloc_align_float((size_t)width * height);
  if(mask == NULL) goto error;
  out = dt_opencl_alloc_device(devid, width, height, sizeof(float));
  if(out == NULL) goto error;
  tmp = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
  if(tmp == NULL) goto error;

  {
    const int kernel = darktable.opencl->blendop->kernel_calc_Y0_mask;
    dt_aligned_pixel_t wb = { piece->pipe->dsc.temperature.coeffs[0],
                              piece->pipe->dsc.temperature.coeffs[1],
                              piece->pipe->dsc.temperature.coeffs[2] };
    if((p->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED) == DT_DEV_DETAIL_MASK_RAWPREPARE)
    {
      wb[0] = wb[1] = wb[2] = 1.0f;
    }
    size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &tmp);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &in);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), &wb[0]);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &wb[1]);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &wb[2]);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  {
    size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    const int kernel = darktable.opencl->blendop->kernel_write_scharr_mask;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &tmp);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &out);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  {
    err = dt_opencl_read_host_from_device(devid, mask, out, width, height, sizeof(float));
    if(err != CL_SUCCESS) goto error;
  }

  p->rawdetail_mask_data = mask;
  memcpy(&p->rawdetail_mask_roi, roi_in, sizeof(dt_iop_roi_t));

  dt_opencl_release_mem_object(out);
  dt_opencl_release_mem_object(tmp);
  dt_print(DT_DEBUG_MASKS, "[dt_dev_write_rawdetail_mask_cl] mode %i (%ix%i)", mode, roi_in->width, roi_in->height);
  return FALSE;

  error:
  fprintf(stderr, "[dt_dev_write_rawdetail_mask_cl] couldn't write detail mask: %s\n", cl_errstr(err));
  dt_dev_clear_rawdetail_mask(p);
  dt_opencl_release_mem_object(out);
  dt_opencl_release_mem_object(tmp);
  dt_free_align(mask);
  return TRUE;
}
#endif

// this expects a mask prepared by the demosaicer and distorts the mask through all pipeline modules
// until target
float *dt_dev_distort_detail_mask(const dt_dev_pixelpipe_t *pipe, float *src, const dt_iop_module_t *target_module)
{
  if(!pipe->rawdetail_mask_data) return NULL;
  gboolean valid = FALSE;
  const int check = pipe->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED;

  GList *source_iter;
  for(source_iter = pipe->nodes; source_iter; source_iter = g_list_next(source_iter))
  {
    const dt_dev_pixelpipe_iop_t *candidate = (dt_dev_pixelpipe_iop_t *)source_iter->data;
    if(((!strcmp(candidate->module->op, "demosaic")) && candidate->enabled) && (check == DT_DEV_DETAIL_MASK_DEMOSAIC))
    {
      valid = TRUE;
      break;
    }
    if(((!strcmp(candidate->module->op, "rawprepare")) && candidate->enabled) && (check == DT_DEV_DETAIL_MASK_RAWPREPARE))
    {
      valid = TRUE;
      break;
    }
  }

  if(!valid) return NULL;
  dt_vprint(DT_DEBUG_MASKS, "[dt_dev_distort_detail_mask] (%ix%i) for module %s\n", pipe->rawdetail_mask_roi.width, pipe->rawdetail_mask_roi.height, target_module->op);

  float *resmask = src;
  float *inmask  = src;
  if(source_iter)
  {
    for(GList *iter = source_iter; iter; iter = g_list_next(iter))
    {
      dt_dev_pixelpipe_iop_t *module = (dt_dev_pixelpipe_iop_t *)iter->data;
      if(module->enabled
         && !(module->module->dev->gui_module
              && module->module->dev->gui_module->operation_tags_filter() & module->module->operation_tags()))
      {
        if(module->module->distort_mask
              && !(!strcmp(module->module->op, "finalscale") // hack against pipes not using finalscale
                    && module->processed_roi_in.width == 0
                    && module->processed_roi_in.height == 0))
        {
          float *tmp = dt_alloc_align_float((size_t)module->processed_roi_out.width * module->processed_roi_out.height);
          dt_vprint(DT_DEBUG_MASKS, "   %s %ix%i -> %ix%i\n", module->module->op, module->processed_roi_in.width, module->processed_roi_in.height, module->processed_roi_out.width, module->processed_roi_out.height);
          module->module->distort_mask(module->module, module, inmask, tmp, &module->processed_roi_in, &module->processed_roi_out);
          resmask = tmp;
          if(inmask != src) dt_free_align(inmask);
          inmask = tmp;
        }
        else if(!module->module->distort_mask &&
                (module->processed_roi_in.width != module->processed_roi_out.width ||
                 module->processed_roi_in.height != module->processed_roi_out.height ||
                 module->processed_roi_in.x != module->processed_roi_out.x ||
                 module->processed_roi_in.y != module->processed_roi_out.y))
              fprintf(stderr, "FIXME: module `%s' changed the roi from %d x %d @ %d / %d to %d x %d | %d / %d but doesn't have "
                 "distort_mask() implemented!\n", module->module->op, module->processed_roi_in.width,
                 module->processed_roi_in.height, module->processed_roi_in.x, module->processed_roi_in.y,
                 module->processed_roi_out.width, module->processed_roi_out.height, module->processed_roi_out.x,
                 module->processed_roi_out.y);

        if(module->module == target_module) break;
      }
    }
  }
  return resmask;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
