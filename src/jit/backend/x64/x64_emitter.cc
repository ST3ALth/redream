#include <math.h>
#include "core/assert.h"
#include "core/math.h"
#include "core/memory.h"
#include "core/profiler.h"
#include "jit/backend/x64/x64_backend.h"
#include "jit/backend/x64/x64_emitter.h"

using namespace re;
using namespace re::jit;
using namespace re::jit::backend;
using namespace re::jit::backend::x64;
using namespace re::jit::ir;

const Xbyak::Reg64 arg0(x64_arg0_idx);
const Xbyak::Reg64 arg1(x64_arg1_idx);
const Xbyak::Reg64 arg2(x64_arg2_idx);
const Xbyak::Reg64 tmp0(x64_tmp0_idx);
const Xbyak::Reg64 tmp1(x64_tmp1_idx);

// callbacks for emitting each IR op
typedef void (*X64Emit)(X64Emitter &, const Instr *);

static X64Emit x64_emitters[NUM_OPS];

#define EMITTER(op)                     \
  void op(X64Emitter &, const Instr *); \
  static struct _x64_##op##_init {      \
    _x64_##op##_init() {                \
      x64_emitters[OP_##op] = &op;      \
    }                                   \
  } x64_##op##_init;                    \
  void op(X64Emitter &e, const Instr *instr)

static bool IsCalleeSaved(const Xbyak::Reg &reg) {
  if (reg.isXMM()) {
    return false;
  }

  static bool callee_saved[16] = {
    false,  // RAX
    false,  // RCX
    false,  // RDX
    true,   // RBX
    false,  // RSP
    true,   // RBP
#if PLATFORM_WINDOWS
    true,  // RSI
    true,  // RDI
#else
    false,  // RSI
    false,  // RDI
#endif
    false,  // R8
    false,  // R9
    false,  // R10
    false,  // R11
    true,   // R12
    true,   // R13
    true,   // R14
    true,   // R15
  };

  return callee_saved[reg.getIdx()];
}

X64Emitter::X64Emitter(const MemoryInterface &memif, void *buffer,
                       size_t buffer_size)
    : CodeGenerator(buffer_size, buffer), memif_(memif) {
  // temporary registers aren't tracked to be pushed and popped
  CHECK(!IsCalleeSaved(tmp0) && !IsCalleeSaved(tmp1));

  modified_ = new int[x64_num_registers];

  Reset();
}

X64Emitter::~X64Emitter() {
  delete[] modified_;
}

void X64Emitter::Reset() {
  modified_marker_ = 0;
  memset(modified_, modified_marker_, sizeof(int) * x64_num_registers);

  // reset codegen buffer
  reset();

  EmitConstants();
}

const uint8_t *X64Emitter::Emit(IRBuilder &builder, int *size) {
  // PROFILER_RUNTIME("X64Emitter::Emit");

  const uint8_t *fn = getCurr();

  int stack_size = 0;
  EmitProlog(builder, &stack_size);
  EmitBody(builder);
  EmitEpilog(builder, stack_size);

  *size = getCurr() - fn;

  return fn;
}

void X64Emitter::EmitConstants() {
  L(xmm_const_[XMM_CONST_ABS_MASK_PS]);
  dq(INT64_C(0x7fffffff7fffffff));
  dq(INT64_C(0x7fffffff7fffffff));

  L(xmm_const_[XMM_CONST_ABS_MASK_PD]);
  dq(INT64_C(0x7fffffffffffffff));
  dq(INT64_C(0x7fffffffffffffff));

  L(xmm_const_[XMM_CONST_SIGN_MASK_PS]);
  dq(INT64_C(0x8000000080000000));
  dq(INT64_C(0x8000000080000000));

  L(xmm_const_[XMM_CONST_SIGN_MASK_PD]);
  dq(INT64_C(0x8000000000000000));
  dq(INT64_C(0x8000000000000000));
}

void X64Emitter::EmitProlog(IRBuilder &builder, int *out_stack_size) {
  int stack_size = STACK_SIZE + builder.locals_size();

  // stack must be 16 byte aligned
  stack_size = align_up(stack_size, 16);

  // add 8 for return address which will be pushed when this is called
  stack_size += 8;

  CHECK_EQ((stack_size + 8) % 16, 0);

  // mark which registers have been modified
  modified_marker_++;

  for (auto instr : builder.instrs()) {
    int i = instr->reg();
    if (i == NO_REGISTER) {
      continue;
    }

    modified_[i] = modified_marker_;
  }

  // push the callee-saved registers which have been modified
  int pushed = 2;

  // always used by guest ctx and memory pointers
  push(r15);
  push(r14);

  for (int i = 0; i < x64_num_registers; i++) {
    const Xbyak::Reg &reg =
        *reinterpret_cast<const Xbyak::Reg *>(x64_registers[i].data);

    if (IsCalleeSaved(reg) && modified_[i] == modified_marker_) {
      push(reg);
      pushed++;
    }
  }

  // if an odd amount of push instructions are emitted stack_size needs to be
  // adjusted to keep the stack aligned
  if ((pushed % 2) == 1) {
    stack_size += 8;
  }

  // adjust stack pointer
  sub(rsp, stack_size);

  // copy guest context and memory base to argument registers
  mov(r14, reinterpret_cast<uint64_t>(memif_.ctx_base));
  mov(r15, reinterpret_cast<uint64_t>(memif_.mem_base));

  *out_stack_size = stack_size;
}

void X64Emitter::EmitBody(IRBuilder &builder) {
  for (auto instr : builder.instrs()) {
    X64Emit emit = x64_emitters[instr->op()];
    CHECK(emit, "Failed to find emitter for %s", Opnames[instr->op()]);

    // reset temp count used by GetRegister
    num_temps_ = 0;

    emit(*this, instr);
  }
}

void X64Emitter::EmitEpilog(IRBuilder &builder, int stack_size) {
  // adjust stack pointer
  add(rsp, stack_size);

  // pop callee-saved registers which have been modified
  for (int i = x64_num_registers - 1; i >= 0; i--) {
    const Xbyak::Reg &reg =
        *reinterpret_cast<const Xbyak::Reg *>(x64_registers[i].data);

    if (IsCalleeSaved(reg) && modified_[i] == modified_marker_) {
      pop(reg);
    }
  }

  // pop r14 and r15
  pop(r14);
  pop(r15);

  ret();
}

// If the value is a local or constant, copy it to a tempory register, else
// return the register allocated for it.
const Xbyak::Reg X64Emitter::GetRegister(const Value *v) {
  if (v->constant()) {
    CHECK_LT(num_temps_, 2);

    Xbyak::Reg tmp = num_temps_++ ? tmp1 : tmp0;

    switch (v->type()) {
      case VALUE_I8:
        tmp = tmp.cvt8();
        break;
      case VALUE_I16:
        tmp = tmp.cvt16();
        break;
      case VALUE_I32:
        tmp = tmp.cvt32();
        break;
      case VALUE_I64:
        // no conversion needed
        break;
      default:
        LOG_FATAL("Unexpected value type");
        break;
    }

    // copy value to the temporary register
    mov(tmp, v->GetZExtValue());

    return tmp;
  }

  int i = v->reg();
  CHECK_NE(i, NO_REGISTER);

  const Xbyak::Reg &reg =
      *reinterpret_cast<const Xbyak::Reg *>(x64_registers[i].data);
  CHECK(reg.isREG());

  switch (v->type()) {
    case VALUE_I8:
      return reg.cvt8();
    case VALUE_I16:
      return reg.cvt16();
    case VALUE_I32:
      return reg.cvt32();
    case VALUE_I64:
      return reg;
    default:
      LOG_FATAL("Unexpected value type");
      break;
  }
}

// If the value isn't allocated a XMM register copy it to a temporary XMM,
// register, else return the XMM register allocated for it.
const Xbyak::Xmm X64Emitter::GetXmmRegister(const Value *v) {
  if (v->constant()) {
    // copy value to the temporary register
    if (v->type() == VALUE_F32) {
      float val = v->f32();
      mov(eax, load<int32_t>(&val));
      vmovd(xmm1, eax);
    } else {
      double val = v->f64();
      mov(rax, load<int64_t>(&val));
      vmovq(xmm1, rax);
    }
    return xmm1;
  }

  int i = v->reg();
  CHECK_NE(i, NO_REGISTER);

  const Xbyak::Xmm &xmm =
      *reinterpret_cast<const Xbyak::Xmm *>(x64_registers[i].data);
  CHECK(xmm.isXMM());
  return xmm;
}

const Xbyak::Address X64Emitter::GetXmmConstant(XmmConstant c) {
  return ptr[rip + xmm_const_[c]];
}

bool X64Emitter::CanEncodeAsImmediate(const Value *v) const {
  if (!v->constant()) {
    return false;
  }

  return v->type() <= VALUE_I32;
}

EMITTER(LOAD_HOST) {
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (IsFloatType(instr->type())) {
    const Xbyak::Xmm result = e.GetXmmRegister(instr);

    switch (instr->type()) {
      case VALUE_F32:
        e.vmovss(result, e.dword[a]);
        break;
      case VALUE_F64:
        e.vmovsd(result, e.qword[a]);
        break;
      default:
        LOG_FATAL("Unexpected result type");
        break;
    }
  } else {
    const Xbyak::Reg result = e.GetRegister(instr);

    switch (instr->type()) {
      case VALUE_I8:
        e.mov(result, e.byte[a]);
        break;
      case VALUE_I16:
        e.mov(result, e.word[a]);
        break;
      case VALUE_I32:
        e.mov(result, e.dword[a]);
        break;
      case VALUE_I64:
        e.mov(result, e.qword[a]);
        break;
      default:
        LOG_FATAL("Unexpected load result type");
        break;
    }
  }
}

EMITTER(STORE_HOST) {
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (IsFloatType(instr->arg1()->type())) {
    const Xbyak::Xmm b = e.GetXmmRegister(instr->arg1());

    switch (instr->arg1()->type()) {
      case VALUE_F32:
        e.vmovss(e.dword[a], b);
        break;
      case VALUE_F64:
        e.vmovsd(e.qword[a], b);
        break;
      default:
        LOG_FATAL("Unexpected value type");
        break;
    }
  } else {
    const Xbyak::Reg b = e.GetRegister(instr->arg1());

    switch (instr->arg1()->type()) {
      case VALUE_I8:
        e.mov(e.byte[a], b);
        break;
      case VALUE_I16:
        e.mov(e.word[a], b);
        break;
      case VALUE_I32:
        e.mov(e.dword[a], b);
        break;
      case VALUE_I64:
        e.mov(e.qword[a], b);
        break;
      default:
        LOG_FATAL("Unexpected store value type");
        break;
    }
  }
}

EMITTER(LOAD_FAST) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  switch (instr->type()) {
    case VALUE_I8:
      e.mov(result, e.byte[a.cvt64() + e.r15]);
      break;
    case VALUE_I16:
      e.mov(result, e.word[a.cvt64() + e.r15]);
      break;
    case VALUE_I32:
      e.mov(result, e.dword[a.cvt64() + e.r15]);
      break;
    case VALUE_I64:
      e.mov(result, e.qword[a.cvt64() + e.r15]);
      break;
    default:
      LOG_FATAL("Unexpected load result type");
      break;
  }
}

EMITTER(STORE_FAST) {
  const Xbyak::Reg a = e.GetRegister(instr->arg0());
  const Xbyak::Reg b = e.GetRegister(instr->arg1());

  switch (instr->arg1()->type()) {
    case VALUE_I8:
      e.mov(e.byte[a.cvt64() + e.r15], b);
      break;
    case VALUE_I16:
      e.mov(e.word[a.cvt64() + e.r15], b);
      break;
    case VALUE_I32:
      e.mov(e.dword[a.cvt64() + e.r15], b);
      break;
    case VALUE_I64:
      e.mov(e.qword[a.cvt64() + e.r15], b);
      break;
    default:
      LOG_FATAL("Unexpected store value type");
      break;
  }
}

EMITTER(LOAD_SLOW) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  void *fn = nullptr;
  switch (instr->type()) {
    case VALUE_I8:
      fn = reinterpret_cast<void *>(e.memif().r8);
      break;
    case VALUE_I16:
      fn = reinterpret_cast<void *>(e.memif().r16);
      break;
    case VALUE_I32:
      fn = reinterpret_cast<void *>(e.memif().r32);
      break;
    case VALUE_I64:
      fn = reinterpret_cast<void *>(e.memif().r64);
      break;
    default:
      LOG_FATAL("Unexpected load result type");
      break;
  }

  e.mov(arg0, reinterpret_cast<uint64_t>(e.memif().mem_self));
  e.mov(arg1, a);
  e.call(reinterpret_cast<void *>(fn));
  e.mov(result, e.rax);
}

EMITTER(STORE_SLOW) {
  const Xbyak::Reg a = e.GetRegister(instr->arg0());
  const Xbyak::Reg b = e.GetRegister(instr->arg1());

  void *fn = nullptr;
  switch (instr->arg1()->type()) {
    case VALUE_I8:
      fn = reinterpret_cast<void *>(e.memif().w8);
      break;
    case VALUE_I16:
      fn = reinterpret_cast<void *>(e.memif().w16);
      break;
    case VALUE_I32:
      fn = reinterpret_cast<void *>(e.memif().w32);
      break;
    case VALUE_I64:
      fn = reinterpret_cast<void *>(e.memif().w64);
      break;
    default:
      LOG_FATAL("Unexpected store value type");
      break;
  }

  e.mov(arg0, reinterpret_cast<uint64_t>(e.memif().mem_self));
  e.mov(arg1, a);
  e.mov(arg2, b);
  e.call(reinterpret_cast<void *>(fn));
}

EMITTER(LOAD_CONTEXT) {
  int offset = instr->arg0()->i32();

  if (IsVectorType(instr->type())) {
    const Xbyak::Xmm result = e.GetXmmRegister(instr);

    switch (instr->type()) {
      case VALUE_V128:
        e.movups(result, e.ptr[e.r14 + offset]);
        break;
      default:
        LOG_FATAL("Unexpected result type");
        break;
    }
  } else if (IsFloatType(instr->type())) {
    const Xbyak::Xmm result = e.GetXmmRegister(instr);

    switch (instr->type()) {
      case VALUE_F32:
        e.vmovss(result, e.dword[e.r14 + offset]);
        break;
      case VALUE_F64:
        e.vmovsd(result, e.qword[e.r14 + offset]);
        break;
      default:
        LOG_FATAL("Unexpected result type");
        break;
    }
  } else {
    const Xbyak::Reg result = e.GetRegister(instr);

    switch (instr->type()) {
      case VALUE_I8:
        e.mov(result, e.byte[e.r14 + offset]);
        break;
      case VALUE_I16:
        e.mov(result, e.word[e.r14 + offset]);
        break;
      case VALUE_I32:
        e.mov(result, e.dword[e.r14 + offset]);
        break;
      case VALUE_I64:
        e.mov(result, e.qword[e.r14 + offset]);
        break;
      default:
        LOG_FATAL("Unexpected result type");
        break;
    }
  }
}

EMITTER(STORE_CONTEXT) {
  int offset = instr->arg0()->i32();

  if (instr->arg1()->constant()) {
    switch (instr->arg1()->type()) {
      case VALUE_I8:
        e.mov(e.byte[e.r14 + offset], instr->arg1()->i8());
        break;
      case VALUE_I16:
        e.mov(e.word[e.r14 + offset], instr->arg1()->i16());
        break;
      case VALUE_I32:
      case VALUE_F32:
        e.mov(e.dword[e.r14 + offset], instr->arg1()->i32());
        break;
      case VALUE_I64:
      case VALUE_F64:
        e.mov(e.qword[e.r14 + offset], instr->arg1()->i64());
        break;
      default:
        LOG_FATAL("Unexpected value type");
        break;
    }
  } else {
    if (IsVectorType(instr->arg1()->type())) {
      const Xbyak::Xmm src = e.GetXmmRegister(instr->arg1());

      switch (instr->arg1()->type()) {
        case VALUE_V128:
          e.vmovups(e.ptr[e.r14 + offset], src);
          break;
        default:
          LOG_FATAL("Unexpected result type");
          break;
      }
    } else if (IsFloatType(instr->arg1()->type())) {
      const Xbyak::Xmm src = e.GetXmmRegister(instr->arg1());

      switch (instr->arg1()->type()) {
        case VALUE_F32:
          e.vmovss(e.dword[e.r14 + offset], src);
          break;
        case VALUE_F64:
          e.vmovsd(e.qword[e.r14 + offset], src);
          break;
        default:
          LOG_FATAL("Unexpected value type");
          break;
      }
    } else {
      const Xbyak::Reg src = e.GetRegister(instr->arg1());

      switch (instr->arg1()->type()) {
        case VALUE_I8:
          e.mov(e.byte[e.r14 + offset], src);
          break;
        case VALUE_I16:
          e.mov(e.word[e.r14 + offset], src);
          break;
        case VALUE_I32:
          e.mov(e.dword[e.r14 + offset], src);
          break;
        case VALUE_I64:
          e.mov(e.qword[e.r14 + offset], src);
          break;
        default:
          LOG_FATAL("Unexpected value type");
          break;
      }
    }
  }
}

EMITTER(LOAD_LOCAL) {
  int offset = STACK_OFFSET_LOCALS + instr->arg0()->i32();

  if (IsVectorType(instr->type())) {
    const Xbyak::Xmm result = e.GetXmmRegister(instr);

    switch (instr->type()) {
      case VALUE_V128:
        e.movups(result, e.ptr[e.rsp + offset]);
        break;
      default:
        LOG_FATAL("Unexpected result type");
        break;
    }
  } else if (IsFloatType(instr->type())) {
    const Xbyak::Xmm result = e.GetXmmRegister(instr);

    switch (instr->type()) {
      case VALUE_F32:
        e.vmovss(result, e.dword[e.rsp + offset]);
        break;
      case VALUE_F64:
        e.vmovsd(result, e.qword[e.rsp + offset]);
        break;
      default:
        LOG_FATAL("Unexpected result type");
        break;
    }
  } else {
    const Xbyak::Reg result = e.GetRegister(instr);

    switch (instr->type()) {
      case VALUE_I8:
        e.mov(result, e.byte[e.rsp + offset]);
        break;
      case VALUE_I16:
        e.mov(result, e.word[e.rsp + offset]);
        break;
      case VALUE_I32:
        e.mov(result, e.dword[e.rsp + offset]);
        break;
      case VALUE_I64:
        e.mov(result, e.qword[e.rsp + offset]);
        break;
      default:
        LOG_FATAL("Unexpected result type");
        break;
    }
  }
}

EMITTER(STORE_LOCAL) {
  int offset = STACK_OFFSET_LOCALS + instr->arg0()->i32();

  CHECK(!instr->arg1()->constant());

  if (IsVectorType(instr->arg1()->type())) {
    const Xbyak::Xmm src = e.GetXmmRegister(instr->arg1());

    switch (instr->arg1()->type()) {
      case VALUE_V128:
        e.vmovups(e.ptr[e.rsp + offset], src);
        break;
      default:
        LOG_FATAL("Unexpected result type");
        break;
    }
  } else if (IsFloatType(instr->arg1()->type())) {
    const Xbyak::Xmm src = e.GetXmmRegister(instr->arg1());

    switch (instr->arg1()->type()) {
      case VALUE_F32:
        e.vmovss(e.dword[e.rsp + offset], src);
        break;
      case VALUE_F64:
        e.vmovsd(e.qword[e.rsp + offset], src);
        break;
      default:
        LOG_FATAL("Unexpected value type");
        break;
    }
  } else {
    const Xbyak::Reg src = e.GetRegister(instr->arg1());

    switch (instr->arg1()->type()) {
      case VALUE_I8:
        e.mov(e.byte[e.rsp + offset], src);
        break;
      case VALUE_I16:
        e.mov(e.word[e.rsp + offset], src);
        break;
      case VALUE_I32:
        e.mov(e.dword[e.rsp + offset], src);
        break;
      case VALUE_I64:
        e.mov(e.qword[e.rsp + offset], src);
        break;
      default:
        LOG_FATAL("Unexpected value type");
        break;
    }
  }
}

EMITTER(FTOI) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());

  switch (instr->type()) {
    case VALUE_I32:
      CHECK_EQ(instr->arg0()->type(), VALUE_F32);
      e.cvttss2si(result, a);
      break;
    case VALUE_I64:
      CHECK_EQ(instr->arg0()->type(), VALUE_F64);
      e.cvttsd2si(result, a);
      break;
    default:
      LOG_FATAL("Unexpected result type");
      break;
  }
}

EMITTER(ITOF) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  switch (instr->type()) {
    case VALUE_F32:
      CHECK_EQ(instr->arg0()->type(), VALUE_I32);
      e.cvtsi2ss(result, a);
      break;
    case VALUE_F64:
      CHECK_EQ(instr->arg0()->type(), VALUE_I64);
      e.cvtsi2sd(result, a);
      break;
    default:
      LOG_FATAL("Unexpected result type");
      break;
  }
}

EMITTER(SEXT) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (a == result) {
    // already the correct width
    return;
  }

  if (result.isBit(64) && a.isBit(32)) {
    e.movsxd(result.cvt64(), a);
  } else {
    e.movsx(result, a);
  }
}

EMITTER(ZEXT) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (a == result) {
    // already the correct width
    return;
  }

  if (result.isBit(64) && a.isBit(32)) {
    // mov will automatically zero fill the upper 32-bits
    e.mov(result.cvt32(), a);
  } else {
    e.movzx(result, a);
  }
}

EMITTER(TRUNC) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (result.getIdx() == a.getIdx()) {
    // noop if already the same register. note, this means the high order bits
    // of the result won't be cleared, but I believe that is fine
    return;
  }

  Xbyak::Reg truncated = a;
  switch (instr->type()) {
    case VALUE_I8:
      truncated = a.cvt8();
      break;
    case VALUE_I16:
      truncated = a.cvt16();
      break;
    case VALUE_I32:
      truncated = a.cvt32();
      break;
    default:
      LOG_FATAL("Unexpected value type");
  }

  if (truncated.isBit(32)) {
    // mov will automatically zero fill the upper 32-bits
    e.mov(result, truncated);
  } else {
    e.movzx(result.cvt32(), truncated);
  }
}

EMITTER(FEXT) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());

  e.cvtss2sd(result, a);
}

EMITTER(FTRUNC) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());

  e.cvtsd2ss(result, a);
}

EMITTER(SELECT) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());
  const Xbyak::Reg b = e.GetRegister(instr->arg1());
  const Xbyak::Reg cond = e.GetRegister(instr->arg2());

  // convert result to Reg32e to please xbyak
  CHECK_GE(result.getBit(), 32);
  Xbyak::Reg32e result_32e(result.getIdx(), result.getBit());

  e.test(cond, cond);
  if (result_32e != a) {
    e.cmovnz(result_32e, a);
  }
  e.cmovz(result_32e, b);
}

EMITTER(CMP) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    e.cmp(a, static_cast<uint32_t>(instr->arg1()->GetZExtValue()));
  } else {
    const Xbyak::Reg b = e.GetRegister(instr->arg1());
    e.cmp(a, b);
  }

  CmpType cmp = static_cast<CmpType>(instr->arg2()->i32());

  switch (cmp) {
    case CMP_EQ:
      e.sete(result);
      break;

    case CMP_NE:
      e.setne(result);
      break;

    case CMP_SGE:
      e.setge(result);
      break;

    case CMP_SGT:
      e.setg(result);
      break;

    case CMP_UGE:
      e.setae(result);
      break;

    case CMP_UGT:
      e.seta(result);
      break;

    case CMP_SLE:
      e.setle(result);
      break;

    case CMP_SLT:
      e.setl(result);
      break;

    case CMP_ULE:
      e.setbe(result);
      break;

    case CMP_ULT:
      e.setb(result);
      break;

    default:
      LOG_FATAL("Unexpected comparison type");
  }
}

EMITTER(FCMP) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());
  const Xbyak::Xmm b = e.GetXmmRegister(instr->arg1());

  if (instr->arg0()->type() == VALUE_F32) {
    e.comiss(a, b);
  } else {
    e.comisd(a, b);
  }

  CmpType cmp = static_cast<CmpType>(instr->arg2()->i32());

  switch (cmp) {
    case CMP_EQ:
      e.sete(result);
      break;

    case CMP_NE:
      e.setne(result);
      break;

    case CMP_SGE:
      e.setae(result);
      break;

    case CMP_SGT:
      e.seta(result);
      break;

    case CMP_SLE:
      e.setbe(result);
      break;

    case CMP_SLT:
      e.setb(result);
      break;

    default:
      LOG_FATAL("Unexpected comparison type");
  }
}

EMITTER(ADD) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (result != a) {
    e.mov(result, a);
  }

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    e.add(result, (uint32_t)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Reg b = e.GetRegister(instr->arg1());
    e.add(result, b);
  }
}

EMITTER(SUB) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (result != a) {
    e.mov(result, a);
  }

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    e.sub(result, (uint32_t)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Reg b = e.GetRegister(instr->arg1());
    e.sub(result, b);
  }
}

EMITTER(SMUL) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());
  const Xbyak::Reg b = e.GetRegister(instr->arg1());

  if (result != a) {
    e.mov(result, a);
  }

  e.imul(result, b);
}

EMITTER(UMUL) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());
  const Xbyak::Reg b = e.GetRegister(instr->arg1());

  if (result != a) {
    e.mov(result, a);
  }

  e.imul(result, b);
}

EMITTER(DIV) {
  LOG_FATAL("Unsupported");
}

EMITTER(NEG) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (result != a) {
    e.mov(result, a);
  }

  e.neg(result);
}

EMITTER(ABS) {
  LOG_FATAL("Unsupported");
  // e.mov(e.rax, *result);
  // e.neg(e.rax);
  // e.cmovl(reinterpret_cast<const Xbyak::Reg *>(result)->cvt32(), e.rax);
}

EMITTER(FADD) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());
  const Xbyak::Xmm b = e.GetXmmRegister(instr->arg1());

  if (instr->type() == VALUE_F32) {
    e.vaddss(result, a, b);
  } else {
    e.vaddsd(result, a, b);
  }
}

EMITTER(FSUB) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());
  const Xbyak::Xmm b = e.GetXmmRegister(instr->arg1());

  if (instr->type() == VALUE_F32) {
    e.vsubss(result, a, b);
  } else {
    e.vsubsd(result, a, b);
  }
}

EMITTER(FMUL) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());
  const Xbyak::Xmm b = e.GetXmmRegister(instr->arg1());

  if (instr->type() == VALUE_F32) {
    e.vmulss(result, a, b);
  } else {
    e.vmulsd(result, a, b);
  }
}

EMITTER(FDIV) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());
  const Xbyak::Xmm b = e.GetXmmRegister(instr->arg1());

  if (instr->type() == VALUE_F32) {
    e.vdivss(result, a, b);
  } else {
    e.vdivsd(result, a, b);
  }
}

EMITTER(FNEG) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());

  if (instr->type() == VALUE_F32) {
    e.vxorps(result, a, e.GetXmmConstant(XMM_CONST_SIGN_MASK_PS));
  } else {
    e.vxorpd(result, a, e.GetXmmConstant(XMM_CONST_SIGN_MASK_PD));
  }
}

EMITTER(FABS) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());

  if (instr->type() == VALUE_F32) {
    e.vandps(result, a, e.GetXmmConstant(XMM_CONST_ABS_MASK_PS));
  } else {
    e.vandpd(result, a, e.GetXmmConstant(XMM_CONST_ABS_MASK_PD));
  }
}

EMITTER(SQRT) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());

  if (instr->type() == VALUE_F32) {
    e.vsqrtss(result, a);
  } else {
    e.vsqrtsd(result, a);
  }
}

EMITTER(VBROADCAST) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());

  e.vbroadcastss(result, a);
}

EMITTER(VADD) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());
  const Xbyak::Xmm b = e.GetXmmRegister(instr->arg1());

  e.vaddps(result, a, b);
}

EMITTER(VDOT) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());
  const Xbyak::Xmm b = e.GetXmmRegister(instr->arg1());

  e.vdpps(result, a, b, 0b11110001);
}

EMITTER(VMUL) {
  const Xbyak::Xmm result = e.GetXmmRegister(instr);
  const Xbyak::Xmm a = e.GetXmmRegister(instr->arg0());
  const Xbyak::Xmm b = e.GetXmmRegister(instr->arg1());

  e.vmulps(result, a, b);
}

EMITTER(AND) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (result != a) {
    e.mov(result, a);
  }

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    e.and (result, (uint32_t)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Reg b = e.GetRegister(instr->arg1());
    e.and (result, b);
  }
}

EMITTER(OR) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (result != a) {
    e.mov(result, a);
  }

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    e.or (result, (uint32_t)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Reg b = e.GetRegister(instr->arg1());
    e.or (result, b);
  }
}

EMITTER(XOR) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (result != a) {
    e.mov(result, a);
  }

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    e.xor (result, (uint32_t)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Reg b = e.GetRegister(instr->arg1());
    e.xor (result, b);
  }
}

EMITTER(NOT) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (result != a) {
    e.mov(result, a);
  }

  e.not(result);
}

EMITTER(SHL) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (result != a) {
    e.mov(result, a);
  }

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    e.shl(result, (int)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Reg b = e.GetRegister(instr->arg1());
    e.mov(e.cl, b);
    e.shl(result, e.cl);
  }
}

EMITTER(ASHR) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (result != a) {
    e.mov(result, a);
  }

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    e.sar(result, (int)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Reg b = e.GetRegister(instr->arg1());
    e.mov(e.cl, b);
    e.sar(result, e.cl);
  }
}

EMITTER(LSHR) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  if (result != a) {
    e.mov(result, a);
  }

  if (e.CanEncodeAsImmediate(instr->arg1())) {
    e.shr(result, (int)instr->arg1()->GetZExtValue());
  } else {
    const Xbyak::Reg b = e.GetRegister(instr->arg1());
    e.mov(e.cl, b);
    e.shr(result, e.cl);
  }
}

EMITTER(ASHD) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg v = e.GetRegister(instr->arg0());
  const Xbyak::Reg n = e.GetRegister(instr->arg1());

  e.inLocalLabel();

  if (result != v) {
    e.mov(result, v);
  }

  // check if we're shifting left or right
  e.test(n, 0x80000000);
  e.jnz(".shr");

  // perform shift left
  e.mov(e.cl, n);
  e.sal(result, e.cl);
  e.jmp(".end");

  // perform right shift
  e.L(".shr");
  e.test(n, 0x1f);
  e.jz(".shr_overflow");
  e.mov(e.cl, n);
  e.neg(e.cl);
  e.sar(result, e.cl);
  e.jmp(".end");

  // right shift overflowed
  e.L(".shr_overflow");
  e.sar(result, 31);

  // shift is done
  e.L(".end");

  e.outLocalLabel();
}

EMITTER(LSHD) {
  const Xbyak::Reg result = e.GetRegister(instr);
  const Xbyak::Reg v = e.GetRegister(instr->arg0());
  const Xbyak::Reg n = e.GetRegister(instr->arg1());

  e.inLocalLabel();

  if (result != v) {
    e.mov(result, v);
  }

  // check if we're shifting left or right
  e.test(n, 0x80000000);
  e.jnz(".shr");

  // perform shift left
  e.mov(e.cl, n);
  e.shl(result, e.cl);
  e.jmp(".end");

  // perform right shift
  e.L(".shr");
  e.test(n, 0x1f);
  e.jz(".shr_overflow");
  e.mov(e.cl, n);
  e.neg(e.cl);
  e.shr(result, e.cl);
  e.jmp(".end");

  // right shift overflowed
  e.L(".shr_overflow");
  e.mov(result, 0x0);

  // shift is done
  e.L(".end");

  e.outLocalLabel();
}

EMITTER(BRANCH) {
  const Xbyak::Reg a = e.GetRegister(instr->arg0());

  e.mov(e.rax, a);
}

EMITTER(BRANCH_COND) {
  const Xbyak::Reg cond = e.GetRegister(instr->arg0());
  const Xbyak::Reg true_addr = e.GetRegister(instr->arg1());
  const Xbyak::Reg false_addr = e.GetRegister(instr->arg2());

  e.test(cond, cond);
  e.cmovnz(e.eax, true_addr);
  e.cmove(e.eax, false_addr);
}

EMITTER(CALL_EXTERNAL) {
  const Xbyak::Reg addr = e.GetRegister(instr->arg0());

  e.mov(arg0, reinterpret_cast<uint64_t>(e.memif().ctx_base));
  if (instr->arg1()) {
    const Xbyak::Reg arg = e.GetRegister(instr->arg1());
    e.mov(arg1, arg);
  }
  e.mov(e.rax, addr);
  e.call(e.rax);
}
