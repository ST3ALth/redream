#include <iostream>
#include "core/math.h"
#include "core/memory.h"
#include "jit/ir/ir_builder.h"
#include "jit/ir/ir_writer.h"

using namespace re::jit;
using namespace re::jit::ir;

const char *re::jit::ir::Opnames[NUM_OPS] = {
#define IR_OP(name) #name,
#include "jit/ir/ir_ops.inc"
};

//
// Value
//
Value::Value(ValueType ty) : type_(ty), constant_(false) {}
Value::Value(int8_t v) : type_(VALUE_I8), constant_(true), i8_(v) {}
Value::Value(int16_t v) : type_(VALUE_I16), constant_(true), i16_(v) {}
Value::Value(int32_t v) : type_(VALUE_I32), constant_(true), i32_(v) {}
Value::Value(int64_t v) : type_(VALUE_I64), constant_(true), i64_(v) {}
Value::Value(float v) : type_(VALUE_F32), constant_(true), f32_(v) {}
Value::Value(double v) : type_(VALUE_F64), constant_(true), f64_(v) {}

uint64_t Value::GetZExtValue() const {
  switch (type_) {
    case VALUE_I8:
      return static_cast<uint8_t>(i8_);
    case VALUE_I16:
      return static_cast<uint16_t>(i16_);
    case VALUE_I32:
      return static_cast<uint32_t>(i32_);
    case VALUE_I64:
      return static_cast<uint64_t>(i64_);
    default:
      LOG_FATAL("Unexpected value type");
      break;
  }
}

void Value::AddRef(Use *ref) {
  refs_.Append(ref);
}

void Value::RemoveRef(Use *ref) {
  refs_.Remove(ref);
}

void Value::ReplaceRefsWith(Value *other) {
  CHECK_NE(this, other);

  // NOTE set_value will modify refs, be careful iterating
  auto it = refs_.begin();
  while (it != refs_.end()) {
    Use *ref = *(it++);
    ref->set_value(other);
  }
}

//
// Use
//
Use::Use(Instr *instr) : instr_(instr), value_(nullptr) {}

Use::~Use() {
  if (value_) {
    value_->RemoveRef(this);
  }
}

//
// Local
//
Local::Local(ValueType ty, Value *offset) : type_(ty), offset_(offset) {}

//
// Instr
//
Instr::Instr(Op op, ValueType result_type)
    : Value(result_type), op_(op), uses_{{this}, {this}, {this}}, tag_(0) {}

Instr::~Instr() {}

//
// IRBuilder
//
IRBuilder::IRBuilder(Arena &arena)
    : arena_(arena), current_instr_(nullptr), locals_size_(0) {}

void IRBuilder::Dump(std::ostream &output) const {
  IRWriter writer;
  writer.Print(*this, output);
}

void IRBuilder::Dump() const {
  Dump(std::cout);
}

InsertPoint IRBuilder::GetInsertPoint() {
  return {current_instr_};
}

void IRBuilder::SetInsertPoint(const InsertPoint &point) {
  current_instr_ = point.instr;
}

void IRBuilder::RemoveInstr(Instr *instr) {
  instrs_.Remove(instr);

  // call destructor manually to release value references
  instr->~Instr();
}

Instr *IRBuilder::LoadHost(Value *addr, ValueType type) {
  CHECK_EQ(VALUE_I64, addr->type());

  Instr *instr = AppendInstr(OP_LOAD_HOST, type);
  instr->set_arg0(addr);
  return instr;
}

void IRBuilder::StoreHost(Value *addr, Value *v) {
  CHECK_EQ(VALUE_I64, addr->type());

  Instr *instr = AppendInstr(OP_STORE_HOST);
  instr->set_arg0(addr);
  instr->set_arg1(v);
}

Instr *IRBuilder::LoadFast(Value *addr, ValueType type) {
  CHECK_EQ(VALUE_I32, addr->type());

  Instr *instr = AppendInstr(OP_LOAD_FAST, type);
  instr->set_arg0(addr);
  return instr;
}

void IRBuilder::StoreFast(Value *addr, Value *v) {
  CHECK_EQ(VALUE_I32, addr->type());

  Instr *instr = AppendInstr(OP_STORE_FAST);
  instr->set_arg0(addr);
  instr->set_arg1(v);
}

Instr *IRBuilder::LoadSlow(Value *addr, ValueType type) {
  CHECK_EQ(VALUE_I32, addr->type());

  Instr *instr = AppendInstr(OP_LOAD_SLOW, type);
  instr->set_arg0(addr);
  return instr;
}

void IRBuilder::StoreSlow(Value *addr, Value *v) {
  CHECK_EQ(VALUE_I32, addr->type());

  Instr *instr = AppendInstr(OP_STORE_SLOW);
  instr->set_arg0(addr);
  instr->set_arg1(v);
}

Instr *IRBuilder::LoadContext(size_t offset, ValueType type) {
  Instr *instr = AppendInstr(OP_LOAD_CONTEXT, type);
  instr->set_arg0(AllocConstant((int32_t)offset));
  return instr;
}

void IRBuilder::StoreContext(size_t offset, Value *v) {
  Instr *instr = AppendInstr(OP_STORE_CONTEXT);
  instr->set_arg0(AllocConstant((int32_t)offset));
  instr->set_arg1(v);
}

Instr *IRBuilder::LoadLocal(Local *local) {
  Instr *instr = AppendInstr(OP_LOAD_LOCAL, local->type());
  instr->set_arg0(local->offset());
  return instr;
}

void IRBuilder::StoreLocal(Local *local, Value *v) {
  Instr *instr = AppendInstr(OP_STORE_LOCAL);
  instr->set_arg0(local->offset());
  instr->set_arg1(v);
}

Instr *IRBuilder::FToI(Value *v, ValueType dest_type) {
  CHECK(IsFloatType(v->type()) && IsIntType(dest_type));

  Instr *instr = AppendInstr(OP_FTOI, dest_type);
  instr->set_arg0(v);
  return instr;
}

Instr *IRBuilder::IToF(Value *v, ValueType dest_type) {
  CHECK(IsIntType(v->type()) && IsFloatType(dest_type));

  Instr *instr = AppendInstr(OP_ITOF, dest_type);
  instr->set_arg0(v);
  return instr;
}

Instr *IRBuilder::SExt(Value *v, ValueType dest_type) {
  CHECK(IsIntType(v->type()) && IsIntType(dest_type));

  Instr *instr = AppendInstr(OP_SEXT, dest_type);
  instr->set_arg0(v);
  return instr;
}

Instr *IRBuilder::ZExt(Value *v, ValueType dest_type) {
  CHECK(IsIntType(v->type()) && IsIntType(dest_type));

  Instr *instr = AppendInstr(OP_ZEXT, dest_type);
  instr->set_arg0(v);
  return instr;
}

Instr *IRBuilder::Trunc(Value *v, ValueType dest_type) {
  CHECK(IsIntType(v->type()) && IsIntType(dest_type));

  Instr *instr = AppendInstr(OP_TRUNC, dest_type);
  instr->set_arg0(v);
  return instr;
}

Instr *IRBuilder::FExt(Value *v, ValueType dest_type) {
  CHECK(v->type() == VALUE_F32 && dest_type == VALUE_F64);

  Instr *instr = AppendInstr(OP_FEXT, dest_type);
  instr->set_arg0(v);
  return instr;
}

Instr *IRBuilder::FTrunc(Value *v, ValueType dest_type) {
  CHECK(v->type() == VALUE_F64 && dest_type == VALUE_F32);

  Instr *instr = AppendInstr(OP_FTRUNC, dest_type);
  instr->set_arg0(v);
  return instr;
}

Instr *IRBuilder::Select(Value *cond, Value *t, Value *f) {
  CHECK(IsIntType(cond->type()) && IsIntType(t->type()) &&
        t->type() == f->type());

  Instr *instr = AppendInstr(OP_SELECT, t->type());
  instr->set_arg0(t);
  instr->set_arg1(f);
  instr->set_arg2(cond);
  return instr;
}

Instr *IRBuilder::Cmp(Value *a, Value *b, CmpType type) {
  CHECK(IsIntType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_CMP, VALUE_I8);
  instr->set_arg0(a);
  instr->set_arg1(b);
  instr->set_arg2(AllocConstant(type));
  return instr;
}

Instr *IRBuilder::CmpEQ(Value *a, Value *b) {
  return Cmp(a, b, CMP_EQ);
}

Instr *IRBuilder::CmpNE(Value *a, Value *b) {
  return Cmp(a, b, CMP_NE);
}

Instr *IRBuilder::CmpSGE(Value *a, Value *b) {
  return Cmp(a, b, CMP_SGE);
}

Instr *IRBuilder::CmpSGT(Value *a, Value *b) {
  return Cmp(a, b, CMP_SGT);
}

Instr *IRBuilder::CmpUGE(Value *a, Value *b) {
  return Cmp(a, b, CMP_UGE);
}

Instr *IRBuilder::CmpUGT(Value *a, Value *b) {
  return Cmp(a, b, CMP_UGT);
}

Instr *IRBuilder::CmpSLE(Value *a, Value *b) {
  return Cmp(a, b, CMP_SLE);
}

Instr *IRBuilder::CmpSLT(Value *a, Value *b) {
  return Cmp(a, b, CMP_SLT);
}

Instr *IRBuilder::CmpULE(Value *a, Value *b) {
  return Cmp(a, b, CMP_ULE);
}

Instr *IRBuilder::CmpULT(Value *a, Value *b) {
  return Cmp(a, b, CMP_ULT);
}

Instr *IRBuilder::FCmp(Value *a, Value *b, CmpType type) {
  CHECK(IsFloatType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_FCMP, VALUE_I8);
  instr->set_arg0(a);
  instr->set_arg1(b);
  instr->set_arg2(AllocConstant(type));
  return instr;
}

Instr *IRBuilder::FCmpEQ(Value *a, Value *b) {
  return FCmp(a, b, CMP_EQ);
}

Instr *IRBuilder::FCmpNE(Value *a, Value *b) {
  return FCmp(a, b, CMP_NE);
}

Instr *IRBuilder::FCmpGE(Value *a, Value *b) {
  return FCmp(a, b, CMP_SGE);
}

Instr *IRBuilder::FCmpGT(Value *a, Value *b) {
  return FCmp(a, b, CMP_SGT);
}

Instr *IRBuilder::FCmpLE(Value *a, Value *b) {
  return FCmp(a, b, CMP_SLE);
}

Instr *IRBuilder::FCmpLT(Value *a, Value *b) {
  return FCmp(a, b, CMP_SLT);
}

Instr *IRBuilder::Add(Value *a, Value *b) {
  CHECK(IsIntType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_ADD, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::Sub(Value *a, Value *b) {
  CHECK(IsIntType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_SUB, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::SMul(Value *a, Value *b) {
  CHECK(IsIntType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_SMUL, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::UMul(Value *a, Value *b) {
  CHECK(IsIntType(a->type()) && a->type() == b->type());

  CHECK(IsIntType(a->type()));
  Instr *instr = AppendInstr(OP_UMUL, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::Div(Value *a, Value *b) {
  CHECK(IsIntType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_DIV, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::Neg(Value *a) {
  CHECK(IsIntType(a->type()));

  Instr *instr = AppendInstr(OP_NEG, a->type());
  instr->set_arg0(a);
  return instr;
}

Instr *IRBuilder::Abs(Value *a) {
  CHECK(IsIntType(a->type()));

  Instr *instr = AppendInstr(OP_ABS, a->type());
  instr->set_arg0(a);
  return instr;
}

Instr *IRBuilder::FAdd(Value *a, Value *b) {
  CHECK(IsFloatType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_FADD, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::FSub(Value *a, Value *b) {
  CHECK(IsFloatType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_FSUB, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::FMul(Value *a, Value *b) {
  CHECK(IsFloatType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_FMUL, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::FDiv(Value *a, Value *b) {
  CHECK(IsFloatType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_FDIV, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::FNeg(Value *a) {
  CHECK(IsFloatType(a->type()));

  Instr *instr = AppendInstr(OP_FNEG, a->type());
  instr->set_arg0(a);
  return instr;
}

Instr *IRBuilder::FAbs(Value *a) {
  CHECK(IsFloatType(a->type()));

  Instr *instr = AppendInstr(OP_FABS, a->type());
  instr->set_arg0(a);
  return instr;
}

Instr *IRBuilder::Sqrt(Value *a) {
  CHECK(IsFloatType(a->type()));

  Instr *instr = AppendInstr(OP_SQRT, a->type());
  instr->set_arg0(a);
  return instr;
}

Instr *IRBuilder::VBroadcast(Value *a) {
  CHECK(a->type() == VALUE_F32);

  Instr *instr = AppendInstr(OP_VBROADCAST, VALUE_V128);
  instr->set_arg0(a);
  return instr;
}

Instr *IRBuilder::VAdd(Value *a, Value *b, ValueType el_type) {
  CHECK(IsVectorType(a->type()) && IsVectorType(b->type()));
  CHECK_EQ(el_type, VALUE_F32);

  Instr *instr = AppendInstr(OP_VADD, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::VDot(Value *a, Value *b, ValueType el_type) {
  CHECK(IsVectorType(a->type()) && IsVectorType(b->type()));
  CHECK_EQ(el_type, VALUE_F32);

  Instr *instr = AppendInstr(OP_VDOT, el_type);
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::VMul(Value *a, Value *b, ValueType el_type) {
  CHECK(IsVectorType(a->type()) && IsVectorType(b->type()));
  CHECK_EQ(el_type, VALUE_F32);

  Instr *instr = AppendInstr(OP_VMUL, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::And(Value *a, Value *b) {
  CHECK(IsIntType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_AND, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::Or(Value *a, Value *b) {
  CHECK(IsIntType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_OR, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::Xor(Value *a, Value *b) {
  CHECK(IsIntType(a->type()) && a->type() == b->type());

  Instr *instr = AppendInstr(OP_XOR, a->type());
  instr->set_arg0(a);
  instr->set_arg1(b);
  return instr;
}

Instr *IRBuilder::Not(Value *a) {
  CHECK(IsIntType(a->type()));

  Instr *instr = AppendInstr(OP_NOT, a->type());
  instr->set_arg0(a);
  return instr;
}

Instr *IRBuilder::Shl(Value *a, Value *n) {
  CHECK(IsIntType(a->type()) && n->type() == VALUE_I32);

  Instr *instr = AppendInstr(OP_SHL, a->type());
  instr->set_arg0(a);
  instr->set_arg1(n);
  return instr;
}

Instr *IRBuilder::Shl(Value *a, int n) {
  return Shl(a, AllocConstant((int32_t)n));
}

Instr *IRBuilder::AShr(Value *a, Value *n) {
  CHECK(IsIntType(a->type()) && n->type() == VALUE_I32);

  Instr *instr = AppendInstr(OP_ASHR, a->type());
  instr->set_arg0(a);
  instr->set_arg1(n);
  return instr;
}

Instr *IRBuilder::AShr(Value *a, int n) {
  return AShr(a, AllocConstant((int32_t)n));
}

Instr *IRBuilder::LShr(Value *a, Value *n) {
  CHECK(IsIntType(a->type()) && n->type() == VALUE_I32);

  Instr *instr = AppendInstr(OP_LSHR, a->type());
  instr->set_arg0(a);
  instr->set_arg1(n);
  return instr;
}

Instr *IRBuilder::LShr(Value *a, int n) {
  return LShr(a, AllocConstant((int32_t)n));
}

Instr *IRBuilder::AShd(Value *a, Value *n) {
  CHECK(a->type() == VALUE_I32 && n->type() == VALUE_I32);

  Instr *instr = AppendInstr(OP_ASHD, a->type());
  instr->set_arg0(a);
  instr->set_arg1(n);
  return instr;
}

Instr *IRBuilder::LShd(Value *a, Value *n) {
  CHECK(a->type() == VALUE_I32 && n->type() == VALUE_I32);

  Instr *instr = AppendInstr(OP_LSHD, a->type());
  instr->set_arg0(a);
  instr->set_arg1(n);
  return instr;
}

void IRBuilder::Branch(Value *dest) {
  Instr *instr = AppendInstr(OP_BRANCH);
  instr->set_arg0(dest);
}

void IRBuilder::BranchCond(Value *cond, Value *true_addr, Value *false_addr) {
  Instr *instr = AppendInstr(OP_BRANCH_COND);
  instr->set_arg0(cond);
  instr->set_arg1(true_addr);
  instr->set_arg2(false_addr);
}

void IRBuilder::CallExternal1(Value *addr) {
  CHECK_EQ(addr->type(), VALUE_I64);

  Instr *instr = AppendInstr(OP_CALL_EXTERNAL);
  instr->set_arg0(addr);
}

void IRBuilder::CallExternal2(Value *addr, Value *arg0) {
  CHECK_EQ(addr->type(), VALUE_I64);
  CHECK_EQ(arg0->type(), VALUE_I64);

  Instr *instr = AppendInstr(OP_CALL_EXTERNAL);
  instr->set_arg0(addr);
  instr->set_arg1(arg0);
}

Value *IRBuilder::AllocConstant(uint8_t c) {
  return AllocConstant((int8_t)c);
}

Value *IRBuilder::AllocConstant(uint16_t c) {
  return AllocConstant((int16_t)c);
}

Value *IRBuilder::AllocConstant(uint32_t c) {
  return AllocConstant((int32_t)c);
}

Value *IRBuilder::AllocConstant(uint64_t c) {
  return AllocConstant((int64_t)c);
}

Value *IRBuilder::AllocConstant(int8_t c) {
  Value *v = arena_.Alloc<Value>();
  new (v) Value(c);
  return v;
}

Value *IRBuilder::AllocConstant(int16_t c) {
  Value *v = arena_.Alloc<Value>();
  new (v) Value(c);
  return v;
}

Value *IRBuilder::AllocConstant(int32_t c) {
  Value *v = arena_.Alloc<Value>();
  new (v) Value(c);
  return v;
}

Value *IRBuilder::AllocConstant(int64_t c) {
  Value *v = arena_.Alloc<Value>();
  new (v) Value(c);
  return v;
}

Value *IRBuilder::AllocConstant(float c) {
  Value *v = arena_.Alloc<Value>();
  new (v) Value(c);
  return v;
}

Value *IRBuilder::AllocConstant(double c) {
  Value *v = arena_.Alloc<Value>();
  new (v) Value(c);
  return v;
}

Local *IRBuilder::AllocLocal(ValueType type) {
  // align local to natural size
  int type_size = SizeForType(type);
  locals_size_ = align_up(locals_size_, type_size);

  Local *l = arena_.Alloc<Local>();
  new (l) Local(type, AllocConstant(locals_size_));
  locals_.Append(l);

  locals_size_ += type_size;

  return l;
}

Instr *IRBuilder::AllocInstr(Op op, ValueType result_type) {
  Instr *instr = arena_.Alloc<Instr>();
  new (instr) Instr(op, result_type);
  return instr;
}

Instr *IRBuilder::AppendInstr(Op op) {
  Instr *instr = AllocInstr(op, VALUE_V);
  instrs_.Insert(current_instr_, instr);
  current_instr_ = instr;
  return instr;
}

Instr *IRBuilder::AppendInstr(Op op, ValueType result_type) {
  Instr *instr = AllocInstr(op, result_type);
  instrs_.Insert(current_instr_, instr);
  current_instr_ = instr;
  return instr;
}
