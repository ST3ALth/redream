#include "jit/frontend/sh4/sh4_translate.h"
#include "core/assert.h"
#include "core/profiler.h"
#include "jit/frontend/sh4/sh4_analyze.h"
#include "jit/frontend/sh4/sh4_context.h"
#include "jit/frontend/sh4/sh4_disasm.h"
#include "jit/frontend/sh4/sh4_frontend.h"
#include "jit/ir/ir.h"

//
// fsca estimate lookup table
//
static uint32_t s_fsca_table[0x20000] = {
#include "jit/frontend/sh4/sh4_fsca.inc"
};

//
// callbacks for translating each sh4 op
//
typedef void (*emit_cb)(struct ir *, int, const struct sh4_instr *,
                        const struct sh4_instr *);

#define EMITTER(name)                                                          \
  void sh4_emit_OP_##name(struct ir *ir, int flags, const struct sh4_instr *i, \
                          const struct sh4_instr *delay)

#define SH4_INSTR(name, desc, instr_code, cycles, flags) static EMITTER(name);
#include "jit/frontend/sh4/sh4_instr.inc"
#undef SH4_INSTR

static emit_cb emit_callbacks[NUM_SH4_OPS] = {
    NULL,  // SH4_OP_INVALID
#define SH4_INSTR(name, desc, instr_code, cycles, flags) &sh4_emit_OP_##name,
#include "jit/frontend/sh4/sh4_instr.inc"
#undef SH4_INSTR
};

// helper functions for accessing the sh4 context, macros are used to cut
// down on copy and paste
#define load_guest(addr, type)                          \
  ((flags & SH4_SLOWMEM) ? ir_load_slow(ir, addr, type) \
                         : ir_load_fast(ir, addr, type))

#define store_guest(addr, v)                              \
  do {                                                    \
    ((flags & SH4_SLOWMEM) ? ir_store_slow(ir, addr, v)   \
                           : ir_store_fast(ir, addr, v)); \
  } while (0)

#define load_gpr(n, type) \
  ir_load_context(ir, offsetof(struct sh4_ctx, r[n]), type)

#define store_gpr(n, v)                                      \
  do {                                                       \
    CHECK_EQ(v->type, VALUE_I32);                            \
    ir_store_context(ir, offsetof(struct sh4_ctx, r[n]), v); \
  } while (0)

// swizzle 32-bit fp loads, see notes in sh4_context.h
#define swizzle_fpr(n, type) (ir_type_size(type) == 4 ? ((n) ^ 1) : (n))

#define load_fpr(n, type) \
  ir_load_context(ir, offsetof(struct sh4_ctx, fr[swizzle_fpr(n, type)]), type)

#define store_fpr(n, v)                                         \
  do {                                                          \
    int tmp = swizzle_fpr(n, v->type);                          \
    ir_store_context(ir, offsetof(struct sh4_ctx, fr[tmp]), v); \
  } while (0)

#define load_xfr(n, type) \
  ir_load_context(ir, offsetof(struct sh4_ctx, xf[swizzle_fpr(n, type)]), type)

#define store_xfr(n, v)                                         \
  do {                                                          \
    int tmp = swizzle_fpr(n, v->type);                          \
    ir_store_context(ir, offsetof(struct sh4_ctx, xf[tmp]), v); \
  } while (0)

#define load_sr() ir_load_context(ir, offsetof(struct sh4_ctx, sr), VALUE_I32)

#define store_sr(v)                                                          \
  do {                                                                       \
    CHECK_EQ(v->type, VALUE_I32);                                            \
    struct ir_value *sr_updated =                                            \
        ir_load_context(ir, offsetof(struct sh4_ctx, SRUpdated), VALUE_I64); \
    struct ir_value *old_sr = load_sr();                                     \
    ir_store_context(ir, offsetof(struct sh4_ctx, sr), v);                   \
    ir_call_external_2(ir, sr_updated, ir_zext(ir, old_sr, VALUE_I64));      \
  } while (0)

#define load_t() ir_and(ir, load_sr(), ir_alloc_i32(ir, T))

#define store_t(v)                                                  \
  do {                                                              \
    struct ir_value *sr = load_sr();                                \
    struct ir_value *sr_t = ir_or(ir, sr, ir_alloc_i32(ir, T));     \
    struct ir_value *sr_not = ir_and(ir, sr, ir_alloc_i32(ir, ~T)); \
    store_sr(ir_select(ir, v, sr_t, sr_not));                       \
  } while (0)

#define load_gbr() ir_load_context(ir, offsetof(struct sh4_ctx, gbr), VALUE_I32)

#define store_gbr(v)                                        \
  do {                                                      \
    ir_store_context(ir, offsetof(struct sh4_ctx, gbr), v); \
  } while (0)

#define load_fpscr()                                                          \
  ir_and(ir, ir_load_context(ir, offsetof(struct sh4_ctx, fpscr), VALUE_I32), \
         ir_alloc_i32(ir, 0x003fffff))

#define store_fpscr(v)                                                        \
  do {                                                                        \
    CHECK_EQ(v->type, VALUE_I32);                                             \
    v = ir_and(ir, v, ir_alloc_i32(ir, 0x003fffff));                          \
                                                                              \
    struct ir_value *fpscr_updated = ir_load_context(                         \
        ir, offsetof(struct sh4_ctx, FPSCRUpdated), VALUE_I64);               \
    struct ir_value *old_fpscr = load_fpscr();                                \
    ir_store_context(ir, offsetof(struct sh4_ctx, fpscr), v);                 \
    ir_call_external_2(ir, fpscr_updated, ir_zext(ir, old_fpscr, VALUE_I64)); \
  } while (0)

#define load_pr() ir_load_context(ir, offsetof(struct sh4_ctx, pr), VALUE_I32);

#define store_pr(v)                                        \
  do {                                                     \
    CHECK_EQ(v->type, VALUE_I32);                          \
    ir_store_context(ir, offsetof(struct sh4_ctx, pr), v); \
  } while (0)

#define emit_delay_instr() sh4_emit_instr(ir, flags, delay, NULL)

static void sh4_invalid_instr(struct ir *ir, uint32_t guest_addr) {
  struct ir_value *invalid_instruction = ir_load_context(
      ir, offsetof(struct sh4_ctx, InvalidInstruction), VALUE_I64);
  ir_call_external_2(ir, invalid_instruction,
                     ir_alloc_i64(ir, (int64_t)guest_addr));
}

static void sh4_emit_instr(struct ir *ir, int flags,
                           const struct sh4_instr *instr,
                           const struct sh4_instr *delay) {
  (emit_callbacks[instr->op])(ir, flags, instr, delay);
}

// MOV     #imm,Rn
EMITTER(MOVI) {
  struct ir_value *v = ir_alloc_i32(ir, (int32_t)(int8_t)i->imm);
  store_gpr(i->Rn, v);
}

// MOV.W   @(disp,PC),Rn
EMITTER(MOVWLPC) {
  uint32_t addr = (i->disp * 2) + i->addr + 4;
  struct ir_value *v = load_guest(ir_alloc_i32(ir, addr), VALUE_I16);
  v = ir_sext(ir, v, VALUE_I32);
  store_gpr(i->Rn, v);
}

// MOV.L   @(disp,PC),Rn
EMITTER(MOVLLPC) {
  uint32_t addr = (i->disp * 4) + (i->addr & ~3) + 4;
  struct ir_value *v = load_guest(ir_alloc_i32(ir, addr), VALUE_I32);
  store_gpr(i->Rn, v);
}

// MOV     Rm,Rn
EMITTER(MOV) {
  struct ir_value *v = load_gpr(i->Rm, VALUE_I32);
  store_gpr(i->Rn, v);
}

// MOV.B   Rm,@Rn
EMITTER(MOVBS) {
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *v = load_gpr(i->Rm, VALUE_I8);
  store_guest(addr, v);
}

// MOV.W   Rm,@Rn
EMITTER(MOVWS) {
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *v = load_gpr(i->Rm, VALUE_I16);
  store_guest(addr, v);
}

// MOV.L   Rm,@Rn
EMITTER(MOVLS) {
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *v = load_gpr(i->Rm, VALUE_I32);
  store_guest(addr, v);
}

// MOV.B   @Rm,Rn
EMITTER(MOVBL) {
  struct ir_value *v = load_guest(load_gpr(i->Rm, VALUE_I32), VALUE_I8);
  v = ir_sext(ir, v, VALUE_I32);
  store_gpr(i->Rn, v);
}

// MOV.W   @Rm,Rn
EMITTER(MOVWL) {
  struct ir_value *v = load_guest(load_gpr(i->Rm, VALUE_I32), VALUE_I16);
  v = ir_sext(ir, v, VALUE_I32);
  store_gpr(i->Rn, v);
}

// MOV.L   @Rm,Rn
EMITTER(MOVLL) {
  struct ir_value *v = load_guest(load_gpr(i->Rm, VALUE_I32), VALUE_I32);
  store_gpr(i->Rn, v);
}

// MOV.B   Rm,@-Rn
EMITTER(MOVBM) {
  // decrease Rn by 1
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  addr = ir_sub(ir, addr, ir_alloc_i32(ir, 1));
  store_gpr(i->Rn, addr);

  // store Rm at (Rn)
  struct ir_value *v = load_gpr(i->Rm, VALUE_I8);
  store_guest(addr, v);
}

// MOV.W   Rm,@-Rn
EMITTER(MOVWM) {
  // decrease Rn by 2
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  addr = ir_sub(ir, addr, ir_alloc_i32(ir, 2));
  store_gpr(i->Rn, addr);

  // store Rm at (Rn)
  struct ir_value *v = load_gpr(i->Rm, VALUE_I16);
  store_guest(addr, v);
}

// MOV.L   Rm,@-Rn
EMITTER(MOVLM) {
  // decrease Rn by 4
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  addr = ir_sub(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);

  // store Rm at (Rn)
  struct ir_value *v = load_gpr(i->Rm, VALUE_I32);
  store_guest(addr, v);
}

// MOV.B   @Rm+,Rn
EMITTER(MOVBP) {
  // store (Rm) at Rn
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I8);
  v = ir_sext(ir, v, VALUE_I32);
  store_gpr(i->Rn, v);

  // increase Rm by 1
  // FIXME if rm != rn???
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 1));
  store_gpr(i->Rm, addr);
}

// MOV.W   @Rm+,Rn
EMITTER(MOVWP) {
  // store (Rm) at Rn
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I16);
  v = ir_sext(ir, v, VALUE_I32);
  store_gpr(i->Rn, v);

  // increase Rm by 2
  // FIXME if rm != rn???
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 2));
  store_gpr(i->Rm, addr);
}

// MOV.L   @Rm+,Rn
EMITTER(MOVLP) {
  // store (Rm) at Rn
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I32);
  store_gpr(i->Rn, v);

  // increase Rm by 2
  // FIXME if rm != rn???
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rm, addr);
}

// MOV.B   R0,@(disp,Rn)
EMITTER(MOVBS0D) {
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, i->disp));
  struct ir_value *v = load_gpr(0, VALUE_I8);
  store_guest(addr, v);
}

// MOV.W   R0,@(disp,Rn)
EMITTER(MOVWS0D) {
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, i->disp * 2));
  struct ir_value *v = load_gpr(0, VALUE_I16);
  store_guest(addr, v);
}

// MOV.L Rm,@(disp,Rn)
EMITTER(MOVLSMD) {
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, i->disp * 4));
  struct ir_value *v = load_gpr(i->Rm, VALUE_I32);
  store_guest(addr, v);
}

// MOV.B   @(disp,Rm),R0
EMITTER(MOVBLD0) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, i->disp));
  struct ir_value *v = load_guest(addr, VALUE_I8);
  v = ir_sext(ir, v, VALUE_I32);
  store_gpr(0, v);
}

// MOV.W   @(disp,Rm),R0
EMITTER(MOVWLD0) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, i->disp * 2));
  struct ir_value *v = load_guest(addr, VALUE_I16);
  v = ir_sext(ir, v, VALUE_I32);
  store_gpr(0, v);
}

// MOV.L   @(disp,Rm),Rn
EMITTER(MOVLLDN) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, i->disp * 4));
  struct ir_value *v = load_guest(addr, VALUE_I32);
  store_gpr(i->Rn, v);
}

// MOV.B   Rm,@(R0,Rn)
EMITTER(MOVBS0) {
  struct ir_value *addr = load_gpr(0, VALUE_I32);
  addr = ir_add(ir, addr, load_gpr(i->Rn, VALUE_I32));
  struct ir_value *v = load_gpr(i->Rm, VALUE_I8);
  store_guest(addr, v);
}

// MOV.W   Rm,@(R0,Rn)
EMITTER(MOVWS0) {
  struct ir_value *addr = load_gpr(0, VALUE_I32);
  addr = ir_add(ir, addr, load_gpr(i->Rn, VALUE_I32));
  struct ir_value *v = load_gpr(i->Rm, VALUE_I16);
  store_guest(addr, v);
}

// MOV.L   Rm,@(R0,Rn)
EMITTER(MOVLS0) {
  struct ir_value *addr = load_gpr(0, VALUE_I32);
  addr = ir_add(ir, addr, load_gpr(i->Rn, VALUE_I32));
  struct ir_value *v = load_gpr(i->Rm, VALUE_I32);
  store_guest(addr, v);
}

// MOV.B   @(R0,Rm),Rn
EMITTER(MOVBL0) {
  struct ir_value *addr = load_gpr(0, VALUE_I32);
  addr = ir_add(ir, addr, load_gpr(i->Rm, VALUE_I32));
  struct ir_value *v = ir_sext(ir, load_guest(addr, VALUE_I8), VALUE_I32);
  store_gpr(i->Rn, v);
}

// MOV.W   @(R0,Rm),Rn
EMITTER(MOVWL0) {
  struct ir_value *addr = load_gpr(0, VALUE_I32);
  addr = ir_add(ir, addr, load_gpr(i->Rm, VALUE_I32));
  struct ir_value *v = load_guest(addr, VALUE_I16);
  v = ir_sext(ir, v, VALUE_I32);
  store_gpr(i->Rn, v);
}

// MOV.L   @(R0,Rm),Rn
EMITTER(MOVLL0) {
  struct ir_value *addr = load_gpr(0, VALUE_I32);
  addr = ir_add(ir, addr, load_gpr(i->Rm, VALUE_I32));
  struct ir_value *v = load_guest(addr, VALUE_I32);
  store_gpr(i->Rn, v);
}

// MOV.B   R0,@(disp,GBR)
EMITTER(MOVBS0G) {
  struct ir_value *addr = load_gbr();
  addr = ir_add(ir, addr, ir_alloc_i32(ir, i->disp));
  struct ir_value *v = load_gpr(0, VALUE_I8);
  store_guest(addr, v);
}

// MOV.W   R0,@(disp,GBR)
EMITTER(MOVWS0G) {
  struct ir_value *addr = load_gbr();
  addr = ir_add(ir, addr, ir_alloc_i32(ir, i->disp * 2));
  struct ir_value *v = load_gpr(0, VALUE_I16);
  store_guest(addr, v);
}

// MOV.L   R0,@(disp,GBR)
EMITTER(MOVLS0G) {
  struct ir_value *addr = load_gbr();
  addr = ir_add(ir, addr, ir_alloc_i32(ir, i->disp * 4));
  struct ir_value *v = load_gpr(0, VALUE_I32);
  store_guest(addr, v);
}

// MOV.B   @(disp,GBR),R0
EMITTER(MOVBLG0) {
  struct ir_value *addr = load_gbr();
  addr = ir_add(ir, addr, ir_alloc_i32(ir, i->disp));
  struct ir_value *v = load_guest(addr, VALUE_I8);
  v = ir_sext(ir, v, VALUE_I32);
  store_gpr(0, v);
}

// MOV.W   @(disp,GBR),R0
EMITTER(MOVWLG0) {
  struct ir_value *addr = load_gbr();
  addr = ir_add(ir, addr, ir_alloc_i32(ir, i->disp * 2));
  struct ir_value *v = load_guest(addr, VALUE_I16);
  v = ir_sext(ir, v, VALUE_I32);
  store_gpr(0, v);
}

// MOV.L   @(disp,GBR),R0
EMITTER(MOVLLG0) {
  struct ir_value *addr = load_gbr();
  addr = ir_add(ir, addr, ir_alloc_i32(ir, i->disp * 4));
  struct ir_value *v = load_guest(addr, VALUE_I32);
  store_gpr(0, v);
}

// MOVA    (disp,PC),R0
EMITTER(MOVA) {
  uint32_t addr = (i->disp * 4) + (i->addr & ~3) + 4;
  store_gpr(0, ir_alloc_i32(ir, addr));
}

// MOVT    Rn
EMITTER(MOVT) {
  store_gpr(i->Rn, load_t());
}

// SWAP.B  Rm,Rn
EMITTER(SWAPB) {
  const int nbits = 8;
  struct ir_value *v = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *mask = ir_alloc_i32(ir, (1u << nbits) - 1);
  struct ir_value *tmp =
      ir_and(ir, ir_xor(ir, v, ir_lshri(ir, v, nbits)), mask);
  struct ir_value *res = ir_xor(ir, v, ir_or(ir, tmp, ir_shli(ir, tmp, nbits)));
  store_gpr(i->Rn, res);
}

// SWAP.W  Rm,Rn
EMITTER(SWAPW) {
  const int nbits = 16;
  struct ir_value *v = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *mask = ir_alloc_i32(ir, (1u << nbits) - 1);
  struct ir_value *tmp =
      ir_and(ir, ir_xor(ir, v, ir_lshri(ir, v, nbits)), mask);
  struct ir_value *res = ir_xor(ir, v, ir_or(ir, tmp, ir_shli(ir, tmp, nbits)));
  store_gpr(i->Rn, res);
}

// XTRCT   Rm,Rn
EMITTER(XTRCT) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  rn = ir_lshri(ir, ir_and(ir, rn, ir_alloc_i32(ir, 0xffff0000)), 16);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  rm = ir_shli(ir, ir_and(ir, rm, ir_alloc_i32(ir, 0x0000ffff)), 16);
  store_gpr(i->Rn, ir_or(ir, rn, rm));
}

// code                 cycles  t-bit
// 0011 nnnn mmmm 1100  1       -
// ADD     Rm,Rn
EMITTER(ADD) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_add(ir, rn, rm);
  store_gpr(i->Rn, v);
}

// code                 cycles  t-bit
// 0111 nnnn iiii iiii  1       -
// ADD     #imm,Rn
EMITTER(ADDI) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *imm = ir_alloc_i32(ir, (int32_t)(int8_t)i->imm);
  struct ir_value *v = ir_add(ir, rn, imm);
  store_gpr(i->Rn, v);
}

// code                 cycles  t-bit
// 0011 nnnn mmmm 1110  1       carry
// ADDC    Rm,Rn
EMITTER(ADDC) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_add(ir, rn, rm);
  v = ir_add(ir, v, load_t());
  store_gpr(i->Rn, v);

  // compute carry flag, taken from Hacker's Delight
  struct ir_value *and_rnrm = ir_and(ir, rn, rm);
  struct ir_value *or_rnrm = ir_or(ir, rn, rm);
  struct ir_value *not_v = ir_not(ir, v);
  struct ir_value *carry = ir_and(ir, or_rnrm, not_v);
  carry = ir_or(ir, and_rnrm, carry);
  store_t(carry);
}

// code                 cycles  t-bit
// 0011 nnnn mmmm 1111  1       overflow
// ADDV    Rm,Rn
EMITTER(ADDV) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_add(ir, rn, rm);
  store_gpr(i->Rn, v);

  // compute overflow flag, taken from Hacker's Delight
  struct ir_value *xor_vrn = ir_xor(ir, v, rn);
  struct ir_value *xor_vrm = ir_xor(ir, v, rm);
  struct ir_value *overflow = ir_lshri(ir, ir_and(ir, xor_vrn, xor_vrm), 31);
  store_t(overflow);
}

// code                 cycles  t-bit
// 1000 1000 iiii iiii  1       comparison result
// CMP/EQ #imm,R0
EMITTER(CMPEQI) {
  struct ir_value *imm = ir_alloc_i32(ir, (int32_t)(int8_t)i->imm);
  struct ir_value *r0 = load_gpr(0, VALUE_I32);
  store_t(ir_cmp_eq(ir, r0, imm));
}

// code                 cycles  t-bit
// 0011 nnnn mmmm 0000  1       comparison result
// CMP/EQ  Rm,Rn
EMITTER(CMPEQ) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  store_t(ir_cmp_eq(ir, rn, rm));
}

// code                 cycles  t-bit
// 0011 nnnn mmmm 0010  1       comparison result
// CMP/HS  Rm,Rn
EMITTER(CMPHS) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  store_t(ir_cmp_uge(ir, rn, rm));
}

// code                 cycles  t-bit
// 0011 nnnn mmmm 0011  1       comparison result
// CMP/GE  Rm,Rn
EMITTER(CMPGE) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  store_t(ir_cmp_sge(ir, rn, rm));
}

// code                 cycles  t-bit
// 0011 nnnn mmmm 0110  1       comparison result
// CMP/HI  Rm,Rn
EMITTER(CMPHI) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  store_t(ir_cmp_ugt(ir, rn, rm));
}

// code                 cycles  t-bit
// 0011 nnnn mmmm 0111  1       comparison result
// CMP/GT  Rm,Rn
EMITTER(CMPGT) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  store_t(ir_cmp_sgt(ir, rn, rm));
}

// code                 cycles  t-bit
// 0100 nnnn 0001 0001  1       comparison result
// CMP/PZ  Rn
EMITTER(CMPPZ) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  store_t(ir_cmp_sge(ir, rn, ir_alloc_i32(ir, 0)));
}

// code                 cycles  t-bit
// 0100 nnnn 0001 0101  1       comparison result
// CMP/PL  Rn
EMITTER(CMPPL) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  store_t(ir_cmp_sgt(ir, rn, ir_alloc_i32(ir, 0)));
}

// code                 cycles  t-bit
// 0010 nnnn mmmm 1100  1       comparison result
// CMP/STR  Rm,Rn
EMITTER(CMPSTR) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *diff = ir_xor(ir, rn, rm);

  // if any diff is zero, the bytes match
  struct ir_value *b4_eq = ir_cmp_eq(
      ir, ir_and(ir, diff, ir_alloc_i32(ir, 0xff000000)), ir_alloc_i32(ir, 0));
  struct ir_value *b3_eq = ir_cmp_eq(
      ir, ir_and(ir, diff, ir_alloc_i32(ir, 0x00ff0000)), ir_alloc_i32(ir, 0));
  struct ir_value *b2_eq = ir_cmp_eq(
      ir, ir_and(ir, diff, ir_alloc_i32(ir, 0x0000ff00)), ir_alloc_i32(ir, 0));
  struct ir_value *b1_eq = ir_cmp_eq(
      ir, ir_and(ir, diff, ir_alloc_i32(ir, 0x000000ff)), ir_alloc_i32(ir, 0));

  store_t(ir_or(ir, ir_or(ir, ir_or(ir, b1_eq, b2_eq), b3_eq), b4_eq));
}

// code                 cycles  t-bit
// 0010 nnnn mmmm 0111  1       calculation result
// DIV0S   Rm,Rn
EMITTER(DIV0S) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *qm = ir_xor(ir, rn, rm);

  // update Q == M flag
  ir_store_context(ir, offsetof(struct sh4_ctx, sr_qm), ir_not(ir, qm));

  // msb of Q ^ M -> T
  store_t(ir_lshri(ir, qm, 31));
}

// code                 cycles  t-bit
// 0000 0000 0001 1001  1       0
// DIV0U
EMITTER(DIV0U) {  //
  ir_store_context(ir, offsetof(struct sh4_ctx, sr_qm),
                   ir_alloc_i32(ir, 0x80000000));

  store_sr(ir_and(ir, load_sr(), ir_alloc_i32(ir, ~T)));
}

// code                 cycles  t-bit
// 0011 nnnn mmmm 0100  1       calculation result
// DIV1 Rm,Rn
EMITTER(DIV1) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);

  // if Q == M, r0 = ~Rm and C = 1; else, r0 = Rm and C = 0
  struct ir_value *qm = ir_ashri(
      ir, ir_load_context(ir, offsetof(struct sh4_ctx, sr_qm), VALUE_I32), 31);
  struct ir_value *r0 = ir_xor(ir, rm, qm);
  struct ir_value *carry = ir_lshri(ir, qm, 31);

  // initialize output bit as (Q == M) ^ Rn
  qm = ir_xor(ir, qm, rn);

  // shift Rn left by 1 and add T
  rn = ir_shli(ir, rn, 1);
  rn = ir_or(ir, rn, load_t());

  // add or subtract Rm based on r0 and C
  struct ir_value *rd = ir_add(ir, rn, r0);
  rd = ir_add(ir, rd, carry);
  store_gpr(i->Rn, rd);

  // if C is cleared, invert output bit
  struct ir_value *and_rnr0 = ir_and(ir, rn, r0);
  struct ir_value *or_rnr0 = ir_or(ir, rn, r0);
  struct ir_value *not_rd = ir_not(ir, rd);
  carry = ir_and(ir, or_rnr0, not_rd);
  carry = ir_or(ir, and_rnr0, carry);
  carry = ir_lshri(ir, carry, 31);
  qm = ir_select(ir, carry, qm, ir_not(ir, qm));
  ir_store_context(ir, offsetof(struct sh4_ctx, sr_qm), qm);

  // set T to output bit (which happens to be Q == M)
  store_t(ir_lshri(ir, qm, 31));
}

// DMULS.L Rm,Rn
EMITTER(DMULS) {
  struct ir_value *rn = ir_sext(ir, load_gpr(i->Rn, VALUE_I32), VALUE_I64);
  struct ir_value *rm = ir_sext(ir, load_gpr(i->Rm, VALUE_I32), VALUE_I64);

  struct ir_value *p = ir_smul(ir, rm, rn);
  struct ir_value *low = ir_trunc(ir, p, VALUE_I32);
  struct ir_value *high = ir_trunc(ir, ir_lshri(ir, p, 32), VALUE_I32);

  ir_store_context(ir, offsetof(struct sh4_ctx, macl), low);
  ir_store_context(ir, offsetof(struct sh4_ctx, mach), high);
}

// DMULU.L Rm,Rn
EMITTER(DMULU) {
  struct ir_value *rn = ir_zext(ir, load_gpr(i->Rn, VALUE_I32), VALUE_I64);
  struct ir_value *rm = ir_zext(ir, load_gpr(i->Rm, VALUE_I32), VALUE_I64);

  struct ir_value *p = ir_umul(ir, rm, rn);
  struct ir_value *low = ir_trunc(ir, p, VALUE_I32);
  struct ir_value *high = ir_trunc(ir, ir_lshri(ir, p, 32), VALUE_I32);

  ir_store_context(ir, offsetof(struct sh4_ctx, macl), low);
  ir_store_context(ir, offsetof(struct sh4_ctx, mach), high);
}

// DT      Rn
EMITTER(DT) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *v = ir_sub(ir, rn, ir_alloc_i32(ir, 1));
  store_gpr(i->Rn, v);
  store_t(ir_cmp_eq(ir, v, ir_alloc_i32(ir, 0)));
}

// EXTS.B  Rm,Rn
EMITTER(EXTSB) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I8);
  struct ir_value *v = ir_sext(ir, rm, VALUE_I32);
  store_gpr(i->Rn, v);
}

// EXTS.W  Rm,Rn
EMITTER(EXTSW) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I16);
  struct ir_value *v = ir_sext(ir, rm, VALUE_I32);
  store_gpr(i->Rn, v);
}

// EXTU.B  Rm,Rn
EMITTER(EXTUB) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I8);
  struct ir_value *v = ir_zext(ir, rm, VALUE_I32);
  store_gpr(i->Rn, v);
}

// EXTU.W  Rm,Rn
EMITTER(EXTUW) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I16);
  struct ir_value *v = ir_zext(ir, rm, VALUE_I32);
  store_gpr(i->Rn, v);
}

// MAC.L   @Rm+,@Rn+
EMITTER(MACL) {
  LOG_FATAL("MACL not implemented");
}

// MAC.W   @Rm+,@Rn+
EMITTER(MACW) {
  LOG_FATAL("MACW not implemented");
}

// MUL.L   Rm,Rn
EMITTER(MULL) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_smul(ir, rn, rm);
  ir_store_context(ir, offsetof(struct sh4_ctx, macl), v);
}

// MULS    Rm,Rn
EMITTER(MULS) {
  struct ir_value *rn = ir_sext(ir, load_gpr(i->Rn, VALUE_I16), VALUE_I32);
  struct ir_value *rm = ir_sext(ir, load_gpr(i->Rm, VALUE_I16), VALUE_I32);
  struct ir_value *v = ir_smul(ir, rn, rm);
  ir_store_context(ir, offsetof(struct sh4_ctx, macl), v);
}

// MULU    Rm,Rn
EMITTER(MULU) {
  struct ir_value *rn = ir_zext(ir, load_gpr(i->Rn, VALUE_I16), VALUE_I32);
  struct ir_value *rm = ir_zext(ir, load_gpr(i->Rm, VALUE_I16), VALUE_I32);
  struct ir_value *v = ir_umul(ir, rn, rm);
  ir_store_context(ir, offsetof(struct sh4_ctx, macl), v);
}

// NEG     Rm,Rn
EMITTER(NEG) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_neg(ir, rm);
  store_gpr(i->Rn, v);
}

// NEGC    Rm,Rn
EMITTER(NEGC) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *t = load_t();
  struct ir_value *v = ir_sub(ir, ir_neg(ir, rm), t);
  store_gpr(i->Rn, v);
  struct ir_value *carry = ir_or(ir, t, rm);
  store_t(carry);
}

// SUB     Rm,Rn
EMITTER(SUB) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_sub(ir, rn, rm);
  store_gpr(i->Rn, v);
}

// SUBC    Rm,Rn
EMITTER(SUBC) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_sub(ir, rn, rm);
  v = ir_sub(ir, v, load_t());
  store_gpr(i->Rn, v);

  // compute carry flag, taken from Hacker's Delight
  struct ir_value *l = ir_and(ir, ir_not(ir, rn), rm);
  struct ir_value *r = ir_and(ir, ir_or(ir, ir_not(ir, rn), rm), v);
  struct ir_value *carry = ir_or(ir, l, r);
  store_t(carry);
}

// SUBV    Rm,Rn
EMITTER(SUBV) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_sub(ir, rn, rm);
  store_gpr(i->Rn, v);

  // compute overflow flag, taken from Hacker's Delight
  struct ir_value *xor_rnrm = ir_xor(ir, rn, rm);
  struct ir_value *xor_vrn = ir_xor(ir, v, rn);
  struct ir_value *overflow = ir_lshri(ir, ir_and(ir, xor_rnrm, xor_vrn), 31);
  store_t(overflow);
}

// code                 cycles  t-bit
// 0010 nnnn mmmm 1001  1       -
// AND     Rm,Rn
EMITTER(AND) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *v = ir_and(ir, rn, rm);
  store_gpr(i->Rn, v);
}

// code                 cycles  t-bit
// 1100 1001 iiii iiii  1       -
// AND     #imm,R0
EMITTER(ANDI) {
  struct ir_value *r0 = load_gpr(0, VALUE_I32);
  struct ir_value *imm = ir_alloc_i32(ir, i->imm);
  struct ir_value *v = ir_and(ir, r0, imm);
  store_gpr(0, v);
}

// code                 cycles  t-bit
// 1100 1101 iiii iiii  1       -
// AND.B   #imm,@(R0,GBR)
EMITTER(ANDB) {
  struct ir_value *addr = load_gpr(0, VALUE_I32);
  addr = ir_add(ir, addr, load_gbr());
  struct ir_value *v = load_guest(addr, VALUE_I8);
  v = ir_and(ir, v, ir_alloc_i8(ir, (int8_t)i->imm));
  store_guest(addr, v);
}

// NOT     Rm,Rn
EMITTER(NOT) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_not(ir, rm);
  store_gpr(i->Rn, v);
}

// OR      Rm,Rn
EMITTER(OR) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_or(ir, rn, rm);
  store_gpr(i->Rn, v);
}

// OR      #imm,R0
EMITTER(ORI) {
  struct ir_value *r0 = load_gpr(0, VALUE_I32);
  struct ir_value *imm = ir_alloc_i32(ir, i->imm);
  struct ir_value *v = ir_or(ir, r0, imm);
  store_gpr(0, v);
}

// OR.B    #imm,@(R0,GBR)
EMITTER(ORB) {
  struct ir_value *addr = load_gpr(0, VALUE_I32);
  addr = ir_add(ir, addr, load_gbr());
  struct ir_value *v = load_guest(addr, VALUE_I8);
  v = ir_or(ir, v, ir_alloc_i8(ir, (int8_t)i->imm));
  store_guest(addr, v);
}

// TAS.B   @Rn
EMITTER(TAS) {
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I8);
  store_guest(addr, ir_or(ir, v, ir_alloc_i8(ir, 0x80)));
  store_t(ir_cmp_eq(ir, v, ir_alloc_i8(ir, 0)));
}

// TST     Rm,Rn
EMITTER(TST) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_and(ir, rn, rm);
  store_t(ir_cmp_eq(ir, v, ir_alloc_i32(ir, 0)));
}

// TST     #imm,R0
EMITTER(TSTI) {
  struct ir_value *r0 = load_gpr(0, VALUE_I32);
  struct ir_value *imm = ir_alloc_i32(ir, i->imm);
  struct ir_value *v = ir_and(ir, r0, imm);
  store_t(ir_cmp_eq(ir, v, ir_alloc_i32(ir, 0)));
}

// TST.B   #imm,@(R0,GBR)
EMITTER(TSTB) {
  struct ir_value *addr = load_gpr(0, VALUE_I32);
  addr = ir_add(ir, addr, load_gbr());
  struct ir_value *data = load_guest(addr, VALUE_I8);
  struct ir_value *imm = ir_alloc_i8(ir, (int8_t)i->imm);
  struct ir_value *v = ir_and(ir, data, imm);
  store_t(ir_cmp_eq(ir, v, ir_alloc_i8(ir, 0)));
}

// XOR     Rm,Rn
EMITTER(XOR) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_xor(ir, rn, rm);
  store_gpr(i->Rn, v);
}

// XOR     #imm,R0
EMITTER(XORI) {
  struct ir_value *r0 = load_gpr(0, VALUE_I32);
  struct ir_value *imm = ir_alloc_i32(ir, i->imm);
  struct ir_value *v = ir_xor(ir, r0, imm);
  store_gpr(0, v);
}

// XOR.B   #imm,@(R0,GBR)
EMITTER(XORB) {
  struct ir_value *addr = load_gpr(0, VALUE_I32);
  addr = ir_add(ir, addr, load_gbr());
  struct ir_value *data = load_guest(addr, VALUE_I8);
  struct ir_value *imm = ir_alloc_i8(ir, (int8_t)i->imm);
  struct ir_value *v = ir_xor(ir, data, imm);
  store_guest(addr, v);
}

// ROTL    Rn
EMITTER(ROTL) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rn_msb =
      ir_and(ir, ir_lshri(ir, rn, 31), ir_alloc_i32(ir, 0x1));
  struct ir_value *v = ir_or(ir, ir_shli(ir, rn, 1), rn_msb);
  store_gpr(i->Rn, v);
  store_t(rn_msb);
}

// ROTR    Rn
EMITTER(ROTR) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rn_lsb = ir_and(ir, rn, ir_alloc_i32(ir, 0x1));
  struct ir_value *v = ir_shli(ir, rn_lsb, 31);
  v = ir_or(ir, v, ir_lshri(ir, rn, 1));
  store_gpr(i->Rn, v);
  store_t(rn_lsb);
}

// ROTCL   Rn
EMITTER(ROTCL) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rn_msb =
      ir_and(ir, ir_lshri(ir, rn, 31), ir_alloc_i32(ir, 0x1));
  struct ir_value *v = ir_shli(ir, rn, 1);
  v = ir_or(ir, v, load_t());
  store_gpr(i->Rn, v);
  store_t(rn_msb);
}

// ROTCR   Rn
EMITTER(ROTCR) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rn_lsb = ir_and(ir, rn, ir_alloc_i32(ir, 0x1));
  struct ir_value *v = ir_shli(ir, load_t(), 31);
  v = ir_or(ir, v, ir_lshri(ir, rn, 1));
  store_gpr(i->Rn, v);
  store_t(rn_lsb);
}

// SHAD    Rm,Rn
EMITTER(SHAD) {
  // when Rm >= 0, Rn << Rm
  // when Rm < 0, Rn >> Rm
  // when shifting right > 32, Rn = (Rn >= 0 ? 0 : -1)
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_ashd(ir, rn, rm);
  store_gpr(i->Rn, v);
}

// SHAL    Rn      (same as SHLL)
EMITTER(SHAL) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rn_msb =
      ir_and(ir, ir_lshri(ir, rn, 31), ir_alloc_i32(ir, 0x1));
  struct ir_value *v = ir_shli(ir, rn, 1);
  store_gpr(i->Rn, v);
  store_t(rn_msb);
}

// SHAR    Rn
EMITTER(SHAR) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rn_lsb = ir_and(ir, rn, ir_alloc_i32(ir, 0x1));
  struct ir_value *v = ir_ashri(ir, rn, 1);
  store_gpr(i->Rn, v);
  store_t(rn_lsb);
}

// SHLD    Rm,Rn
EMITTER(SHLD) {
  // when Rm >= 0, Rn << Rm
  // when Rm < 0, Rn >> Rm
  // when shifting right >= 32, Rn = 0
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = ir_lshd(ir, rn, rm);
  store_gpr(i->Rn, v);
}

// SHLL    Rn      (same as SHAL)
EMITTER(SHLL) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rn_msb =
      ir_and(ir, ir_lshri(ir, rn, 31), ir_alloc_i32(ir, 0x1));
  struct ir_value *v = ir_shli(ir, rn, 1);
  store_gpr(i->Rn, v);
  store_t(rn_msb);
}

// SHLR    Rn
EMITTER(SHLR) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *rn_lsb = ir_and(ir, rn, ir_alloc_i32(ir, 0x1));
  struct ir_value *v = ir_lshri(ir, rn, 1);
  store_gpr(i->Rn, v);
  store_t(rn_lsb);
}

// SHLL2   Rn
EMITTER(SHLL2) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *v = ir_shli(ir, rn, 2);
  store_gpr(i->Rn, v);
}

// SHLR2   Rn
EMITTER(SHLR2) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *v = ir_lshri(ir, rn, 2);
  store_gpr(i->Rn, v);
}

// SHLL8   Rn
EMITTER(SHLL8) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *v = ir_shli(ir, rn, 8);
  store_gpr(i->Rn, v);
}

// SHLR8   Rn
EMITTER(SHLR8) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *v = ir_lshri(ir, rn, 8);
  store_gpr(i->Rn, v);
}

// SHLL16  Rn
EMITTER(SHLL16) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *v = ir_shli(ir, rn, 16);
  store_gpr(i->Rn, v);
}

// SHLR16  Rn
EMITTER(SHLR16) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *v = ir_lshri(ir, rn, 16);
  store_gpr(i->Rn, v);
}

// code                 cycles  t-bit
// 1000 1011 dddd dddd  3/1     -
// BF      disp
EMITTER(BF) {
  uint32_t dest_addr = ((int8_t)i->disp * 2) + i->addr + 4;
  struct ir_value *cond = load_t();
  ir_branch_cond(ir, cond, ir_alloc_i32(ir, i->addr + 2),
                 ir_alloc_i32(ir, dest_addr));
}

// code                 cycles  t-bit
// 1000 1111 dddd dddd  3/1     -
// BFS     disp
EMITTER(BFS) {
  struct ir_value *cond = load_t();
  emit_delay_instr();
  uint32_t dest_addr = ((int8_t)i->disp * 2) + i->addr + 4;
  ir_branch_cond(ir, cond, ir_alloc_i32(ir, i->addr + 4),
                 ir_alloc_i32(ir, dest_addr));
}

// code                 cycles  t-bit
// 1000 1001 dddd dddd  3/1     -
// BT      disp
EMITTER(BT) {
  uint32_t dest_addr = ((int8_t)i->disp * 2) + i->addr + 4;
  struct ir_value *cond = load_t();
  ir_branch_cond(ir, cond, ir_alloc_i32(ir, dest_addr),
                 ir_alloc_i32(ir, i->addr + 2));
}

// code                 cycles  t-bit
// 1000 1101 dddd dddd  2/1     -
// BTS     disp
EMITTER(BTS) {
  struct ir_value *cond = load_t();
  emit_delay_instr();
  uint32_t dest_addr = ((int8_t)i->disp * 2) + i->addr + 4;
  ir_branch_cond(ir, cond, ir_alloc_i32(ir, dest_addr),
                 ir_alloc_i32(ir, i->addr + 4));
}

// code                 cycles  t-bit
// 1010 dddd dddd dddd  2       -
// BRA     disp
EMITTER(BRA) {
  emit_delay_instr();
  int32_t disp = ((i->disp & 0xfff) << 20) >>
                 20;  // 12-bit displacement must be sign extended
  uint32_t dest_addr = (disp * 2) + i->addr + 4;
  ir_branch(ir, ir_alloc_i32(ir, dest_addr));
}

// code                 cycles  t-bit
// 0000 mmmm 0010 0011  2       -
// BRAF    Rn
EMITTER(BRAF) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  emit_delay_instr();
  struct ir_value *dest_addr = ir_add(ir, ir_alloc_i32(ir, i->addr + 4), rn);
  ir_branch(ir, dest_addr);
}

// code                 cycles  t-bit
// 1011 dddd dddd dddd  2       -
// BSR     disp
EMITTER(BSR) {
  emit_delay_instr();
  int32_t disp = ((i->disp & 0xfff) << 20) >>
                 20;  // 12-bit displacement must be sign extended
  uint32_t ret_addr = i->addr + 4;
  uint32_t dest_addr = ret_addr + disp * 2;
  store_pr(ir_alloc_i32(ir, ret_addr));
  ir_branch(ir, ir_alloc_i32(ir, dest_addr));
}

// code                 cycles  t-bit
// 0000 mmmm 0000 0011  2       -
// BSRF    Rn
EMITTER(BSRF) {
  struct ir_value *rn = load_gpr(i->Rn, VALUE_I32);
  emit_delay_instr();
  struct ir_value *ret_addr = ir_alloc_i32(ir, i->addr + 4);
  struct ir_value *dest_addr = ir_add(ir, rn, ret_addr);
  store_pr(ret_addr);
  ir_branch(ir, dest_addr);
}

// JMP     @Rm
EMITTER(JMP) {
  struct ir_value *dest_addr = load_gpr(i->Rn, VALUE_I32);
  emit_delay_instr();
  ir_branch(ir, dest_addr);
}

// JSR     @Rn
EMITTER(JSR) {
  struct ir_value *dest_addr = load_gpr(i->Rn, VALUE_I32);
  emit_delay_instr();
  struct ir_value *ret_addr = ir_alloc_i32(ir, i->addr + 4);
  store_pr(ret_addr);
  ir_branch(ir, dest_addr);
}

// RTS
EMITTER(RTS) {
  struct ir_value *dest_addr = load_pr();
  emit_delay_instr();
  ir_branch(ir, dest_addr);
}

// code                 cycles  t-bit
// 0000 0000 0010 1000  1       -
// CLRMAC
EMITTER(CLRMAC) {
  ir_store_context(ir, offsetof(struct sh4_ctx, mach), ir_alloc_i32(ir, 0));
  ir_store_context(ir, offsetof(struct sh4_ctx, macl), ir_alloc_i32(ir, 0));
}

EMITTER(CLRS) {
  struct ir_value *sr = load_sr();
  sr = ir_and(ir, sr, ir_alloc_i32(ir, ~S));
  store_sr(sr);
}

// code                 cycles  t-bit
// 0000 0000 0000 1000  1       -
// CLRT
EMITTER(CLRT) {
  store_t(ir_alloc_i32(ir, 0));
}

// LDC     Rm,SR
EMITTER(LDCSR) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  store_sr(rm);
}

// LDC     Rm,GBR
EMITTER(LDCGBR) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  store_gbr(rm);
}

// LDC     Rm,VBR
EMITTER(LDCVBR) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, vbr), rm);
}

// LDC     Rm,SSR
EMITTER(LDCSSR) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, ssr), rm);
}

// LDC     Rm,SPC
EMITTER(LDCSPC) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, spc), rm);
}

// LDC     Rm,DBR
EMITTER(LDCDBR) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, dbr), rm);
}

// LDC.L   Rm,Rn_BANK
EMITTER(LDCRBANK) {
  int reg = i->Rn & 0x7;
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, ralt) + reg * 4, rm);
}

// LDC.L   @Rm+,SR
EMITTER(LDCMSR) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I32);
  store_sr(v);
  // reload Rm, sr store could have swapped banks
  addr = load_gpr(i->Rm, VALUE_I32);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rm, addr);
}

// LDC.L   @Rm+,GBR
EMITTER(LDCMGBR) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I32);
  store_gbr(v);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rm, addr);
}

// LDC.L   @Rm+,VBR
EMITTER(LDCMVBR) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, vbr), v);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rm, addr);
}

// LDC.L   @Rm+,SSR
EMITTER(LDCMSSR) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, ssr), v);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rm, addr);
}

// LDC.L   @Rm+,SPC
EMITTER(LDCMSPC) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, spc), v);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rm, addr);
}

// LDC.L   @Rm+,DBR
EMITTER(LDCMDBR) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, dbr), v);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rm, addr);
}

// LDC.L   @Rm+,Rn_BANK
EMITTER(LDCMRBANK) {
  int reg = i->Rn & 0x7;
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  store_gpr(i->Rm, ir_add(ir, addr, ir_alloc_i32(ir, 4)));
  struct ir_value *v = load_guest(addr, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, ralt) + reg * 4, v);
}

// LDS     Rm,MACH
EMITTER(LDSMACH) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, mach), rm);
}

// LDS     Rm,MACL
EMITTER(LDSMACL) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, macl), rm);
}

// LDS     Rm,PR
EMITTER(LDSPR) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  store_pr(rm);
}

// LDS.L   @Rm+,MACH
EMITTER(LDSMMACH) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, mach), v);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rm, addr);
}

// LDS.L   @Rm+,MACL
EMITTER(LDSMMACL) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, macl), v);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rm, addr);
}

// LDS.L   @Rm+,PR
EMITTER(LDSMPR) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I32);
  store_pr(v);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rm, addr);
}

// MOVCA.L     R0,@Rn
EMITTER(MOVCAL) {
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  struct ir_value *r0 = load_gpr(0, VALUE_I32);
  store_guest(addr, r0);
}

// NOP
EMITTER(NOP) {}

// OCBI
EMITTER(OCBI) {}

// OCBP
EMITTER(OCBP) {}

// OCBWB
EMITTER(OCBWB) {}

// PREF     @Rn
EMITTER(PREF) {
  struct ir_value *prefetch =
      ir_load_context(ir, offsetof(struct sh4_ctx, Prefetch), VALUE_I64);
  struct ir_value *addr = ir_zext(ir, load_gpr(i->Rn, VALUE_I32), VALUE_I64);
  ir_call_external_2(ir, prefetch, addr);
}

// RTE
EMITTER(RTE) {
  struct ir_value *spc =
      ir_load_context(ir, offsetof(struct sh4_ctx, spc), VALUE_I32);
  struct ir_value *ssr =
      ir_load_context(ir, offsetof(struct sh4_ctx, ssr), VALUE_I32);
  store_sr(ssr);
  emit_delay_instr();
  ir_branch(ir, spc);
}

// SETS
EMITTER(SETS) {
  store_sr(ir_or(ir, load_sr(), ir_alloc_i32(ir, S)));
}

// SETT
EMITTER(SETT) {
  store_t(ir_alloc_i32(ir, 1));
}

// SLEEP
EMITTER(SLEEP) {
  LOG_FATAL("SLEEP not implemented");
}

// STC     SR,Rn
EMITTER(STCSR) {
  struct ir_value *v = load_sr();
  store_gpr(i->Rn, v);
}

// STC     GBR,Rn
EMITTER(STCGBR) {
  struct ir_value *v = load_gbr();
  store_gpr(i->Rn, v);
}

// STC     VBR,Rn
EMITTER(STCVBR) {
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, vbr), VALUE_I32);
  store_gpr(i->Rn, v);
}

// STC     SSR,Rn
EMITTER(STCSSR) {
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, ssr), VALUE_I32);
  store_gpr(i->Rn, v);
}

// STC     SPC,Rn
EMITTER(STCSPC) {
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, spc), VALUE_I32);
  store_gpr(i->Rn, v);
}

// STC     SGR,Rn
EMITTER(STCSGR) {
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, sgr), VALUE_I32);
  store_gpr(i->Rn, v);
}

// STC     DBR,Rn
EMITTER(STCDBR) {
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, dbr), VALUE_I32);
  store_gpr(i->Rn, v);
}

// STC     Rm_BANK,Rn
EMITTER(STCRBANK) {
  int reg = i->Rm & 0x7;
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, ralt) + reg * 4, VALUE_I32);
  store_gpr(i->Rn, v);
}

// STC.L   SR,@-Rn
EMITTER(STCMSR) {
  struct ir_value *addr =
      ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);
  struct ir_value *v = load_sr();
  store_guest(addr, v);
}

// STC.L   GBR,@-Rn
EMITTER(STCMGBR) {
  struct ir_value *addr =
      ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);
  struct ir_value *v = load_gbr();
  store_guest(addr, v);
}

// STC.L   VBR,@-Rn
EMITTER(STCMVBR) {
  struct ir_value *addr =
      ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, vbr), VALUE_I32);
  store_guest(addr, v);
}

// STC.L   SSR,@-Rn
EMITTER(STCMSSR) {
  struct ir_value *addr =
      ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, ssr), VALUE_I32);
  store_guest(addr, v);
}

// STC.L   SPC,@-Rn
EMITTER(STCMSPC) {
  struct ir_value *addr =
      ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, spc), VALUE_I32);
  store_guest(addr, v);
}

// STC.L   SGR,@-Rn
EMITTER(STCMSGR) {
  struct ir_value *addr =
      ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, sgr), VALUE_I32);
  store_guest(addr, v);
}

// STC.L   DBR,@-Rn
EMITTER(STCMDBR) {
  struct ir_value *addr =
      ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, dbr), VALUE_I32);
  store_guest(addr, v);
}

// STC.L   Rm_BANK,@-Rn
EMITTER(STCMRBANK) {
  int reg = i->Rm & 0x7;
  struct ir_value *addr =
      ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, ralt) + reg * 4, VALUE_I32);
  store_guest(addr, v);
}

// STS     MACH,Rn
EMITTER(STSMACH) {
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, mach), VALUE_I32);
  store_gpr(i->Rn, v);
}

// STS     MACL,Rn
EMITTER(STSMACL) {
  struct ir_value *v =
      ir_load_context(ir, offsetof(struct sh4_ctx, macl), VALUE_I32);
  store_gpr(i->Rn, v);
}

// STS     PR,Rn
EMITTER(STSPR) {
  struct ir_value *v = load_pr();
  store_gpr(i->Rn, v);
}

// STS.L   MACH,@-Rn
EMITTER(STSMMACH) {
  struct ir_value *addr =
      ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);

  struct ir_value *mach =
      ir_load_context(ir, offsetof(struct sh4_ctx, mach), VALUE_I32);
  store_guest(addr, mach);
}

// STS.L   MACL,@-Rn
EMITTER(STSMMACL) {
  struct ir_value *addr =
      ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);

  struct ir_value *macl =
      ir_load_context(ir, offsetof(struct sh4_ctx, macl), VALUE_I32);
  store_guest(addr, macl);
}

// STS.L   PR,@-Rn
EMITTER(STSMPR) {
  struct ir_value *addr =
      ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);

  struct ir_value *pr = load_pr();
  store_guest(addr, pr);
}

// TRAPA   #imm
EMITTER(TRAPA) {
  LOG_FATAL("TRAPA not implemented");
}

// FLDI0  FRn 1111nnnn10001101
EMITTER(FLDI0) {
  store_fpr(i->Rn, ir_alloc_i32(ir, 0));
}

// FLDI1  FRn 1111nnnn10011101
EMITTER(FLDI1) {
  store_fpr(i->Rn, ir_alloc_i32(ir, 0x3F800000));
}

// FMOV    FRm,FRn 1111nnnnmmmm1100
// FMOV    DRm,DRn 1111nnn0mmm01100
// FMOV    XDm,DRn 1111nnn0mmm11100
// FMOV    DRm,XDn 1111nnn1mmm01100
// FMOV    XDm,XDn 1111nnn1mmm11100
EMITTER(FMOV) {
  if (flags & SH4_DOUBLE_SZ) {
    if (i->Rm & 1) {
      struct ir_value *rm = load_xfr(i->Rm & 0xe, VALUE_I64);
      if (i->Rn & 1) {
        store_xfr(i->Rn & 0xe, rm);
      } else {
        store_fpr(i->Rn, rm);
      }
    } else {
      struct ir_value *rm = load_fpr(i->Rm, VALUE_I64);
      if (i->Rn & 1) {
        store_xfr(i->Rn & 0xe, rm);
      } else {
        store_fpr(i->Rn, rm);
      }
    }
  } else {
    store_fpr(i->Rn, load_fpr(i->Rm, VALUE_I32));
  }
}

// FMOV.S  @Rm,FRn 1111nnnnmmmm1000
// FMOV    @Rm,DRn 1111nnn0mmmm1000
// FMOV    @Rm,XDn 1111nnn1mmmm1000
EMITTER(FMOV_LOAD) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);

  if (flags & SH4_DOUBLE_SZ) {
    struct ir_value *v_low = load_guest(addr, VALUE_I32);
    struct ir_value *v_high =
        load_guest(ir_add(ir, addr, ir_alloc_i32(ir, 4)), VALUE_I32);
    if (i->Rn & 1) {
      store_xfr(i->Rn & 0xe, v_low);
      store_xfr(i->Rn, v_high);
    } else {
      store_fpr(i->Rn, v_low);
      store_fpr(i->Rn | 0x1, v_high);
    }
  } else {
    store_fpr(i->Rn, load_guest(addr, VALUE_I32));
  }
}

// FMOV.S  @(R0,Rm),FRn 1111nnnnmmmm0110
// FMOV    @(R0,Rm),DRn 1111nnn0mmmm0110
// FMOV    @(R0,Rm),XDn 1111nnn1mmmm0110
EMITTER(FMOV_INDEX_LOAD) {
  struct ir_value *addr =
      ir_add(ir, load_gpr(0, VALUE_I32), load_gpr(i->Rm, VALUE_I32));

  if (flags & SH4_DOUBLE_SZ) {
    struct ir_value *v_low = load_guest(addr, VALUE_I32);
    struct ir_value *v_high =
        load_guest(ir_add(ir, addr, ir_alloc_i32(ir, 4)), VALUE_I32);
    if (i->Rn & 1) {
      store_xfr(i->Rn & 0xe, v_low);
      store_xfr(i->Rn, v_high);
    } else {
      store_fpr(i->Rn, v_low);
      store_fpr(i->Rn | 0x1, v_high);
    }
  } else {
    store_fpr(i->Rn, load_guest(addr, VALUE_I32));
  }
}

// FMOV.S  FRm,@Rn 1111nnnnmmmm1010
// FMOV    DRm,@Rn 1111nnnnmmm01010
// FMOV    XDm,@Rn 1111nnnnmmm11010
EMITTER(FMOV_STORE) {
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);

  if (flags & SH4_DOUBLE_SZ) {
    struct ir_value *addr_low = addr;
    struct ir_value *addr_high = ir_add(ir, addr, ir_alloc_i32(ir, 4));
    if (i->Rm & 1) {
      store_guest(addr_low, load_xfr(i->Rm & 0xe, VALUE_I32));
      store_guest(addr_high, load_xfr(i->Rm, VALUE_I32));
    } else {
      store_guest(addr_low, load_fpr(i->Rm, VALUE_I32));
      store_guest(addr_high, load_fpr(i->Rm | 0x1, VALUE_I32));
    }
  } else {
    store_guest(addr, load_fpr(i->Rm, VALUE_I32));
  }
}

// FMOV.S  FRm,@(R0,Rn) 1111nnnnmmmm0111
// FMOV    DRm,@(R0,Rn) 1111nnnnmmm00111
// FMOV    XDm,@(R0,Rn) 1111nnnnmmm10111
EMITTER(FMOV_INDEX_STORE) {
  struct ir_value *addr =
      ir_add(ir, load_gpr(0, VALUE_I32), load_gpr(i->Rn, VALUE_I32));

  if (flags & SH4_DOUBLE_SZ) {
    struct ir_value *addr_low = addr;
    struct ir_value *addr_high = ir_add(ir, addr, ir_alloc_i32(ir, 4));
    if (i->Rm & 1) {
      store_guest(addr_low, load_xfr(i->Rm & 0xe, VALUE_I32));
      store_guest(addr_high, load_xfr(i->Rm, VALUE_I32));
    } else {
      store_guest(addr_low, load_fpr(i->Rm, VALUE_I32));
      store_guest(addr_high, load_fpr(i->Rm | 0x1, VALUE_I32));
    }
  } else {
    store_guest(addr, load_fpr(i->Rm, VALUE_I32));
  }
}

// FMOV.S  FRm,@-Rn 1111nnnnmmmm1011
// FMOV    DRm,@-Rn 1111nnnnmmm01011
// FMOV    XDm,@-Rn 1111nnnnmmm11011
EMITTER(FMOV_SAVE) {
  if (flags & SH4_DOUBLE_SZ) {
    struct ir_value *addr =
        ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 8));
    store_gpr(i->Rn, addr);

    struct ir_value *addr_low = addr;
    struct ir_value *addr_high = ir_add(ir, addr, ir_alloc_i32(ir, 4));

    if (i->Rm & 1) {
      store_guest(addr_low, load_xfr(i->Rm & 0xe, VALUE_I32));
      store_guest(addr_high, load_xfr(i->Rm, VALUE_I32));
    } else {
      store_guest(addr_low, load_fpr(i->Rm, VALUE_I32));
      store_guest(addr_high, load_fpr(i->Rm | 0x1, VALUE_I32));
    }
  } else {
    struct ir_value *addr =
        ir_sub(ir, load_gpr(i->Rn, VALUE_I32), ir_alloc_i32(ir, 4));
    store_gpr(i->Rn, addr);
    store_guest(addr, load_fpr(i->Rm, VALUE_I32));
  }
}

// FMOV.S  @Rm+,FRn 1111nnnnmmmm1001
// FMOV    @Rm+,DRn 1111nnn0mmmm1001
// FMOV    @Rm+,XDn 1111nnn1mmmm1001
EMITTER(FMOV_RESTORE) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);

  if (flags & SH4_DOUBLE_SZ) {
    struct ir_value *v_low = load_guest(addr, VALUE_I32);
    struct ir_value *v_high =
        load_guest(ir_add(ir, addr, ir_alloc_i32(ir, 4)), VALUE_I32);
    if (i->Rn & 1) {
      store_xfr(i->Rn & 0xe, v_low);
      store_xfr(i->Rn, v_high);
    } else {
      store_fpr(i->Rn, v_low);
      store_fpr(i->Rn | 0x1, v_high);
    }
    store_gpr(i->Rm, ir_add(ir, addr, ir_alloc_i32(ir, 8)));
  } else {
    store_fpr(i->Rn, load_guest(addr, VALUE_I32));
    store_gpr(i->Rm, ir_add(ir, addr, ir_alloc_i32(ir, 4)));
  }
}

// FLDS FRm,FPUL 1111mmmm00011101
EMITTER(FLDS) {
  struct ir_value *rn = load_fpr(i->Rm, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, fpul), rn);
}

// FSTS FPUL,FRn 1111nnnn00001101
EMITTER(FSTS) {
  struct ir_value *fpul =
      ir_load_context(ir, offsetof(struct sh4_ctx, fpul), VALUE_I32);
  store_fpr(i->Rn, fpul);
}

// FABS FRn PR=0 1111nnnn01011101
// FABS DRn PR=1 1111nnn001011101
EMITTER(FABS) {
  if (flags & SH4_DOUBLE_PR) {
    int n = i->Rn & 0xe;
    struct ir_value *v = ir_fabs(ir, load_fpr(n, VALUE_F64));
    store_fpr(n, v);
  } else {
    struct ir_value *v = ir_fabs(ir, load_fpr(i->Rn, VALUE_F32));
    store_fpr(i->Rn, v);
  }
}

// FSRRA FRn PR=0 1111nnnn01111101
EMITTER(FSRRA) {
  struct ir_value *frn = load_fpr(i->Rn, VALUE_F32);
  struct ir_value *v = ir_fdiv(ir, ir_alloc_f32(ir, 1.0f), ir_sqrt(ir, frn));
  store_fpr(i->Rn, v);
}

// FADD FRm,FRn PR=0 1111nnnnmmmm0000
// FADD DRm,DRn PR=1 1111nnn0mmm00000
EMITTER(FADD) {
  if (flags & SH4_DOUBLE_PR) {
    int n = i->Rn & 0xe;
    int m = i->Rm & 0xe;
    struct ir_value *drn = load_fpr(n, VALUE_F64);
    struct ir_value *drm = load_fpr(m, VALUE_F64);
    struct ir_value *v = ir_fadd(ir, drn, drm);
    store_fpr(n, v);
  } else {
    struct ir_value *frn = load_fpr(i->Rn, VALUE_F32);
    struct ir_value *frm = load_fpr(i->Rm, VALUE_F32);
    struct ir_value *v = ir_fadd(ir, frn, frm);
    store_fpr(i->Rn, v);
  }
}

// FCMP/EQ FRm,FRn PR=0 1111nnnnmmmm0100
// FCMP/EQ DRm,DRn PR=1 1111nnn0mmm00100
EMITTER(FCMPEQ) {
  if (flags & SH4_DOUBLE_PR) {
    int n = i->Rn & 0xe;
    int m = i->Rm & 0xe;
    struct ir_value *drn = load_fpr(n, VALUE_F64);
    struct ir_value *drm = load_fpr(m, VALUE_F64);
    struct ir_value *v = ir_fcmp_eq(ir, drn, drm);
    store_t(v);
  } else {
    struct ir_value *frn = load_fpr(i->Rn, VALUE_F32);
    struct ir_value *frm = load_fpr(i->Rm, VALUE_F32);
    struct ir_value *v = ir_fcmp_eq(ir, frn, frm);
    store_t(v);
  }
}

// FCMP/GT FRm,FRn PR=0 1111nnnnmmmm0101
// FCMP/GT DRm,DRn PR=1 1111nnn0mmm00101
EMITTER(FCMPGT) {
  if (flags & SH4_DOUBLE_PR) {
    int n = i->Rn & 0xe;
    int m = i->Rm & 0xe;
    struct ir_value *drn = load_fpr(n, VALUE_F64);
    struct ir_value *drm = load_fpr(m, VALUE_F64);
    struct ir_value *v = ir_fcmp_gt(ir, drn, drm);
    store_t(v);
  } else {
    struct ir_value *frn = load_fpr(i->Rn, VALUE_F32);
    struct ir_value *frm = load_fpr(i->Rm, VALUE_F32);
    struct ir_value *v = ir_fcmp_gt(ir, frn, frm);
    store_t(v);
  }
}

// FDIV FRm,FRn PR=0 1111nnnnmmmm0011
// FDIV DRm,DRn PR=1 1111nnn0mmm00011
EMITTER(FDIV) {
  if (flags & SH4_DOUBLE_PR) {
    int n = i->Rn & 0xe;
    int m = i->Rm & 0xe;
    struct ir_value *drn = load_fpr(n, VALUE_F64);
    struct ir_value *drm = load_fpr(m, VALUE_F64);
    struct ir_value *v = ir_fdiv(ir, drn, drm);
    store_fpr(n, v);
  } else {
    struct ir_value *frn = load_fpr(i->Rn, VALUE_F32);
    struct ir_value *frm = load_fpr(i->Rm, VALUE_F32);
    struct ir_value *v = ir_fdiv(ir, frn, frm);
    store_fpr(i->Rn, v);
  }
}

// FLOAT FPUL,FRn PR=0 1111nnnn00101101
// FLOAT FPUL,DRn PR=1 1111nnn000101101
EMITTER(FLOAT) {
  struct ir_value *fpul =
      ir_load_context(ir, offsetof(struct sh4_ctx, fpul), VALUE_I32);

  if (flags & SH4_DOUBLE_PR) {
    int n = i->Rn & 0xe;
    struct ir_value *v = ir_itof(ir, ir_sext(ir, fpul, VALUE_I64), VALUE_F64);
    store_fpr(n, v);
  } else {
    struct ir_value *v = ir_itof(ir, fpul, VALUE_F32);
    store_fpr(i->Rn, v);
  }
}

// FMAC FR0,FRm,FRn PR=0 1111nnnnmmmm1110
EMITTER(FMAC) {
  CHECK(!(flags & SH4_DOUBLE_PR));

  struct ir_value *frn = load_fpr(i->Rn, VALUE_F32);
  struct ir_value *frm = load_fpr(i->Rm, VALUE_F32);
  struct ir_value *fr0 = load_fpr(0, VALUE_F32);
  struct ir_value *v = ir_fadd(ir, ir_fmul(ir, fr0, frm), frn);
  store_fpr(i->Rn, v);
}

// FMUL FRm,FRn PR=0 1111nnnnmmmm0010
// FMUL DRm,DRn PR=1 1111nnn0mmm00010
EMITTER(FMUL) {
  if (flags & SH4_DOUBLE_PR) {
    int n = i->Rn & 0xe;
    int m = i->Rm & 0xe;
    struct ir_value *drn = load_fpr(n, VALUE_F64);
    struct ir_value *drm = load_fpr(m, VALUE_F64);
    struct ir_value *v = ir_fmul(ir, drn, drm);
    store_fpr(n, v);
  } else {
    struct ir_value *frn = load_fpr(i->Rn, VALUE_F32);
    struct ir_value *frm = load_fpr(i->Rm, VALUE_F32);
    struct ir_value *v = ir_fmul(ir, frn, frm);
    store_fpr(i->Rn, v);
  }
}

// FNEG FRn PR=0 1111nnnn01001101
// FNEG DRn PR=1 1111nnn001001101
EMITTER(FNEG) {
  if (flags & SH4_DOUBLE_PR) {
    int n = i->Rn & 0xe;
    struct ir_value *drn = load_fpr(n, VALUE_F64);
    struct ir_value *v = ir_fneg(ir, drn);
    store_fpr(n, v);
  } else {
    struct ir_value *frn = load_fpr(i->Rn, VALUE_F32);
    struct ir_value *v = ir_fneg(ir, frn);
    store_fpr(i->Rn, v);
  }
}

// FSQRT FRn PR=0 1111nnnn01101101
// FSQRT DRn PR=1 1111nnnn01101101
EMITTER(FSQRT) {
  if (flags & SH4_DOUBLE_PR) {
    int n = i->Rn & 0xe;
    struct ir_value *drn = load_fpr(n, VALUE_F64);
    struct ir_value *v = ir_sqrt(ir, drn);
    store_fpr(n, v);
  } else {
    struct ir_value *frn = load_fpr(i->Rn, VALUE_F32);
    struct ir_value *v = ir_sqrt(ir, frn);
    store_fpr(i->Rn, v);
  }
}

// FSUB FRm,FRn PR=0 1111nnnnmmmm0001
// FSUB DRm,DRn PR=1 1111nnn0mmm00001
EMITTER(FSUB) {
  if (flags & SH4_DOUBLE_PR) {
    int n = i->Rn & 0xe;
    int m = i->Rm & 0xe;
    struct ir_value *drn = load_fpr(n, VALUE_F64);
    struct ir_value *drm = load_fpr(m, VALUE_F64);
    struct ir_value *v = ir_fsub(ir, drn, drm);
    store_fpr(n, v);
  } else {
    struct ir_value *frn = load_fpr(i->Rn, VALUE_F32);
    struct ir_value *frm = load_fpr(i->Rm, VALUE_F32);
    struct ir_value *v = ir_fsub(ir, frn, frm);
    store_fpr(i->Rn, v);
  }
}

// FTRC FRm,FPUL PR=0 1111mmmm00111101
// FTRC DRm,FPUL PR=1 1111mmm000111101
EMITTER(FTRC) {
  if (flags & SH4_DOUBLE_PR) {
    int m = i->Rm & 0xe;
    struct ir_value *drm = load_fpr(m, VALUE_F64);
    struct ir_value *dpv = ir_trunc(ir, ir_ftoi(ir, drm, VALUE_I64), VALUE_I32);
    ir_store_context(ir, offsetof(struct sh4_ctx, fpul), dpv);
  } else {
    struct ir_value *frm = load_fpr(i->Rm, VALUE_F32);
    struct ir_value *spv = ir_ftoi(ir, frm, VALUE_I32);
    ir_store_context(ir, offsetof(struct sh4_ctx, fpul), spv);
  }
}

// FCNVDS DRm,FPUL PR=1 1111mmm010111101
EMITTER(FCNVDS) {
  CHECK(flags & SH4_DOUBLE_PR);

  // TODO rounding modes?

  int m = i->Rm & 0xe;
  struct ir_value *dpv = load_fpr(m, VALUE_F64);
  struct ir_value *spv = ir_ftrunc(ir, dpv, VALUE_F32);
  ir_store_context(ir, offsetof(struct sh4_ctx, fpul), spv);
}

// FCNVSD FPUL, DRn PR=1 1111nnn010101101
EMITTER(FCNVSD) {
  CHECK(flags & SH4_DOUBLE_PR);

  // TODO rounding modes?

  struct ir_value *spv =
      ir_load_context(ir, offsetof(struct sh4_ctx, fpul), VALUE_F32);
  struct ir_value *dpv = ir_fext(ir, spv, VALUE_F64);
  int n = i->Rn & 0xe;
  store_fpr(n, dpv);
}

// LDS     Rm,FPSCR
EMITTER(LDSFPSCR) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  store_fpscr(rm);
}

// LDS     Rm,FPUL
EMITTER(LDSFPUL) {
  struct ir_value *rm = load_gpr(i->Rm, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, fpul), rm);
}

// LDS.L   @Rm+,FPSCR
EMITTER(LDSMFPSCR) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I32);
  store_fpscr(v);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rm, addr);
}

// LDS.L   @Rm+,FPUL
EMITTER(LDSMFPUL) {
  struct ir_value *addr = load_gpr(i->Rm, VALUE_I32);
  struct ir_value *v = load_guest(addr, VALUE_I32);
  ir_store_context(ir, offsetof(struct sh4_ctx, fpul), v);
  addr = ir_add(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rm, addr);
}

// STS     FPSCR,Rn
EMITTER(STSFPSCR) {
  struct ir_value *fpscr = load_fpscr();
  store_gpr(i->Rn, fpscr);
}

// STS     FPUL,Rn
EMITTER(STSFPUL) {
  struct ir_value *fpul =
      ir_load_context(ir, offsetof(struct sh4_ctx, fpul), VALUE_I32);
  store_gpr(i->Rn, fpul);
}

// STS.L   FPSCR,@-Rn
EMITTER(STSMFPSCR) {
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  addr = ir_sub(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);
  store_guest(addr, load_fpscr());
}

// STS.L   FPUL,@-Rn
EMITTER(STSMFPUL) {
  struct ir_value *addr = load_gpr(i->Rn, VALUE_I32);
  addr = ir_sub(ir, addr, ir_alloc_i32(ir, 4));
  store_gpr(i->Rn, addr);
  struct ir_value *fpul =
      ir_load_context(ir, offsetof(struct sh4_ctx, fpul), VALUE_I32);
  store_guest(addr, fpul);
}

// FIPR FVm,FVn PR=0 1111nnmm11101101
EMITTER(FIPR) {
  int m = i->Rm << 2;
  int n = i->Rn << 2;

  struct ir_value *fvn = load_fpr(n, VALUE_V128);
  struct ir_value *fvm = load_fpr(m, VALUE_V128);
  struct ir_value *dp = ir_vdot(ir, fvn, fvm, VALUE_F32);
  store_fpr(n + 3, dp);
}

// FSCA FPUL,DRn PR=0 1111nnn011111101
EMITTER(FSCA) {
  int n = i->Rn << 1;

  struct ir_value *fpul =
      ir_load_context(ir, offsetof(struct sh4_ctx, fpul), VALUE_I16);
  fpul = ir_zext(ir, fpul, VALUE_I64);

  struct ir_value *fsca_table = ir_alloc_i64(ir, (int64_t)s_fsca_table);
  struct ir_value *fsca_offset = ir_shli(ir, fpul, 3);
  struct ir_value *addr = ir_add(ir, fsca_table, fsca_offset);

  store_fpr(n, ir_load_host(ir, addr, VALUE_F32));
  store_fpr(n + 1,
            ir_load_host(ir, ir_add(ir, addr, ir_alloc_i64(ir, 4)), VALUE_F32));
}

// FTRV XMTRX,FVn PR=0 1111nn0111111101
EMITTER(FTRV) {
  int n = i->Rn << 2;

  struct ir_value *col0 = load_xfr(0, VALUE_V128);
  struct ir_value *row0 = ir_vbroadcast(ir, load_fpr(n + 0, VALUE_F32));
  struct ir_value *result = ir_vmul(ir, col0, row0, VALUE_F32);

  struct ir_value *col1 = load_xfr(4, VALUE_V128);
  struct ir_value *row1 = ir_vbroadcast(ir, load_fpr(n + 1, VALUE_F32));
  result = ir_vadd(ir, result, ir_vmul(ir, col1, row1, VALUE_F32), VALUE_F32);

  struct ir_value *col2 = load_xfr(8, VALUE_V128);
  struct ir_value *row2 = ir_vbroadcast(ir, load_fpr(n + 2, VALUE_F32));
  result = ir_vadd(ir, result, ir_vmul(ir, col2, row2, VALUE_F32), VALUE_F32);

  struct ir_value *col3 = load_xfr(12, VALUE_V128);
  struct ir_value *row3 = ir_vbroadcast(ir, load_fpr(n + 3, VALUE_F32));
  result = ir_vadd(ir, result, ir_vmul(ir, col3, row3, VALUE_F32), VALUE_F32);

  store_fpr(n, result);
}

// FRCHG 1111101111111101
EMITTER(FRCHG) {
  struct ir_value *fpscr = load_fpscr();
  struct ir_value *v = ir_xor(ir, fpscr, ir_alloc_i32(ir, FR));
  store_fpscr(v);
}

// FSCHG 1111001111111101
EMITTER(FSCHG) {
  struct ir_value *fpscr = load_fpscr();
  struct ir_value *v = ir_xor(ir, fpscr, ir_alloc_i32(ir, SZ));
  store_fpscr(v);
}

void sh4_translate(uint32_t guest_addr, uint8_t *guest_ptr, int size, int flags,
                   struct ir *ir) {
  // PROFILER_RUNTIME("SH4ir::Emit");
  struct sh4_instr delay_instr;

  int i = 0;
  int guest_cycles = 0;

  while (i < size) {
    struct sh4_instr instr = {0};
    instr.addr = guest_addr + i;
    instr.opcode = *(uint16_t *)(guest_ptr + i);

    if (!sh4_disasm(&instr)) {
      sh4_invalid_instr(ir, instr.addr);
      break;
    }

    i += 2;
    guest_cycles += instr.cycles;

    if (instr.flags & SH4_FLAG_DELAYED) {
      delay_instr.addr = guest_addr + i;
      delay_instr.opcode = *(uint16_t *)(guest_ptr + i);

      // instruction must be valid, breakpoints on delay instructions aren't
      // currently supported
      CHECK(sh4_disasm(&delay_instr));

      // delay instruction itself should never have a delay instr
      CHECK(!(delay_instr.flags & SH4_FLAG_DELAYED));

      i += 2;
      guest_cycles += delay_instr.cycles;
    }

    sh4_emit_instr(ir, flags, &instr, &delay_instr);
  }

  struct ir_instr *tail_instr =
      list_last_entry(&ir->instrs, struct ir_instr, it);

  // if the block was terminated before a branch instruction, emit a
  // fallthrough branch to the next pc
  if (tail_instr->op != OP_BRANCH && tail_instr->op != OP_BRANCH_COND) {
    ir_branch(ir, ir_alloc_i32(ir, guest_addr + i));
  }

  // emit block epilog
  ir->current_instr = list_prev_entry(tail_instr, struct ir_instr, it);

  // update remaining cycles
  struct ir_value *num_cycles =
      ir_load_context(ir, offsetof(struct sh4_ctx, num_cycles), VALUE_I32);
  num_cycles = ir_sub(ir, num_cycles, ir_alloc_i32(ir, guest_cycles));
  ir_store_context(ir, offsetof(struct sh4_ctx, num_cycles), num_cycles);

  // update num instructions
  struct ir_value *num_instrs =
      ir_load_context(ir, offsetof(struct sh4_ctx, num_instrs), VALUE_I32);
  num_instrs = ir_add(ir, num_instrs, ir_alloc_i32(ir, size >> 1));
  ir_store_context(ir, offsetof(struct sh4_ctx, num_instrs), num_instrs);
}
