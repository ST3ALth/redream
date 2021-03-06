#ifndef PVR_H
#define PVR_H

#include <stdint.h>
#include "hw/dreamcast.h"
#include "hw/holly/pvr_types.h"
#include "hw/memory.h"
#include "hw/scheduler.h"

struct dreamcast;
struct holly;

struct pvr {
  struct device base;

  struct scheduler *scheduler;
  struct holly *holly;
  struct address_space *space;

  uint8_t *palette_ram;
  uint8_t *video_ram;
  uint32_t reg[NUM_PVR_REGS];
  void *reg_data[NUM_PVR_REGS];
  reg_read_cb reg_read[NUM_PVR_REGS];
  reg_write_cb reg_write[NUM_PVR_REGS];
  struct timer *line_timer;
  int line_clock;
  uint32_t current_scanline;

#define PVR_REG(offset, name, default, type) type *name;
#include "hw/holly/pvr_regs.inc"
#undef PVR_REG
};

struct pvr *pvr_create(struct dreamcast *dc);
void pvr_destroy(struct pvr *pvr);

AM_DECLARE(pvr_reg_map);
AM_DECLARE(pvr_vram_map);

#endif
