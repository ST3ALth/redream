#ifndef X64_BACKEND_H
#define X64_BACKEND_H

#include <capstone.h>
#include "jit/backend/backend.h"
#include "jit/backend/x64/x64_emitter.h"

namespace re {
namespace jit {
namespace backend {
namespace x64 {

extern const Register x64_registers[];
extern const int x64_num_registers;
extern const int x64_arg0_idx;
extern const int x64_arg1_idx;
extern const int x64_arg2_idx;
extern const int x64_tmp0_idx;
extern const int x64_tmp1_idx;

typedef void (*SlowmemThunk)();

class X64Backend : public Backend {
 public:
  X64Backend(const MemoryInterface &memif);
  ~X64Backend();

  const Register *registers() const;
  int num_registers() const;

  void Reset();

  const uint8_t *AssembleCode(ir::IRBuilder &builder, int *size);
  void DumpCode(const uint8_t *host_addr, int size);

  bool HandleFastmemException(struct re_exception_s *ex);

 private:
  void EmitThunks();

  csh capstone_handle_;
  X64Emitter emitter_;
  SlowmemThunk load_thunk_[16];
  SlowmemThunk store_thunk_;
};
}
}
}
}

#endif
