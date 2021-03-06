#ifndef DEBUGGER_H
#define DEBUGGER_H

#include <stdbool.h>
#include <stdint.h>

struct dreamcast;
struct debugger;

bool debugger_init(struct debugger *dbg);
void debugger_trap(struct debugger *dbg);
void debugger_tick(struct debugger *dbg);

struct debugger *debugger_create(struct dreamcast *dc);
void debugger_destroy(struct debugger *dbg);

#endif
