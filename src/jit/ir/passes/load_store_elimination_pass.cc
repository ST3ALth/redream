#include "core/memory.h"
#include "jit/ir/passes/load_store_elimination_pass.h"

using namespace re::jit::ir;
using namespace re::jit::ir::passes;

DEFINE_STAT(num_loads_removed, "Number of loads eliminated");
DEFINE_STAT(num_stores_removed, "Number of stores eliminated");

LoadStoreEliminationPass::LoadStoreEliminationPass()
    : available_(nullptr), num_available_(0) {}

void LoadStoreEliminationPass::Run(IRBuilder &builder) {
  Reset();

  // eliminate redundant loads
  {
    auto it = builder.instrs().begin();
    auto end = builder.instrs().end();

    ClearAvailable();

    while (it != end) {
      Instr *instr = *(it++);

      if (instr->op() == OP_LOAD_CONTEXT) {
        // if there is already a value available for this offset, reuse it and
        // remove this redundant load
        int offset = instr->arg0()->i32();
        Value *available = GetAvailable(offset);

        if (available && available->type() == instr->type()) {
          instr->ReplaceRefsWith(available);
          builder.RemoveInstr(instr);

          num_loads_removed++;

          continue;
        }

        SetAvailable(offset, instr);
      } else if (instr->op() == OP_STORE_CONTEXT) {
        int offset = instr->arg0()->i32();

        // mark the value being stored as available
        SetAvailable(offset, instr->arg1());
      }
    }
  }

  // eliminate dead stores
  {
    // iterate in reverse so the current instruction is the one being removed
    auto it = builder.instrs().rbegin();
    auto end = builder.instrs().rend();

    ClearAvailable();

    while (it != end) {
      Instr *instr = *(it++);

      if (instr->op() == OP_LOAD_CONTEXT) {
        int offset = instr->arg0()->i32();
        int size = SizeForType(instr->type());

        EraseAvailable(offset, size);
      } else if (instr->op() == OP_STORE_CONTEXT) {
        // if subsequent stores have been made for this offset that would
        // overwrite it completely, mark instruction as dead
        int offset = instr->arg0()->i32();
        Value *available = GetAvailable(offset);
        int available_size = available ? SizeForType(available->type()) : 0;
        int store_size = SizeForType(instr->arg1()->type());

        if (available_size >= store_size) {
          builder.RemoveInstr(instr);

          num_stores_removed++;

          continue;
        }

        SetAvailable(offset, instr->arg1());
      }
    }
  }
}

void LoadStoreEliminationPass::Reset() {
  ClearAvailable();
}

void LoadStoreEliminationPass::Reserve(int offset) {
  int reserve = offset + 1;

  if (reserve <= num_available_) {
    return;
  }

  // resize availability array to hold new entry
  available_ = reinterpret_cast<AvailableEntry *>(
      realloc(available_, reserve * sizeof(AvailableEntry)));

  // memset the newly allocated entries
  memset(available_ + num_available_, 0,
         (reserve - num_available_) * sizeof(AvailableEntry));

  num_available_ = reserve;
}

void LoadStoreEliminationPass::ClearAvailable() {
  if (!available_) {
    return;
  }

  memset(available_, 0, num_available_ * sizeof(AvailableEntry));
}

Value *LoadStoreEliminationPass::GetAvailable(int offset) {
  Reserve(offset);

  AvailableEntry &entry = available_[offset];

  // entries are added for the entire range of an available value to help with
  // invalidation. if this entry doesn't start at the requested offset, it's
  // not actually valid for reuse
  if (entry.offset != offset) {
    return nullptr;
  }

  return entry.value;
}

void LoadStoreEliminationPass::EraseAvailable(int offset, int size) {
  int begin = offset;
  int end = offset + size - 1;

  Reserve(end);

  // if the invalidation range intersects with an entry, merge that entry into
  // the invalidation range
  AvailableEntry &begin_entry = available_[begin];
  AvailableEntry &end_entry = available_[end];

  if (begin_entry.value) {
    begin = begin_entry.offset;
  }

  if (end_entry.value) {
    end = end_entry.offset + SizeForType(end_entry.value->type()) - 1;
  }

  for (; begin <= end; begin++) {
    AvailableEntry &entry = available_[begin];
    entry.offset = 0;
    entry.value = nullptr;
  }
}

void LoadStoreEliminationPass::SetAvailable(int offset, Value *v) {
  int size = SizeForType(v->type());
  int begin = offset;
  int end = offset + size - 1;

  Reserve(end);

  EraseAvailable(offset, size);

  // add entries for the entire range to aid in invalidation. only the initial
  // entry where offset == entry.offset is valid for reuse
  for (; begin <= end; begin++) {
    AvailableEntry &entry = available_[begin];
    entry.offset = offset;
    entry.value = v;
  }
}
