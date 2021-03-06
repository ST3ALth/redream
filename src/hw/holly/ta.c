#include "hw/holly/ta.h"
#include "core/list.h"
#include "core/profiler.h"
#include "core/rb_tree.h"
#include "core/string.h"
#include "hw/holly/holly.h"
#include "hw/holly/pixel_convert.h"
#include "hw/holly/pvr.h"
#include "hw/holly/tr.h"
#include "hw/holly/trace.h"
#include "hw/sh4/sh4.h"
#include "renderer/backend.h"
#include "sys/exception_handler.h"
#include "sys/filesystem.h"
#include "sys/thread.h"
#include "ui/nuklear.h"

#define TA_MAX_CONTEXTS 32

struct ta_texture_entry {
  struct texture_entry base;
  struct memory_watch *texture_watch;
  struct memory_watch *palette_watch;
  struct list_node free_it;
  struct rb_node live_it;
};

struct ta {
  struct device base;
  struct texture_provider provider;
  struct rb *rb;
  struct tr *tr;

  struct scheduler *scheduler;
  struct holly *holly;
  struct pvr *pvr;
  struct address_space *space;
  uint8_t *video_ram;
  uint8_t *palette_ram;

  // texture cache entry pool. free entries are in a linked list, live entries
  // are in a tree ordered by texture key, textures queued for invalidation are
  // in the the invalid_entries linked list
  struct ta_texture_entry entries[1024];
  struct list free_entries;
  struct rb_tree live_entries;
  int num_invalidated;

  // tile context pool. free contexts are in a linked list, live contexts are
  // are in a tree ordered by the context's guest address
  struct tile_ctx contexts[TA_MAX_CONTEXTS];
  struct list free_contexts;
  struct rb_tree live_contexts;

  // the pending context is the last context requested to be rendered by the
  // emulation thread. a mutex is used to synchronize access with the graphics
  // thread
  mutex_t pending_mutex;
  struct tile_ctx *pending_context;

  // last parsed pending context
  struct render_ctx render_context;

  // buffers used by the tile contexts. allocating here instead of inside each
  // tile_ctx to avoid blowing the stack when a tile_ctx is needed temporarily
  // on the stack for searching
  uint8_t params[TA_MAX_CONTEXTS * TA_MAX_PARAMS];

  // buffers used by render context
  struct surface surfs[TA_MAX_SURFS];
  struct vertex verts[TA_MAX_VERTS];
  int sorted_surfs[TA_MAX_SURFS];

  // debug info
  int frame;
  int frames_skipped;
  int num_textures;
  struct trace_writer *trace_writer;
};

int g_param_sizes[0x100 * TA_NUM_PARAMS * TA_NUM_VERT_TYPES];
int g_poly_types[0x100 * TA_NUM_PARAMS * TA_NUM_LISTS];
int g_vertex_types[0x100 * TA_NUM_PARAMS * TA_NUM_LISTS];

static holly_interrupt_t list_interrupts[] = {
    HOLLY_INTC_TAEOINT,   // TA_LIST_OPAQUE
    HOLLY_INTC_TAEOMINT,  // TA_LIST_OPAQUE_MODVOL
    HOLLY_INTC_TAETINT,   // TA_LIST_TRANSLUCENT
    HOLLY_INTC_TAETMINT,  // TA_LIST_TRANSLUCENT_MODVOL
    HOLLY_INTC_TAEPTIN    // TA_LIST_PUNCH_THROUGH
};

static int ta_entry_cmp(const struct rb_node *rb_lhs,
                        const struct rb_node *rb_rhs) {
  const struct ta_texture_entry *lhs =
      rb_entry(rb_lhs, const struct ta_texture_entry, live_it);
  const struct ta_texture_entry *rhs =
      rb_entry(rb_rhs, const struct ta_texture_entry, live_it);
  return (int)(tr_texture_key(lhs->base.tsp, lhs->base.tcw) -
               tr_texture_key(rhs->base.tsp, rhs->base.tcw));
}

static int ta_context_cmp(const struct rb_node *rb_lhs,
                          const struct rb_node *rb_rhs) {
  const struct tile_ctx *lhs = rb_entry(rb_lhs, const struct tile_ctx, live_it);
  const struct tile_ctx *rhs = rb_entry(rb_rhs, const struct tile_ctx, live_it);
  return (int)(lhs->addr - rhs->addr);
}

static struct rb_callbacks ta_entry_cb = {&ta_entry_cmp, NULL, NULL};
static struct rb_callbacks ta_context_cb = {&ta_context_cmp, NULL, NULL};

// See "57.1.1.2 Parameter Combinations" for information on the polygon types.
static int ta_get_poly_type_raw(union pcw pcw) {
  if (pcw.list_type == TA_LIST_OPAQUE_MODVOL ||
      pcw.list_type == TA_LIST_TRANSLUCENT_MODVOL) {
    return 6;
  }

  if (pcw.para_type == TA_PARAM_SPRITE) {
    return 5;
  }

  if (pcw.volume) {
    if (pcw.col_type == 0) {
      return 3;
    }
    if (pcw.col_type == 2) {
      return 4;
    }
    if (pcw.col_type == 3) {
      return 3;
    }
  }

  if (pcw.col_type == 0 || pcw.col_type == 1 || pcw.col_type == 3) {
    return 0;
  }
  if (pcw.col_type == 2 && pcw.texture && !pcw.offset) {
    return 1;
  }
  if (pcw.col_type == 2 && pcw.texture && pcw.offset) {
    return 2;
  }
  if (pcw.col_type == 2 && !pcw.texture) {
    return 1;
  }

  return 0;
}

// See "57.1.1.2 Parameter Combinations" for information on the vertex types.
static int ta_get_vert_type_raw(union pcw pcw) {
  if (pcw.list_type == TA_LIST_OPAQUE_MODVOL ||
      pcw.list_type == TA_LIST_TRANSLUCENT_MODVOL) {
    return 17;
  }

  if (pcw.para_type == TA_PARAM_SPRITE) {
    return pcw.texture ? 16 : 15;
  }

  if (pcw.volume) {
    if (pcw.texture) {
      if (pcw.col_type == 0) {
        return pcw.uv_16bit ? 12 : 11;
      }
      if (pcw.col_type == 2 || pcw.col_type == 3) {
        return pcw.uv_16bit ? 14 : 13;
      }
    }

    if (pcw.col_type == 0) {
      return 9;
    }
    if (pcw.col_type == 2 || pcw.col_type == 3) {
      return 10;
    }
  }

  if (pcw.texture) {
    if (pcw.col_type == 0) {
      return pcw.uv_16bit ? 4 : 3;
    }
    if (pcw.col_type == 1) {
      return pcw.uv_16bit ? 6 : 5;
    }
    if (pcw.col_type == 2 || pcw.col_type == 3) {
      return pcw.uv_16bit ? 8 : 7;
    }
  }

  if (pcw.col_type == 0) {
    return 0;
  }
  if (pcw.col_type == 1) {
    return 1;
  }
  if (pcw.col_type == 2 || pcw.col_type == 3) {
    return 2;
  }

  return 0;
}

// Parameter size can be determined by only the union pcw for every parameter
// other
// than vertex parameters. For vertex parameters, the vertex type derived from
// the last poly or modifier volume parameter is needed.
static int ta_get_param_size_raw(union pcw pcw, int vertex_type) {
  switch (pcw.para_type) {
    case TA_PARAM_END_OF_LIST:
      return 32;
    case TA_PARAM_USER_TILE_CLIP:
      return 32;
    case TA_PARAM_OBJ_LIST_SET:
      return 32;
    case TA_PARAM_POLY_OR_VOL: {
      int type = ta_get_poly_type_raw(pcw);
      return type == 0 || type == 1 || type == 3 ? 32 : 64;
    }
    case TA_PARAM_SPRITE:
      return 32;
    case TA_PARAM_VERTEX: {
      return vertex_type == 0 || vertex_type == 1 || vertex_type == 2 ||
                     vertex_type == 3 || vertex_type == 4 || vertex_type == 7 ||
                     vertex_type == 8 || vertex_type == 9 || vertex_type == 10
                 ? 32
                 : 64;
    }
    default:
      return 0;
  }
}

static void ta_soft_reset(struct ta *ta) {
  // FIXME what are we supposed to do here?
}

static struct ta_texture_entry *ta_alloc_texture(struct ta *ta, union tsp tsp,
                                                 union tcw tcw) {
  // remove from free list
  struct ta_texture_entry *entry =
      list_first_entry(&ta->free_entries, struct ta_texture_entry, free_it);
  CHECK_NOTNULL(entry);
  list_remove(&ta->free_entries, &entry->free_it);

  // reset entry
  memset(entry, 0, sizeof(*entry));
  entry->base.tsp = tsp;
  entry->base.tcw = tcw;

  // add to live tree
  rb_insert(&ta->live_entries, &entry->live_it, &ta_entry_cb);

  ta->num_textures++;

  return entry;
}

static struct ta_texture_entry *ta_find_texture(struct ta *ta, union tsp tsp,
                                                union tcw tcw) {
  struct ta_texture_entry search;
  search.base.tsp = tsp;
  search.base.tcw = tcw;

  return rb_find_entry(&ta->live_entries, &search, struct ta_texture_entry,
                       live_it, &ta_entry_cb);
}

static struct texture_entry *ta_texture_provider_find_texture(void *data,
                                                              union tsp tsp,
                                                              union tcw tcw) {
  struct ta_texture_entry *entry = ta_find_texture(data, tsp, tcw);

  if (!entry) {
    return NULL;
  }

  return &entry->base;
}

static void ta_clear_textures(struct ta *ta) {
  LOG_INFO("Texture cache cleared");

  struct rb_node *it = rb_first(&ta->live_entries);

  while (it) {
    struct rb_node *next = rb_next(it);

    struct ta_texture_entry *entry =
        rb_entry(it, struct ta_texture_entry, live_it);

    entry->base.dirty = 1;

    it = next;
  }
}

static void ta_texture_invalidated(const struct exception *ex, void *data) {
  struct ta_texture_entry *entry = data;
  entry->texture_watch = NULL;
  entry->base.dirty = 1;
}

static void ta_palette_invalidated(const struct exception *ex, void *data) {
  struct ta_texture_entry *entry = data;
  entry->palette_watch = NULL;
  entry->base.dirty = 1;
}

static struct tile_ctx *ta_get_context(struct ta *ta, uint32_t addr) {
  struct tile_ctx search;
  search.addr = addr;

  return rb_find_entry(&ta->live_contexts, &search, struct tile_ctx, live_it,
                       &ta_context_cb);
}

static struct tile_ctx *ta_alloc_context(struct ta *ta, uint32_t addr) {
  // remove from free list
  struct tile_ctx *ctx =
      list_first_entry(&ta->free_contexts, struct tile_ctx, free_it);
  CHECK_NOTNULL(ctx);
  list_remove(&ta->free_contexts, &ctx->free_it);

  // reset context
  uint8_t *params = ctx->params;
  memset(ctx, 0, sizeof(*ctx));
  ctx->addr = addr;
  ctx->params = params;

  // add to live tree
  rb_insert(&ta->live_contexts, &ctx->live_it, &ta_context_cb);

  return ctx;
}

static void ta_unlink_context(struct ta *ta, struct tile_ctx *ctx) {
  rb_unlink(&ta->live_contexts, &ctx->live_it, &ta_context_cb);
}

static void ta_free_context(struct ta *ta, struct tile_ctx *ctx) {
  list_add(&ta->free_contexts, &ctx->free_it);
}

static void ta_init_context(struct ta *ta, uint32_t addr) {
  struct tile_ctx *ctx = ta_get_context(ta, addr);

  if (!ctx) {
    ctx = ta_alloc_context(ta, addr);
  }

  ctx->addr = addr;
  ctx->cursor = 0;
  ctx->size = 0;
  ctx->last_poly = NULL;
  ctx->last_vertex = NULL;
  ctx->list_type = 0;
  ctx->vertex_type = 0;
}

static void ta_write_context(struct ta *ta, uint32_t addr, uint32_t value) {
  struct tile_ctx *ctx = ta_get_context(ta, addr);
  CHECK_NOTNULL(ctx);

  CHECK_LT(ctx->size + 4, TA_MAX_PARAMS);
  *(uint32_t *)&ctx->params[ctx->size] = value;
  ctx->size += 4;

  // each TA command is either 32 or 64 bytes, with the pcw being in the first
  // 32 bytes always. check every 32 bytes to see if the command has been
  // completely received or not
  if (ctx->size % 32 == 0) {
    void *param = &ctx->params[ctx->cursor];
    union pcw pcw = *(union pcw *)param;

    int size = ta_get_param_size(pcw, ctx->vertex_type);
    int recv = ctx->size - ctx->cursor;

    if (recv < size) {
      // wait for the entire command
      return;
    }

    if (pcw.para_type == TA_PARAM_END_OF_LIST) {
      holly_raise_interrupt(ta->holly, list_interrupts[ctx->list_type]);

      ctx->last_poly = NULL;
      ctx->last_vertex = NULL;
      ctx->list_type = 0;
      ctx->vertex_type = 0;
    } else if (pcw.para_type == TA_PARAM_OBJ_LIST_SET) {
      LOG_FATAL("TA_PARAM_OBJ_LIST_SET unsupported");
    } else if (pcw.para_type == TA_PARAM_POLY_OR_VOL) {
      ctx->last_poly = (union poly_param *)param;
      ctx->last_vertex = NULL;
      ctx->list_type = ctx->last_poly->type0.pcw.list_type;
      ctx->vertex_type = ta_get_vert_type(ctx->last_poly->type0.pcw);
    } else if (pcw.para_type == TA_PARAM_SPRITE) {
      ctx->last_poly = (union poly_param *)param;
      ctx->last_vertex = NULL;
      ctx->list_type = ctx->last_poly->type0.pcw.list_type;
      ctx->vertex_type = ta_get_vert_type(ctx->last_poly->type0.pcw);
    }

    ctx->cursor += recv;
  }
}

static void ta_register_texture(struct ta *ta, union tsp tsp, union tcw tcw) {
  struct ta_texture_entry *entry = ta_find_texture(ta, tsp, tcw);
  int new_entry = 0;

  if (!entry) {
    entry = ta_alloc_texture(ta, tsp, tcw);
    new_entry = 1;
  }

  // mark texture source valid for the current frame
  entry->base.frame = ta->frame;

  // set texture address
  if (!entry->base.texture) {
    uint32_t texture_addr = tcw.texture_addr << 3;
    int width = 8 << tsp.texture_u_size;
    int height = 8 << tsp.texture_v_size;
    int element_size_bits = tcw.pixel_format == TA_PIXEL_8BPP
                                ? 8
                                : tcw.pixel_format == TA_PIXEL_4BPP ? 4 : 16;
    entry->base.texture = &ta->video_ram[texture_addr];
    entry->base.texture_size = (width * height * element_size_bits) >> 3;
  }

  // set palette address
  if (!entry->base.palette) {
    if (tcw.pixel_format == TA_PIXEL_4BPP ||
        tcw.pixel_format == TA_PIXEL_8BPP) {
      uint32_t palette_addr = 0;
      int palette_size = 0;

      // palette ram is 4096 bytes, with each palette entry being 4 bytes each,
      // resulting in 1 << 10 indexes
      if (tcw.pixel_format == TA_PIXEL_4BPP) {
        // in 4bpp mode, the palette selector represents the upper 6 bits of the
        // palette index, with the remaining 4 bits being filled in by the
        // texture
        palette_addr = (tcw.p.palette_selector << 4) * 4;
        palette_size = (1 << 4) * 4;
      } else if (tcw.pixel_format == TA_PIXEL_8BPP) {
        // in 4bpp mode, the palette selector represents the upper 2 bits of the
        // palette index, with the remaining 8 bits being filled in by the
        // texture
        palette_addr = ((tcw.p.palette_selector & 0x30) << 4) * 4;
        palette_size = (1 << 8) * 4;
      }

      entry->base.palette = &ta->palette_ram[palette_addr];
      entry->base.palette_size = palette_size;
    }
  }

  // add write callback in order to invalidate on future writes. the callback
  // address will be page aligned, therefore it will be triggered falsely in
  // some cases. over invalidate in these cases
  if (!entry->texture_watch) {
    entry->texture_watch =
        add_single_write_watch(entry->base.texture, entry->base.texture_size,
                               &ta_texture_invalidated, entry);
  }

  if (entry->base.palette && !entry->palette_watch) {
    entry->palette_watch =
        add_single_write_watch(entry->base.palette, entry->base.palette_size,
                               &ta_palette_invalidated, entry);
  }

  // ad new entries to the trace
  if (ta->trace_writer && new_entry) {
    trace_writer_insert_texture(ta->trace_writer, tsp, tcw, entry->base.palette,
                                entry->base.palette_size, entry->base.texture,
                                entry->base.texture_size);
  }
}

static void ta_register_textures(struct ta *ta, struct tile_ctx *ctx,
                                 int *num_polys) {
  const uint8_t *data = ctx->params;
  const uint8_t *end = ctx->params + ctx->size;
  int vertex_type = 0;

  *num_polys = 0;

  while (data < end) {
    union pcw pcw = *(union pcw *)data;

    switch (pcw.para_type) {
      case TA_PARAM_POLY_OR_VOL:
      case TA_PARAM_SPRITE: {
        const union poly_param *param = (const union poly_param *)data;

        vertex_type = ta_get_vert_type(param->type0.pcw);

        if (param->type0.pcw.texture) {
          ta_register_texture(ta, param->type0.tsp, param->type0.tcw);
        }

        (*num_polys)++;
      } break;

      default:
        break;
    }

    data += ta_get_param_size(pcw, vertex_type);
  }
}

static void ta_save_register_state(struct ta *ta, struct tile_ctx *ctx) {
  struct pvr *pvr = ta->pvr;

  // autosort
  if (!pvr->FPU_PARAM_CFG->region_header_type) {
    ctx->autosort = !pvr->ISP_FEED_CFG->presort;
  } else {
    uint32_t region_data = as_read32(ta->space, 0x05000000 + *pvr->REGION_BASE);
    ctx->autosort = !(region_data & 0x20000000);
  }

  // texture stride
  ctx->stride = pvr->TEXT_CONTROL->stride * 32;

  // texture palette pixel format
  ctx->pal_pxl_format = pvr->PAL_RAM_CTRL->pixel_format;

  // write out video width to help with unprojecting the screen space
  // coordinates
  if (pvr->SPG_CONTROL->interlace ||
      (!pvr->SPG_CONTROL->NTSC && !pvr->SPG_CONTROL->PAL)) {
    // interlaced and VGA mode both render at full resolution
    ctx->video_width = 640;
    ctx->video_height = 480;
  } else {
    ctx->video_width = 320;
    ctx->video_height = 240;
  }

  // according to the hardware docs, this is the correct calculation of the
  // background ISP address. however, in practice, the second TA buffer's ISP
  // address comes out to be 0x800000 when booting the bios and the vram is
  // only 8mb total. by examining a raw memory dump, the ISP data is only ever
  // available at 0x0 when booting the bios, so masking this seems to be the
  // correct solution
  uint32_t vram_offset =
      0x05000000 +
      ((ctx->addr + pvr->ISP_BACKGND_T->tag_address * 4) & 0x7fffff);

  // get surface parameters
  ctx->bg_isp.full = as_read32(ta->space, vram_offset);
  ctx->bg_tsp.full = as_read32(ta->space, vram_offset + 4);
  ctx->bg_tcw.full = as_read32(ta->space, vram_offset + 8);
  vram_offset += 12;

  // get the background depth
  ctx->bg_depth = *(float *)pvr->ISP_BACKGND_D;

  // get the byte size for each vertex. normally, the byte size is
  // ISP_BACKGND_T.skip + 3, but if parameter selection volume mode is in
  // effect and the shadow bit is 1, then the byte size is
  // ISP_BACKGND_T.skip * 2 + 3
  int vertex_size = pvr->ISP_BACKGND_T->skip;
  if (!pvr->FPU_SHAD_SCALE->intensity_volume_mode &&
      pvr->ISP_BACKGND_T->shadow) {
    vertex_size *= 2;
  }
  vertex_size = (vertex_size + 3) * 4;

  // skip to the first vertex
  vram_offset += pvr->ISP_BACKGND_T->tag_offset * vertex_size;

  // copy vertex data to context
  for (int i = 0, bg_offset = 0; i < 3; i++) {
    CHECK_LE(bg_offset + vertex_size, (int)sizeof(ctx->bg_vertices));

    as_memcpy_to_host(ta->space, &ctx->bg_vertices[bg_offset], vram_offset,
                      vertex_size);

    bg_offset += vertex_size;
    vram_offset += vertex_size;
  }
}

static void ta_end_render(struct ta *ta) {
  // let the game know rendering is complete
  holly_raise_interrupt(ta->holly, HOLLY_INTC_PCEOVINT);
  holly_raise_interrupt(ta->holly, HOLLY_INTC_PCEOIINT);
  holly_raise_interrupt(ta->holly, HOLLY_INTC_PCEOTINT);
}

static void ta_render_timer(void *data) {
  struct ta *ta = data;

  // ideally, the graphics thread has parsed the pending context, uploaded its
  // textures, etc. during the estimated render time. however, if it hasn't
  // finished, the emulation thread must be paused to avoid altering
  // the yet-to-be-uploaded texture memory
  mutex_lock(ta->pending_mutex);
  mutex_unlock(ta->pending_mutex);

  ta_end_render(ta);
}

static void ta_start_render(struct ta *ta, uint32_t addr) {
  struct tile_ctx *ctx = ta_get_context(ta, addr);
  CHECK_NOTNULL(ctx);

  // save off required register state that may be modified by the time the
  // context is rendered
  ta_save_register_state(ta, ctx);

  // if the graphics thread is still parsing the previous context, skip this one
  if (!mutex_trylock(ta->pending_mutex)) {
    ta_unlink_context(ta, ctx);
    ta_free_context(ta, ctx);
    ta_end_render(ta);
    ta->frames_skipped++;
    return;
  }

  // free the previous pending context if it wasn't rendered
  if (ta->pending_context) {
    ta_free_context(ta, ta->pending_context);
    ta->pending_context = NULL;
  }

  // set the new pending context
  ta_unlink_context(ta, ctx);
  ta->pending_context = ctx;

  // increment internal frame number. this frame number is assigned to each
  // texture source registered by this context
  ta->frame++;

  // register the source of each texture referenced by the context with the
  // tile renderer. note, the process of actually uploading the texture to the
  // render backend happens lazily while rendering the context (keeping all
  // backend operations on the same thread). this registration just lets the
  // backend know where the texture's source data is
  int num_polys = 0;
  ta_register_textures(ta, ta->pending_context, &num_polys);

  // supposedly, the dreamcast can push around ~3 million polygons per second
  // through the TA / PVR. with that in mind, a very poor estimate can be made
  // for how long the TA would take to render a frame based on the number of
  // polys pushed: 1,000,000,000 / 3,000,000 = 333 nanoseconds per polygon
  int64_t ns = num_polys * INT64_C(333);
  scheduler_start_timer(ta->scheduler, &ta_render_timer, ta, ns);

  if (ta->trace_writer) {
    trace_writer_render_context(ta->trace_writer, ta->pending_context);
  }

  // unlock the mutex, enabling the graphics thread to start parsing the
  // pending context
  mutex_unlock(ta->pending_mutex);
}

static void ta_write_poly_fifo(struct ta *ta, uint32_t addr, uint32_t value) {
  ta_write_context(ta, ta->pvr->TA_ISP_BASE->base_address, value);
}

static void ta_write_texture_fifo(struct ta *ta, uint32_t addr,
                                  uint32_t value) {
  addr &= 0xeeffffff;
  *(uint32_t *)&ta->video_ram[addr] = value;
}

REG_W32(struct ta *ta, SOFTRESET) {
  if (!(*new_value & 0x1)) {
    return;
  }

  ta_soft_reset(ta);
}

REG_W32(struct ta *ta, TA_LIST_INIT) {
  if (!(*new_value & 0x80000000)) {
    return;
  }

  ta_init_context(ta, ta->pvr->TA_ISP_BASE->base_address);
}

REG_W32(struct ta *ta, TA_LIST_CONT) {
  if (!(*new_value & 0x80000000)) {
    return;
  }

  LOG_WARNING("Unsupported TA_LIST_CONT");
}

REG_W32(struct ta *ta, STARTRENDER) {
  if (!*new_value) {
    return;
  }

  ta_start_render(ta, ta->pvr->PARAM_BASE->base_address);
}

static bool ta_init(struct device *dev) {
  struct ta *ta = container_of(dev, struct ta, base);
  struct dreamcast *dc = ta->base.dc;

  ta->scheduler = dc->scheduler;
  ta->holly = dc->holly;
  ta->pvr = dc->pvr;
  ta->space = dc->sh4->base.memory->space;
  ta->video_ram = as_translate(ta->space, 0x04000000);
  ta->palette_ram = as_translate(ta->space, 0x005f9000);

  for (int i = 0; i < array_size(ta->entries); i++) {
    struct ta_texture_entry *entry = &ta->entries[i];
    list_add(&ta->free_entries, &entry->free_it);
  }

  for (int i = 0; i < array_size(ta->contexts); i++) {
    struct tile_ctx *ctx = &ta->contexts[i];

    ctx->params = ta->params + (TA_MAX_PARAMS * i);

    list_add(&ta->free_contexts, &ctx->free_it);
  }

// initialize registers
#define TA_REG_R32(name)        \
  ta->pvr->reg_data[name] = ta; \
  ta->pvr->reg_read[name] = (reg_read_cb)&name##_r;
#define TA_REG_W32(name)        \
  ta->pvr->reg_data[name] = ta; \
  ta->pvr->reg_write[name] = (reg_write_cb)&name##_w;
  TA_REG_W32(SOFTRESET);
  TA_REG_W32(TA_LIST_INIT);
  TA_REG_W32(TA_LIST_CONT);
  TA_REG_W32(STARTRENDER);
#undef TA_REG_R32
#undef TA_REG_W32

  return true;
}

static void ta_toggle_tracing(struct ta *ta) {
  if (!ta->trace_writer) {
    char filename[PATH_MAX];
    get_next_trace_filename(filename, sizeof(filename));

    ta->trace_writer = trace_writer_open(filename);

    if (!ta->trace_writer) {
      LOG_INFO("Failed to start tracing");
      return;
    }

    // clear texture cache in order to generate insert events for all
    // textures referenced while tracing
    ta_clear_textures(ta);

    LOG_INFO("Begin tracing to %s", filename);
  } else {
    trace_writer_close(ta->trace_writer);
    ta->trace_writer = NULL;

    LOG_INFO("End tracing");
  }
}

static void ta_paint(struct device *dev) {
  struct ta *ta = container_of(dev, struct ta, base);
  struct render_ctx *rctx = &ta->render_context;

  mutex_lock(ta->pending_mutex);

  if (ta->pending_context) {
    rctx->surfs = ta->surfs;
    rctx->surfs_size = array_size(ta->surfs);
    rctx->verts = ta->verts;
    rctx->verts_size = array_size(ta->verts);
    rctx->sorted_surfs = ta->sorted_surfs;
    rctx->sorted_surfs_size = array_size(ta->sorted_surfs);

    tr_parse_context(ta->tr, ta->pending_context, ta->frame, rctx);

    ta_free_context(ta, ta->pending_context);
    ta->pending_context = NULL;
  }

  mutex_unlock(ta->pending_mutex);

  tr_render_context(ta->tr, rctx);
}

static void ta_paint_debug_menu(struct device *dev, struct nk_context *ctx) {
  struct ta *ta = container_of(dev, struct ta, base);

  if (nk_tree_push(ctx, NK_TREE_TAB, "ta", NK_MINIMIZED)) {
    nk_value_int(ctx, "frames skipped", ta->frames_skipped);
    nk_value_int(ctx, "num textures", ta->num_textures);

    if (!ta->trace_writer &&
        nk_button_label(ctx, "start trace", NK_BUTTON_DEFAULT)) {
      ta_toggle_tracing(ta);
    } else if (ta->trace_writer &&
               nk_button_label(ctx, "stop trace", NK_BUTTON_DEFAULT)) {
      ta_toggle_tracing(ta);
    }

    nk_tree_pop(ctx);
  }
}

void ta_build_tables() {
  static bool initialized = false;

  if (initialized) {
    return;
  }

  initialized = true;

  for (int i = 0; i < 0x100; i++) {
    union pcw pcw = *(union pcw *)&i;

    for (int j = 0; j < TA_NUM_PARAMS; j++) {
      pcw.para_type = j;

      for (int k = 0; k < TA_NUM_VERT_TYPES; k++) {
        g_param_sizes[i * TA_NUM_PARAMS * TA_NUM_VERT_TYPES +
                      j * TA_NUM_VERT_TYPES + k] =
            ta_get_param_size_raw(pcw, k);
      }
    }
  }

  for (int i = 0; i < 0x100; i++) {
    union pcw pcw = *(union pcw *)&i;

    for (int j = 0; j < TA_NUM_PARAMS; j++) {
      pcw.para_type = j;

      for (int k = 0; k < TA_NUM_LISTS; k++) {
        pcw.list_type = k;

        g_poly_types[i * TA_NUM_PARAMS * TA_NUM_LISTS + j * TA_NUM_LISTS + k] =
            ta_get_poly_type_raw(pcw);
        g_vertex_types[i * TA_NUM_PARAMS * TA_NUM_LISTS + j * TA_NUM_LISTS +
                       k] = ta_get_vert_type_raw(pcw);
      }
    }
  }
}

struct ta *ta_create(struct dreamcast *dc, struct rb *rb) {
  ta_build_tables();

  struct ta *ta = dc_create_device(dc, sizeof(struct ta), "ta", &ta_init);
  ta->base.window =
      window_interface_create(&ta_paint, &ta_paint_debug_menu, NULL);
  ta->provider =
      (struct texture_provider){ta, &ta_texture_provider_find_texture};
  ta->rb = rb;
  ta->tr = tr_create(ta->rb, &ta->provider);
  ta->pending_mutex = mutex_create();

  return ta;
}

void ta_destroy(struct ta *ta) {
  mutex_destroy(ta->pending_mutex);
  tr_destroy(ta->tr);
  window_interface_destroy(ta->base.window);
  dc_destroy_device(&ta->base);
}

// clang-format off
AM_BEGIN(struct ta, ta_fifo_map);
  AM_RANGE(0x0000000, 0x07fffff) AM_HANDLE(NULL,
                                           NULL,
                                           NULL,
                                           NULL,
                                           NULL,
                                           NULL,
                                           (w32_cb)&ta_write_poly_fifo,
                                           NULL)
  AM_RANGE(0x1000000, 0x1ffffff) AM_HANDLE(NULL,
                                           NULL,
                                           NULL,
                                           NULL,
                                           NULL,
                                           NULL,
                                           (w32_cb)&ta_write_texture_fifo,
                                           NULL)
AM_END();
// clang-format on
