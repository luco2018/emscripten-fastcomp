//===- ExpandArithWithOverflow.cpp - Expand out uses of *.with.overflow----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The llvm.*.with.overflow.*() intrinsics are awkward for PNaCl
// support because they return structs, and we want to omit struct
// types from IR in PNaCl's stable ABI.
//
// However, llvm.{umul,uadd}.with.overflow.*() are used by Clang to
// implement an overflow check for C++'s new[] operator.  This pass
// expands out these uses so that PNaCl does not have to support
// *.with.overflow as part of PNaCl's stable ABI.
//
// This pass only handles adding/multiplying by a constant, which is
// the only use of *.with.overflow that is currently generated by
// Clang (unless '-ftrapv' is passed to Clang).
//
// X * Const overflows iff X > UINT_MAX / Const, where UINT_MAX is the
// maximum value for the integer type being used.
//
// Similarly, X + Const overflows iff X > UINT_MAX - Const.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/NaCl.h"

using namespace llvm;

namespace {
  // This is a ModulePass so that the pass can easily iterate over all
  // uses of the intrinsics.
  class ExpandArithWithOverflow : public ModulePass {
  public:
    static char ID; // Pass identification, replacement for typeid
    ExpandArithWithOverflow() : ModulePass(ID) {
      initializeExpandArithWithOverflowPass(*PassRegistry::getPassRegistry());
    }

    virtual bool runOnModule(Module &M);
  };
}

char ExpandArithWithOverflow::ID = 0;
INITIALIZE_PASS(ExpandArithWithOverflow, "expand-arith-with-overflow",
                "Expand out some uses of *.with.overflow intrinsics",
                false, false)

static uint64_t UintTypeMax(unsigned Bits) {
  // Avoid doing 1 << 64 because that is undefined on a uint64_t.
  if (Bits == 64)
    return ~(uint64_t) 0;
  return (((uint64_t) 1) << Bits) - 1;
}

static int64_t SintTypeMax(unsigned Bits) {
  return (((int64_t) 1) << (Bits-1)) - 1;
}

static int64_t SintTypeMin(unsigned Bits) {
  return -(((int64_t) 1) << (Bits-1));
}

static Value *CreateInsertValue(Value *StructVal, unsigned Index,
                                Value *Field, Instruction *BasedOn) {
  SmallVector<unsigned, 1> EVIndexes;
  EVIndexes.push_back(Index);
  return CopyDebug(InsertValueInst::Create(
                       StructVal, Field, EVIndexes,
                       BasedOn->getName() + ".insert", BasedOn), BasedOn);
}

static bool ExpandOpForIntSize(Module *M, unsigned Bits, bool Mul, bool Signed) {
  IntegerType *IntTy = IntegerType::get(M->getContext(), Bits);
  SmallVector<Type *, 1> Types;
  Types.push_back(IntTy);
  Intrinsic::ID ID;
  if (!Signed) {
    ID = (Mul ? Intrinsic::umul_with_overflow
              : Intrinsic::uadd_with_overflow);
  } else {
    ID = (Mul ? Intrinsic::smul_with_overflow
              : Intrinsic::sadd_with_overflow);
  }
  std::string Name = Intrinsic::getName(ID, Types);
  Function *Intrinsic = M->getFunction(Name);
  if (!Intrinsic)
    return false;

  SmallVector<CallInst *, 64> Calls;
  for (User *U : Intrinsic->users()) {
    if (CallInst *Call = dyn_cast<CallInst>(U))
      Calls.push_back(Call);
    else
      report_fatal_error("ExpandArithWithOverflow: Taking the address of a "
                         "*.with.overflow intrinsic is not allowed");
  }

  for (CallInst *Call : Calls) {
    Value *VariableArg, *VariableArg2 = nullptr;
    ConstantInt *ConstantArg;
    if (ConstantInt *C = dyn_cast<ConstantInt>(Call->getArgOperand(0))) {
      VariableArg = Call->getArgOperand(1);
      ConstantArg = C;
    } else if (ConstantInt *C = dyn_cast<ConstantInt>(Call->getArgOperand(1))) {
      VariableArg = Call->getArgOperand(0);
      ConstantArg = C;
    } else if (!Mul) {
      // XXX EMSCRIPTEN: generalize this to nonconstant values, easy for addition
      VariableArg = Call->getArgOperand(0);
      VariableArg2 = Call->getArgOperand(1);
    } else {
      errs() << "Use: " << *Call << "\n";
      report_fatal_error("ExpandArithWithOverflow: At least one argument of "
                         "*.with.overflow must be a constant");
    }

    Value *ArithResult, *OverflowResult;

    if (!VariableArg2 && !Signed) {
      ArithResult = BinaryOperator::Create(
          (Mul ? Instruction::Mul : Instruction::Add), VariableArg, ConstantArg,
          Call->getName() + ".arith", Call);

      uint64_t ArgMax;
      if (Mul) {
        ArgMax = UintTypeMax(Bits) / ConstantArg->getZExtValue();
      } else {
        ArgMax = UintTypeMax(Bits) - ConstantArg->getZExtValue();
      }
      OverflowResult = new ICmpInst(
          Call, CmpInst::ICMP_UGT, VariableArg, ConstantInt::get(IntTy, ArgMax),
          Call->getName() + ".overflow");
    } else {
      // XXX EMSCRIPTEN: generalize this to nonconstant values, easy for addition
      //                 also for signed values
      assert(!Mul);
      if (!VariableArg2) VariableArg2 = ConstantArg;
      ArithResult = BinaryOperator::Create(Instruction::Add,
          VariableArg, VariableArg2,
          Call->getName() + ".arith", Call);
      if (!Signed) {
        // If x+y < x (or y), unsigned 32 addition, then an overflow occurred
        OverflowResult = new ICmpInst(
            Call, CmpInst::ICMP_ULT, ArithResult, VariableArg,
            Call->getName() + ".overflow");
      } else {
        // In the signed case, we care if the sum is >127 or <-128. When looked at
        // as an unsigned number, that is precisely when the sum is >= 128
        Value *PositiveTemp = BinaryOperator::Create(Instruction::Add,
            VariableArg, ConstantInt::get(IntTy, SintTypeMin(Bits)),
            Call->getName() + ".postemp", Call);
        Value *NegativeTemp = BinaryOperator::Create(Instruction::Add,
            VariableArg, ConstantInt::get(IntTy, SintTypeMax(Bits)),
            Call->getName() + ".negtemp", Call);
        Value *PositiveCheck = new ICmpInst(
            Call, CmpInst::ICMP_SLT, ArithResult, PositiveTemp,
            Call->getName() + ".poscheck");
        Value *NegativeCheck = new ICmpInst(
            Call, CmpInst::ICMP_SGT, ArithResult, NegativeTemp,
            Call->getName() + ".negcheck");
        Value *IsPositive = new ICmpInst(
            Call, CmpInst::ICMP_SGT, VariableArg, ConstantInt::get(IntTy, 0),
            Call->getName() + ".ispos");
        OverflowResult = SelectInst::Create(
            IsPositive, PositiveCheck, NegativeCheck,
            Call->getName() + ".select",
            Call);
      }
    }

    // Construct the struct result.
    Value *NewStruct = UndefValue::get(Call->getType());
    NewStruct = CreateInsertValue(NewStruct, 0, ArithResult, Call);
    NewStruct = CreateInsertValue(NewStruct, 1, OverflowResult, Call);
    Call->replaceAllUsesWith(NewStruct);
    Call->eraseFromParent();
  }

  Intrinsic->eraseFromParent();
  return true;
}

static bool ExpandForIntSize(Module *M, unsigned Bits) {
  bool Modified = false;
  Modified |= ExpandOpForIntSize(M, Bits, true, false); // Expand umul
  Modified |= ExpandOpForIntSize(M, Bits, false, false); // Expand uadd
  Modified |= ExpandOpForIntSize(M, Bits, false, true); // Expand sadd (for ubsan) XXX EMSCRIPTEN
  return Modified;
}

bool ExpandArithWithOverflow::runOnModule(Module &M) {
  bool Modified = false;
  Modified |= ExpandForIntSize(&M, 64);
  Modified |= ExpandForIntSize(&M, 32);
  Modified |= ExpandForIntSize(&M, 16);
  Modified |= ExpandForIntSize(&M, 8);
  return Modified;
}

ModulePass *llvm::createExpandArithWithOverflowPass() {
  return new ExpandArithWithOverflow();
}
