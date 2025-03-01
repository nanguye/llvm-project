//===-- TargetLowering.cpp - Implement the TargetLowering class -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This implements the TargetLowering class.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetMachine.h"
#include <cctype>
using namespace llvm;

/// NOTE: The TargetMachine owns TLOF.
TargetLowering::TargetLowering(const TargetMachine &tm)
    : TargetLoweringBase(tm) {}

const char *TargetLowering::getTargetNodeName(unsigned Opcode) const {
  return nullptr;
}

bool TargetLowering::isPositionIndependent() const {
  return getTargetMachine().isPositionIndependent();
}

/// Check whether a given call node is in tail position within its function. If
/// so, it sets Chain to the input chain of the tail call.
bool TargetLowering::isInTailCallPosition(SelectionDAG &DAG, SDNode *Node,
                                          SDValue &Chain) const {
  const Function &F = DAG.getMachineFunction().getFunction();

  // Conservatively require the attributes of the call to match those of
  // the return. Ignore NoAlias and NonNull because they don't affect the
  // call sequence.
  AttributeList CallerAttrs = F.getAttributes();
  if (AttrBuilder(CallerAttrs, AttributeList::ReturnIndex)
          .removeAttribute(Attribute::NoAlias)
          .removeAttribute(Attribute::NonNull)
          .hasAttributes())
    return false;

  // It's not safe to eliminate the sign / zero extension of the return value.
  if (CallerAttrs.hasAttribute(AttributeList::ReturnIndex, Attribute::ZExt) ||
      CallerAttrs.hasAttribute(AttributeList::ReturnIndex, Attribute::SExt))
    return false;

  // Check if the only use is a function return node.
  return isUsedByReturnOnly(Node, Chain);
}

bool TargetLowering::parametersInCSRMatch(const MachineRegisterInfo &MRI,
    const uint32_t *CallerPreservedMask,
    const SmallVectorImpl<CCValAssign> &ArgLocs,
    const SmallVectorImpl<SDValue> &OutVals) const {
  for (unsigned I = 0, E = ArgLocs.size(); I != E; ++I) {
    const CCValAssign &ArgLoc = ArgLocs[I];
    if (!ArgLoc.isRegLoc())
      continue;
    Register Reg = ArgLoc.getLocReg();
    // Only look at callee saved registers.
    if (MachineOperand::clobbersPhysReg(CallerPreservedMask, Reg))
      continue;
    // Check that we pass the value used for the caller.
    // (We look for a CopyFromReg reading a virtual register that is used
    //  for the function live-in value of register Reg)
    SDValue Value = OutVals[I];
    if (Value->getOpcode() != ISD::CopyFromReg)
      return false;
    unsigned ArgReg = cast<RegisterSDNode>(Value->getOperand(1))->getReg();
    if (MRI.getLiveInPhysReg(ArgReg) != Reg)
      return false;
  }
  return true;
}

/// Set CallLoweringInfo attribute flags based on a call instruction
/// and called function attributes.
void TargetLoweringBase::ArgListEntry::setAttributes(const CallBase *Call,
                                                     unsigned ArgIdx) {
  IsSExt = Call->paramHasAttr(ArgIdx, Attribute::SExt);
  IsZExt = Call->paramHasAttr(ArgIdx, Attribute::ZExt);
  IsInReg = Call->paramHasAttr(ArgIdx, Attribute::InReg);
  IsSRet = Call->paramHasAttr(ArgIdx, Attribute::StructRet);
  IsNest = Call->paramHasAttr(ArgIdx, Attribute::Nest);
  IsByVal = Call->paramHasAttr(ArgIdx, Attribute::ByVal);
  IsInAlloca = Call->paramHasAttr(ArgIdx, Attribute::InAlloca);
  IsReturned = Call->paramHasAttr(ArgIdx, Attribute::Returned);
  IsSwiftSelf = Call->paramHasAttr(ArgIdx, Attribute::SwiftSelf);
  IsSwiftError = Call->paramHasAttr(ArgIdx, Attribute::SwiftError);
  Alignment = Call->getParamAlignment(ArgIdx);
  ByValType = nullptr;
  if (Call->paramHasAttr(ArgIdx, Attribute::ByVal))
    ByValType = Call->getParamByValType(ArgIdx);
}

/// Generate a libcall taking the given operands as arguments and returning a
/// result of type RetVT.
std::pair<SDValue, SDValue>
TargetLowering::makeLibCall(SelectionDAG &DAG, RTLIB::Libcall LC, EVT RetVT,
                            ArrayRef<SDValue> Ops,
                            MakeLibCallOptions CallOptions,
                            const SDLoc &dl,
                            SDValue InChain) const {
  if (!InChain)
    InChain = DAG.getEntryNode();

  TargetLowering::ArgListTy Args;
  Args.reserve(Ops.size());

  TargetLowering::ArgListEntry Entry;
  for (unsigned i = 0; i < Ops.size(); ++i) {
    SDValue NewOp = Ops[i];
    Entry.Node = NewOp;
    Entry.Ty = Entry.Node.getValueType().getTypeForEVT(*DAG.getContext());
    Entry.IsSExt = shouldSignExtendTypeInLibCall(NewOp.getValueType(),
                                                 CallOptions.IsSExt);
    Entry.IsZExt = !Entry.IsSExt;

    if (CallOptions.IsSoften &&
        !shouldExtendTypeInLibCall(CallOptions.OpsVTBeforeSoften[i])) {
      Entry.IsSExt = Entry.IsZExt = false;
    }
    Args.push_back(Entry);
  }

  if (LC == RTLIB::UNKNOWN_LIBCALL)
    report_fatal_error("Unsupported library call operation!");
  SDValue Callee = DAG.getExternalSymbol(getLibcallName(LC),
                                         getPointerTy(DAG.getDataLayout()));

  Type *RetTy = RetVT.getTypeForEVT(*DAG.getContext());
  TargetLowering::CallLoweringInfo CLI(DAG);
  bool signExtend = shouldSignExtendTypeInLibCall(RetVT, CallOptions.IsSExt);
  bool zeroExtend = !signExtend;

  if (CallOptions.IsSoften &&
      !shouldExtendTypeInLibCall(CallOptions.RetVTBeforeSoften)) {
    signExtend = zeroExtend = false;
  }

  CLI.setDebugLoc(dl)
      .setChain(InChain)
      .setLibCallee(getLibcallCallingConv(LC), RetTy, Callee, std::move(Args))
      .setNoReturn(CallOptions.DoesNotReturn)
      .setDiscardResult(!CallOptions.IsReturnValueUsed)
      .setIsPostTypeLegalization(CallOptions.IsPostTypeLegalization)
      .setSExtResult(signExtend)
      .setZExtResult(zeroExtend);
  return LowerCallTo(CLI);
}

bool
TargetLowering::findOptimalMemOpLowering(std::vector<EVT> &MemOps,
                                         unsigned Limit, uint64_t Size,
                                         unsigned DstAlign, unsigned SrcAlign,
                                         bool IsMemset,
                                         bool ZeroMemset,
                                         bool MemcpyStrSrc,
                                         bool AllowOverlap,
                                         unsigned DstAS, unsigned SrcAS,
                                         const AttributeList &FuncAttributes) const {
  // If 'SrcAlign' is zero, that means the memory operation does not need to
  // load the value, i.e. memset or memcpy from constant string. Otherwise,
  // it's the inferred alignment of the source. 'DstAlign', on the other hand,
  // is the specified alignment of the memory operation. If it is zero, that
  // means it's possible to change the alignment of the destination.
  // 'MemcpyStrSrc' indicates whether the memcpy source is constant so it does
  // not need to be loaded.
  if (!(SrcAlign == 0 || SrcAlign >= DstAlign))
    return false;

  EVT VT = getOptimalMemOpType(Size, DstAlign, SrcAlign,
                               IsMemset, ZeroMemset, MemcpyStrSrc,
                               FuncAttributes);

  if (VT == MVT::Other) {
    // Use the largest integer type whose alignment constraints are satisfied.
    // We only need to check DstAlign here as SrcAlign is always greater or
    // equal to DstAlign (or zero).
    VT = MVT::i64;
    while (DstAlign && DstAlign < VT.getSizeInBits() / 8 &&
           !allowsMisalignedMemoryAccesses(VT, DstAS, DstAlign))
      VT = (MVT::SimpleValueType)(VT.getSimpleVT().SimpleTy - 1);
    assert(VT.isInteger());

    // Find the largest legal integer type.
    MVT LVT = MVT::i64;
    while (!isTypeLegal(LVT))
      LVT = (MVT::SimpleValueType)(LVT.SimpleTy - 1);
    assert(LVT.isInteger());

    // If the type we've chosen is larger than the largest legal integer type
    // then use that instead.
    if (VT.bitsGT(LVT))
      VT = LVT;
  }

  unsigned NumMemOps = 0;
  while (Size != 0) {
    unsigned VTSize = VT.getSizeInBits() / 8;
    while (VTSize > Size) {
      // For now, only use non-vector load / store's for the left-over pieces.
      EVT NewVT = VT;
      unsigned NewVTSize;

      bool Found = false;
      if (VT.isVector() || VT.isFloatingPoint()) {
        NewVT = (VT.getSizeInBits() > 64) ? MVT::i64 : MVT::i32;
        if (isOperationLegalOrCustom(ISD::STORE, NewVT) &&
            isSafeMemOpType(NewVT.getSimpleVT()))
          Found = true;
        else if (NewVT == MVT::i64 &&
                 isOperationLegalOrCustom(ISD::STORE, MVT::f64) &&
                 isSafeMemOpType(MVT::f64)) {
          // i64 is usually not legal on 32-bit targets, but f64 may be.
          NewVT = MVT::f64;
          Found = true;
        }
      }

      if (!Found) {
        do {
          NewVT = (MVT::SimpleValueType)(NewVT.getSimpleVT().SimpleTy - 1);
          if (NewVT == MVT::i8)
            break;
        } while (!isSafeMemOpType(NewVT.getSimpleVT()));
      }
      NewVTSize = NewVT.getSizeInBits() / 8;

      // If the new VT cannot cover all of the remaining bits, then consider
      // issuing a (or a pair of) unaligned and overlapping load / store.
      bool Fast;
      if (NumMemOps && AllowOverlap && NewVTSize < Size &&
          allowsMisalignedMemoryAccesses(VT, DstAS, DstAlign,
                                         MachineMemOperand::MONone, &Fast) &&
          Fast)
        VTSize = Size;
      else {
        VT = NewVT;
        VTSize = NewVTSize;
      }
    }

    if (++NumMemOps > Limit)
      return false;

    MemOps.push_back(VT);
    Size -= VTSize;
  }

  return true;
}

/// Soften the operands of a comparison. This code is shared among BR_CC,
/// SELECT_CC, and SETCC handlers.
void TargetLowering::softenSetCCOperands(SelectionDAG &DAG, EVT VT,
                                         SDValue &NewLHS, SDValue &NewRHS,
                                         ISD::CondCode &CCCode,
                                         const SDLoc &dl, const SDValue OldLHS,
                                         const SDValue OldRHS) const {
  assert((VT == MVT::f32 || VT == MVT::f64 || VT == MVT::f128 || VT == MVT::ppcf128)
         && "Unsupported setcc type!");

  // Expand into one or more soft-fp libcall(s).
  RTLIB::Libcall LC1 = RTLIB::UNKNOWN_LIBCALL, LC2 = RTLIB::UNKNOWN_LIBCALL;
  bool ShouldInvertCC = false;
  switch (CCCode) {
  case ISD::SETEQ:
  case ISD::SETOEQ:
    LC1 = (VT == MVT::f32) ? RTLIB::OEQ_F32 :
          (VT == MVT::f64) ? RTLIB::OEQ_F64 :
          (VT == MVT::f128) ? RTLIB::OEQ_F128 : RTLIB::OEQ_PPCF128;
    break;
  case ISD::SETNE:
  case ISD::SETUNE:
    LC1 = (VT == MVT::f32) ? RTLIB::UNE_F32 :
          (VT == MVT::f64) ? RTLIB::UNE_F64 :
          (VT == MVT::f128) ? RTLIB::UNE_F128 : RTLIB::UNE_PPCF128;
    break;
  case ISD::SETGE:
  case ISD::SETOGE:
    LC1 = (VT == MVT::f32) ? RTLIB::OGE_F32 :
          (VT == MVT::f64) ? RTLIB::OGE_F64 :
          (VT == MVT::f128) ? RTLIB::OGE_F128 : RTLIB::OGE_PPCF128;
    break;
  case ISD::SETLT:
  case ISD::SETOLT:
    LC1 = (VT == MVT::f32) ? RTLIB::OLT_F32 :
          (VT == MVT::f64) ? RTLIB::OLT_F64 :
          (VT == MVT::f128) ? RTLIB::OLT_F128 : RTLIB::OLT_PPCF128;
    break;
  case ISD::SETLE:
  case ISD::SETOLE:
    LC1 = (VT == MVT::f32) ? RTLIB::OLE_F32 :
          (VT == MVT::f64) ? RTLIB::OLE_F64 :
          (VT == MVT::f128) ? RTLIB::OLE_F128 : RTLIB::OLE_PPCF128;
    break;
  case ISD::SETGT:
  case ISD::SETOGT:
    LC1 = (VT == MVT::f32) ? RTLIB::OGT_F32 :
          (VT == MVT::f64) ? RTLIB::OGT_F64 :
          (VT == MVT::f128) ? RTLIB::OGT_F128 : RTLIB::OGT_PPCF128;
    break;
  case ISD::SETUO:
    LC1 = (VT == MVT::f32) ? RTLIB::UO_F32 :
          (VT == MVT::f64) ? RTLIB::UO_F64 :
          (VT == MVT::f128) ? RTLIB::UO_F128 : RTLIB::UO_PPCF128;
    break;
  case ISD::SETO:
    LC1 = (VT == MVT::f32) ? RTLIB::O_F32 :
          (VT == MVT::f64) ? RTLIB::O_F64 :
          (VT == MVT::f128) ? RTLIB::O_F128 : RTLIB::O_PPCF128;
    break;
  case ISD::SETONE:
    // SETONE = SETOLT | SETOGT
    LC1 = (VT == MVT::f32) ? RTLIB::OLT_F32 :
          (VT == MVT::f64) ? RTLIB::OLT_F64 :
          (VT == MVT::f128) ? RTLIB::OLT_F128 : RTLIB::OLT_PPCF128;
    LC2 = (VT == MVT::f32) ? RTLIB::OGT_F32 :
          (VT == MVT::f64) ? RTLIB::OGT_F64 :
          (VT == MVT::f128) ? RTLIB::OGT_F128 : RTLIB::OGT_PPCF128;
    break;
  case ISD::SETUEQ:
    LC1 = (VT == MVT::f32) ? RTLIB::UO_F32 :
          (VT == MVT::f64) ? RTLIB::UO_F64 :
          (VT == MVT::f128) ? RTLIB::UO_F128 : RTLIB::UO_PPCF128;
    LC2 = (VT == MVT::f32) ? RTLIB::OEQ_F32 :
          (VT == MVT::f64) ? RTLIB::OEQ_F64 :
          (VT == MVT::f128) ? RTLIB::OEQ_F128 : RTLIB::OEQ_PPCF128;
    break;
  default:
    // Invert CC for unordered comparisons
    ShouldInvertCC = true;
    switch (CCCode) {
    case ISD::SETULT:
      LC1 = (VT == MVT::f32) ? RTLIB::OGE_F32 :
            (VT == MVT::f64) ? RTLIB::OGE_F64 :
            (VT == MVT::f128) ? RTLIB::OGE_F128 : RTLIB::OGE_PPCF128;
      break;
    case ISD::SETULE:
      LC1 = (VT == MVT::f32) ? RTLIB::OGT_F32 :
            (VT == MVT::f64) ? RTLIB::OGT_F64 :
            (VT == MVT::f128) ? RTLIB::OGT_F128 : RTLIB::OGT_PPCF128;
      break;
    case ISD::SETUGT:
      LC1 = (VT == MVT::f32) ? RTLIB::OLE_F32 :
            (VT == MVT::f64) ? RTLIB::OLE_F64 :
            (VT == MVT::f128) ? RTLIB::OLE_F128 : RTLIB::OLE_PPCF128;
      break;
    case ISD::SETUGE:
      LC1 = (VT == MVT::f32) ? RTLIB::OLT_F32 :
            (VT == MVT::f64) ? RTLIB::OLT_F64 :
            (VT == MVT::f128) ? RTLIB::OLT_F128 : RTLIB::OLT_PPCF128;
      break;
    default: llvm_unreachable("Do not know how to soften this setcc!");
    }
  }

  // Use the target specific return value for comparions lib calls.
  EVT RetVT = getCmpLibcallReturnType();
  SDValue Ops[2] = {NewLHS, NewRHS};
  TargetLowering::MakeLibCallOptions CallOptions;
  EVT OpsVT[2] = { OldLHS.getValueType(),
                   OldRHS.getValueType() };
  CallOptions.setTypeListBeforeSoften(OpsVT, RetVT, true);
  NewLHS = makeLibCall(DAG, LC1, RetVT, Ops, CallOptions, dl).first;
  NewRHS = DAG.getConstant(0, dl, RetVT);

  CCCode = getCmpLibcallCC(LC1);
  if (ShouldInvertCC)
    CCCode = getSetCCInverse(CCCode, /*isInteger=*/true);

  if (LC2 != RTLIB::UNKNOWN_LIBCALL) {
    SDValue Tmp = DAG.getNode(
        ISD::SETCC, dl,
        getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), RetVT),
        NewLHS, NewRHS, DAG.getCondCode(CCCode));
    NewLHS = makeLibCall(DAG, LC2, RetVT, Ops, CallOptions, dl).first;
    NewLHS = DAG.getNode(
        ISD::SETCC, dl,
        getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), RetVT),
        NewLHS, NewRHS, DAG.getCondCode(getCmpLibcallCC(LC2)));
    NewLHS = DAG.getNode(ISD::OR, dl, Tmp.getValueType(), Tmp, NewLHS);
    NewRHS = SDValue();
  }
}

/// Return the entry encoding for a jump table in the current function. The
/// returned value is a member of the MachineJumpTableInfo::JTEntryKind enum.
unsigned TargetLowering::getJumpTableEncoding() const {
  // In non-pic modes, just use the address of a block.
  if (!isPositionIndependent())
    return MachineJumpTableInfo::EK_BlockAddress;

  // In PIC mode, if the target supports a GPRel32 directive, use it.
  if (getTargetMachine().getMCAsmInfo()->getGPRel32Directive() != nullptr)
    return MachineJumpTableInfo::EK_GPRel32BlockAddress;

  // Otherwise, use a label difference.
  return MachineJumpTableInfo::EK_LabelDifference32;
}

SDValue TargetLowering::getPICJumpTableRelocBase(SDValue Table,
                                                 SelectionDAG &DAG) const {
  // If our PIC model is GP relative, use the global offset table as the base.
  unsigned JTEncoding = getJumpTableEncoding();

  if ((JTEncoding == MachineJumpTableInfo::EK_GPRel64BlockAddress) ||
      (JTEncoding == MachineJumpTableInfo::EK_GPRel32BlockAddress))
    return DAG.getGLOBAL_OFFSET_TABLE(getPointerTy(DAG.getDataLayout()));

  return Table;
}

/// This returns the relocation base for the given PIC jumptable, the same as
/// getPICJumpTableRelocBase, but as an MCExpr.
const MCExpr *
TargetLowering::getPICJumpTableRelocBaseExpr(const MachineFunction *MF,
                                             unsigned JTI,MCContext &Ctx) const{
  // The normal PIC reloc base is the label at the start of the jump table.
  return MCSymbolRefExpr::create(MF->getJTISymbol(JTI, Ctx), Ctx);
}

bool
TargetLowering::isOffsetFoldingLegal(const GlobalAddressSDNode *GA) const {
  const TargetMachine &TM = getTargetMachine();
  const GlobalValue *GV = GA->getGlobal();

  // If the address is not even local to this DSO we will have to load it from
  // a got and then add the offset.
  if (!TM.shouldAssumeDSOLocal(*GV->getParent(), GV))
    return false;

  // If the code is position independent we will have to add a base register.
  if (isPositionIndependent())
    return false;

  // Otherwise we can do it.
  return true;
}

//===----------------------------------------------------------------------===//
//  Optimization Methods
//===----------------------------------------------------------------------===//

/// If the specified instruction has a constant integer operand and there are
/// bits set in that constant that are not demanded, then clear those bits and
/// return true.
bool TargetLowering::ShrinkDemandedConstant(SDValue Op, const APInt &Demanded,
                                            TargetLoweringOpt &TLO) const {
  SDLoc DL(Op);
  unsigned Opcode = Op.getOpcode();

  // Do target-specific constant optimization.
  if (targetShrinkDemandedConstant(Op, Demanded, TLO))
    return TLO.New.getNode();

  // FIXME: ISD::SELECT, ISD::SELECT_CC
  switch (Opcode) {
  default:
    break;
  case ISD::XOR:
  case ISD::AND:
  case ISD::OR: {
    auto *Op1C = dyn_cast<ConstantSDNode>(Op.getOperand(1));
    if (!Op1C)
      return false;

    // If this is a 'not' op, don't touch it because that's a canonical form.
    const APInt &C = Op1C->getAPIntValue();
    if (Opcode == ISD::XOR && Demanded.isSubsetOf(C))
      return false;

    if (!C.isSubsetOf(Demanded)) {
      EVT VT = Op.getValueType();
      SDValue NewC = TLO.DAG.getConstant(Demanded & C, DL, VT);
      SDValue NewOp = TLO.DAG.getNode(Opcode, DL, VT, Op.getOperand(0), NewC);
      return TLO.CombineTo(Op, NewOp);
    }

    break;
  }
  }

  return false;
}

/// Convert x+y to (VT)((SmallVT)x+(SmallVT)y) if the casts are free.
/// This uses isZExtFree and ZERO_EXTEND for the widening cast, but it could be
/// generalized for targets with other types of implicit widening casts.
bool TargetLowering::ShrinkDemandedOp(SDValue Op, unsigned BitWidth,
                                      const APInt &Demanded,
                                      TargetLoweringOpt &TLO) const {
  assert(Op.getNumOperands() == 2 &&
         "ShrinkDemandedOp only supports binary operators!");
  assert(Op.getNode()->getNumValues() == 1 &&
         "ShrinkDemandedOp only supports nodes with one result!");

  SelectionDAG &DAG = TLO.DAG;
  SDLoc dl(Op);

  // Early return, as this function cannot handle vector types.
  if (Op.getValueType().isVector())
    return false;

  // Don't do this if the node has another user, which may require the
  // full value.
  if (!Op.getNode()->hasOneUse())
    return false;

  // Search for the smallest integer type with free casts to and from
  // Op's type. For expedience, just check power-of-2 integer types.
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  unsigned DemandedSize = Demanded.getActiveBits();
  unsigned SmallVTBits = DemandedSize;
  if (!isPowerOf2_32(SmallVTBits))
    SmallVTBits = NextPowerOf2(SmallVTBits);
  for (; SmallVTBits < BitWidth; SmallVTBits = NextPowerOf2(SmallVTBits)) {
    EVT SmallVT = EVT::getIntegerVT(*DAG.getContext(), SmallVTBits);
    if (TLI.isTruncateFree(Op.getValueType(), SmallVT) &&
        TLI.isZExtFree(SmallVT, Op.getValueType())) {
      // We found a type with free casts.
      SDValue X = DAG.getNode(
          Op.getOpcode(), dl, SmallVT,
          DAG.getNode(ISD::TRUNCATE, dl, SmallVT, Op.getOperand(0)),
          DAG.getNode(ISD::TRUNCATE, dl, SmallVT, Op.getOperand(1)));
      assert(DemandedSize <= SmallVTBits && "Narrowed below demanded bits?");
      SDValue Z = DAG.getNode(ISD::ANY_EXTEND, dl, Op.getValueType(), X);
      return TLO.CombineTo(Op, Z);
    }
  }
  return false;
}

bool TargetLowering::SimplifyDemandedBits(SDValue Op, const APInt &DemandedBits,
                                          DAGCombinerInfo &DCI) const {
  SelectionDAG &DAG = DCI.DAG;
  TargetLoweringOpt TLO(DAG, !DCI.isBeforeLegalize(),
                        !DCI.isBeforeLegalizeOps());
  KnownBits Known;

  bool Simplified = SimplifyDemandedBits(Op, DemandedBits, Known, TLO);
  if (Simplified) {
    DCI.AddToWorklist(Op.getNode());
    DCI.CommitTargetLoweringOpt(TLO);
  }
  return Simplified;
}

bool TargetLowering::SimplifyDemandedBits(SDValue Op, const APInt &DemandedBits,
                                          KnownBits &Known,
                                          TargetLoweringOpt &TLO,
                                          unsigned Depth,
                                          bool AssumeSingleUse) const {
  EVT VT = Op.getValueType();
  APInt DemandedElts = VT.isVector()
                           ? APInt::getAllOnesValue(VT.getVectorNumElements())
                           : APInt(1, 1);
  return SimplifyDemandedBits(Op, DemandedBits, DemandedElts, Known, TLO, Depth,
                              AssumeSingleUse);
}

// TODO: Can we merge SelectionDAG::GetDemandedBits into this?
// TODO: Under what circumstances can we create nodes? Constant folding?
SDValue TargetLowering::SimplifyMultipleUseDemandedBits(
    SDValue Op, const APInt &DemandedBits, const APInt &DemandedElts,
    SelectionDAG &DAG, unsigned Depth) const {
  // Limit search depth.
  if (Depth >= SelectionDAG::MaxRecursionDepth)
    return SDValue();

  // Ignore UNDEFs.
  if (Op.isUndef())
    return SDValue();

  // Not demanding any bits/elts from Op.
  if (DemandedBits == 0 || DemandedElts == 0)
    return DAG.getUNDEF(Op.getValueType());

  unsigned NumElts = DemandedElts.getBitWidth();
  KnownBits LHSKnown, RHSKnown;
  switch (Op.getOpcode()) {
  case ISD::BITCAST: {
    SDValue Src = peekThroughBitcasts(Op.getOperand(0));
    EVT SrcVT = Src.getValueType();
    EVT DstVT = Op.getValueType();
    unsigned NumSrcEltBits = SrcVT.getScalarSizeInBits();
    unsigned NumDstEltBits = DstVT.getScalarSizeInBits();

    if (NumSrcEltBits == NumDstEltBits)
      if (SDValue V = SimplifyMultipleUseDemandedBits(
              Src, DemandedBits, DemandedElts, DAG, Depth + 1))
        return DAG.getBitcast(DstVT, V);

    // TODO - bigendian once we have test coverage.
    if (SrcVT.isVector() && (NumDstEltBits % NumSrcEltBits) == 0 &&
        DAG.getDataLayout().isLittleEndian()) {
      unsigned Scale = NumDstEltBits / NumSrcEltBits;
      unsigned NumSrcElts = SrcVT.getVectorNumElements();
      APInt DemandedSrcBits = APInt::getNullValue(NumSrcEltBits);
      APInt DemandedSrcElts = APInt::getNullValue(NumSrcElts);
      for (unsigned i = 0; i != Scale; ++i) {
        unsigned Offset = i * NumSrcEltBits;
        APInt Sub = DemandedBits.extractBits(NumSrcEltBits, Offset);
        if (!Sub.isNullValue()) {
          DemandedSrcBits |= Sub;
          for (unsigned j = 0; j != NumElts; ++j)
            if (DemandedElts[j])
              DemandedSrcElts.setBit((j * Scale) + i);
        }
      }

      if (SDValue V = SimplifyMultipleUseDemandedBits(
              Src, DemandedSrcBits, DemandedSrcElts, DAG, Depth + 1))
        return DAG.getBitcast(DstVT, V);
    }

    // TODO - bigendian once we have test coverage.
    if ((NumSrcEltBits % NumDstEltBits) == 0 &&
        DAG.getDataLayout().isLittleEndian()) {
      unsigned Scale = NumSrcEltBits / NumDstEltBits;
      unsigned NumSrcElts = SrcVT.isVector() ? SrcVT.getVectorNumElements() : 1;
      APInt DemandedSrcBits = APInt::getNullValue(NumSrcEltBits);
      APInt DemandedSrcElts = APInt::getNullValue(NumSrcElts);
      for (unsigned i = 0; i != NumElts; ++i)
        if (DemandedElts[i]) {
          unsigned Offset = (i % Scale) * NumDstEltBits;
          DemandedSrcBits.insertBits(DemandedBits, Offset);
          DemandedSrcElts.setBit(i / Scale);
        }

      if (SDValue V = SimplifyMultipleUseDemandedBits(
              Src, DemandedSrcBits, DemandedSrcElts, DAG, Depth + 1))
        return DAG.getBitcast(DstVT, V);
    }

    break;
  }
  case ISD::AND: {
    LHSKnown = DAG.computeKnownBits(Op.getOperand(0), DemandedElts, Depth + 1);
    RHSKnown = DAG.computeKnownBits(Op.getOperand(1), DemandedElts, Depth + 1);

    // If all of the demanded bits are known 1 on one side, return the other.
    // These bits cannot contribute to the result of the 'and' in this
    // context.
    if (DemandedBits.isSubsetOf(LHSKnown.Zero | RHSKnown.One))
      return Op.getOperand(0);
    if (DemandedBits.isSubsetOf(RHSKnown.Zero | LHSKnown.One))
      return Op.getOperand(1);
    break;
  }
  case ISD::OR: {
    LHSKnown = DAG.computeKnownBits(Op.getOperand(0), DemandedElts, Depth + 1);
    RHSKnown = DAG.computeKnownBits(Op.getOperand(1), DemandedElts, Depth + 1);

    // If all of the demanded bits are known zero on one side, return the
    // other.  These bits cannot contribute to the result of the 'or' in this
    // context.
    if (DemandedBits.isSubsetOf(LHSKnown.One | RHSKnown.Zero))
      return Op.getOperand(0);
    if (DemandedBits.isSubsetOf(RHSKnown.One | LHSKnown.Zero))
      return Op.getOperand(1);
    break;
  }
  case ISD::XOR: {
    LHSKnown = DAG.computeKnownBits(Op.getOperand(0), DemandedElts, Depth + 1);
    RHSKnown = DAG.computeKnownBits(Op.getOperand(1), DemandedElts, Depth + 1);

    // If all of the demanded bits are known zero on one side, return the
    // other.
    if (DemandedBits.isSubsetOf(RHSKnown.Zero))
      return Op.getOperand(0);
    if (DemandedBits.isSubsetOf(LHSKnown.Zero))
      return Op.getOperand(1);
    break;
  }
  case ISD::SIGN_EXTEND_INREG: {
    // If none of the extended bits are demanded, eliminate the sextinreg.
    EVT ExVT = cast<VTSDNode>(Op.getOperand(1))->getVT();
    if (DemandedBits.getActiveBits() <= ExVT.getScalarSizeInBits())
      return Op.getOperand(0);
    break;
  }
  case ISD::INSERT_VECTOR_ELT: {
    // If we don't demand the inserted element, return the base vector.
    SDValue Vec = Op.getOperand(0);
    auto *CIdx = dyn_cast<ConstantSDNode>(Op.getOperand(2));
    EVT VecVT = Vec.getValueType();
    if (CIdx && CIdx->getAPIntValue().ult(VecVT.getVectorNumElements()) &&
        !DemandedElts[CIdx->getZExtValue()])
      return Vec;
    break;
  }
  case ISD::VECTOR_SHUFFLE: {
    ArrayRef<int> ShuffleMask = cast<ShuffleVectorSDNode>(Op)->getMask();

    // If all the demanded elts are from one operand and are inline,
    // then we can use the operand directly.
    bool AllUndef = true, IdentityLHS = true, IdentityRHS = true;
    for (unsigned i = 0; i != NumElts; ++i) {
      int M = ShuffleMask[i];
      if (M < 0 || !DemandedElts[i])
        continue;
      AllUndef = false;
      IdentityLHS &= (M == (int)i);
      IdentityRHS &= ((M - NumElts) == i);
    }

    if (AllUndef)
      return DAG.getUNDEF(Op.getValueType());
    if (IdentityLHS)
      return Op.getOperand(0);
    if (IdentityRHS)
      return Op.getOperand(1);
    break;
  }
  default:
    if (Op.getOpcode() >= ISD::BUILTIN_OP_END)
      if (SDValue V = SimplifyMultipleUseDemandedBitsForTargetNode(
              Op, DemandedBits, DemandedElts, DAG, Depth))
        return V;
    break;
  }
  return SDValue();
}

/// Look at Op. At this point, we know that only the OriginalDemandedBits of the
/// result of Op are ever used downstream. If we can use this information to
/// simplify Op, create a new simplified DAG node and return true, returning the
/// original and new nodes in Old and New. Otherwise, analyze the expression and
/// return a mask of Known bits for the expression (used to simplify the
/// caller).  The Known bits may only be accurate for those bits in the
/// OriginalDemandedBits and OriginalDemandedElts.
bool TargetLowering::SimplifyDemandedBits(
    SDValue Op, const APInt &OriginalDemandedBits,
    const APInt &OriginalDemandedElts, KnownBits &Known, TargetLoweringOpt &TLO,
    unsigned Depth, bool AssumeSingleUse) const {
  unsigned BitWidth = OriginalDemandedBits.getBitWidth();
  assert(Op.getScalarValueSizeInBits() == BitWidth &&
         "Mask size mismatches value type size!");

  unsigned NumElts = OriginalDemandedElts.getBitWidth();
  assert((!Op.getValueType().isVector() ||
          NumElts == Op.getValueType().getVectorNumElements()) &&
         "Unexpected vector size");

  APInt DemandedBits = OriginalDemandedBits;
  APInt DemandedElts = OriginalDemandedElts;
  SDLoc dl(Op);
  auto &DL = TLO.DAG.getDataLayout();

  // Don't know anything.
  Known = KnownBits(BitWidth);

  // Undef operand.
  if (Op.isUndef())
    return false;

  if (Op.getOpcode() == ISD::Constant) {
    // We know all of the bits for a constant!
    Known.One = cast<ConstantSDNode>(Op)->getAPIntValue();
    Known.Zero = ~Known.One;
    return false;
  }

  // Other users may use these bits.
  EVT VT = Op.getValueType();
  if (!Op.getNode()->hasOneUse() && !AssumeSingleUse) {
    if (Depth != 0) {
      // If not at the root, Just compute the Known bits to
      // simplify things downstream.
      Known = TLO.DAG.computeKnownBits(Op, DemandedElts, Depth);
      return false;
    }
    // If this is the root being simplified, allow it to have multiple uses,
    // just set the DemandedBits/Elts to all bits.
    DemandedBits = APInt::getAllOnesValue(BitWidth);
    DemandedElts = APInt::getAllOnesValue(NumElts);
  } else if (OriginalDemandedBits == 0 || OriginalDemandedElts == 0) {
    // Not demanding any bits/elts from Op.
    return TLO.CombineTo(Op, TLO.DAG.getUNDEF(VT));
  } else if (Depth >= SelectionDAG::MaxRecursionDepth) {
    // Limit search depth.
    return false;
  }

  KnownBits Known2, KnownOut;
  switch (Op.getOpcode()) {
  case ISD::TargetConstant:
    llvm_unreachable("Can't simplify this node");
  case ISD::SCALAR_TO_VECTOR: {
    if (!DemandedElts[0])
      return TLO.CombineTo(Op, TLO.DAG.getUNDEF(VT));

    KnownBits SrcKnown;
    SDValue Src = Op.getOperand(0);
    unsigned SrcBitWidth = Src.getScalarValueSizeInBits();
    APInt SrcDemandedBits = DemandedBits.zextOrSelf(SrcBitWidth);
    if (SimplifyDemandedBits(Src, SrcDemandedBits, SrcKnown, TLO, Depth + 1))
      return true;
    Known = SrcKnown.zextOrTrunc(BitWidth, false);
    break;
  }
  case ISD::BUILD_VECTOR:
    // Collect the known bits that are shared by every demanded element.
    // TODO: Call SimplifyDemandedBits for non-constant demanded elements.
    Known = TLO.DAG.computeKnownBits(Op, DemandedElts, Depth);
    return false; // Don't fall through, will infinitely loop.
  case ISD::LOAD: {
    LoadSDNode *LD = cast<LoadSDNode>(Op);
    if (getTargetConstantFromLoad(LD)) {
      Known = TLO.DAG.computeKnownBits(Op, DemandedElts, Depth);
      return false; // Don't fall through, will infinitely loop.
    }
    break;
  }
  case ISD::INSERT_VECTOR_ELT: {
    SDValue Vec = Op.getOperand(0);
    SDValue Scl = Op.getOperand(1);
    auto *CIdx = dyn_cast<ConstantSDNode>(Op.getOperand(2));
    EVT VecVT = Vec.getValueType();

    // If index isn't constant, assume we need all vector elements AND the
    // inserted element.
    APInt DemandedVecElts(DemandedElts);
    if (CIdx && CIdx->getAPIntValue().ult(VecVT.getVectorNumElements())) {
      unsigned Idx = CIdx->getZExtValue();
      DemandedVecElts.clearBit(Idx);

      // Inserted element is not required.
      if (!DemandedElts[Idx])
        return TLO.CombineTo(Op, Vec);
    }

    KnownBits KnownScl;
    unsigned NumSclBits = Scl.getScalarValueSizeInBits();
    APInt DemandedSclBits = DemandedBits.zextOrTrunc(NumSclBits);
    if (SimplifyDemandedBits(Scl, DemandedSclBits, KnownScl, TLO, Depth + 1))
      return true;

    Known = KnownScl.zextOrTrunc(BitWidth, false);

    KnownBits KnownVec;
    if (SimplifyDemandedBits(Vec, DemandedBits, DemandedVecElts, KnownVec, TLO,
                             Depth + 1))
      return true;

    if (!!DemandedVecElts) {
      Known.One &= KnownVec.One;
      Known.Zero &= KnownVec.Zero;
    }

    return false;
  }
  case ISD::INSERT_SUBVECTOR: {
    SDValue Base = Op.getOperand(0);
    SDValue Sub = Op.getOperand(1);
    EVT SubVT = Sub.getValueType();
    unsigned NumSubElts = SubVT.getVectorNumElements();

    // If index isn't constant, assume we need the original demanded base
    // elements and ALL the inserted subvector elements.
    APInt BaseElts = DemandedElts;
    APInt SubElts = APInt::getAllOnesValue(NumSubElts);
    if (isa<ConstantSDNode>(Op.getOperand(2))) {
      const APInt &Idx = Op.getConstantOperandAPInt(2);
      if (Idx.ule(NumElts - NumSubElts)) {
        unsigned SubIdx = Idx.getZExtValue();
        SubElts = DemandedElts.extractBits(NumSubElts, SubIdx);
        BaseElts.insertBits(APInt::getNullValue(NumSubElts), SubIdx);
      }
    }

    KnownBits KnownSub, KnownBase;
    if (SimplifyDemandedBits(Sub, DemandedBits, SubElts, KnownSub, TLO,
                             Depth + 1))
      return true;
    if (SimplifyDemandedBits(Base, DemandedBits, BaseElts, KnownBase, TLO,
                             Depth + 1))
      return true;

    Known.Zero.setAllBits();
    Known.One.setAllBits();
    if (!!SubElts) {
        Known.One &= KnownSub.One;
        Known.Zero &= KnownSub.Zero;
    }
    if (!!BaseElts) {
        Known.One &= KnownBase.One;
        Known.Zero &= KnownBase.Zero;
    }
    break;
  }
  case ISD::EXTRACT_SUBVECTOR: {
    // If index isn't constant, assume we need all the source vector elements.
    SDValue Src = Op.getOperand(0);
    ConstantSDNode *SubIdx = dyn_cast<ConstantSDNode>(Op.getOperand(1));
    unsigned NumSrcElts = Src.getValueType().getVectorNumElements();
    APInt SrcElts = APInt::getAllOnesValue(NumSrcElts);
    if (SubIdx && SubIdx->getAPIntValue().ule(NumSrcElts - NumElts)) {
      // Offset the demanded elts by the subvector index.
      uint64_t Idx = SubIdx->getZExtValue();
      SrcElts = DemandedElts.zextOrSelf(NumSrcElts).shl(Idx);
    }
    if (SimplifyDemandedBits(Src, DemandedBits, SrcElts, Known, TLO, Depth + 1))
      return true;
    break;
  }
  case ISD::CONCAT_VECTORS: {
    Known.Zero.setAllBits();
    Known.One.setAllBits();
    EVT SubVT = Op.getOperand(0).getValueType();
    unsigned NumSubVecs = Op.getNumOperands();
    unsigned NumSubElts = SubVT.getVectorNumElements();
    for (unsigned i = 0; i != NumSubVecs; ++i) {
      APInt DemandedSubElts =
          DemandedElts.extractBits(NumSubElts, i * NumSubElts);
      if (SimplifyDemandedBits(Op.getOperand(i), DemandedBits, DemandedSubElts,
                               Known2, TLO, Depth + 1))
        return true;
      // Known bits are shared by every demanded subvector element.
      if (!!DemandedSubElts) {
        Known.One &= Known2.One;
        Known.Zero &= Known2.Zero;
      }
    }
    break;
  }
  case ISD::VECTOR_SHUFFLE: {
    ArrayRef<int> ShuffleMask = cast<ShuffleVectorSDNode>(Op)->getMask();

    // Collect demanded elements from shuffle operands..
    APInt DemandedLHS(NumElts, 0);
    APInt DemandedRHS(NumElts, 0);
    for (unsigned i = 0; i != NumElts; ++i) {
      if (!DemandedElts[i])
        continue;
      int M = ShuffleMask[i];
      if (M < 0) {
        // For UNDEF elements, we don't know anything about the common state of
        // the shuffle result.
        DemandedLHS.clearAllBits();
        DemandedRHS.clearAllBits();
        break;
      }
      assert(0 <= M && M < (int)(2 * NumElts) && "Shuffle index out of range");
      if (M < (int)NumElts)
        DemandedLHS.setBit(M);
      else
        DemandedRHS.setBit(M - NumElts);
    }

    if (!!DemandedLHS || !!DemandedRHS) {
      SDValue Op0 = Op.getOperand(0);
      SDValue Op1 = Op.getOperand(1);

      Known.Zero.setAllBits();
      Known.One.setAllBits();
      if (!!DemandedLHS) {
        if (SimplifyDemandedBits(Op0, DemandedBits, DemandedLHS, Known2, TLO,
                                 Depth + 1))
          return true;
        Known.One &= Known2.One;
        Known.Zero &= Known2.Zero;
      }
      if (!!DemandedRHS) {
        if (SimplifyDemandedBits(Op1, DemandedBits, DemandedRHS, Known2, TLO,
                                 Depth + 1))
          return true;
        Known.One &= Known2.One;
        Known.Zero &= Known2.Zero;
      }

      // Attempt to avoid multi-use ops if we don't need anything from them.
      SDValue DemandedOp0 = SimplifyMultipleUseDemandedBits(
          Op0, DemandedBits, DemandedLHS, TLO.DAG, Depth + 1);
      SDValue DemandedOp1 = SimplifyMultipleUseDemandedBits(
          Op1, DemandedBits, DemandedRHS, TLO.DAG, Depth + 1);
      if (DemandedOp0 || DemandedOp1) {
        Op0 = DemandedOp0 ? DemandedOp0 : Op0;
        Op1 = DemandedOp1 ? DemandedOp1 : Op1;
        SDValue NewOp = TLO.DAG.getVectorShuffle(VT, dl, Op0, Op1, ShuffleMask);
        return TLO.CombineTo(Op, NewOp);
      }
    }
    break;
  }
  case ISD::AND: {
    SDValue Op0 = Op.getOperand(0);
    SDValue Op1 = Op.getOperand(1);

    // If the RHS is a constant, check to see if the LHS would be zero without
    // using the bits from the RHS.  Below, we use knowledge about the RHS to
    // simplify the LHS, here we're using information from the LHS to simplify
    // the RHS.
    if (ConstantSDNode *RHSC = isConstOrConstSplat(Op1)) {
      // Do not increment Depth here; that can cause an infinite loop.
      KnownBits LHSKnown = TLO.DAG.computeKnownBits(Op0, DemandedElts, Depth);
      // If the LHS already has zeros where RHSC does, this 'and' is dead.
      if ((LHSKnown.Zero & DemandedBits) ==
          (~RHSC->getAPIntValue() & DemandedBits))
        return TLO.CombineTo(Op, Op0);

      // If any of the set bits in the RHS are known zero on the LHS, shrink
      // the constant.
      if (ShrinkDemandedConstant(Op, ~LHSKnown.Zero & DemandedBits, TLO))
        return true;

      // Bitwise-not (xor X, -1) is a special case: we don't usually shrink its
      // constant, but if this 'and' is only clearing bits that were just set by
      // the xor, then this 'and' can be eliminated by shrinking the mask of
      // the xor. For example, for a 32-bit X:
      // and (xor (srl X, 31), -1), 1 --> xor (srl X, 31), 1
      if (isBitwiseNot(Op0) && Op0.hasOneUse() &&
          LHSKnown.One == ~RHSC->getAPIntValue()) {
        SDValue Xor = TLO.DAG.getNode(ISD::XOR, dl, VT, Op0.getOperand(0), Op1);
        return TLO.CombineTo(Op, Xor);
      }
    }

    if (SimplifyDemandedBits(Op1, DemandedBits, DemandedElts, Known, TLO,
                             Depth + 1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    if (SimplifyDemandedBits(Op0, ~Known.Zero & DemandedBits, DemandedElts,
                             Known2, TLO, Depth + 1))
      return true;
    assert(!Known2.hasConflict() && "Bits known to be one AND zero?");

    // Attempt to avoid multi-use ops if we don't need anything from them.
    if (!DemandedBits.isAllOnesValue() || !DemandedElts.isAllOnesValue()) {
      SDValue DemandedOp0 = SimplifyMultipleUseDemandedBits(
          Op0, DemandedBits, DemandedElts, TLO.DAG, Depth + 1);
      SDValue DemandedOp1 = SimplifyMultipleUseDemandedBits(
          Op1, DemandedBits, DemandedElts, TLO.DAG, Depth + 1);
      if (DemandedOp0 || DemandedOp1) {
        Op0 = DemandedOp0 ? DemandedOp0 : Op0;
        Op1 = DemandedOp1 ? DemandedOp1 : Op1;
        SDValue NewOp = TLO.DAG.getNode(Op.getOpcode(), dl, VT, Op0, Op1);
        return TLO.CombineTo(Op, NewOp);
      }
    }

    // If all of the demanded bits are known one on one side, return the other.
    // These bits cannot contribute to the result of the 'and'.
    if (DemandedBits.isSubsetOf(Known2.Zero | Known.One))
      return TLO.CombineTo(Op, Op0);
    if (DemandedBits.isSubsetOf(Known.Zero | Known2.One))
      return TLO.CombineTo(Op, Op1);
    // If all of the demanded bits in the inputs are known zeros, return zero.
    if (DemandedBits.isSubsetOf(Known.Zero | Known2.Zero))
      return TLO.CombineTo(Op, TLO.DAG.getConstant(0, dl, VT));
    // If the RHS is a constant, see if we can simplify it.
    if (ShrinkDemandedConstant(Op, ~Known2.Zero & DemandedBits, TLO))
      return true;
    // If the operation can be done in a smaller type, do so.
    if (ShrinkDemandedOp(Op, BitWidth, DemandedBits, TLO))
      return true;

    // Output known-1 bits are only known if set in both the LHS & RHS.
    Known.One &= Known2.One;
    // Output known-0 are known to be clear if zero in either the LHS | RHS.
    Known.Zero |= Known2.Zero;
    break;
  }
  case ISD::OR: {
    SDValue Op0 = Op.getOperand(0);
    SDValue Op1 = Op.getOperand(1);

    if (SimplifyDemandedBits(Op1, DemandedBits, DemandedElts, Known, TLO,
                             Depth + 1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    if (SimplifyDemandedBits(Op0, ~Known.One & DemandedBits, DemandedElts,
                             Known2, TLO, Depth + 1))
      return true;
    assert(!Known2.hasConflict() && "Bits known to be one AND zero?");

    // Attempt to avoid multi-use ops if we don't need anything from them.
    if (!DemandedBits.isAllOnesValue() || !DemandedElts.isAllOnesValue()) {
      SDValue DemandedOp0 = SimplifyMultipleUseDemandedBits(
          Op0, DemandedBits, DemandedElts, TLO.DAG, Depth + 1);
      SDValue DemandedOp1 = SimplifyMultipleUseDemandedBits(
          Op1, DemandedBits, DemandedElts, TLO.DAG, Depth + 1);
      if (DemandedOp0 || DemandedOp1) {
        Op0 = DemandedOp0 ? DemandedOp0 : Op0;
        Op1 = DemandedOp1 ? DemandedOp1 : Op1;
        SDValue NewOp = TLO.DAG.getNode(Op.getOpcode(), dl, VT, Op0, Op1);
        return TLO.CombineTo(Op, NewOp);
      }
    }

    // If all of the demanded bits are known zero on one side, return the other.
    // These bits cannot contribute to the result of the 'or'.
    if (DemandedBits.isSubsetOf(Known2.One | Known.Zero))
      return TLO.CombineTo(Op, Op0);
    if (DemandedBits.isSubsetOf(Known.One | Known2.Zero))
      return TLO.CombineTo(Op, Op1);
    // If the RHS is a constant, see if we can simplify it.
    if (ShrinkDemandedConstant(Op, DemandedBits, TLO))
      return true;
    // If the operation can be done in a smaller type, do so.
    if (ShrinkDemandedOp(Op, BitWidth, DemandedBits, TLO))
      return true;

    // Output known-0 bits are only known if clear in both the LHS & RHS.
    Known.Zero &= Known2.Zero;
    // Output known-1 are known to be set if set in either the LHS | RHS.
    Known.One |= Known2.One;
    break;
  }
  case ISD::XOR: {
    SDValue Op0 = Op.getOperand(0);
    SDValue Op1 = Op.getOperand(1);

    if (SimplifyDemandedBits(Op1, DemandedBits, DemandedElts, Known, TLO,
                             Depth + 1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    if (SimplifyDemandedBits(Op0, DemandedBits, DemandedElts, Known2, TLO,
                             Depth + 1))
      return true;
    assert(!Known2.hasConflict() && "Bits known to be one AND zero?");

    // Attempt to avoid multi-use ops if we don't need anything from them.
    if (!DemandedBits.isAllOnesValue() || !DemandedElts.isAllOnesValue()) {
      SDValue DemandedOp0 = SimplifyMultipleUseDemandedBits(
          Op0, DemandedBits, DemandedElts, TLO.DAG, Depth + 1);
      SDValue DemandedOp1 = SimplifyMultipleUseDemandedBits(
          Op1, DemandedBits, DemandedElts, TLO.DAG, Depth + 1);
      if (DemandedOp0 || DemandedOp1) {
        Op0 = DemandedOp0 ? DemandedOp0 : Op0;
        Op1 = DemandedOp1 ? DemandedOp1 : Op1;
        SDValue NewOp = TLO.DAG.getNode(Op.getOpcode(), dl, VT, Op0, Op1);
        return TLO.CombineTo(Op, NewOp);
      }
    }

    // If all of the demanded bits are known zero on one side, return the other.
    // These bits cannot contribute to the result of the 'xor'.
    if (DemandedBits.isSubsetOf(Known.Zero))
      return TLO.CombineTo(Op, Op0);
    if (DemandedBits.isSubsetOf(Known2.Zero))
      return TLO.CombineTo(Op, Op1);
    // If the operation can be done in a smaller type, do so.
    if (ShrinkDemandedOp(Op, BitWidth, DemandedBits, TLO))
      return true;

    // If all of the unknown bits are known to be zero on one side or the other
    // (but not both) turn this into an *inclusive* or.
    //    e.g. (A & C1)^(B & C2) -> (A & C1)|(B & C2) iff C1&C2 == 0
    if (DemandedBits.isSubsetOf(Known.Zero | Known2.Zero))
      return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::OR, dl, VT, Op0, Op1));

    // Output known-0 bits are known if clear or set in both the LHS & RHS.
    KnownOut.Zero = (Known.Zero & Known2.Zero) | (Known.One & Known2.One);
    // Output known-1 are known to be set if set in only one of the LHS, RHS.
    KnownOut.One = (Known.Zero & Known2.One) | (Known.One & Known2.Zero);

    if (ConstantSDNode *C = isConstOrConstSplat(Op1)) {
      // If one side is a constant, and all of the known set bits on the other
      // side are also set in the constant, turn this into an AND, as we know
      // the bits will be cleared.
      //    e.g. (X | C1) ^ C2 --> (X | C1) & ~C2 iff (C1&C2) == C2
      // NB: it is okay if more bits are known than are requested
      if (C->getAPIntValue() == Known2.One) {
        SDValue ANDC =
            TLO.DAG.getConstant(~C->getAPIntValue() & DemandedBits, dl, VT);
        return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::AND, dl, VT, Op0, ANDC));
      }

      // If the RHS is a constant, see if we can change it. Don't alter a -1
      // constant because that's a 'not' op, and that is better for combining
      // and codegen.
      if (!C->isAllOnesValue()) {
        if (DemandedBits.isSubsetOf(C->getAPIntValue())) {
          // We're flipping all demanded bits. Flip the undemanded bits too.
          SDValue New = TLO.DAG.getNOT(dl, Op0, VT);
          return TLO.CombineTo(Op, New);
        }
        // If we can't turn this into a 'not', try to shrink the constant.
        if (ShrinkDemandedConstant(Op, DemandedBits, TLO))
          return true;
      }
    }

    Known = std::move(KnownOut);
    break;
  }
  case ISD::SELECT:
    if (SimplifyDemandedBits(Op.getOperand(2), DemandedBits, Known, TLO,
                             Depth + 1))
      return true;
    if (SimplifyDemandedBits(Op.getOperand(1), DemandedBits, Known2, TLO,
                             Depth + 1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    assert(!Known2.hasConflict() && "Bits known to be one AND zero?");

    // If the operands are constants, see if we can simplify them.
    if (ShrinkDemandedConstant(Op, DemandedBits, TLO))
      return true;

    // Only known if known in both the LHS and RHS.
    Known.One &= Known2.One;
    Known.Zero &= Known2.Zero;
    break;
  case ISD::SELECT_CC:
    if (SimplifyDemandedBits(Op.getOperand(3), DemandedBits, Known, TLO,
                             Depth + 1))
      return true;
    if (SimplifyDemandedBits(Op.getOperand(2), DemandedBits, Known2, TLO,
                             Depth + 1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    assert(!Known2.hasConflict() && "Bits known to be one AND zero?");

    // If the operands are constants, see if we can simplify them.
    if (ShrinkDemandedConstant(Op, DemandedBits, TLO))
      return true;

    // Only known if known in both the LHS and RHS.
    Known.One &= Known2.One;
    Known.Zero &= Known2.Zero;
    break;
  case ISD::SETCC: {
    SDValue Op0 = Op.getOperand(0);
    SDValue Op1 = Op.getOperand(1);
    ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(2))->get();
    // If (1) we only need the sign-bit, (2) the setcc operands are the same
    // width as the setcc result, and (3) the result of a setcc conforms to 0 or
    // -1, we may be able to bypass the setcc.
    if (DemandedBits.isSignMask() &&
        Op0.getScalarValueSizeInBits() == BitWidth &&
        getBooleanContents(VT) ==
            BooleanContent::ZeroOrNegativeOneBooleanContent) {
      // If we're testing X < 0, then this compare isn't needed - just use X!
      // FIXME: We're limiting to integer types here, but this should also work
      // if we don't care about FP signed-zero. The use of SETLT with FP means
      // that we don't care about NaNs.
      if (CC == ISD::SETLT && Op1.getValueType().isInteger() &&
          (isNullConstant(Op1) || ISD::isBuildVectorAllZeros(Op1.getNode())))
        return TLO.CombineTo(Op, Op0);

      // TODO: Should we check for other forms of sign-bit comparisons?
      // Examples: X <= -1, X >= 0
    }
    if (getBooleanContents(Op0.getValueType()) ==
            TargetLowering::ZeroOrOneBooleanContent &&
        BitWidth > 1)
      Known.Zero.setBitsFrom(1);
    break;
  }
  case ISD::SHL: {
    SDValue Op0 = Op.getOperand(0);
    SDValue Op1 = Op.getOperand(1);

    if (ConstantSDNode *SA = isConstOrConstSplat(Op1, DemandedElts)) {
      // If the shift count is an invalid immediate, don't do anything.
      if (SA->getAPIntValue().uge(BitWidth))
        break;

      unsigned ShAmt = SA->getZExtValue();
      if (ShAmt == 0)
        return TLO.CombineTo(Op, Op0);

      // If this is ((X >>u C1) << ShAmt), see if we can simplify this into a
      // single shift.  We can do this if the bottom bits (which are shifted
      // out) are never demanded.
      // TODO - support non-uniform vector amounts.
      if (Op0.getOpcode() == ISD::SRL) {
        if (!DemandedBits.intersects(APInt::getLowBitsSet(BitWidth, ShAmt))) {
          if (ConstantSDNode *SA2 =
                  isConstOrConstSplat(Op0.getOperand(1), DemandedElts)) {
            if (SA2->getAPIntValue().ult(BitWidth)) {
              unsigned C1 = SA2->getZExtValue();
              unsigned Opc = ISD::SHL;
              int Diff = ShAmt - C1;
              if (Diff < 0) {
                Diff = -Diff;
                Opc = ISD::SRL;
              }

              SDValue NewSA = TLO.DAG.getConstant(Diff, dl, Op1.getValueType());
              return TLO.CombineTo(
                  Op, TLO.DAG.getNode(Opc, dl, VT, Op0.getOperand(0), NewSA));
            }
          }
        }
      }

      if (SimplifyDemandedBits(Op0, DemandedBits.lshr(ShAmt), DemandedElts,
                               Known, TLO, Depth + 1))
        return true;

      // Try shrinking the operation as long as the shift amount will still be
      // in range.
      if ((ShAmt < DemandedBits.getActiveBits()) &&
          ShrinkDemandedOp(Op, BitWidth, DemandedBits, TLO))
        return true;

      // Convert (shl (anyext x, c)) to (anyext (shl x, c)) if the high bits
      // are not demanded. This will likely allow the anyext to be folded away.
      if (Op0.getOpcode() == ISD::ANY_EXTEND) {
        SDValue InnerOp = Op0.getOperand(0);
        EVT InnerVT = InnerOp.getValueType();
        unsigned InnerBits = InnerVT.getScalarSizeInBits();
        if (ShAmt < InnerBits && DemandedBits.getActiveBits() <= InnerBits &&
            isTypeDesirableForOp(ISD::SHL, InnerVT)) {
          EVT ShTy = getShiftAmountTy(InnerVT, DL);
          if (!APInt(BitWidth, ShAmt).isIntN(ShTy.getSizeInBits()))
            ShTy = InnerVT;
          SDValue NarrowShl =
              TLO.DAG.getNode(ISD::SHL, dl, InnerVT, InnerOp,
                              TLO.DAG.getConstant(ShAmt, dl, ShTy));
          return TLO.CombineTo(
              Op, TLO.DAG.getNode(ISD::ANY_EXTEND, dl, VT, NarrowShl));
        }
        // Repeat the SHL optimization above in cases where an extension
        // intervenes: (shl (anyext (shr x, c1)), c2) to
        // (shl (anyext x), c2-c1).  This requires that the bottom c1 bits
        // aren't demanded (as above) and that the shifted upper c1 bits of
        // x aren't demanded.
        if (Op0.hasOneUse() && InnerOp.getOpcode() == ISD::SRL &&
            InnerOp.hasOneUse()) {
          if (ConstantSDNode *SA2 =
                  isConstOrConstSplat(InnerOp.getOperand(1))) {
            unsigned InnerShAmt = SA2->getLimitedValue(InnerBits);
            if (InnerShAmt < ShAmt && InnerShAmt < InnerBits &&
                DemandedBits.getActiveBits() <=
                    (InnerBits - InnerShAmt + ShAmt) &&
                DemandedBits.countTrailingZeros() >= ShAmt) {
              SDValue NewSA = TLO.DAG.getConstant(ShAmt - InnerShAmt, dl,
                                                  Op1.getValueType());
              SDValue NewExt = TLO.DAG.getNode(ISD::ANY_EXTEND, dl, VT,
                                               InnerOp.getOperand(0));
              return TLO.CombineTo(
                  Op, TLO.DAG.getNode(ISD::SHL, dl, VT, NewExt, NewSA));
            }
          }
        }
      }

      Known.Zero <<= ShAmt;
      Known.One <<= ShAmt;
      // low bits known zero.
      Known.Zero.setLowBits(ShAmt);
    }
    break;
  }
  case ISD::SRL: {
    SDValue Op0 = Op.getOperand(0);
    SDValue Op1 = Op.getOperand(1);

    if (ConstantSDNode *SA = isConstOrConstSplat(Op1, DemandedElts)) {
      // If the shift count is an invalid immediate, don't do anything.
      if (SA->getAPIntValue().uge(BitWidth))
        break;

      unsigned ShAmt = SA->getZExtValue();
      if (ShAmt == 0)
        return TLO.CombineTo(Op, Op0);

      EVT ShiftVT = Op1.getValueType();
      APInt InDemandedMask = (DemandedBits << ShAmt);

      // If the shift is exact, then it does demand the low bits (and knows that
      // they are zero).
      if (Op->getFlags().hasExact())
        InDemandedMask.setLowBits(ShAmt);

      // If this is ((X << C1) >>u ShAmt), see if we can simplify this into a
      // single shift.  We can do this if the top bits (which are shifted out)
      // are never demanded.
      // TODO - support non-uniform vector amounts.
      if (Op0.getOpcode() == ISD::SHL) {
        if (ConstantSDNode *SA2 =
                isConstOrConstSplat(Op0.getOperand(1), DemandedElts)) {
          if (!DemandedBits.intersects(
                  APInt::getHighBitsSet(BitWidth, ShAmt))) {
            if (SA2->getAPIntValue().ult(BitWidth)) {
              unsigned C1 = SA2->getZExtValue();
              unsigned Opc = ISD::SRL;
              int Diff = ShAmt - C1;
              if (Diff < 0) {
                Diff = -Diff;
                Opc = ISD::SHL;
              }

              SDValue NewSA = TLO.DAG.getConstant(Diff, dl, ShiftVT);
              return TLO.CombineTo(
                  Op, TLO.DAG.getNode(Opc, dl, VT, Op0.getOperand(0), NewSA));
            }
          }
        }
      }

      // Compute the new bits that are at the top now.
      if (SimplifyDemandedBits(Op0, InDemandedMask, DemandedElts, Known, TLO,
                               Depth + 1))
        return true;
      assert(!Known.hasConflict() && "Bits known to be one AND zero?");
      Known.Zero.lshrInPlace(ShAmt);
      Known.One.lshrInPlace(ShAmt);

      Known.Zero.setHighBits(ShAmt); // High bits known zero.
    }
    break;
  }
  case ISD::SRA: {
    SDValue Op0 = Op.getOperand(0);
    SDValue Op1 = Op.getOperand(1);

    // If this is an arithmetic shift right and only the low-bit is set, we can
    // always convert this into a logical shr, even if the shift amount is
    // variable.  The low bit of the shift cannot be an input sign bit unless
    // the shift amount is >= the size of the datatype, which is undefined.
    if (DemandedBits.isOneValue())
      return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::SRL, dl, VT, Op0, Op1));

    if (ConstantSDNode *SA = isConstOrConstSplat(Op1, DemandedElts)) {
      // If the shift count is an invalid immediate, don't do anything.
      if (SA->getAPIntValue().uge(BitWidth))
        break;

      unsigned ShAmt = SA->getZExtValue();
      if (ShAmt == 0)
        return TLO.CombineTo(Op, Op0);

      APInt InDemandedMask = (DemandedBits << ShAmt);

      // If the shift is exact, then it does demand the low bits (and knows that
      // they are zero).
      if (Op->getFlags().hasExact())
        InDemandedMask.setLowBits(ShAmt);

      // If any of the demanded bits are produced by the sign extension, we also
      // demand the input sign bit.
      if (DemandedBits.countLeadingZeros() < ShAmt)
        InDemandedMask.setSignBit();

      if (SimplifyDemandedBits(Op0, InDemandedMask, DemandedElts, Known, TLO,
                               Depth + 1))
        return true;
      assert(!Known.hasConflict() && "Bits known to be one AND zero?");
      Known.Zero.lshrInPlace(ShAmt);
      Known.One.lshrInPlace(ShAmt);

      // If the input sign bit is known to be zero, or if none of the top bits
      // are demanded, turn this into an unsigned shift right.
      if (Known.Zero[BitWidth - ShAmt - 1] ||
          DemandedBits.countLeadingZeros() >= ShAmt) {
        SDNodeFlags Flags;
        Flags.setExact(Op->getFlags().hasExact());
        return TLO.CombineTo(
            Op, TLO.DAG.getNode(ISD::SRL, dl, VT, Op0, Op1, Flags));
      }

      int Log2 = DemandedBits.exactLogBase2();
      if (Log2 >= 0) {
        // The bit must come from the sign.
        SDValue NewSA =
            TLO.DAG.getConstant(BitWidth - 1 - Log2, dl, Op1.getValueType());
        return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::SRL, dl, VT, Op0, NewSA));
      }

      if (Known.One[BitWidth - ShAmt - 1])
        // New bits are known one.
        Known.One.setHighBits(ShAmt);
    }
    break;
  }
  case ISD::FSHL:
  case ISD::FSHR: {
    SDValue Op0 = Op.getOperand(0);
    SDValue Op1 = Op.getOperand(1);
    SDValue Op2 = Op.getOperand(2);
    bool IsFSHL = (Op.getOpcode() == ISD::FSHL);

    if (ConstantSDNode *SA = isConstOrConstSplat(Op2, DemandedElts)) {
      unsigned Amt = SA->getAPIntValue().urem(BitWidth);

      // For fshl, 0-shift returns the 1st arg.
      // For fshr, 0-shift returns the 2nd arg.
      if (Amt == 0) {
        if (SimplifyDemandedBits(IsFSHL ? Op0 : Op1, DemandedBits, DemandedElts,
                                 Known, TLO, Depth + 1))
          return true;
        break;
      }

      // fshl: (Op0 << Amt) | (Op1 >> (BW - Amt))
      // fshr: (Op0 << (BW - Amt)) | (Op1 >> Amt)
      APInt Demanded0 = DemandedBits.lshr(IsFSHL ? Amt : (BitWidth - Amt));
      APInt Demanded1 = DemandedBits << (IsFSHL ? (BitWidth - Amt) : Amt);
      if (SimplifyDemandedBits(Op0, Demanded0, DemandedElts, Known2, TLO,
                               Depth + 1))
        return true;
      if (SimplifyDemandedBits(Op1, Demanded1, DemandedElts, Known, TLO,
                               Depth + 1))
        return true;

      Known2.One <<= (IsFSHL ? Amt : (BitWidth - Amt));
      Known2.Zero <<= (IsFSHL ? Amt : (BitWidth - Amt));
      Known.One.lshrInPlace(IsFSHL ? (BitWidth - Amt) : Amt);
      Known.Zero.lshrInPlace(IsFSHL ? (BitWidth - Amt) : Amt);
      Known.One |= Known2.One;
      Known.Zero |= Known2.Zero;
    }
    break;
  }
  case ISD::BITREVERSE: {
    SDValue Src = Op.getOperand(0);
    APInt DemandedSrcBits = DemandedBits.reverseBits();
    if (SimplifyDemandedBits(Src, DemandedSrcBits, DemandedElts, Known2, TLO,
                             Depth + 1))
      return true;
    Known.One = Known2.One.reverseBits();
    Known.Zero = Known2.Zero.reverseBits();
    break;
  }
  case ISD::SIGN_EXTEND_INREG: {
    SDValue Op0 = Op.getOperand(0);
    EVT ExVT = cast<VTSDNode>(Op.getOperand(1))->getVT();
    unsigned ExVTBits = ExVT.getScalarSizeInBits();

    // If we only care about the highest bit, don't bother shifting right.
    if (DemandedBits.isSignMask()) {
      unsigned NumSignBits = TLO.DAG.ComputeNumSignBits(Op0);
      bool AlreadySignExtended = NumSignBits >= BitWidth - ExVTBits + 1;
      // However if the input is already sign extended we expect the sign
      // extension to be dropped altogether later and do not simplify.
      if (!AlreadySignExtended) {
        // Compute the correct shift amount type, which must be getShiftAmountTy
        // for scalar types after legalization.
        EVT ShiftAmtTy = VT;
        if (TLO.LegalTypes() && !ShiftAmtTy.isVector())
          ShiftAmtTy = getShiftAmountTy(ShiftAmtTy, DL);

        SDValue ShiftAmt =
            TLO.DAG.getConstant(BitWidth - ExVTBits, dl, ShiftAmtTy);
        return TLO.CombineTo(Op,
                             TLO.DAG.getNode(ISD::SHL, dl, VT, Op0, ShiftAmt));
      }
    }

    // If none of the extended bits are demanded, eliminate the sextinreg.
    if (DemandedBits.getActiveBits() <= ExVTBits)
      return TLO.CombineTo(Op, Op0);

    APInt InputDemandedBits = DemandedBits.getLoBits(ExVTBits);

    // Since the sign extended bits are demanded, we know that the sign
    // bit is demanded.
    InputDemandedBits.setBit(ExVTBits - 1);

    if (SimplifyDemandedBits(Op0, InputDemandedBits, Known, TLO, Depth + 1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");

    // If the sign bit of the input is known set or clear, then we know the
    // top bits of the result.

    // If the input sign bit is known zero, convert this into a zero extension.
    if (Known.Zero[ExVTBits - 1])
      return TLO.CombineTo(
          Op, TLO.DAG.getZeroExtendInReg(Op0, dl, ExVT.getScalarType()));

    APInt Mask = APInt::getLowBitsSet(BitWidth, ExVTBits);
    if (Known.One[ExVTBits - 1]) { // Input sign bit known set
      Known.One.setBitsFrom(ExVTBits);
      Known.Zero &= Mask;
    } else { // Input sign bit unknown
      Known.Zero &= Mask;
      Known.One &= Mask;
    }
    break;
  }
  case ISD::BUILD_PAIR: {
    EVT HalfVT = Op.getOperand(0).getValueType();
    unsigned HalfBitWidth = HalfVT.getScalarSizeInBits();

    APInt MaskLo = DemandedBits.getLoBits(HalfBitWidth).trunc(HalfBitWidth);
    APInt MaskHi = DemandedBits.getHiBits(HalfBitWidth).trunc(HalfBitWidth);

    KnownBits KnownLo, KnownHi;

    if (SimplifyDemandedBits(Op.getOperand(0), MaskLo, KnownLo, TLO, Depth + 1))
      return true;

    if (SimplifyDemandedBits(Op.getOperand(1), MaskHi, KnownHi, TLO, Depth + 1))
      return true;

    Known.Zero = KnownLo.Zero.zext(BitWidth) |
                 KnownHi.Zero.zext(BitWidth).shl(HalfBitWidth);

    Known.One = KnownLo.One.zext(BitWidth) |
                KnownHi.One.zext(BitWidth).shl(HalfBitWidth);
    break;
  }
  case ISD::ZERO_EXTEND:
  case ISD::ZERO_EXTEND_VECTOR_INREG: {
    SDValue Src = Op.getOperand(0);
    EVT SrcVT = Src.getValueType();
    unsigned InBits = SrcVT.getScalarSizeInBits();
    unsigned InElts = SrcVT.isVector() ? SrcVT.getVectorNumElements() : 1;
    bool IsVecInReg = Op.getOpcode() == ISD::ZERO_EXTEND_VECTOR_INREG;

    // If none of the top bits are demanded, convert this into an any_extend.
    if (DemandedBits.getActiveBits() <= InBits) {
      // If we only need the non-extended bits of the bottom element
      // then we can just bitcast to the result.
      if (IsVecInReg && DemandedElts == 1 &&
          VT.getSizeInBits() == SrcVT.getSizeInBits() &&
          TLO.DAG.getDataLayout().isLittleEndian())
        return TLO.CombineTo(Op, TLO.DAG.getBitcast(VT, Src));

      unsigned Opc =
          IsVecInReg ? ISD::ANY_EXTEND_VECTOR_INREG : ISD::ANY_EXTEND;
      if (!TLO.LegalOperations() || isOperationLegal(Opc, VT))
        return TLO.CombineTo(Op, TLO.DAG.getNode(Opc, dl, VT, Src));
    }

    APInt InDemandedBits = DemandedBits.trunc(InBits);
    APInt InDemandedElts = DemandedElts.zextOrSelf(InElts);
    if (SimplifyDemandedBits(Src, InDemandedBits, InDemandedElts, Known, TLO,
                             Depth + 1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    assert(Known.getBitWidth() == InBits && "Src width has changed?");
    Known = Known.zext(BitWidth, true /* ExtendedBitsAreKnownZero */);
    break;
  }
  case ISD::SIGN_EXTEND:
  case ISD::SIGN_EXTEND_VECTOR_INREG: {
    SDValue Src = Op.getOperand(0);
    EVT SrcVT = Src.getValueType();
    unsigned InBits = SrcVT.getScalarSizeInBits();
    unsigned InElts = SrcVT.isVector() ? SrcVT.getVectorNumElements() : 1;
    bool IsVecInReg = Op.getOpcode() == ISD::SIGN_EXTEND_VECTOR_INREG;

    // If none of the top bits are demanded, convert this into an any_extend.
    if (DemandedBits.getActiveBits() <= InBits) {
      // If we only need the non-extended bits of the bottom element
      // then we can just bitcast to the result.
      if (IsVecInReg && DemandedElts == 1 &&
          VT.getSizeInBits() == SrcVT.getSizeInBits() &&
          TLO.DAG.getDataLayout().isLittleEndian())
        return TLO.CombineTo(Op, TLO.DAG.getBitcast(VT, Src));

      unsigned Opc =
          IsVecInReg ? ISD::ANY_EXTEND_VECTOR_INREG : ISD::ANY_EXTEND;
      if (!TLO.LegalOperations() || isOperationLegal(Opc, VT))
        return TLO.CombineTo(Op, TLO.DAG.getNode(Opc, dl, VT, Src));
    }

    APInt InDemandedBits = DemandedBits.trunc(InBits);
    APInt InDemandedElts = DemandedElts.zextOrSelf(InElts);

    // Since some of the sign extended bits are demanded, we know that the sign
    // bit is demanded.
    InDemandedBits.setBit(InBits - 1);

    if (SimplifyDemandedBits(Src, InDemandedBits, InDemandedElts, Known, TLO,
                             Depth + 1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    assert(Known.getBitWidth() == InBits && "Src width has changed?");

    // If the sign bit is known one, the top bits match.
    Known = Known.sext(BitWidth);

    // If the sign bit is known zero, convert this to a zero extend.
    if (Known.isNonNegative()) {
      unsigned Opc =
          IsVecInReg ? ISD::ZERO_EXTEND_VECTOR_INREG : ISD::ZERO_EXTEND;
      if (!TLO.LegalOperations() || isOperationLegal(Opc, VT))
        return TLO.CombineTo(Op, TLO.DAG.getNode(Opc, dl, VT, Src));
    }
    break;
  }
  case ISD::ANY_EXTEND:
  case ISD::ANY_EXTEND_VECTOR_INREG: {
    SDValue Src = Op.getOperand(0);
    EVT SrcVT = Src.getValueType();
    unsigned InBits = SrcVT.getScalarSizeInBits();
    unsigned InElts = SrcVT.isVector() ? SrcVT.getVectorNumElements() : 1;
    bool IsVecInReg = Op.getOpcode() == ISD::ANY_EXTEND_VECTOR_INREG;

    // If we only need the bottom element then we can just bitcast.
    // TODO: Handle ANY_EXTEND?
    if (IsVecInReg && DemandedElts == 1 &&
        VT.getSizeInBits() == SrcVT.getSizeInBits() &&
        TLO.DAG.getDataLayout().isLittleEndian())
      return TLO.CombineTo(Op, TLO.DAG.getBitcast(VT, Src));

    APInt InDemandedBits = DemandedBits.trunc(InBits);
    APInt InDemandedElts = DemandedElts.zextOrSelf(InElts);
    if (SimplifyDemandedBits(Src, InDemandedBits, InDemandedElts, Known, TLO,
                             Depth + 1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    assert(Known.getBitWidth() == InBits && "Src width has changed?");
    Known = Known.zext(BitWidth, false /* => any extend */);
    break;
  }
  case ISD::TRUNCATE: {
    SDValue Src = Op.getOperand(0);

    // Simplify the input, using demanded bit information, and compute the known
    // zero/one bits live out.
    unsigned OperandBitWidth = Src.getScalarValueSizeInBits();
    APInt TruncMask = DemandedBits.zext(OperandBitWidth);
    if (SimplifyDemandedBits(Src, TruncMask, Known, TLO, Depth + 1))
      return true;
    Known = Known.trunc(BitWidth);

    // Attempt to avoid multi-use ops if we don't need anything from them.
    if (SDValue NewSrc = SimplifyMultipleUseDemandedBits(
            Src, TruncMask, DemandedElts, TLO.DAG, Depth + 1))
      return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::TRUNCATE, dl, VT, NewSrc));

    // If the input is only used by this truncate, see if we can shrink it based
    // on the known demanded bits.
    if (Src.getNode()->hasOneUse()) {
      switch (Src.getOpcode()) {
      default:
        break;
      case ISD::SRL:
        // Shrink SRL by a constant if none of the high bits shifted in are
        // demanded.
        if (TLO.LegalTypes() && !isTypeDesirableForOp(ISD::SRL, VT))
          // Do not turn (vt1 truncate (vt2 srl)) into (vt1 srl) if vt1 is
          // undesirable.
          break;

        auto *ShAmt = dyn_cast<ConstantSDNode>(Src.getOperand(1));
        if (!ShAmt || ShAmt->getAPIntValue().uge(BitWidth))
          break;

        SDValue Shift = Src.getOperand(1);
        uint64_t ShVal = ShAmt->getZExtValue();

        if (TLO.LegalTypes())
          Shift = TLO.DAG.getConstant(ShVal, dl, getShiftAmountTy(VT, DL));

        APInt HighBits =
            APInt::getHighBitsSet(OperandBitWidth, OperandBitWidth - BitWidth);
        HighBits.lshrInPlace(ShVal);
        HighBits = HighBits.trunc(BitWidth);

        if (!(HighBits & DemandedBits)) {
          // None of the shifted in bits are needed.  Add a truncate of the
          // shift input, then shift it.
          SDValue NewTrunc =
              TLO.DAG.getNode(ISD::TRUNCATE, dl, VT, Src.getOperand(0));
          return TLO.CombineTo(
              Op, TLO.DAG.getNode(ISD::SRL, dl, VT, NewTrunc, Shift));
        }
        break;
      }
    }

    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    break;
  }
  case ISD::AssertZext: {
    // AssertZext demands all of the high bits, plus any of the low bits
    // demanded by its users.
    EVT ZVT = cast<VTSDNode>(Op.getOperand(1))->getVT();
    APInt InMask = APInt::getLowBitsSet(BitWidth, ZVT.getSizeInBits());
    if (SimplifyDemandedBits(Op.getOperand(0), ~InMask | DemandedBits, Known,
                             TLO, Depth + 1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");

    Known.Zero |= ~InMask;
    break;
  }
  case ISD::EXTRACT_VECTOR_ELT: {
    SDValue Src = Op.getOperand(0);
    SDValue Idx = Op.getOperand(1);
    unsigned NumSrcElts = Src.getValueType().getVectorNumElements();
    unsigned EltBitWidth = Src.getScalarValueSizeInBits();

    // Demand the bits from every vector element without a constant index.
    APInt DemandedSrcElts = APInt::getAllOnesValue(NumSrcElts);
    if (auto *CIdx = dyn_cast<ConstantSDNode>(Idx))
      if (CIdx->getAPIntValue().ult(NumSrcElts))
        DemandedSrcElts = APInt::getOneBitSet(NumSrcElts, CIdx->getZExtValue());

    // If BitWidth > EltBitWidth the value is anyext:ed. So we do not know
    // anything about the extended bits.
    APInt DemandedSrcBits = DemandedBits;
    if (BitWidth > EltBitWidth)
      DemandedSrcBits = DemandedSrcBits.trunc(EltBitWidth);

    if (SimplifyDemandedBits(Src, DemandedSrcBits, DemandedSrcElts, Known2, TLO,
                             Depth + 1))
      return true;

    Known = Known2;
    if (BitWidth > EltBitWidth)
      Known = Known.zext(BitWidth, false /* => any extend */);
    break;
  }
  case ISD::BITCAST: {
    SDValue Src = Op.getOperand(0);
    EVT SrcVT = Src.getValueType();
    unsigned NumSrcEltBits = SrcVT.getScalarSizeInBits();

    // If this is an FP->Int bitcast and if the sign bit is the only
    // thing demanded, turn this into a FGETSIGN.
    if (!TLO.LegalOperations() && !VT.isVector() && !SrcVT.isVector() &&
        DemandedBits == APInt::getSignMask(Op.getValueSizeInBits()) &&
        SrcVT.isFloatingPoint()) {
      bool OpVTLegal = isOperationLegalOrCustom(ISD::FGETSIGN, VT);
      bool i32Legal = isOperationLegalOrCustom(ISD::FGETSIGN, MVT::i32);
      if ((OpVTLegal || i32Legal) && VT.isSimple() && SrcVT != MVT::f16 &&
          SrcVT != MVT::f128) {
        // Cannot eliminate/lower SHL for f128 yet.
        EVT Ty = OpVTLegal ? VT : MVT::i32;
        // Make a FGETSIGN + SHL to move the sign bit into the appropriate
        // place.  We expect the SHL to be eliminated by other optimizations.
        SDValue Sign = TLO.DAG.getNode(ISD::FGETSIGN, dl, Ty, Src);
        unsigned OpVTSizeInBits = Op.getValueSizeInBits();
        if (!OpVTLegal && OpVTSizeInBits > 32)
          Sign = TLO.DAG.getNode(ISD::ZERO_EXTEND, dl, VT, Sign);
        unsigned ShVal = Op.getValueSizeInBits() - 1;
        SDValue ShAmt = TLO.DAG.getConstant(ShVal, dl, VT);
        return TLO.CombineTo(Op,
                             TLO.DAG.getNode(ISD::SHL, dl, VT, Sign, ShAmt));
      }
    }

    // Bitcast from a vector using SimplifyDemanded Bits/VectorElts.
    // Demand the elt/bit if any of the original elts/bits are demanded.
    // TODO - bigendian once we have test coverage.
    if (SrcVT.isVector() && (BitWidth % NumSrcEltBits) == 0 &&
        TLO.DAG.getDataLayout().isLittleEndian()) {
      unsigned Scale = BitWidth / NumSrcEltBits;
      unsigned NumSrcElts = SrcVT.getVectorNumElements();
      APInt DemandedSrcBits = APInt::getNullValue(NumSrcEltBits);
      APInt DemandedSrcElts = APInt::getNullValue(NumSrcElts);
      for (unsigned i = 0; i != Scale; ++i) {
        unsigned Offset = i * NumSrcEltBits;
        APInt Sub = DemandedBits.extractBits(NumSrcEltBits, Offset);
        if (!Sub.isNullValue()) {
          DemandedSrcBits |= Sub;
          for (unsigned j = 0; j != NumElts; ++j)
            if (DemandedElts[j])
              DemandedSrcElts.setBit((j * Scale) + i);
        }
      }

      APInt KnownSrcUndef, KnownSrcZero;
      if (SimplifyDemandedVectorElts(Src, DemandedSrcElts, KnownSrcUndef,
                                     KnownSrcZero, TLO, Depth + 1))
        return true;

      KnownBits KnownSrcBits;
      if (SimplifyDemandedBits(Src, DemandedSrcBits, DemandedSrcElts,
                               KnownSrcBits, TLO, Depth + 1))
        return true;
    } else if ((NumSrcEltBits % BitWidth) == 0 &&
               TLO.DAG.getDataLayout().isLittleEndian()) {
      unsigned Scale = NumSrcEltBits / BitWidth;
      unsigned NumSrcElts = SrcVT.isVector() ? SrcVT.getVectorNumElements() : 1;
      APInt DemandedSrcBits = APInt::getNullValue(NumSrcEltBits);
      APInt DemandedSrcElts = APInt::getNullValue(NumSrcElts);
      for (unsigned i = 0; i != NumElts; ++i)
        if (DemandedElts[i]) {
          unsigned Offset = (i % Scale) * BitWidth;
          DemandedSrcBits.insertBits(DemandedBits, Offset);
          DemandedSrcElts.setBit(i / Scale);
        }

      if (SrcVT.isVector()) {
        APInt KnownSrcUndef, KnownSrcZero;
        if (SimplifyDemandedVectorElts(Src, DemandedSrcElts, KnownSrcUndef,
                                       KnownSrcZero, TLO, Depth + 1))
          return true;
      }

      KnownBits KnownSrcBits;
      if (SimplifyDemandedBits(Src, DemandedSrcBits, DemandedSrcElts,
                               KnownSrcBits, TLO, Depth + 1))
        return true;
    }

    // If this is a bitcast, let computeKnownBits handle it.  Only do this on a
    // recursive call where Known may be useful to the caller.
    if (Depth > 0) {
      Known = TLO.DAG.computeKnownBits(Op, DemandedElts, Depth);
      return false;
    }
    break;
  }
  case ISD::ADD:
  case ISD::MUL:
  case ISD::SUB: {
    // Add, Sub, and Mul don't demand any bits in positions beyond that
    // of the highest bit demanded of them.
    SDValue Op0 = Op.getOperand(0), Op1 = Op.getOperand(1);
    SDNodeFlags Flags = Op.getNode()->getFlags();
    unsigned DemandedBitsLZ = DemandedBits.countLeadingZeros();
    APInt LoMask = APInt::getLowBitsSet(BitWidth, BitWidth - DemandedBitsLZ);
    if (SimplifyDemandedBits(Op0, LoMask, DemandedElts, Known2, TLO,
                             Depth + 1) ||
        SimplifyDemandedBits(Op1, LoMask, DemandedElts, Known2, TLO,
                             Depth + 1) ||
        // See if the operation should be performed at a smaller bit width.
        ShrinkDemandedOp(Op, BitWidth, DemandedBits, TLO)) {
      if (Flags.hasNoSignedWrap() || Flags.hasNoUnsignedWrap()) {
        // Disable the nsw and nuw flags. We can no longer guarantee that we
        // won't wrap after simplification.
        Flags.setNoSignedWrap(false);
        Flags.setNoUnsignedWrap(false);
        SDValue NewOp =
            TLO.DAG.getNode(Op.getOpcode(), dl, VT, Op0, Op1, Flags);
        return TLO.CombineTo(Op, NewOp);
      }
      return true;
    }

    // Attempt to avoid multi-use ops if we don't need anything from them.
    if (!LoMask.isAllOnesValue() || !DemandedElts.isAllOnesValue()) {
      SDValue DemandedOp0 = SimplifyMultipleUseDemandedBits(
          Op0, LoMask, DemandedElts, TLO.DAG, Depth + 1);
      SDValue DemandedOp1 = SimplifyMultipleUseDemandedBits(
          Op1, LoMask, DemandedElts, TLO.DAG, Depth + 1);
      if (DemandedOp0 || DemandedOp1) {
        Flags.setNoSignedWrap(false);
        Flags.setNoUnsignedWrap(false);
        Op0 = DemandedOp0 ? DemandedOp0 : Op0;
        Op1 = DemandedOp1 ? DemandedOp1 : Op1;
        SDValue NewOp =
            TLO.DAG.getNode(Op.getOpcode(), dl, VT, Op0, Op1, Flags);
        return TLO.CombineTo(Op, NewOp);
      }
    }

    // If we have a constant operand, we may be able to turn it into -1 if we
    // do not demand the high bits. This can make the constant smaller to
    // encode, allow more general folding, or match specialized instruction
    // patterns (eg, 'blsr' on x86). Don't bother changing 1 to -1 because that
    // is probably not useful (and could be detrimental).
    ConstantSDNode *C = isConstOrConstSplat(Op1);
    APInt HighMask = APInt::getHighBitsSet(BitWidth, DemandedBitsLZ);
    if (C && !C->isAllOnesValue() && !C->isOne() &&
        (C->getAPIntValue() | HighMask).isAllOnesValue()) {
      SDValue Neg1 = TLO.DAG.getAllOnesConstant(dl, VT);
      // Disable the nsw and nuw flags. We can no longer guarantee that we
      // won't wrap after simplification.
      Flags.setNoSignedWrap(false);
      Flags.setNoUnsignedWrap(false);
      SDValue NewOp = TLO.DAG.getNode(Op.getOpcode(), dl, VT, Op0, Neg1, Flags);
      return TLO.CombineTo(Op, NewOp);
    }

    LLVM_FALLTHROUGH;
  }
  default:
    if (Op.getOpcode() >= ISD::BUILTIN_OP_END) {
      if (SimplifyDemandedBitsForTargetNode(Op, DemandedBits, DemandedElts,
                                            Known, TLO, Depth))
        return true;
      break;
    }

    // Just use computeKnownBits to compute output bits.
    Known = TLO.DAG.computeKnownBits(Op, DemandedElts, Depth);
    break;
  }

  // If we know the value of all of the demanded bits, return this as a
  // constant.
  if (DemandedBits.isSubsetOf(Known.Zero | Known.One)) {
    // Avoid folding to a constant if any OpaqueConstant is involved.
    const SDNode *N = Op.getNode();
    for (SDNodeIterator I = SDNodeIterator::begin(N),
                        E = SDNodeIterator::end(N);
         I != E; ++I) {
      SDNode *Op = *I;
      if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op))
        if (C->isOpaque())
          return false;
    }
    // TODO: Handle float bits as well.
    if (VT.isInteger())
      return TLO.CombineTo(Op, TLO.DAG.getConstant(Known.One, dl, VT));
  }

  return false;
}

bool TargetLowering::SimplifyDemandedVectorElts(SDValue Op,
                                                const APInt &DemandedElts,
                                                APInt &KnownUndef,
                                                APInt &KnownZero,
                                                DAGCombinerInfo &DCI) const {
  SelectionDAG &DAG = DCI.DAG;
  TargetLoweringOpt TLO(DAG, !DCI.isBeforeLegalize(),
                        !DCI.isBeforeLegalizeOps());

  bool Simplified =
      SimplifyDemandedVectorElts(Op, DemandedElts, KnownUndef, KnownZero, TLO);
  if (Simplified) {
    DCI.AddToWorklist(Op.getNode());
    DCI.CommitTargetLoweringOpt(TLO);
  }

  return Simplified;
}

/// Given a vector binary operation and known undefined elements for each input
/// operand, compute whether each element of the output is undefined.
static APInt getKnownUndefForVectorBinop(SDValue BO, SelectionDAG &DAG,
                                         const APInt &UndefOp0,
                                         const APInt &UndefOp1) {
  EVT VT = BO.getValueType();
  assert(DAG.getTargetLoweringInfo().isBinOp(BO.getOpcode()) && VT.isVector() &&
         "Vector binop only");

  EVT EltVT = VT.getVectorElementType();
  unsigned NumElts = VT.getVectorNumElements();
  assert(UndefOp0.getBitWidth() == NumElts &&
         UndefOp1.getBitWidth() == NumElts && "Bad type for undef analysis");

  auto getUndefOrConstantElt = [&](SDValue V, unsigned Index,
                                   const APInt &UndefVals) {
    if (UndefVals[Index])
      return DAG.getUNDEF(EltVT);

    if (auto *BV = dyn_cast<BuildVectorSDNode>(V)) {
      // Try hard to make sure that the getNode() call is not creating temporary
      // nodes. Ignore opaque integers because they do not constant fold.
      SDValue Elt = BV->getOperand(Index);
      auto *C = dyn_cast<ConstantSDNode>(Elt);
      if (isa<ConstantFPSDNode>(Elt) || Elt.isUndef() || (C && !C->isOpaque()))
        return Elt;
    }

    return SDValue();
  };

  APInt KnownUndef = APInt::getNullValue(NumElts);
  for (unsigned i = 0; i != NumElts; ++i) {
    // If both inputs for this element are either constant or undef and match
    // the element type, compute the constant/undef result for this element of
    // the vector.
    // TODO: Ideally we would use FoldConstantArithmetic() here, but that does
    // not handle FP constants. The code within getNode() should be refactored
    // to avoid the danger of creating a bogus temporary node here.
    SDValue C0 = getUndefOrConstantElt(BO.getOperand(0), i, UndefOp0);
    SDValue C1 = getUndefOrConstantElt(BO.getOperand(1), i, UndefOp1);
    if (C0 && C1 && C0.getValueType() == EltVT && C1.getValueType() == EltVT)
      if (DAG.getNode(BO.getOpcode(), SDLoc(BO), EltVT, C0, C1).isUndef())
        KnownUndef.setBit(i);
  }
  return KnownUndef;
}

bool TargetLowering::SimplifyDemandedVectorElts(
    SDValue Op, const APInt &OriginalDemandedElts, APInt &KnownUndef,
    APInt &KnownZero, TargetLoweringOpt &TLO, unsigned Depth,
    bool AssumeSingleUse) const {
  EVT VT = Op.getValueType();
  APInt DemandedElts = OriginalDemandedElts;
  unsigned NumElts = DemandedElts.getBitWidth();
  assert(VT.isVector() && "Expected vector op");
  assert(VT.getVectorNumElements() == NumElts &&
         "Mask size mismatches value type element count!");

  KnownUndef = KnownZero = APInt::getNullValue(NumElts);

  // Undef operand.
  if (Op.isUndef()) {
    KnownUndef.setAllBits();
    return false;
  }

  // If Op has other users, assume that all elements are needed.
  if (!Op.getNode()->hasOneUse() && !AssumeSingleUse)
    DemandedElts.setAllBits();

  // Not demanding any elements from Op.
  if (DemandedElts == 0) {
    KnownUndef.setAllBits();
    return TLO.CombineTo(Op, TLO.DAG.getUNDEF(VT));
  }

  // Limit search depth.
  if (Depth >= SelectionDAG::MaxRecursionDepth)
    return false;

  SDLoc DL(Op);
  unsigned EltSizeInBits = VT.getScalarSizeInBits();

  switch (Op.getOpcode()) {
  case ISD::SCALAR_TO_VECTOR: {
    if (!DemandedElts[0]) {
      KnownUndef.setAllBits();
      return TLO.CombineTo(Op, TLO.DAG.getUNDEF(VT));
    }
    KnownUndef.setHighBits(NumElts - 1);
    break;
  }
  case ISD::BITCAST: {
    SDValue Src = Op.getOperand(0);
    EVT SrcVT = Src.getValueType();

    // We only handle vectors here.
    // TODO - investigate calling SimplifyDemandedBits/ComputeKnownBits?
    if (!SrcVT.isVector())
      break;

    // Fast handling of 'identity' bitcasts.
    unsigned NumSrcElts = SrcVT.getVectorNumElements();
    if (NumSrcElts == NumElts)
      return SimplifyDemandedVectorElts(Src, DemandedElts, KnownUndef,
                                        KnownZero, TLO, Depth + 1);

    APInt SrcZero, SrcUndef;
    APInt SrcDemandedElts = APInt::getNullValue(NumSrcElts);

    // Bitcast from 'large element' src vector to 'small element' vector, we
    // must demand a source element if any DemandedElt maps to it.
    if ((NumElts % NumSrcElts) == 0) {
      unsigned Scale = NumElts / NumSrcElts;
      for (unsigned i = 0; i != NumElts; ++i)
        if (DemandedElts[i])
          SrcDemandedElts.setBit(i / Scale);

      if (SimplifyDemandedVectorElts(Src, SrcDemandedElts, SrcUndef, SrcZero,
                                     TLO, Depth + 1))
        return true;

      // Try calling SimplifyDemandedBits, converting demanded elts to the bits
      // of the large element.
      // TODO - bigendian once we have test coverage.
      if (TLO.DAG.getDataLayout().isLittleEndian()) {
        unsigned SrcEltSizeInBits = SrcVT.getScalarSizeInBits();
        APInt SrcDemandedBits = APInt::getNullValue(SrcEltSizeInBits);
        for (unsigned i = 0; i != NumElts; ++i)
          if (DemandedElts[i]) {
            unsigned Ofs = (i % Scale) * EltSizeInBits;
            SrcDemandedBits.setBits(Ofs, Ofs + EltSizeInBits);
          }

        KnownBits Known;
        if (SimplifyDemandedBits(Src, SrcDemandedBits, Known, TLO, Depth + 1))
          return true;
      }

      // If the src element is zero/undef then all the output elements will be -
      // only demanded elements are guaranteed to be correct.
      for (unsigned i = 0; i != NumSrcElts; ++i) {
        if (SrcDemandedElts[i]) {
          if (SrcZero[i])
            KnownZero.setBits(i * Scale, (i + 1) * Scale);
          if (SrcUndef[i])
            KnownUndef.setBits(i * Scale, (i + 1) * Scale);
        }
      }
    }

    // Bitcast from 'small element' src vector to 'large element' vector, we
    // demand all smaller source elements covered by the larger demanded element
    // of this vector.
    if ((NumSrcElts % NumElts) == 0) {
      unsigned Scale = NumSrcElts / NumElts;
      for (unsigned i = 0; i != NumElts; ++i)
        if (DemandedElts[i])
          SrcDemandedElts.setBits(i * Scale, (i + 1) * Scale);

      if (SimplifyDemandedVectorElts(Src, SrcDemandedElts, SrcUndef, SrcZero,
                                     TLO, Depth + 1))
        return true;

      // If all the src elements covering an output element are zero/undef, then
      // the output element will be as well, assuming it was demanded.
      for (unsigned i = 0; i != NumElts; ++i) {
        if (DemandedElts[i]) {
          if (SrcZero.extractBits(Scale, i * Scale).isAllOnesValue())
            KnownZero.setBit(i);
          if (SrcUndef.extractBits(Scale, i * Scale).isAllOnesValue())
            KnownUndef.setBit(i);
        }
      }
    }
    break;
  }
  case ISD::BUILD_VECTOR: {
    // Check all elements and simplify any unused elements with UNDEF.
    if (!DemandedElts.isAllOnesValue()) {
      // Don't simplify BROADCASTS.
      if (llvm::any_of(Op->op_values(),
                       [&](SDValue Elt) { return Op.getOperand(0) != Elt; })) {
        SmallVector<SDValue, 32> Ops(Op->op_begin(), Op->op_end());
        bool Updated = false;
        for (unsigned i = 0; i != NumElts; ++i) {
          if (!DemandedElts[i] && !Ops[i].isUndef()) {
            Ops[i] = TLO.DAG.getUNDEF(Ops[0].getValueType());
            KnownUndef.setBit(i);
            Updated = true;
          }
        }
        if (Updated)
          return TLO.CombineTo(Op, TLO.DAG.getBuildVector(VT, DL, Ops));
      }
    }
    for (unsigned i = 0; i != NumElts; ++i) {
      SDValue SrcOp = Op.getOperand(i);
      if (SrcOp.isUndef()) {
        KnownUndef.setBit(i);
      } else if (EltSizeInBits == SrcOp.getScalarValueSizeInBits() &&
                 (isNullConstant(SrcOp) || isNullFPConstant(SrcOp))) {
        KnownZero.setBit(i);
      }
    }
    break;
  }
  case ISD::CONCAT_VECTORS: {
    EVT SubVT = Op.getOperand(0).getValueType();
    unsigned NumSubVecs = Op.getNumOperands();
    unsigned NumSubElts = SubVT.getVectorNumElements();
    for (unsigned i = 0; i != NumSubVecs; ++i) {
      SDValue SubOp = Op.getOperand(i);
      APInt SubElts = DemandedElts.extractBits(NumSubElts, i * NumSubElts);
      APInt SubUndef, SubZero;
      if (SimplifyDemandedVectorElts(SubOp, SubElts, SubUndef, SubZero, TLO,
                                     Depth + 1))
        return true;
      KnownUndef.insertBits(SubUndef, i * NumSubElts);
      KnownZero.insertBits(SubZero, i * NumSubElts);
    }
    break;
  }
  case ISD::INSERT_SUBVECTOR: {
    if (!isa<ConstantSDNode>(Op.getOperand(2)))
      break;
    SDValue Base = Op.getOperand(0);
    SDValue Sub = Op.getOperand(1);
    EVT SubVT = Sub.getValueType();
    unsigned NumSubElts = SubVT.getVectorNumElements();
    const APInt &Idx = Op.getConstantOperandAPInt(2);
    if (Idx.ugt(NumElts - NumSubElts))
      break;
    unsigned SubIdx = Idx.getZExtValue();
    APInt SubElts = DemandedElts.extractBits(NumSubElts, SubIdx);
    APInt SubUndef, SubZero;
    if (SimplifyDemandedVectorElts(Sub, SubElts, SubUndef, SubZero, TLO,
                                   Depth + 1))
      return true;
    APInt BaseElts = DemandedElts;
    BaseElts.insertBits(APInt::getNullValue(NumSubElts), SubIdx);

    // If none of the base operand elements are demanded, replace it with undef.
    if (!BaseElts && !Base.isUndef())
      return TLO.CombineTo(Op,
                           TLO.DAG.getNode(ISD::INSERT_SUBVECTOR, DL, VT,
                                           TLO.DAG.getUNDEF(VT),
                                           Op.getOperand(1),
                                           Op.getOperand(2)));

    if (SimplifyDemandedVectorElts(Base, BaseElts, KnownUndef, KnownZero, TLO,
                                   Depth + 1))
      return true;
    KnownUndef.insertBits(SubUndef, SubIdx);
    KnownZero.insertBits(SubZero, SubIdx);
    break;
  }
  case ISD::EXTRACT_SUBVECTOR: {
    SDValue Src = Op.getOperand(0);
    ConstantSDNode *SubIdx = dyn_cast<ConstantSDNode>(Op.getOperand(1));
    unsigned NumSrcElts = Src.getValueType().getVectorNumElements();
    if (SubIdx && SubIdx->getAPIntValue().ule(NumSrcElts - NumElts)) {
      // Offset the demanded elts by the subvector index.
      uint64_t Idx = SubIdx->getZExtValue();
      APInt SrcElts = DemandedElts.zextOrSelf(NumSrcElts).shl(Idx);
      APInt SrcUndef, SrcZero;
      if (SimplifyDemandedVectorElts(Src, SrcElts, SrcUndef, SrcZero, TLO,
                                     Depth + 1))
        return true;
      KnownUndef = SrcUndef.extractBits(NumElts, Idx);
      KnownZero = SrcZero.extractBits(NumElts, Idx);
    }
    break;
  }
  case ISD::INSERT_VECTOR_ELT: {
    SDValue Vec = Op.getOperand(0);
    SDValue Scl = Op.getOperand(1);
    auto *CIdx = dyn_cast<ConstantSDNode>(Op.getOperand(2));

    // For a legal, constant insertion index, if we don't need this insertion
    // then strip it, else remove it from the demanded elts.
    if (CIdx && CIdx->getAPIntValue().ult(NumElts)) {
      unsigned Idx = CIdx->getZExtValue();
      if (!DemandedElts[Idx])
        return TLO.CombineTo(Op, Vec);

      APInt DemandedVecElts(DemandedElts);
      DemandedVecElts.clearBit(Idx);
      if (SimplifyDemandedVectorElts(Vec, DemandedVecElts, KnownUndef,
                                     KnownZero, TLO, Depth + 1))
        return true;

      KnownUndef.clearBit(Idx);
      if (Scl.isUndef())
        KnownUndef.setBit(Idx);

      KnownZero.clearBit(Idx);
      if (isNullConstant(Scl) || isNullFPConstant(Scl))
        KnownZero.setBit(Idx);
      break;
    }

    APInt VecUndef, VecZero;
    if (SimplifyDemandedVectorElts(Vec, DemandedElts, VecUndef, VecZero, TLO,
                                   Depth + 1))
      return true;
    // Without knowing the insertion index we can't set KnownUndef/KnownZero.
    break;
  }
  case ISD::VSELECT: {
    // Try to transform the select condition based on the current demanded
    // elements.
    // TODO: If a condition element is undef, we can choose from one arm of the
    //       select (and if one arm is undef, then we can propagate that to the
    //       result).
    // TODO - add support for constant vselect masks (see IR version of this).
    APInt UnusedUndef, UnusedZero;
    if (SimplifyDemandedVectorElts(Op.getOperand(0), DemandedElts, UnusedUndef,
                                   UnusedZero, TLO, Depth + 1))
      return true;

    // See if we can simplify either vselect operand.
    APInt DemandedLHS(DemandedElts);
    APInt DemandedRHS(DemandedElts);
    APInt UndefLHS, ZeroLHS;
    APInt UndefRHS, ZeroRHS;
    if (SimplifyDemandedVectorElts(Op.getOperand(1), DemandedLHS, UndefLHS,
                                   ZeroLHS, TLO, Depth + 1))
      return true;
    if (SimplifyDemandedVectorElts(Op.getOperand(2), DemandedRHS, UndefRHS,
                                   ZeroRHS, TLO, Depth + 1))
      return true;

    KnownUndef = UndefLHS & UndefRHS;
    KnownZero = ZeroLHS & ZeroRHS;
    break;
  }
  case ISD::VECTOR_SHUFFLE: {
    ArrayRef<int> ShuffleMask = cast<ShuffleVectorSDNode>(Op)->getMask();

    // Collect demanded elements from shuffle operands..
    APInt DemandedLHS(NumElts, 0);
    APInt DemandedRHS(NumElts, 0);
    for (unsigned i = 0; i != NumElts; ++i) {
      int M = ShuffleMask[i];
      if (M < 0 || !DemandedElts[i])
        continue;
      assert(0 <= M && M < (int)(2 * NumElts) && "Shuffle index out of range");
      if (M < (int)NumElts)
        DemandedLHS.setBit(M);
      else
        DemandedRHS.setBit(M - NumElts);
    }

    // See if we can simplify either shuffle operand.
    APInt UndefLHS, ZeroLHS;
    APInt UndefRHS, ZeroRHS;
    if (SimplifyDemandedVectorElts(Op.getOperand(0), DemandedLHS, UndefLHS,
                                   ZeroLHS, TLO, Depth + 1))
      return true;
    if (SimplifyDemandedVectorElts(Op.getOperand(1), DemandedRHS, UndefRHS,
                                   ZeroRHS, TLO, Depth + 1))
      return true;

    // Simplify mask using undef elements from LHS/RHS.
    bool Updated = false;
    bool IdentityLHS = true, IdentityRHS = true;
    SmallVector<int, 32> NewMask(ShuffleMask.begin(), ShuffleMask.end());
    for (unsigned i = 0; i != NumElts; ++i) {
      int &M = NewMask[i];
      if (M < 0)
        continue;
      if (!DemandedElts[i] || (M < (int)NumElts && UndefLHS[M]) ||
          (M >= (int)NumElts && UndefRHS[M - NumElts])) {
        Updated = true;
        M = -1;
      }
      IdentityLHS &= (M < 0) || (M == (int)i);
      IdentityRHS &= (M < 0) || ((M - NumElts) == i);
    }

    // Update legal shuffle masks based on demanded elements if it won't reduce
    // to Identity which can cause premature removal of the shuffle mask.
    if (Updated && !IdentityLHS && !IdentityRHS && !TLO.LegalOps) {
      SDValue LegalShuffle =
          buildLegalVectorShuffle(VT, DL, Op.getOperand(0), Op.getOperand(1),
                                  NewMask, TLO.DAG);
      if (LegalShuffle)
        return TLO.CombineTo(Op, LegalShuffle);
    }

    // Propagate undef/zero elements from LHS/RHS.
    for (unsigned i = 0; i != NumElts; ++i) {
      int M = ShuffleMask[i];
      if (M < 0) {
        KnownUndef.setBit(i);
      } else if (M < (int)NumElts) {
        if (UndefLHS[M])
          KnownUndef.setBit(i);
        if (ZeroLHS[M])
          KnownZero.setBit(i);
      } else {
        if (UndefRHS[M - NumElts])
          KnownUndef.setBit(i);
        if (ZeroRHS[M - NumElts])
          KnownZero.setBit(i);
      }
    }
    break;
  }
  case ISD::ANY_EXTEND_VECTOR_INREG:
  case ISD::SIGN_EXTEND_VECTOR_INREG:
  case ISD::ZERO_EXTEND_VECTOR_INREG: {
    APInt SrcUndef, SrcZero;
    SDValue Src = Op.getOperand(0);
    unsigned NumSrcElts = Src.getValueType().getVectorNumElements();
    APInt DemandedSrcElts = DemandedElts.zextOrSelf(NumSrcElts);
    if (SimplifyDemandedVectorElts(Src, DemandedSrcElts, SrcUndef, SrcZero, TLO,
                                   Depth + 1))
      return true;
    KnownZero = SrcZero.zextOrTrunc(NumElts);
    KnownUndef = SrcUndef.zextOrTrunc(NumElts);

    if (Op.getOpcode() == ISD::ANY_EXTEND_VECTOR_INREG &&
        Op.getValueSizeInBits() == Src.getValueSizeInBits() &&
        DemandedSrcElts == 1 && TLO.DAG.getDataLayout().isLittleEndian()) {
      // aext - if we just need the bottom element then we can bitcast.
      return TLO.CombineTo(Op, TLO.DAG.getBitcast(VT, Src));
    }

    if (Op.getOpcode() == ISD::ZERO_EXTEND_VECTOR_INREG) {
      // zext(undef) upper bits are guaranteed to be zero.
      if (DemandedElts.isSubsetOf(KnownUndef))
        return TLO.CombineTo(Op, TLO.DAG.getConstant(0, SDLoc(Op), VT));
      KnownUndef.clearAllBits();
    }
    break;
  }

  // TODO: There are more binop opcodes that could be handled here - MUL, MIN,
  // MAX, saturated math, etc.
  case ISD::OR:
  case ISD::XOR:
  case ISD::ADD:
  case ISD::SUB:
  case ISD::FADD:
  case ISD::FSUB:
  case ISD::FMUL:
  case ISD::FDIV:
  case ISD::FREM: {
    APInt UndefRHS, ZeroRHS;
    if (SimplifyDemandedVectorElts(Op.getOperand(1), DemandedElts, UndefRHS,
                                   ZeroRHS, TLO, Depth + 1))
      return true;
    APInt UndefLHS, ZeroLHS;
    if (SimplifyDemandedVectorElts(Op.getOperand(0), DemandedElts, UndefLHS,
                                   ZeroLHS, TLO, Depth + 1))
      return true;

    KnownZero = ZeroLHS & ZeroRHS;
    KnownUndef = getKnownUndefForVectorBinop(Op, TLO.DAG, UndefLHS, UndefRHS);
    break;
  }
  case ISD::SHL:
  case ISD::SRL:
  case ISD::SRA:
  case ISD::ROTL:
  case ISD::ROTR: {
    APInt UndefRHS, ZeroRHS;
    if (SimplifyDemandedVectorElts(Op.getOperand(1), DemandedElts, UndefRHS,
                                   ZeroRHS, TLO, Depth + 1))
      return true;
    APInt UndefLHS, ZeroLHS;
    if (SimplifyDemandedVectorElts(Op.getOperand(0), DemandedElts, UndefLHS,
                                   ZeroLHS, TLO, Depth + 1))
      return true;

    KnownZero = ZeroLHS;
    KnownUndef = UndefLHS & UndefRHS; // TODO: use getKnownUndefForVectorBinop?
    break;
  }
  case ISD::MUL:
  case ISD::AND: {
    APInt SrcUndef, SrcZero;
    if (SimplifyDemandedVectorElts(Op.getOperand(1), DemandedElts, SrcUndef,
                                   SrcZero, TLO, Depth + 1))
      return true;
    if (SimplifyDemandedVectorElts(Op.getOperand(0), DemandedElts, KnownUndef,
                                   KnownZero, TLO, Depth + 1))
      return true;

    // If either side has a zero element, then the result element is zero, even
    // if the other is an UNDEF.
    // TODO: Extend getKnownUndefForVectorBinop to also deal with known zeros
    // and then handle 'and' nodes with the rest of the binop opcodes.
    KnownZero |= SrcZero;
    KnownUndef &= SrcUndef;
    KnownUndef &= ~KnownZero;
    break;
  }
  case ISD::TRUNCATE:
  case ISD::SIGN_EXTEND:
  case ISD::ZERO_EXTEND:
    if (SimplifyDemandedVectorElts(Op.getOperand(0), DemandedElts, KnownUndef,
                                   KnownZero, TLO, Depth + 1))
      return true;

    if (Op.getOpcode() == ISD::ZERO_EXTEND) {
      // zext(undef) upper bits are guaranteed to be zero.
      if (DemandedElts.isSubsetOf(KnownUndef))
        return TLO.CombineTo(Op, TLO.DAG.getConstant(0, SDLoc(Op), VT));
      KnownUndef.clearAllBits();
    }
    break;
  default: {
    if (Op.getOpcode() >= ISD::BUILTIN_OP_END) {
      if (SimplifyDemandedVectorEltsForTargetNode(Op, DemandedElts, KnownUndef,
                                                  KnownZero, TLO, Depth))
        return true;
    } else {
      KnownBits Known;
      APInt DemandedBits = APInt::getAllOnesValue(EltSizeInBits);
      if (SimplifyDemandedBits(Op, DemandedBits, OriginalDemandedElts, Known,
                               TLO, Depth, AssumeSingleUse))
        return true;
    }
    break;
  }
  }
  assert((KnownUndef & KnownZero) == 0 && "Elements flagged as undef AND zero");

  // Constant fold all undef cases.
  // TODO: Handle zero cases as well.
  if (DemandedElts.isSubsetOf(KnownUndef))
    return TLO.CombineTo(Op, TLO.DAG.getUNDEF(VT));

  return false;
}

/// Determine which of the bits specified in Mask are known to be either zero or
/// one and return them in the Known.
void TargetLowering::computeKnownBitsForTargetNode(const SDValue Op,
                                                   KnownBits &Known,
                                                   const APInt &DemandedElts,
                                                   const SelectionDAG &DAG,
                                                   unsigned Depth) const {
  assert((Op.getOpcode() >= ISD::BUILTIN_OP_END ||
          Op.getOpcode() == ISD::INTRINSIC_WO_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_W_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_VOID) &&
         "Should use MaskedValueIsZero if you don't know whether Op"
         " is a target node!");
  Known.resetAll();
}

void TargetLowering::computeKnownBitsForTargetInstr(
    GISelKnownBits &Analysis, Register R, KnownBits &Known,
    const APInt &DemandedElts, const MachineRegisterInfo &MRI,
    unsigned Depth) const {
  Known.resetAll();
}

void TargetLowering::computeKnownBitsForFrameIndex(const SDValue Op,
                                                   KnownBits &Known,
                                                   const APInt &DemandedElts,
                                                   const SelectionDAG &DAG,
                                                   unsigned Depth) const {
  assert(isa<FrameIndexSDNode>(Op) && "expected FrameIndex");

  if (unsigned Align = DAG.InferPtrAlignment(Op)) {
    // The low bits are known zero if the pointer is aligned.
    Known.Zero.setLowBits(Log2_32(Align));
  }
}

/// This method can be implemented by targets that want to expose additional
/// information about sign bits to the DAG Combiner.
unsigned TargetLowering::ComputeNumSignBitsForTargetNode(SDValue Op,
                                                         const APInt &,
                                                         const SelectionDAG &,
                                                         unsigned Depth) const {
  assert((Op.getOpcode() >= ISD::BUILTIN_OP_END ||
          Op.getOpcode() == ISD::INTRINSIC_WO_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_W_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_VOID) &&
         "Should use ComputeNumSignBits if you don't know whether Op"
         " is a target node!");
  return 1;
}

bool TargetLowering::SimplifyDemandedVectorEltsForTargetNode(
    SDValue Op, const APInt &DemandedElts, APInt &KnownUndef, APInt &KnownZero,
    TargetLoweringOpt &TLO, unsigned Depth) const {
  assert((Op.getOpcode() >= ISD::BUILTIN_OP_END ||
          Op.getOpcode() == ISD::INTRINSIC_WO_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_W_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_VOID) &&
         "Should use SimplifyDemandedVectorElts if you don't know whether Op"
         " is a target node!");
  return false;
}

bool TargetLowering::SimplifyDemandedBitsForTargetNode(
    SDValue Op, const APInt &DemandedBits, const APInt &DemandedElts,
    KnownBits &Known, TargetLoweringOpt &TLO, unsigned Depth) const {
  assert((Op.getOpcode() >= ISD::BUILTIN_OP_END ||
          Op.getOpcode() == ISD::INTRINSIC_WO_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_W_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_VOID) &&
         "Should use SimplifyDemandedBits if you don't know whether Op"
         " is a target node!");
  computeKnownBitsForTargetNode(Op, Known, DemandedElts, TLO.DAG, Depth);
  return false;
}

SDValue TargetLowering::SimplifyMultipleUseDemandedBitsForTargetNode(
    SDValue Op, const APInt &DemandedBits, const APInt &DemandedElts,
    SelectionDAG &DAG, unsigned Depth) const {
  assert(
      (Op.getOpcode() >= ISD::BUILTIN_OP_END ||
       Op.getOpcode() == ISD::INTRINSIC_WO_CHAIN ||
       Op.getOpcode() == ISD::INTRINSIC_W_CHAIN ||
       Op.getOpcode() == ISD::INTRINSIC_VOID) &&
      "Should use SimplifyMultipleUseDemandedBits if you don't know whether Op"
      " is a target node!");
  return SDValue();
}

SDValue
TargetLowering::buildLegalVectorShuffle(EVT VT, const SDLoc &DL, SDValue N0,
                                        SDValue N1, MutableArrayRef<int> Mask,
                                        SelectionDAG &DAG) const {
  bool LegalMask = isShuffleMaskLegal(Mask, VT);
  if (!LegalMask) {
    std::swap(N0, N1);
    ShuffleVectorSDNode::commuteMask(Mask);
    LegalMask = isShuffleMaskLegal(Mask, VT);
  }

  if (!LegalMask)
    return SDValue();

  return DAG.getVectorShuffle(VT, DL, N0, N1, Mask);
}

const Constant *TargetLowering::getTargetConstantFromLoad(LoadSDNode*) const {
  return nullptr;
}

bool TargetLowering::isKnownNeverNaNForTargetNode(SDValue Op,
                                                  const SelectionDAG &DAG,
                                                  bool SNaN,
                                                  unsigned Depth) const {
  assert((Op.getOpcode() >= ISD::BUILTIN_OP_END ||
          Op.getOpcode() == ISD::INTRINSIC_WO_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_W_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_VOID) &&
         "Should use isKnownNeverNaN if you don't know whether Op"
         " is a target node!");
  return false;
}

// FIXME: Ideally, this would use ISD::isConstantSplatVector(), but that must
// work with truncating build vectors and vectors with elements of less than
// 8 bits.
bool TargetLowering::isConstTrueVal(const SDNode *N) const {
  if (!N)
    return false;

  APInt CVal;
  if (auto *CN = dyn_cast<ConstantSDNode>(N)) {
    CVal = CN->getAPIntValue();
  } else if (auto *BV = dyn_cast<BuildVectorSDNode>(N)) {
    auto *CN = BV->getConstantSplatNode();
    if (!CN)
      return false;

    // If this is a truncating build vector, truncate the splat value.
    // Otherwise, we may fail to match the expected values below.
    unsigned BVEltWidth = BV->getValueType(0).getScalarSizeInBits();
    CVal = CN->getAPIntValue();
    if (BVEltWidth < CVal.getBitWidth())
      CVal = CVal.trunc(BVEltWidth);
  } else {
    return false;
  }

  switch (getBooleanContents(N->getValueType(0))) {
  case UndefinedBooleanContent:
    return CVal[0];
  case ZeroOrOneBooleanContent:
    return CVal.isOneValue();
  case ZeroOrNegativeOneBooleanContent:
    return CVal.isAllOnesValue();
  }

  llvm_unreachable("Invalid boolean contents");
}

bool TargetLowering::isConstFalseVal(const SDNode *N) const {
  if (!N)
    return false;

  const ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N);
  if (!CN) {
    const BuildVectorSDNode *BV = dyn_cast<BuildVectorSDNode>(N);
    if (!BV)
      return false;

    // Only interested in constant splats, we don't care about undef
    // elements in identifying boolean constants and getConstantSplatNode
    // returns NULL if all ops are undef;
    CN = BV->getConstantSplatNode();
    if (!CN)
      return false;
  }

  if (getBooleanContents(N->getValueType(0)) == UndefinedBooleanContent)
    return !CN->getAPIntValue()[0];

  return CN->isNullValue();
}

bool TargetLowering::isExtendedTrueVal(const ConstantSDNode *N, EVT VT,
                                       bool SExt) const {
  if (VT == MVT::i1)
    return N->isOne();

  TargetLowering::BooleanContent Cnt = getBooleanContents(VT);
  switch (Cnt) {
  case TargetLowering::ZeroOrOneBooleanContent:
    // An extended value of 1 is always true, unless its original type is i1,
    // in which case it will be sign extended to -1.
    return (N->isOne() && !SExt) || (SExt && (N->getValueType(0) != MVT::i1));
  case TargetLowering::UndefinedBooleanContent:
  case TargetLowering::ZeroOrNegativeOneBooleanContent:
    return N->isAllOnesValue() && SExt;
  }
  llvm_unreachable("Unexpected enumeration.");
}

/// This helper function of SimplifySetCC tries to optimize the comparison when
/// either operand of the SetCC node is a bitwise-and instruction.
SDValue TargetLowering::foldSetCCWithAnd(EVT VT, SDValue N0, SDValue N1,
                                         ISD::CondCode Cond, const SDLoc &DL,
                                         DAGCombinerInfo &DCI) const {
  // Match these patterns in any of their permutations:
  // (X & Y) == Y
  // (X & Y) != Y
  if (N1.getOpcode() == ISD::AND && N0.getOpcode() != ISD::AND)
    std::swap(N0, N1);

  EVT OpVT = N0.getValueType();
  if (N0.getOpcode() != ISD::AND || !OpVT.isInteger() ||
      (Cond != ISD::SETEQ && Cond != ISD::SETNE))
    return SDValue();

  SDValue X, Y;
  if (N0.getOperand(0) == N1) {
    X = N0.getOperand(1);
    Y = N0.getOperand(0);
  } else if (N0.getOperand(1) == N1) {
    X = N0.getOperand(0);
    Y = N0.getOperand(1);
  } else {
    return SDValue();
  }

  SelectionDAG &DAG = DCI.DAG;
  SDValue Zero = DAG.getConstant(0, DL, OpVT);
  if (DAG.isKnownToBeAPowerOfTwo(Y)) {
    // Simplify X & Y == Y to X & Y != 0 if Y has exactly one bit set.
    // Note that where Y is variable and is known to have at most one bit set
    // (for example, if it is Z & 1) we cannot do this; the expressions are not
    // equivalent when Y == 0.
    Cond = ISD::getSetCCInverse(Cond, /*isInteger=*/true);
    if (DCI.isBeforeLegalizeOps() ||
        isCondCodeLegal(Cond, N0.getSimpleValueType()))
      return DAG.getSetCC(DL, VT, N0, Zero, Cond);
  } else if (N0.hasOneUse() && hasAndNotCompare(Y)) {
    // If the target supports an 'and-not' or 'and-complement' logic operation,
    // try to use that to make a comparison operation more efficient.
    // But don't do this transform if the mask is a single bit because there are
    // more efficient ways to deal with that case (for example, 'bt' on x86 or
    // 'rlwinm' on PPC).

    // Bail out if the compare operand that we want to turn into a zero is
    // already a zero (otherwise, infinite loop).
    auto *YConst = dyn_cast<ConstantSDNode>(Y);
    if (YConst && YConst->isNullValue())
      return SDValue();

    // Transform this into: ~X & Y == 0.
    SDValue NotX = DAG.getNOT(SDLoc(X), X, OpVT);
    SDValue NewAnd = DAG.getNode(ISD::AND, SDLoc(N0), OpVT, NotX, Y);
    return DAG.getSetCC(DL, VT, NewAnd, Zero, Cond);
  }

  return SDValue();
}

/// There are multiple IR patterns that could be checking whether certain
/// truncation of a signed number would be lossy or not. The pattern which is
/// best at IR level, may not lower optimally. Thus, we want to unfold it.
/// We are looking for the following pattern: (KeptBits is a constant)
///   (add %x, (1 << (KeptBits-1))) srccond (1 << KeptBits)
/// KeptBits won't be bitwidth(x), that will be constant-folded to true/false.
/// KeptBits also can't be 1, that would have been folded to  %x dstcond 0
/// We will unfold it into the natural trunc+sext pattern:
///   ((%x << C) a>> C) dstcond %x
/// Where  C = bitwidth(x) - KeptBits  and  C u< bitwidth(x)
SDValue TargetLowering::optimizeSetCCOfSignedTruncationCheck(
    EVT SCCVT, SDValue N0, SDValue N1, ISD::CondCode Cond, DAGCombinerInfo &DCI,
    const SDLoc &DL) const {
  // We must be comparing with a constant.
  ConstantSDNode *C1;
  if (!(C1 = dyn_cast<ConstantSDNode>(N1)))
    return SDValue();

  // N0 should be:  add %x, (1 << (KeptBits-1))
  if (N0->getOpcode() != ISD::ADD)
    return SDValue();

  // And we must be 'add'ing a constant.
  ConstantSDNode *C01;
  if (!(C01 = dyn_cast<ConstantSDNode>(N0->getOperand(1))))
    return SDValue();

  SDValue X = N0->getOperand(0);
  EVT XVT = X.getValueType();

  // Validate constants ...

  APInt I1 = C1->getAPIntValue();

  ISD::CondCode NewCond;
  if (Cond == ISD::CondCode::SETULT) {
    NewCond = ISD::CondCode::SETEQ;
  } else if (Cond == ISD::CondCode::SETULE) {
    NewCond = ISD::CondCode::SETEQ;
    // But need to 'canonicalize' the constant.
    I1 += 1;
  } else if (Cond == ISD::CondCode::SETUGT) {
    NewCond = ISD::CondCode::SETNE;
    // But need to 'canonicalize' the constant.
    I1 += 1;
  } else if (Cond == ISD::CondCode::SETUGE) {
    NewCond = ISD::CondCode::SETNE;
  } else
    return SDValue();

  APInt I01 = C01->getAPIntValue();

  auto checkConstants = [&I1, &I01]() -> bool {
    // Both of them must be power-of-two, and the constant from setcc is bigger.
    return I1.ugt(I01) && I1.isPowerOf2() && I01.isPowerOf2();
  };

  if (checkConstants()) {
    // Great, e.g. got  icmp ult i16 (add i16 %x, 128), 256
  } else {
    // What if we invert constants? (and the target predicate)
    I1.negate();
    I01.negate();
    NewCond = getSetCCInverse(NewCond, /*isInteger=*/true);
    if (!checkConstants())
      return SDValue();
    // Great, e.g. got  icmp uge i16 (add i16 %x, -128), -256
  }

  // They are power-of-two, so which bit is set?
  const unsigned KeptBits = I1.logBase2();
  const unsigned KeptBitsMinusOne = I01.logBase2();

  // Magic!
  if (KeptBits != (KeptBitsMinusOne + 1))
    return SDValue();
  assert(KeptBits > 0 && KeptBits < XVT.getSizeInBits() && "unreachable");

  // We don't want to do this in every single case.
  SelectionDAG &DAG = DCI.DAG;
  if (!DAG.getTargetLoweringInfo().shouldTransformSignedTruncationCheck(
          XVT, KeptBits))
    return SDValue();

  const unsigned MaskedBits = XVT.getSizeInBits() - KeptBits;
  assert(MaskedBits > 0 && MaskedBits < XVT.getSizeInBits() && "unreachable");

  // Unfold into:  ((%x << C) a>> C) cond %x
  // Where 'cond' will be either 'eq' or 'ne'.
  SDValue ShiftAmt = DAG.getConstant(MaskedBits, DL, XVT);
  SDValue T0 = DAG.getNode(ISD::SHL, DL, XVT, X, ShiftAmt);
  SDValue T1 = DAG.getNode(ISD::SRA, DL, XVT, T0, ShiftAmt);
  SDValue T2 = DAG.getSetCC(DL, SCCVT, T1, X, NewCond);

  return T2;
}

// (X & (C l>>/<< Y)) ==/!= 0  -->  ((X <</l>> Y) & C) ==/!= 0
SDValue TargetLowering::optimizeSetCCByHoistingAndByConstFromLogicalShift(
    EVT SCCVT, SDValue N0, SDValue N1C, ISD::CondCode Cond,
    DAGCombinerInfo &DCI, const SDLoc &DL) const {
  assert(isConstOrConstSplat(N1C) &&
         isConstOrConstSplat(N1C)->getAPIntValue().isNullValue() &&
         "Should be a comparison with 0.");
  assert((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
         "Valid only for [in]equality comparisons.");

  unsigned NewShiftOpcode;
  SDValue X, C, Y;

  SelectionDAG &DAG = DCI.DAG;
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();

  // Look for '(C l>>/<< Y)'.
  auto Match = [&NewShiftOpcode, &X, &C, &Y, &TLI, &DAG](SDValue V) {
    // The shift should be one-use.
    if (!V.hasOneUse())
      return false;
    unsigned OldShiftOpcode = V.getOpcode();
    switch (OldShiftOpcode) {
    case ISD::SHL:
      NewShiftOpcode = ISD::SRL;
      break;
    case ISD::SRL:
      NewShiftOpcode = ISD::SHL;
      break;
    default:
      return false; // must be a logical shift.
    }
    // We should be shifting a constant.
    // FIXME: best to use isConstantOrConstantVector().
    C = V.getOperand(0);
    ConstantSDNode *CC =
        isConstOrConstSplat(C, /*AllowUndefs=*/true, /*AllowTruncation=*/true);
    if (!CC)
      return false;
    Y = V.getOperand(1);

    ConstantSDNode *XC =
        isConstOrConstSplat(X, /*AllowUndefs=*/true, /*AllowTruncation=*/true);
    return TLI.shouldProduceAndByConstByHoistingConstFromShiftsLHSOfAnd(
        X, XC, CC, Y, OldShiftOpcode, NewShiftOpcode, DAG);
  };

  // LHS of comparison should be an one-use 'and'.
  if (N0.getOpcode() != ISD::AND || !N0.hasOneUse())
    return SDValue();

  X = N0.getOperand(0);
  SDValue Mask = N0.getOperand(1);

  // 'and' is commutative!
  if (!Match(Mask)) {
    std::swap(X, Mask);
    if (!Match(Mask))
      return SDValue();
  }

  EVT VT = X.getValueType();

  // Produce:
  // ((X 'OppositeShiftOpcode' Y) & C) Cond 0
  SDValue T0 = DAG.getNode(NewShiftOpcode, DL, VT, X, Y);
  SDValue T1 = DAG.getNode(ISD::AND, DL, VT, T0, C);
  SDValue T2 = DAG.getSetCC(DL, SCCVT, T1, N1C, Cond);
  return T2;
}

/// Try to fold an equality comparison with a {add/sub/xor} binary operation as
/// the 1st operand (N0). Callers are expected to swap the N0/N1 parameters to
/// handle the commuted versions of these patterns.
SDValue TargetLowering::foldSetCCWithBinOp(EVT VT, SDValue N0, SDValue N1,
                                           ISD::CondCode Cond, const SDLoc &DL,
                                           DAGCombinerInfo &DCI) const {
  unsigned BOpcode = N0.getOpcode();
  assert((BOpcode == ISD::ADD || BOpcode == ISD::SUB || BOpcode == ISD::XOR) &&
         "Unexpected binop");
  assert((Cond == ISD::SETEQ || Cond == ISD::SETNE) && "Unexpected condcode");

  // (X + Y) == X --> Y == 0
  // (X - Y) == X --> Y == 0
  // (X ^ Y) == X --> Y == 0
  SelectionDAG &DAG = DCI.DAG;
  EVT OpVT = N0.getValueType();
  SDValue X = N0.getOperand(0);
  SDValue Y = N0.getOperand(1);
  if (X == N1)
    return DAG.getSetCC(DL, VT, Y, DAG.getConstant(0, DL, OpVT), Cond);

  if (Y != N1)
    return SDValue();

  // (X + Y) == Y --> X == 0
  // (X ^ Y) == Y --> X == 0
  if (BOpcode == ISD::ADD || BOpcode == ISD::XOR)
    return DAG.getSetCC(DL, VT, X, DAG.getConstant(0, DL, OpVT), Cond);

  // The shift would not be valid if the operands are boolean (i1).
  if (!N0.hasOneUse() || OpVT.getScalarSizeInBits() == 1)
    return SDValue();

  // (X - Y) == Y --> X == Y << 1
  EVT ShiftVT = getShiftAmountTy(OpVT, DAG.getDataLayout(),
                                 !DCI.isBeforeLegalize());
  SDValue One = DAG.getConstant(1, DL, ShiftVT);
  SDValue YShl1 = DAG.getNode(ISD::SHL, DL, N1.getValueType(), Y, One);
  if (!DCI.isCalledByLegalizer())
    DCI.AddToWorklist(YShl1.getNode());
  return DAG.getSetCC(DL, VT, X, YShl1, Cond);
}

/// Try to simplify a setcc built with the specified operands and cc. If it is
/// unable to simplify it, return a null SDValue.
SDValue TargetLowering::SimplifySetCC(EVT VT, SDValue N0, SDValue N1,
                                      ISD::CondCode Cond, bool foldBooleans,
                                      DAGCombinerInfo &DCI,
                                      const SDLoc &dl) const {
  SelectionDAG &DAG = DCI.DAG;
  const DataLayout &Layout = DAG.getDataLayout();
  EVT OpVT = N0.getValueType();

  // Constant fold or commute setcc.
  if (SDValue Fold = DAG.FoldSetCC(VT, N0, N1, Cond, dl))
    return Fold;

  // Ensure that the constant occurs on the RHS and fold constant comparisons.
  // TODO: Handle non-splat vector constants. All undef causes trouble.
  ISD::CondCode SwappedCC = ISD::getSetCCSwappedOperands(Cond);
  if (isConstOrConstSplat(N0) &&
      (DCI.isBeforeLegalizeOps() ||
       isCondCodeLegal(SwappedCC, N0.getSimpleValueType())))
    return DAG.getSetCC(dl, VT, N1, N0, SwappedCC);

  // If we have a subtract with the same 2 non-constant operands as this setcc
  // -- but in reverse order -- then try to commute the operands of this setcc
  // to match. A matching pair of setcc (cmp) and sub may be combined into 1
  // instruction on some targets.
  if (!isConstOrConstSplat(N0) && !isConstOrConstSplat(N1) &&
      (DCI.isBeforeLegalizeOps() ||
       isCondCodeLegal(SwappedCC, N0.getSimpleValueType())) &&
      DAG.getNodeIfExists(ISD::SUB, DAG.getVTList(OpVT), { N1, N0 } ) &&
      !DAG.getNodeIfExists(ISD::SUB, DAG.getVTList(OpVT), { N0, N1 } ))
    return DAG.getSetCC(dl, VT, N1, N0, SwappedCC);

  if (auto *N1C = dyn_cast<ConstantSDNode>(N1.getNode())) {
    const APInt &C1 = N1C->getAPIntValue();

    // If the LHS is '(srl (ctlz x), 5)', the RHS is 0/1, and this is an
    // equality comparison, then we're just comparing whether X itself is
    // zero.
    if (N0.getOpcode() == ISD::SRL && (C1.isNullValue() || C1.isOneValue()) &&
        N0.getOperand(0).getOpcode() == ISD::CTLZ &&
        N0.getOperand(1).getOpcode() == ISD::Constant) {
      const APInt &ShAmt = N0.getConstantOperandAPInt(1);
      if ((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
          ShAmt == Log2_32(N0.getValueSizeInBits())) {
        if ((C1 == 0) == (Cond == ISD::SETEQ)) {
          // (srl (ctlz x), 5) == 0  -> X != 0
          // (srl (ctlz x), 5) != 1  -> X != 0
          Cond = ISD::SETNE;
        } else {
          // (srl (ctlz x), 5) != 0  -> X == 0
          // (srl (ctlz x), 5) == 1  -> X == 0
          Cond = ISD::SETEQ;
        }
        SDValue Zero = DAG.getConstant(0, dl, N0.getValueType());
        return DAG.getSetCC(dl, VT, N0.getOperand(0).getOperand(0),
                            Zero, Cond);
      }
    }

    SDValue CTPOP = N0;
    // Look through truncs that don't change the value of a ctpop.
    if (N0.hasOneUse() && N0.getOpcode() == ISD::TRUNCATE)
      CTPOP = N0.getOperand(0);

    if (CTPOP.hasOneUse() && CTPOP.getOpcode() == ISD::CTPOP &&
        (N0 == CTPOP ||
         N0.getValueSizeInBits() > Log2_32_Ceil(CTPOP.getValueSizeInBits()))) {
      EVT CTVT = CTPOP.getValueType();
      SDValue CTOp = CTPOP.getOperand(0);

      // (ctpop x) u< 2 -> (x & x-1) == 0
      // (ctpop x) u> 1 -> (x & x-1) != 0
      if ((Cond == ISD::SETULT && C1 == 2) || (Cond == ISD::SETUGT && C1 == 1)){
        SDValue NegOne = DAG.getAllOnesConstant(dl, CTVT);
        SDValue Add = DAG.getNode(ISD::ADD, dl, CTVT, CTOp, NegOne);
        SDValue And = DAG.getNode(ISD::AND, dl, CTVT, CTOp, Add);
        ISD::CondCode CC = Cond == ISD::SETULT ? ISD::SETEQ : ISD::SETNE;
        return DAG.getSetCC(dl, VT, And, DAG.getConstant(0, dl, CTVT), CC);
      }

      // If ctpop is not supported, expand a power-of-2 comparison based on it.
      if (C1 == 1 && !isOperationLegalOrCustom(ISD::CTPOP, CTVT) &&
          (Cond == ISD::SETEQ || Cond == ISD::SETNE)) {
        // (ctpop x) == 1 --> (x != 0) && ((x & x-1) == 0)
        // (ctpop x) != 1 --> (x == 0) || ((x & x-1) != 0)
        SDValue Zero = DAG.getConstant(0, dl, CTVT);
        SDValue NegOne = DAG.getAllOnesConstant(dl, CTVT);
        ISD::CondCode InvCond = ISD::getSetCCInverse(Cond, true);
        SDValue Add = DAG.getNode(ISD::ADD, dl, CTVT, CTOp, NegOne);
        SDValue And = DAG.getNode(ISD::AND, dl, CTVT, CTOp, Add);
        SDValue LHS = DAG.getSetCC(dl, VT, CTOp, Zero, InvCond);
        SDValue RHS = DAG.getSetCC(dl, VT, And, Zero, Cond);
        unsigned LogicOpcode = Cond == ISD::SETEQ ? ISD::AND : ISD::OR;
        return DAG.getNode(LogicOpcode, dl, VT, LHS, RHS);
      }
    }

    // (zext x) == C --> x == (trunc C)
    // (sext x) == C --> x == (trunc C)
    if ((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
        DCI.isBeforeLegalize() && N0->hasOneUse()) {
      unsigned MinBits = N0.getValueSizeInBits();
      SDValue PreExt;
      bool Signed = false;
      if (N0->getOpcode() == ISD::ZERO_EXTEND) {
        // ZExt
        MinBits = N0->getOperand(0).getValueSizeInBits();
        PreExt = N0->getOperand(0);
      } else if (N0->getOpcode() == ISD::AND) {
        // DAGCombine turns costly ZExts into ANDs
        if (auto *C = dyn_cast<ConstantSDNode>(N0->getOperand(1)))
          if ((C->getAPIntValue()+1).isPowerOf2()) {
            MinBits = C->getAPIntValue().countTrailingOnes();
            PreExt = N0->getOperand(0);
          }
      } else if (N0->getOpcode() == ISD::SIGN_EXTEND) {
        // SExt
        MinBits = N0->getOperand(0).getValueSizeInBits();
        PreExt = N0->getOperand(0);
        Signed = true;
      } else if (auto *LN0 = dyn_cast<LoadSDNode>(N0)) {
        // ZEXTLOAD / SEXTLOAD
        if (LN0->getExtensionType() == ISD::ZEXTLOAD) {
          MinBits = LN0->getMemoryVT().getSizeInBits();
          PreExt = N0;
        } else if (LN0->getExtensionType() == ISD::SEXTLOAD) {
          Signed = true;
          MinBits = LN0->getMemoryVT().getSizeInBits();
          PreExt = N0;
        }
      }

      // Figure out how many bits we need to preserve this constant.
      unsigned ReqdBits = Signed ?
        C1.getBitWidth() - C1.getNumSignBits() + 1 :
        C1.getActiveBits();

      // Make sure we're not losing bits from the constant.
      if (MinBits > 0 &&
          MinBits < C1.getBitWidth() &&
          MinBits >= ReqdBits) {
        EVT MinVT = EVT::getIntegerVT(*DAG.getContext(), MinBits);
        if (isTypeDesirableForOp(ISD::SETCC, MinVT)) {
          // Will get folded away.
          SDValue Trunc = DAG.getNode(ISD::TRUNCATE, dl, MinVT, PreExt);
          if (MinBits == 1 && C1 == 1)
            // Invert the condition.
            return DAG.getSetCC(dl, VT, Trunc, DAG.getConstant(0, dl, MVT::i1),
                                Cond == ISD::SETEQ ? ISD::SETNE : ISD::SETEQ);
          SDValue C = DAG.getConstant(C1.trunc(MinBits), dl, MinVT);
          return DAG.getSetCC(dl, VT, Trunc, C, Cond);
        }

        // If truncating the setcc operands is not desirable, we can still
        // simplify the expression in some cases:
        // setcc ([sz]ext (setcc x, y, cc)), 0, setne) -> setcc (x, y, cc)
        // setcc ([sz]ext (setcc x, y, cc)), 0, seteq) -> setcc (x, y, inv(cc))
        // setcc (zext (setcc x, y, cc)), 1, setne) -> setcc (x, y, inv(cc))
        // setcc (zext (setcc x, y, cc)), 1, seteq) -> setcc (x, y, cc)
        // setcc (sext (setcc x, y, cc)), -1, setne) -> setcc (x, y, inv(cc))
        // setcc (sext (setcc x, y, cc)), -1, seteq) -> setcc (x, y, cc)
        SDValue TopSetCC = N0->getOperand(0);
        unsigned N0Opc = N0->getOpcode();
        bool SExt = (N0Opc == ISD::SIGN_EXTEND);
        if (TopSetCC.getValueType() == MVT::i1 && VT == MVT::i1 &&
            TopSetCC.getOpcode() == ISD::SETCC &&
            (N0Opc == ISD::ZERO_EXTEND || N0Opc == ISD::SIGN_EXTEND) &&
            (isConstFalseVal(N1C) ||
             isExtendedTrueVal(N1C, N0->getValueType(0), SExt))) {

          bool Inverse = (N1C->isNullValue() && Cond == ISD::SETEQ) ||
                         (!N1C->isNullValue() && Cond == ISD::SETNE);

          if (!Inverse)
            return TopSetCC;

          ISD::CondCode InvCond = ISD::getSetCCInverse(
              cast<CondCodeSDNode>(TopSetCC.getOperand(2))->get(),
              TopSetCC.getOperand(0).getValueType().isInteger());
          return DAG.getSetCC(dl, VT, TopSetCC.getOperand(0),
                                      TopSetCC.getOperand(1),
                                      InvCond);
        }
      }
    }

    // If the LHS is '(and load, const)', the RHS is 0, the test is for
    // equality or unsigned, and all 1 bits of the const are in the same
    // partial word, see if we can shorten the load.
    if (DCI.isBeforeLegalize() &&
        !ISD::isSignedIntSetCC(Cond) &&
        N0.getOpcode() == ISD::AND && C1 == 0 &&
        N0.getNode()->hasOneUse() &&
        isa<LoadSDNode>(N0.getOperand(0)) &&
        N0.getOperand(0).getNode()->hasOneUse() &&
        isa<ConstantSDNode>(N0.getOperand(1))) {
      LoadSDNode *Lod = cast<LoadSDNode>(N0.getOperand(0));
      APInt bestMask;
      unsigned bestWidth = 0, bestOffset = 0;
      if (Lod->isSimple() && Lod->isUnindexed()) {
        unsigned origWidth = N0.getValueSizeInBits();
        unsigned maskWidth = origWidth;
        // We can narrow (e.g.) 16-bit extending loads on 32-bit target to
        // 8 bits, but have to be careful...
        if (Lod->getExtensionType() != ISD::NON_EXTLOAD)
          origWidth = Lod->getMemoryVT().getSizeInBits();
        const APInt &Mask = N0.getConstantOperandAPInt(1);
        for (unsigned width = origWidth / 2; width>=8; width /= 2) {
          APInt newMask = APInt::getLowBitsSet(maskWidth, width);
          for (unsigned offset=0; offset<origWidth/width; offset++) {
            if (Mask.isSubsetOf(newMask)) {
              if (Layout.isLittleEndian())
                bestOffset = (uint64_t)offset * (width/8);
              else
                bestOffset = (origWidth/width - offset - 1) * (width/8);
              bestMask = Mask.lshr(offset * (width/8) * 8);
              bestWidth = width;
              break;
            }
            newMask <<= width;
          }
        }
      }
      if (bestWidth) {
        EVT newVT = EVT::getIntegerVT(*DAG.getContext(), bestWidth);
        if (newVT.isRound() &&
            shouldReduceLoadWidth(Lod, ISD::NON_EXTLOAD, newVT)) {
          EVT PtrType = Lod->getOperand(1).getValueType();
          SDValue Ptr = Lod->getBasePtr();
          if (bestOffset != 0)
            Ptr = DAG.getNode(ISD::ADD, dl, PtrType, Lod->getBasePtr(),
                              DAG.getConstant(bestOffset, dl, PtrType));
          unsigned NewAlign = MinAlign(Lod->getAlignment(), bestOffset);
          SDValue NewLoad = DAG.getLoad(
              newVT, dl, Lod->getChain(), Ptr,
              Lod->getPointerInfo().getWithOffset(bestOffset), NewAlign);
          return DAG.getSetCC(dl, VT,
                              DAG.getNode(ISD::AND, dl, newVT, NewLoad,
                                      DAG.getConstant(bestMask.trunc(bestWidth),
                                                      dl, newVT)),
                              DAG.getConstant(0LL, dl, newVT), Cond);
        }
      }
    }

    // If the LHS is a ZERO_EXTEND, perform the comparison on the input.
    if (N0.getOpcode() == ISD::ZERO_EXTEND) {
      unsigned InSize = N0.getOperand(0).getValueSizeInBits();

      // If the comparison constant has bits in the upper part, the
      // zero-extended value could never match.
      if (C1.intersects(APInt::getHighBitsSet(C1.getBitWidth(),
                                              C1.getBitWidth() - InSize))) {
        switch (Cond) {
        case ISD::SETUGT:
        case ISD::SETUGE:
        case ISD::SETEQ:
          return DAG.getConstant(0, dl, VT);
        case ISD::SETULT:
        case ISD::SETULE:
        case ISD::SETNE:
          return DAG.getConstant(1, dl, VT);
        case ISD::SETGT:
        case ISD::SETGE:
          // True if the sign bit of C1 is set.
          return DAG.getConstant(C1.isNegative(), dl, VT);
        case ISD::SETLT:
        case ISD::SETLE:
          // True if the sign bit of C1 isn't set.
          return DAG.getConstant(C1.isNonNegative(), dl, VT);
        default:
          break;
        }
      }

      // Otherwise, we can perform the comparison with the low bits.
      switch (Cond) {
      case ISD::SETEQ:
      case ISD::SETNE:
      case ISD::SETUGT:
      case ISD::SETUGE:
      case ISD::SETULT:
      case ISD::SETULE: {
        EVT newVT = N0.getOperand(0).getValueType();
        if (DCI.isBeforeLegalizeOps() ||
            (isOperationLegal(ISD::SETCC, newVT) &&
             isCondCodeLegal(Cond, newVT.getSimpleVT()))) {
          EVT NewSetCCVT = getSetCCResultType(Layout, *DAG.getContext(), newVT);
          SDValue NewConst = DAG.getConstant(C1.trunc(InSize), dl, newVT);

          SDValue NewSetCC = DAG.getSetCC(dl, NewSetCCVT, N0.getOperand(0),
                                          NewConst, Cond);
          return DAG.getBoolExtOrTrunc(NewSetCC, dl, VT, N0.getValueType());
        }
        break;
      }
      default:
        break; // todo, be more careful with signed comparisons
      }
    } else if (N0.getOpcode() == ISD::SIGN_EXTEND_INREG &&
               (Cond == ISD::SETEQ || Cond == ISD::SETNE)) {
      EVT ExtSrcTy = cast<VTSDNode>(N0.getOperand(1))->getVT();
      unsigned ExtSrcTyBits = ExtSrcTy.getSizeInBits();
      EVT ExtDstTy = N0.getValueType();
      unsigned ExtDstTyBits = ExtDstTy.getSizeInBits();

      // If the constant doesn't fit into the number of bits for the source of
      // the sign extension, it is impossible for both sides to be equal.
      if (C1.getMinSignedBits() > ExtSrcTyBits)
        return DAG.getConstant(Cond == ISD::SETNE, dl, VT);

      SDValue ZextOp;
      EVT Op0Ty = N0.getOperand(0).getValueType();
      if (Op0Ty == ExtSrcTy) {
        ZextOp = N0.getOperand(0);
      } else {
        APInt Imm = APInt::getLowBitsSet(ExtDstTyBits, ExtSrcTyBits);
        ZextOp = DAG.getNode(ISD::AND, dl, Op0Ty, N0.getOperand(0),
                             DAG.getConstant(Imm, dl, Op0Ty));
      }
      if (!DCI.isCalledByLegalizer())
        DCI.AddToWorklist(ZextOp.getNode());
      // Otherwise, make this a use of a zext.
      return DAG.getSetCC(dl, VT, ZextOp,
                          DAG.getConstant(C1 & APInt::getLowBitsSet(
                                                              ExtDstTyBits,
                                                              ExtSrcTyBits),
                                          dl, ExtDstTy),
                          Cond);
    } else if ((N1C->isNullValue() || N1C->isOne()) &&
                (Cond == ISD::SETEQ || Cond == ISD::SETNE)) {
      // SETCC (SETCC), [0|1], [EQ|NE]  -> SETCC
      if (N0.getOpcode() == ISD::SETCC &&
          isTypeLegal(VT) && VT.bitsLE(N0.getValueType()) &&
          (N0.getValueType() == MVT::i1 ||
           getBooleanContents(N0.getOperand(0).getValueType()) ==
                       ZeroOrOneBooleanContent)) {
        bool TrueWhenTrue = (Cond == ISD::SETEQ) ^ (!N1C->isOne());
        if (TrueWhenTrue)
          return DAG.getNode(ISD::TRUNCATE, dl, VT, N0);
        // Invert the condition.
        ISD::CondCode CC = cast<CondCodeSDNode>(N0.getOperand(2))->get();
        CC = ISD::getSetCCInverse(CC,
                                  N0.getOperand(0).getValueType().isInteger());
        if (DCI.isBeforeLegalizeOps() ||
            isCondCodeLegal(CC, N0.getOperand(0).getSimpleValueType()))
          return DAG.getSetCC(dl, VT, N0.getOperand(0), N0.getOperand(1), CC);
      }

      if ((N0.getOpcode() == ISD::XOR ||
           (N0.getOpcode() == ISD::AND &&
            N0.getOperand(0).getOpcode() == ISD::XOR &&
            N0.getOperand(1) == N0.getOperand(0).getOperand(1))) &&
          isa<ConstantSDNode>(N0.getOperand(1)) &&
          cast<ConstantSDNode>(N0.getOperand(1))->isOne()) {
        // If this is (X^1) == 0/1, swap the RHS and eliminate the xor.  We
        // can only do this if the top bits are known zero.
        unsigned BitWidth = N0.getValueSizeInBits();
        if (DAG.MaskedValueIsZero(N0,
                                  APInt::getHighBitsSet(BitWidth,
                                                        BitWidth-1))) {
          // Okay, get the un-inverted input value.
          SDValue Val;
          if (N0.getOpcode() == ISD::XOR) {
            Val = N0.getOperand(0);
          } else {
            assert(N0.getOpcode() == ISD::AND &&
                    N0.getOperand(0).getOpcode() == ISD::XOR);
            // ((X^1)&1)^1 -> X & 1
            Val = DAG.getNode(ISD::AND, dl, N0.getValueType(),
                              N0.getOperand(0).getOperand(0),
                              N0.getOperand(1));
          }

          return DAG.getSetCC(dl, VT, Val, N1,
                              Cond == ISD::SETEQ ? ISD::SETNE : ISD::SETEQ);
        }
      } else if (N1C->isOne()) {
        SDValue Op0 = N0;
        if (Op0.getOpcode() == ISD::TRUNCATE)
          Op0 = Op0.getOperand(0);

        if ((Op0.getOpcode() == ISD::XOR) &&
            Op0.getOperand(0).getOpcode() == ISD::SETCC &&
            Op0.getOperand(1).getOpcode() == ISD::SETCC) {
          SDValue XorLHS = Op0.getOperand(0);
          SDValue XorRHS = Op0.getOperand(1);
          // Ensure that the input setccs return an i1 type or 0/1 value.
          if (Op0.getValueType() == MVT::i1 ||
              (getBooleanContents(XorLHS.getOperand(0).getValueType()) ==
                      ZeroOrOneBooleanContent &&
               getBooleanContents(XorRHS.getOperand(0).getValueType()) ==
                        ZeroOrOneBooleanContent)) {
            // (xor (setcc), (setcc)) == / != 1 -> (setcc) != / == (setcc)
            Cond = (Cond == ISD::SETEQ) ? ISD::SETNE : ISD::SETEQ;
            return DAG.getSetCC(dl, VT, XorLHS, XorRHS, Cond);
          }
        }
        if (Op0.getOpcode() == ISD::AND &&
            isa<ConstantSDNode>(Op0.getOperand(1)) &&
            cast<ConstantSDNode>(Op0.getOperand(1))->isOne()) {
          // If this is (X&1) == / != 1, normalize it to (X&1) != / == 0.
          if (Op0.getValueType().bitsGT(VT))
            Op0 = DAG.getNode(ISD::AND, dl, VT,
                          DAG.getNode(ISD::TRUNCATE, dl, VT, Op0.getOperand(0)),
                          DAG.getConstant(1, dl, VT));
          else if (Op0.getValueType().bitsLT(VT))
            Op0 = DAG.getNode(ISD::AND, dl, VT,
                        DAG.getNode(ISD::ANY_EXTEND, dl, VT, Op0.getOperand(0)),
                        DAG.getConstant(1, dl, VT));

          return DAG.getSetCC(dl, VT, Op0,
                              DAG.getConstant(0, dl, Op0.getValueType()),
                              Cond == ISD::SETEQ ? ISD::SETNE : ISD::SETEQ);
        }
        if (Op0.getOpcode() == ISD::AssertZext &&
            cast<VTSDNode>(Op0.getOperand(1))->getVT() == MVT::i1)
          return DAG.getSetCC(dl, VT, Op0,
                              DAG.getConstant(0, dl, Op0.getValueType()),
                              Cond == ISD::SETEQ ? ISD::SETNE : ISD::SETEQ);
      }
    }

    // Given:
    //   icmp eq/ne (urem %x, %y), 0
    // Iff %x has 0 or 1 bits set, and %y has at least 2 bits set, omit 'urem':
    //   icmp eq/ne %x, 0
    if (N0.getOpcode() == ISD::UREM && N1C->isNullValue() &&
        (Cond == ISD::SETEQ || Cond == ISD::SETNE)) {
      KnownBits XKnown = DAG.computeKnownBits(N0.getOperand(0));
      KnownBits YKnown = DAG.computeKnownBits(N0.getOperand(1));
      if (XKnown.countMaxPopulation() == 1 && YKnown.countMinPopulation() >= 2)
        return DAG.getSetCC(dl, VT, N0.getOperand(0), N1, Cond);
    }

    if (SDValue V =
            optimizeSetCCOfSignedTruncationCheck(VT, N0, N1, Cond, DCI, dl))
      return V;
  }

  // These simplifications apply to splat vectors as well.
  // TODO: Handle more splat vector cases.
  if (auto *N1C = isConstOrConstSplat(N1)) {
    const APInt &C1 = N1C->getAPIntValue();

    APInt MinVal, MaxVal;
    unsigned OperandBitSize = N1C->getValueType(0).getScalarSizeInBits();
    if (ISD::isSignedIntSetCC(Cond)) {
      MinVal = APInt::getSignedMinValue(OperandBitSize);
      MaxVal = APInt::getSignedMaxValue(OperandBitSize);
    } else {
      MinVal = APInt::getMinValue(OperandBitSize);
      MaxVal = APInt::getMaxValue(OperandBitSize);
    }

    // Canonicalize GE/LE comparisons to use GT/LT comparisons.
    if (Cond == ISD::SETGE || Cond == ISD::SETUGE) {
      // X >= MIN --> true
      if (C1 == MinVal)
        return DAG.getBoolConstant(true, dl, VT, OpVT);

      if (!VT.isVector()) { // TODO: Support this for vectors.
        // X >= C0 --> X > (C0 - 1)
        APInt C = C1 - 1;
        ISD::CondCode NewCC = (Cond == ISD::SETGE) ? ISD::SETGT : ISD::SETUGT;
        if ((DCI.isBeforeLegalizeOps() ||
             isCondCodeLegal(NewCC, VT.getSimpleVT())) &&
            (!N1C->isOpaque() || (C.getBitWidth() <= 64 &&
                                  isLegalICmpImmediate(C.getSExtValue())))) {
          return DAG.getSetCC(dl, VT, N0,
                              DAG.getConstant(C, dl, N1.getValueType()),
                              NewCC);
        }
      }
    }

    if (Cond == ISD::SETLE || Cond == ISD::SETULE) {
      // X <= MAX --> true
      if (C1 == MaxVal)
        return DAG.getBoolConstant(true, dl, VT, OpVT);

      // X <= C0 --> X < (C0 + 1)
      if (!VT.isVector()) { // TODO: Support this for vectors.
        APInt C = C1 + 1;
        ISD::CondCode NewCC = (Cond == ISD::SETLE) ? ISD::SETLT : ISD::SETULT;
        if ((DCI.isBeforeLegalizeOps() ||
             isCondCodeLegal(NewCC, VT.getSimpleVT())) &&
            (!N1C->isOpaque() || (C.getBitWidth() <= 64 &&
                                  isLegalICmpImmediate(C.getSExtValue())))) {
          return DAG.getSetCC(dl, VT, N0,
                              DAG.getConstant(C, dl, N1.getValueType()),
                              NewCC);
        }
      }
    }

    if (Cond == ISD::SETLT || Cond == ISD::SETULT) {
      if (C1 == MinVal)
        return DAG.getBoolConstant(false, dl, VT, OpVT); // X < MIN --> false

      // TODO: Support this for vectors after legalize ops.
      if (!VT.isVector() || DCI.isBeforeLegalizeOps()) {
        // Canonicalize setlt X, Max --> setne X, Max
        if (C1 == MaxVal)
          return DAG.getSetCC(dl, VT, N0, N1, ISD::SETNE);

        // If we have setult X, 1, turn it into seteq X, 0
        if (C1 == MinVal+1)
          return DAG.getSetCC(dl, VT, N0,
                              DAG.getConstant(MinVal, dl, N0.getValueType()),
                              ISD::SETEQ);
      }
    }

    if (Cond == ISD::SETGT || Cond == ISD::SETUGT) {
      if (C1 == MaxVal)
        return DAG.getBoolConstant(false, dl, VT, OpVT); // X > MAX --> false

      // TODO: Support this for vectors after legalize ops.
      if (!VT.isVector() || DCI.isBeforeLegalizeOps()) {
        // Canonicalize setgt X, Min --> setne X, Min
        if (C1 == MinVal)
          return DAG.getSetCC(dl, VT, N0, N1, ISD::SETNE);

        // If we have setugt X, Max-1, turn it into seteq X, Max
        if (C1 == MaxVal-1)
          return DAG.getSetCC(dl, VT, N0,
                              DAG.getConstant(MaxVal, dl, N0.getValueType()),
                              ISD::SETEQ);
      }
    }

    if (Cond == ISD::SETEQ || Cond == ISD::SETNE) {
      // (X & (C l>>/<< Y)) ==/!= 0  -->  ((X <</l>> Y) & C) ==/!= 0
      if (C1.isNullValue())
        if (SDValue CC = optimizeSetCCByHoistingAndByConstFromLogicalShift(
                VT, N0, N1, Cond, DCI, dl))
          return CC;
    }

    // If we have "setcc X, C0", check to see if we can shrink the immediate
    // by changing cc.
    // TODO: Support this for vectors after legalize ops.
    if (!VT.isVector() || DCI.isBeforeLegalizeOps()) {
      // SETUGT X, SINTMAX  -> SETLT X, 0
      if (Cond == ISD::SETUGT &&
          C1 == APInt::getSignedMaxValue(OperandBitSize))
        return DAG.getSetCC(dl, VT, N0,
                            DAG.getConstant(0, dl, N1.getValueType()),
                            ISD::SETLT);

      // SETULT X, SINTMIN  -> SETGT X, -1
      if (Cond == ISD::SETULT &&
          C1 == APInt::getSignedMinValue(OperandBitSize)) {
        SDValue ConstMinusOne =
            DAG.getConstant(APInt::getAllOnesValue(OperandBitSize), dl,
                            N1.getValueType());
        return DAG.getSetCC(dl, VT, N0, ConstMinusOne, ISD::SETGT);
      }
    }
  }

  // Back to non-vector simplifications.
  // TODO: Can we do these for vector splats?
  if (auto *N1C = dyn_cast<ConstantSDNode>(N1.getNode())) {
    const TargetLowering &TLI = DAG.getTargetLoweringInfo();
    const APInt &C1 = N1C->getAPIntValue();
    EVT ShValTy = N0.getValueType();

    // Fold bit comparisons when we can.
    if ((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
        (VT == ShValTy || (isTypeLegal(VT) && VT.bitsLE(ShValTy))) &&
        N0.getOpcode() == ISD::AND) {
      if (auto *AndRHS = dyn_cast<ConstantSDNode>(N0.getOperand(1))) {
        EVT ShiftTy =
            getShiftAmountTy(ShValTy, Layout, !DCI.isBeforeLegalize());
        if (Cond == ISD::SETNE && C1 == 0) {// (X & 8) != 0  -->  (X & 8) >> 3
          // Perform the xform if the AND RHS is a single bit.
          unsigned ShCt = AndRHS->getAPIntValue().logBase2();
          if (AndRHS->getAPIntValue().isPowerOf2() &&
              !TLI.shouldAvoidTransformToShift(ShValTy, ShCt)) {
            return DAG.getNode(ISD::TRUNCATE, dl, VT,
                               DAG.getNode(ISD::SRL, dl, ShValTy, N0,
                                           DAG.getConstant(ShCt, dl, ShiftTy)));
          }
        } else if (Cond == ISD::SETEQ && C1 == AndRHS->getAPIntValue()) {
          // (X & 8) == 8  -->  (X & 8) >> 3
          // Perform the xform if C1 is a single bit.
          unsigned ShCt = C1.logBase2();
          if (C1.isPowerOf2() &&
              !TLI.shouldAvoidTransformToShift(ShValTy, ShCt)) {
            return DAG.getNode(ISD::TRUNCATE, dl, VT,
                               DAG.getNode(ISD::SRL, dl, ShValTy, N0,
                                           DAG.getConstant(ShCt, dl, ShiftTy)));
          }
        }
      }
    }

    if (C1.getMinSignedBits() <= 64 &&
        !isLegalICmpImmediate(C1.getSExtValue())) {
      EVT ShiftTy = getShiftAmountTy(ShValTy, Layout, !DCI.isBeforeLegalize());
      // (X & -256) == 256 -> (X >> 8) == 1
      if ((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
          N0.getOpcode() == ISD::AND && N0.hasOneUse()) {
        if (auto *AndRHS = dyn_cast<ConstantSDNode>(N0.getOperand(1))) {
          const APInt &AndRHSC = AndRHS->getAPIntValue();
          if ((-AndRHSC).isPowerOf2() && (AndRHSC & C1) == C1) {
            unsigned ShiftBits = AndRHSC.countTrailingZeros();
            if (!TLI.shouldAvoidTransformToShift(ShValTy, ShiftBits)) {
              SDValue Shift =
                DAG.getNode(ISD::SRL, dl, ShValTy, N0.getOperand(0),
                            DAG.getConstant(ShiftBits, dl, ShiftTy));
              SDValue CmpRHS = DAG.getConstant(C1.lshr(ShiftBits), dl, ShValTy);
              return DAG.getSetCC(dl, VT, Shift, CmpRHS, Cond);
            }
          }
        }
      } else if (Cond == ISD::SETULT || Cond == ISD::SETUGE ||
                 Cond == ISD::SETULE || Cond == ISD::SETUGT) {
        bool AdjOne = (Cond == ISD::SETULE || Cond == ISD::SETUGT);
        // X <  0x100000000 -> (X >> 32) <  1
        // X >= 0x100000000 -> (X >> 32) >= 1
        // X <= 0x0ffffffff -> (X >> 32) <  1
        // X >  0x0ffffffff -> (X >> 32) >= 1
        unsigned ShiftBits;
        APInt NewC = C1;
        ISD::CondCode NewCond = Cond;
        if (AdjOne) {
          ShiftBits = C1.countTrailingOnes();
          NewC = NewC + 1;
          NewCond = (Cond == ISD::SETULE) ? ISD::SETULT : ISD::SETUGE;
        } else {
          ShiftBits = C1.countTrailingZeros();
        }
        NewC.lshrInPlace(ShiftBits);
        if (ShiftBits && NewC.getMinSignedBits() <= 64 &&
            isLegalICmpImmediate(NewC.getSExtValue()) &&
            !TLI.shouldAvoidTransformToShift(ShValTy, ShiftBits)) {
          SDValue Shift = DAG.getNode(ISD::SRL, dl, ShValTy, N0,
                                      DAG.getConstant(ShiftBits, dl, ShiftTy));
          SDValue CmpRHS = DAG.getConstant(NewC, dl, ShValTy);
          return DAG.getSetCC(dl, VT, Shift, CmpRHS, NewCond);
        }
      }
    }
  }

  if (!isa<ConstantFPSDNode>(N0) && isa<ConstantFPSDNode>(N1)) {
    auto *CFP = cast<ConstantFPSDNode>(N1);
    assert(!CFP->getValueAPF().isNaN() && "Unexpected NaN value");

    // Otherwise, we know the RHS is not a NaN.  Simplify the node to drop the
    // constant if knowing that the operand is non-nan is enough.  We prefer to
    // have SETO(x,x) instead of SETO(x, 0.0) because this avoids having to
    // materialize 0.0.
    if (Cond == ISD::SETO || Cond == ISD::SETUO)
      return DAG.getSetCC(dl, VT, N0, N0, Cond);

    // setcc (fneg x), C -> setcc swap(pred) x, -C
    if (N0.getOpcode() == ISD::FNEG) {
      ISD::CondCode SwapCond = ISD::getSetCCSwappedOperands(Cond);
      if (DCI.isBeforeLegalizeOps() ||
          isCondCodeLegal(SwapCond, N0.getSimpleValueType())) {
        SDValue NegN1 = DAG.getNode(ISD::FNEG, dl, N0.getValueType(), N1);
        return DAG.getSetCC(dl, VT, N0.getOperand(0), NegN1, SwapCond);
      }
    }

    // If the condition is not legal, see if we can find an equivalent one
    // which is legal.
    if (!isCondCodeLegal(Cond, N0.getSimpleValueType())) {
      // If the comparison was an awkward floating-point == or != and one of
      // the comparison operands is infinity or negative infinity, convert the
      // condition to a less-awkward <= or >=.
      if (CFP->getValueAPF().isInfinity()) {
        if (CFP->getValueAPF().isNegative()) {
          if (Cond == ISD::SETOEQ &&
              isCondCodeLegal(ISD::SETOLE, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETOLE);
          if (Cond == ISD::SETUEQ &&
              isCondCodeLegal(ISD::SETOLE, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETULE);
          if (Cond == ISD::SETUNE &&
              isCondCodeLegal(ISD::SETUGT, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETUGT);
          if (Cond == ISD::SETONE &&
              isCondCodeLegal(ISD::SETUGT, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETOGT);
        } else {
          if (Cond == ISD::SETOEQ &&
              isCondCodeLegal(ISD::SETOGE, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETOGE);
          if (Cond == ISD::SETUEQ &&
              isCondCodeLegal(ISD::SETOGE, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETUGE);
          if (Cond == ISD::SETUNE &&
              isCondCodeLegal(ISD::SETULT, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETULT);
          if (Cond == ISD::SETONE &&
              isCondCodeLegal(ISD::SETULT, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETOLT);
        }
      }
    }
  }

  if (N0 == N1) {
    // The sext(setcc()) => setcc() optimization relies on the appropriate
    // constant being emitted.
    assert(!N0.getValueType().isInteger() &&
           "Integer types should be handled by FoldSetCC");

    bool EqTrue = ISD::isTrueWhenEqual(Cond);
    unsigned UOF = ISD::getUnorderedFlavor(Cond);
    if (UOF == 2) // FP operators that are undefined on NaNs.
      return DAG.getBoolConstant(EqTrue, dl, VT, OpVT);
    if (UOF == unsigned(EqTrue))
      return DAG.getBoolConstant(EqTrue, dl, VT, OpVT);
    // Otherwise, we can't fold it.  However, we can simplify it to SETUO/SETO
    // if it is not already.
    ISD::CondCode NewCond = UOF == 0 ? ISD::SETO : ISD::SETUO;
    if (NewCond != Cond &&
        (DCI.isBeforeLegalizeOps() ||
                            isCondCodeLegal(NewCond, N0.getSimpleValueType())))
      return DAG.getSetCC(dl, VT, N0, N1, NewCond);
  }

  if ((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
      N0.getValueType().isInteger()) {
    if (N0.getOpcode() == ISD::ADD || N0.getOpcode() == ISD::SUB ||
        N0.getOpcode() == ISD::XOR) {
      // Simplify (X+Y) == (X+Z) -->  Y == Z
      if (N0.getOpcode() == N1.getOpcode()) {
        if (N0.getOperand(0) == N1.getOperand(0))
          return DAG.getSetCC(dl, VT, N0.getOperand(1), N1.getOperand(1), Cond);
        if (N0.getOperand(1) == N1.getOperand(1))
          return DAG.getSetCC(dl, VT, N0.getOperand(0), N1.getOperand(0), Cond);
        if (isCommutativeBinOp(N0.getOpcode())) {
          // If X op Y == Y op X, try other combinations.
          if (N0.getOperand(0) == N1.getOperand(1))
            return DAG.getSetCC(dl, VT, N0.getOperand(1), N1.getOperand(0),
                                Cond);
          if (N0.getOperand(1) == N1.getOperand(0))
            return DAG.getSetCC(dl, VT, N0.getOperand(0), N1.getOperand(1),
                                Cond);
        }
      }

      // If RHS is a legal immediate value for a compare instruction, we need
      // to be careful about increasing register pressure needlessly.
      bool LegalRHSImm = false;

      if (auto *RHSC = dyn_cast<ConstantSDNode>(N1)) {
        if (auto *LHSR = dyn_cast<ConstantSDNode>(N0.getOperand(1))) {
          // Turn (X+C1) == C2 --> X == C2-C1
          if (N0.getOpcode() == ISD::ADD && N0.getNode()->hasOneUse()) {
            return DAG.getSetCC(dl, VT, N0.getOperand(0),
                                DAG.getConstant(RHSC->getAPIntValue()-
                                                LHSR->getAPIntValue(),
                                dl, N0.getValueType()), Cond);
          }

          // Turn (X^C1) == C2 into X == C1^C2 iff X&~C1 = 0.
          if (N0.getOpcode() == ISD::XOR)
            // If we know that all of the inverted bits are zero, don't bother
            // performing the inversion.
            if (DAG.MaskedValueIsZero(N0.getOperand(0), ~LHSR->getAPIntValue()))
              return
                DAG.getSetCC(dl, VT, N0.getOperand(0),
                             DAG.getConstant(LHSR->getAPIntValue() ^
                                               RHSC->getAPIntValue(),
                                             dl, N0.getValueType()),
                             Cond);
        }

        // Turn (C1-X) == C2 --> X == C1-C2
        if (auto *SUBC = dyn_cast<ConstantSDNode>(N0.getOperand(0))) {
          if (N0.getOpcode() == ISD::SUB && N0.getNode()->hasOneUse()) {
            return
              DAG.getSetCC(dl, VT, N0.getOperand(1),
                           DAG.getConstant(SUBC->getAPIntValue() -
                                             RHSC->getAPIntValue(),
                                           dl, N0.getValueType()),
                           Cond);
          }
        }

        // Could RHSC fold directly into a compare?
        if (RHSC->getValueType(0).getSizeInBits() <= 64)
          LegalRHSImm = isLegalICmpImmediate(RHSC->getSExtValue());
      }

      // (X+Y) == X --> Y == 0 and similar folds.
      // Don't do this if X is an immediate that can fold into a cmp
      // instruction and X+Y has other uses. It could be an induction variable
      // chain, and the transform would increase register pressure.
      if (!LegalRHSImm || N0.hasOneUse())
        if (SDValue V = foldSetCCWithBinOp(VT, N0, N1, Cond, dl, DCI))
          return V;
    }

    if (N1.getOpcode() == ISD::ADD || N1.getOpcode() == ISD::SUB ||
        N1.getOpcode() == ISD::XOR)
      if (SDValue V = foldSetCCWithBinOp(VT, N1, N0, Cond, dl, DCI))
        return V;

    if (SDValue V = foldSetCCWithAnd(VT, N0, N1, Cond, dl, DCI))
      return V;
  }

  // Fold remainder of division by a constant.
  if ((N0.getOpcode() == ISD::UREM || N0.getOpcode() == ISD::SREM) &&
      N0.hasOneUse() && (Cond == ISD::SETEQ || Cond == ISD::SETNE)) {
    AttributeList Attr = DAG.getMachineFunction().getFunction().getAttributes();

    // When division is cheap or optimizing for minimum size,
    // fall through to DIVREM creation by skipping this fold.
    if (!isIntDivCheap(VT, Attr) && !Attr.hasFnAttribute(Attribute::MinSize)) {
      if (N0.getOpcode() == ISD::UREM) {
        if (SDValue Folded = buildUREMEqFold(VT, N0, N1, Cond, DCI, dl))
          return Folded;
      } else if (N0.getOpcode() == ISD::SREM) {
        if (SDValue Folded = buildSREMEqFold(VT, N0, N1, Cond, DCI, dl))
          return Folded;
      }
    }
  }

  // Fold away ALL boolean setcc's.
  if (N0.getValueType().getScalarType() == MVT::i1 && foldBooleans) {
    SDValue Temp;
    switch (Cond) {
    default: llvm_unreachable("Unknown integer setcc!");
    case ISD::SETEQ:  // X == Y  -> ~(X^Y)
      Temp = DAG.getNode(ISD::XOR, dl, OpVT, N0, N1);
      N0 = DAG.getNOT(dl, Temp, OpVT);
      if (!DCI.isCalledByLegalizer())
        DCI.AddToWorklist(Temp.getNode());
      break;
    case ISD::SETNE:  // X != Y   -->  (X^Y)
      N0 = DAG.getNode(ISD::XOR, dl, OpVT, N0, N1);
      break;
    case ISD::SETGT:  // X >s Y   -->  X == 0 & Y == 1  -->  ~X & Y
    case ISD::SETULT: // X <u Y   -->  X == 0 & Y == 1  -->  ~X & Y
      Temp = DAG.getNOT(dl, N0, OpVT);
      N0 = DAG.getNode(ISD::AND, dl, OpVT, N1, Temp);
      if (!DCI.isCalledByLegalizer())
        DCI.AddToWorklist(Temp.getNode());
      break;
    case ISD::SETLT:  // X <s Y   --> X == 1 & Y == 0  -->  ~Y & X
    case ISD::SETUGT: // X >u Y   --> X == 1 & Y == 0  -->  ~Y & X
      Temp = DAG.getNOT(dl, N1, OpVT);
      N0 = DAG.getNode(ISD::AND, dl, OpVT, N0, Temp);
      if (!DCI.isCalledByLegalizer())
        DCI.AddToWorklist(Temp.getNode());
      break;
    case ISD::SETULE: // X <=u Y  --> X == 0 | Y == 1  -->  ~X | Y
    case ISD::SETGE:  // X >=s Y  --> X == 0 | Y == 1  -->  ~X | Y
      Temp = DAG.getNOT(dl, N0, OpVT);
      N0 = DAG.getNode(ISD::OR, dl, OpVT, N1, Temp);
      if (!DCI.isCalledByLegalizer())
        DCI.AddToWorklist(Temp.getNode());
      break;
    case ISD::SETUGE: // X >=u Y  --> X == 1 | Y == 0  -->  ~Y | X
    case ISD::SETLE:  // X <=s Y  --> X == 1 | Y == 0  -->  ~Y | X
      Temp = DAG.getNOT(dl, N1, OpVT);
      N0 = DAG.getNode(ISD::OR, dl, OpVT, N0, Temp);
      break;
    }
    if (VT.getScalarType() != MVT::i1) {
      if (!DCI.isCalledByLegalizer())
        DCI.AddToWorklist(N0.getNode());
      // FIXME: If running after legalize, we probably can't do this.
      ISD::NodeType ExtendCode = getExtendForContent(getBooleanContents(OpVT));
      N0 = DAG.getNode(ExtendCode, dl, VT, N0);
    }
    return N0;
  }

  // Could not fold it.
  return SDValue();
}

/// Returns true (and the GlobalValue and the offset) if the node is a
/// GlobalAddress + offset.
bool TargetLowering::isGAPlusOffset(SDNode *WN, const GlobalValue *&GA,
                                    int64_t &Offset) const {

  SDNode *N = unwrapAddress(SDValue(WN, 0)).getNode();

  if (auto *GASD = dyn_cast<GlobalAddressSDNode>(N)) {
    GA = GASD->getGlobal();
    Offset += GASD->getOffset();
    return true;
  }

  if (N->getOpcode() == ISD::ADD) {
    SDValue N1 = N->getOperand(0);
    SDValue N2 = N->getOperand(1);
    if (isGAPlusOffset(N1.getNode(), GA, Offset)) {
      if (auto *V = dyn_cast<ConstantSDNode>(N2)) {
        Offset += V->getSExtValue();
        return true;
      }
    } else if (isGAPlusOffset(N2.getNode(), GA, Offset)) {
      if (auto *V = dyn_cast<ConstantSDNode>(N1)) {
        Offset += V->getSExtValue();
        return true;
      }
    }
  }

  return false;
}

SDValue TargetLowering::PerformDAGCombine(SDNode *N,
                                          DAGCombinerInfo &DCI) const {
  // Default implementation: no optimization.
  return SDValue();
}

//===----------------------------------------------------------------------===//
//  Inline Assembler Implementation Methods
//===----------------------------------------------------------------------===//

TargetLowering::ConstraintType
TargetLowering::getConstraintType(StringRef Constraint) const {
  unsigned S = Constraint.size();

  if (S == 1) {
    switch (Constraint[0]) {
    default: break;
    case 'r':
      return C_RegisterClass;
    case 'm': // memory
    case 'o': // offsetable
    case 'V': // not offsetable
      return C_Memory;
    case 'n': // Simple Integer
    case 'E': // Floating Point Constant
    case 'F': // Floating Point Constant
      return C_Immediate;
    case 'i': // Simple Integer or Relocatable Constant
    case 's': // Relocatable Constant
    case 'p': // Address.
    case 'X': // Allow ANY value.
    case 'I': // Target registers.
    case 'J':
    case 'K':
    case 'L':
    case 'M':
    case 'N':
    case 'O':
    case 'P':
    case '<':
    case '>':
      return C_Other;
    }
  }

  if (S > 1 && Constraint[0] == '{' && Constraint[S - 1] == '}') {
    if (S == 8 && Constraint.substr(1, 6) == "memory") // "{memory}"
      return C_Memory;
    return C_Register;
  }
  return C_Unknown;
}

/// Try to replace an X constraint, which matches anything, with another that
/// has more specific requirements based on the type of the corresponding
/// operand.
const char *TargetLowering::LowerXConstraint(EVT ConstraintVT) const {
  if (ConstraintVT.isInteger())
    return "r";
  if (ConstraintVT.isFloatingPoint())
    return "f"; // works for many targets
  return nullptr;
}

SDValue TargetLowering::LowerAsmOutputForConstraint(
    SDValue &Chain, SDValue &Flag, SDLoc DL, const AsmOperandInfo &OpInfo,
    SelectionDAG &DAG) const {
  return SDValue();
}

/// Lower the specified operand into the Ops vector.
/// If it is invalid, don't add anything to Ops.
void TargetLowering::LowerAsmOperandForConstraint(SDValue Op,
                                                  std::string &Constraint,
                                                  std::vector<SDValue> &Ops,
                                                  SelectionDAG &DAG) const {

  if (Constraint.length() > 1) return;

  char ConstraintLetter = Constraint[0];
  switch (ConstraintLetter) {
  default: break;
  case 'X':     // Allows any operand; labels (basic block) use this.
    if (Op.getOpcode() == ISD::BasicBlock ||
        Op.getOpcode() == ISD::TargetBlockAddress) {
      Ops.push_back(Op);
      return;
    }
    LLVM_FALLTHROUGH;
  case 'i':    // Simple Integer or Relocatable Constant
  case 'n':    // Simple Integer
  case 's': {  // Relocatable Constant

    GlobalAddressSDNode *GA;
    ConstantSDNode *C;
    BlockAddressSDNode *BA;
    uint64_t Offset = 0;

    // Match (GA) or (C) or (GA+C) or (GA-C) or ((GA+C)+C) or (((GA+C)+C)+C),
    // etc., since getelementpointer is variadic. We can't use
    // SelectionDAG::FoldSymbolOffset because it expects the GA to be accessible
    // while in this case the GA may be furthest from the root node which is
    // likely an ISD::ADD.
    while (1) {
      if ((GA = dyn_cast<GlobalAddressSDNode>(Op)) && ConstraintLetter != 'n') {
        Ops.push_back(DAG.getTargetGlobalAddress(GA->getGlobal(), SDLoc(Op),
                                                 GA->getValueType(0),
                                                 Offset + GA->getOffset()));
        return;
      } else if ((C = dyn_cast<ConstantSDNode>(Op)) &&
                 ConstraintLetter != 's') {
        // gcc prints these as sign extended.  Sign extend value to 64 bits
        // now; without this it would get ZExt'd later in
        // ScheduleDAGSDNodes::EmitNode, which is very generic.
        bool IsBool = C->getConstantIntValue()->getBitWidth() == 1;
        BooleanContent BCont = getBooleanContents(MVT::i64);
        ISD::NodeType ExtOpc = IsBool ? getExtendForContent(BCont)
                                      : ISD::SIGN_EXTEND;
        int64_t ExtVal = ExtOpc == ISD::ZERO_EXTEND ? C->getZExtValue()
                                                    : C->getSExtValue();
        Ops.push_back(DAG.getTargetConstant(Offset + ExtVal,
                                            SDLoc(C), MVT::i64));
        return;
      } else if ((BA = dyn_cast<BlockAddressSDNode>(Op)) &&
                 ConstraintLetter != 'n') {
        Ops.push_back(DAG.getTargetBlockAddress(
            BA->getBlockAddress(), BA->getValueType(0),
            Offset + BA->getOffset(), BA->getTargetFlags()));
        return;
      } else {
        const unsigned OpCode = Op.getOpcode();
        if (OpCode == ISD::ADD || OpCode == ISD::SUB) {
          if ((C = dyn_cast<ConstantSDNode>(Op.getOperand(0))))
            Op = Op.getOperand(1);
          // Subtraction is not commutative.
          else if (OpCode == ISD::ADD &&
                   (C = dyn_cast<ConstantSDNode>(Op.getOperand(1))))
            Op = Op.getOperand(0);
          else
            return;
          Offset += (OpCode == ISD::ADD ? 1 : -1) * C->getSExtValue();
          continue;
        }
      }
      return;
    }
    break;
  }
  }
}

std::pair<unsigned, const TargetRegisterClass *>
TargetLowering::getRegForInlineAsmConstraint(const TargetRegisterInfo *RI,
                                             StringRef Constraint,
                                             MVT VT) const {
  if (Constraint.empty() || Constraint[0] != '{')
    return std::make_pair(0u, static_cast<TargetRegisterClass *>(nullptr));
  assert(*(Constraint.end() - 1) == '}' && "Not a brace enclosed constraint?");

  // Remove the braces from around the name.
  StringRef RegName(Constraint.data() + 1, Constraint.size() - 2);

  std::pair<unsigned, const TargetRegisterClass *> R =
      std::make_pair(0u, static_cast<const TargetRegisterClass *>(nullptr));

  // Figure out which register class contains this reg.
  for (const TargetRegisterClass *RC : RI->regclasses()) {
    // If none of the value types for this register class are valid, we
    // can't use it.  For example, 64-bit reg classes on 32-bit targets.
    if (!isLegalRC(*RI, *RC))
      continue;

    for (TargetRegisterClass::iterator I = RC->begin(), E = RC->end();
         I != E; ++I) {
      if (RegName.equals_lower(RI->getRegAsmName(*I))) {
        std::pair<unsigned, const TargetRegisterClass *> S =
            std::make_pair(*I, RC);

        // If this register class has the requested value type, return it,
        // otherwise keep searching and return the first class found
        // if no other is found which explicitly has the requested type.
        if (RI->isTypeLegalForClass(*RC, VT))
          return S;
        if (!R.second)
          R = S;
      }
    }
  }

  return R;
}

//===----------------------------------------------------------------------===//
// Constraint Selection.

/// Return true of this is an input operand that is a matching constraint like
/// "4".
bool TargetLowering::AsmOperandInfo::isMatchingInputConstraint() const {
  assert(!ConstraintCode.empty() && "No known constraint!");
  return isdigit(static_cast<unsigned char>(ConstraintCode[0]));
}

/// If this is an input matching constraint, this method returns the output
/// operand it matches.
unsigned TargetLowering::AsmOperandInfo::getMatchedOperand() const {
  assert(!ConstraintCode.empty() && "No known constraint!");
  return atoi(ConstraintCode.c_str());
}

/// Split up the constraint string from the inline assembly value into the
/// specific constraints and their prefixes, and also tie in the associated
/// operand values.
/// If this returns an empty vector, and if the constraint string itself
/// isn't empty, there was an error parsing.
TargetLowering::AsmOperandInfoVector
TargetLowering::ParseConstraints(const DataLayout &DL,
                                 const TargetRegisterInfo *TRI,
                                 ImmutableCallSite CS) const {
  /// Information about all of the constraints.
  AsmOperandInfoVector ConstraintOperands;
  const InlineAsm *IA = cast<InlineAsm>(CS.getCalledValue());
  unsigned maCount = 0; // Largest number of multiple alternative constraints.

  // Do a prepass over the constraints, canonicalizing them, and building up the
  // ConstraintOperands list.
  unsigned ArgNo = 0; // ArgNo - The argument of the CallInst.
  unsigned ResNo = 0; // ResNo - The result number of the next output.

  for (InlineAsm::ConstraintInfo &CI : IA->ParseConstraints()) {
    ConstraintOperands.emplace_back(std::move(CI));
    AsmOperandInfo &OpInfo = ConstraintOperands.back();

    // Update multiple alternative constraint count.
    if (OpInfo.multipleAlternatives.size() > maCount)
      maCount = OpInfo.multipleAlternatives.size();

    OpInfo.ConstraintVT = MVT::Other;

    // Compute the value type for each operand.
    switch (OpInfo.Type) {
    case InlineAsm::isOutput:
      // Indirect outputs just consume an argument.
      if (OpInfo.isIndirect) {
        OpInfo.CallOperandVal = const_cast<Value *>(CS.getArgument(ArgNo++));
        break;
      }

      // The return value of the call is this value.  As such, there is no
      // corresponding argument.
      assert(!CS.getType()->isVoidTy() &&
             "Bad inline asm!");
      if (StructType *STy = dyn_cast<StructType>(CS.getType())) {
        OpInfo.ConstraintVT =
            getSimpleValueType(DL, STy->getElementType(ResNo));
      } else {
        assert(ResNo == 0 && "Asm only has one result!");
        OpInfo.ConstraintVT = getSimpleValueType(DL, CS.getType());
      }
      ++ResNo;
      break;
    case InlineAsm::isInput:
      OpInfo.CallOperandVal = const_cast<Value *>(CS.getArgument(ArgNo++));
      break;
    case InlineAsm::isClobber:
      // Nothing to do.
      break;
    }

    if (OpInfo.CallOperandVal) {
      llvm::Type *OpTy = OpInfo.CallOperandVal->getType();
      if (OpInfo.isIndirect) {
        llvm::PointerType *PtrTy = dyn_cast<PointerType>(OpTy);
        if (!PtrTy)
          report_fatal_error("Indirect operand for inline asm not a pointer!");
        OpTy = PtrTy->getElementType();
      }

      // Look for vector wrapped in a struct. e.g. { <16 x i8> }.
      if (StructType *STy = dyn_cast<StructType>(OpTy))
        if (STy->getNumElements() == 1)
          OpTy = STy->getElementType(0);

      // If OpTy is not a single value, it may be a struct/union that we
      // can tile with integers.
      if (!OpTy->isSingleValueType() && OpTy->isSized()) {
        unsigned BitSize = DL.getTypeSizeInBits(OpTy);
        switch (BitSize) {
        default: break;
        case 1:
        case 8:
        case 16:
        case 32:
        case 64:
        case 128:
          OpInfo.ConstraintVT =
              MVT::getVT(IntegerType::get(OpTy->getContext(), BitSize), true);
          break;
        }
      } else if (PointerType *PT = dyn_cast<PointerType>(OpTy)) {
        unsigned PtrSize = DL.getPointerSizeInBits(PT->getAddressSpace());
        OpInfo.ConstraintVT = MVT::getIntegerVT(PtrSize);
      } else {
        OpInfo.ConstraintVT = MVT::getVT(OpTy, true);
      }
    }
  }

  // If we have multiple alternative constraints, select the best alternative.
  if (!ConstraintOperands.empty()) {
    if (maCount) {
      unsigned bestMAIndex = 0;
      int bestWeight = -1;
      // weight:  -1 = invalid match, and 0 = so-so match to 5 = good match.
      int weight = -1;
      unsigned maIndex;
      // Compute the sums of the weights for each alternative, keeping track
      // of the best (highest weight) one so far.
      for (maIndex = 0; maIndex < maCount; ++maIndex) {
        int weightSum = 0;
        for (unsigned cIndex = 0, eIndex = ConstraintOperands.size();
             cIndex != eIndex; ++cIndex) {
          AsmOperandInfo &OpInfo = ConstraintOperands[cIndex];
          if (OpInfo.Type == InlineAsm::isClobber)
            continue;

          // If this is an output operand with a matching input operand,
          // look up the matching input. If their types mismatch, e.g. one
          // is an integer, the other is floating point, or their sizes are
          // different, flag it as an maCantMatch.
          if (OpInfo.hasMatchingInput()) {
            AsmOperandInfo &Input = ConstraintOperands[OpInfo.MatchingInput];
            if (OpInfo.ConstraintVT != Input.ConstraintVT) {
              if ((OpInfo.ConstraintVT.isInteger() !=
                   Input.ConstraintVT.isInteger()) ||
                  (OpInfo.ConstraintVT.getSizeInBits() !=
                   Input.ConstraintVT.getSizeInBits())) {
                weightSum = -1; // Can't match.
                break;
              }
            }
          }
          weight = getMultipleConstraintMatchWeight(OpInfo, maIndex);
          if (weight == -1) {
            weightSum = -1;
            break;
          }
          weightSum += weight;
        }
        // Update best.
        if (weightSum > bestWeight) {
          bestWeight = weightSum;
          bestMAIndex = maIndex;
        }
      }

      // Now select chosen alternative in each constraint.
      for (unsigned cIndex = 0, eIndex = ConstraintOperands.size();
           cIndex != eIndex; ++cIndex) {
        AsmOperandInfo &cInfo = ConstraintOperands[cIndex];
        if (cInfo.Type == InlineAsm::isClobber)
          continue;
        cInfo.selectAlternative(bestMAIndex);
      }
    }
  }

  // Check and hook up tied operands, choose constraint code to use.
  for (unsigned cIndex = 0, eIndex = ConstraintOperands.size();
       cIndex != eIndex; ++cIndex) {
    AsmOperandInfo &OpInfo = ConstraintOperands[cIndex];

    // If this is an output operand with a matching input operand, look up the
    // matching input. If their types mismatch, e.g. one is an integer, the
    // other is floating point, or their sizes are different, flag it as an
    // error.
    if (OpInfo.hasMatchingInput()) {
      AsmOperandInfo &Input = ConstraintOperands[OpInfo.MatchingInput];

      if (OpInfo.ConstraintVT != Input.ConstraintVT) {
        std::pair<unsigned, const TargetRegisterClass *> MatchRC =
            getRegForInlineAsmConstraint(TRI, OpInfo.ConstraintCode,
                                         OpInfo.ConstraintVT);
        std::pair<unsigned, const TargetRegisterClass *> InputRC =
            getRegForInlineAsmConstraint(TRI, Input.ConstraintCode,
                                         Input.ConstraintVT);
        if ((OpInfo.ConstraintVT.isInteger() !=
             Input.ConstraintVT.isInteger()) ||
            (MatchRC.second != InputRC.second)) {
          report_fatal_error("Unsupported asm: input constraint"
                             " with a matching output constraint of"
                             " incompatible type!");
        }
      }
    }
  }

  return ConstraintOperands;
}

/// Return an integer indicating how general CT is.
static unsigned getConstraintGenerality(TargetLowering::ConstraintType CT) {
  switch (CT) {
  case TargetLowering::C_Immediate:
  case TargetLowering::C_Other:
  case TargetLowering::C_Unknown:
    return 0;
  case TargetLowering::C_Register:
    return 1;
  case TargetLowering::C_RegisterClass:
    return 2;
  case TargetLowering::C_Memory:
    return 3;
  }
  llvm_unreachable("Invalid constraint type");
}

/// Examine constraint type and operand type and determine a weight value.
/// This object must already have been set up with the operand type
/// and the current alternative constraint selected.
TargetLowering::ConstraintWeight
  TargetLowering::getMultipleConstraintMatchWeight(
    AsmOperandInfo &info, int maIndex) const {
  InlineAsm::ConstraintCodeVector *rCodes;
  if (maIndex >= (int)info.multipleAlternatives.size())
    rCodes = &info.Codes;
  else
    rCodes = &info.multipleAlternatives[maIndex].Codes;
  ConstraintWeight BestWeight = CW_Invalid;

  // Loop over the options, keeping track of the most general one.
  for (unsigned i = 0, e = rCodes->size(); i != e; ++i) {
    ConstraintWeight weight =
      getSingleConstraintMatchWeight(info, (*rCodes)[i].c_str());
    if (weight > BestWeight)
      BestWeight = weight;
  }

  return BestWeight;
}

/// Examine constraint type and operand type and determine a weight value.
/// This object must already have been set up with the operand type
/// and the current alternative constraint selected.
TargetLowering::ConstraintWeight
  TargetLowering::getSingleConstraintMatchWeight(
    AsmOperandInfo &info, const char *constraint) const {
  ConstraintWeight weight = CW_Invalid;
  Value *CallOperandVal = info.CallOperandVal;
    // If we don't have a value, we can't do a match,
    // but allow it at the lowest weight.
  if (!CallOperandVal)
    return CW_Default;
  // Look at the constraint type.
  switch (*constraint) {
    case 'i': // immediate integer.
    case 'n': // immediate integer with a known value.
      if (isa<ConstantInt>(CallOperandVal))
        weight = CW_Constant;
      break;
    case 's': // non-explicit intregal immediate.
      if (isa<GlobalValue>(CallOperandVal))
        weight = CW_Constant;
      break;
    case 'E': // immediate float if host format.
    case 'F': // immediate float.
      if (isa<ConstantFP>(CallOperandVal))
        weight = CW_Constant;
      break;
    case '<': // memory operand with autodecrement.
    case '>': // memory operand with autoincrement.
    case 'm': // memory operand.
    case 'o': // offsettable memory operand
    case 'V': // non-offsettable memory operand
      weight = CW_Memory;
      break;
    case 'r': // general register.
    case 'g': // general register, memory operand or immediate integer.
              // note: Clang converts "g" to "imr".
      if (CallOperandVal->getType()->isIntegerTy())
        weight = CW_Register;
      break;
    case 'X': // any operand.
  default:
    weight = CW_Default;
    break;
  }
  return weight;
}

/// If there are multiple different constraints that we could pick for this
/// operand (e.g. "imr") try to pick the 'best' one.
/// This is somewhat tricky: constraints fall into four classes:
///    Other         -> immediates and magic values
///    Register      -> one specific register
///    RegisterClass -> a group of regs
///    Memory        -> memory
/// Ideally, we would pick the most specific constraint possible: if we have
/// something that fits into a register, we would pick it.  The problem here
/// is that if we have something that could either be in a register or in
/// memory that use of the register could cause selection of *other*
/// operands to fail: they might only succeed if we pick memory.  Because of
/// this the heuristic we use is:
///
///  1) If there is an 'other' constraint, and if the operand is valid for
///     that constraint, use it.  This makes us take advantage of 'i'
///     constraints when available.
///  2) Otherwise, pick the most general constraint present.  This prefers
///     'm' over 'r', for example.
///
static void ChooseConstraint(TargetLowering::AsmOperandInfo &OpInfo,
                             const TargetLowering &TLI,
                             SDValue Op, SelectionDAG *DAG) {
  assert(OpInfo.Codes.size() > 1 && "Doesn't have multiple constraint options");
  unsigned BestIdx = 0;
  TargetLowering::ConstraintType BestType = TargetLowering::C_Unknown;
  int BestGenerality = -1;

  // Loop over the options, keeping track of the most general one.
  for (unsigned i = 0, e = OpInfo.Codes.size(); i != e; ++i) {
    TargetLowering::ConstraintType CType =
      TLI.getConstraintType(OpInfo.Codes[i]);

    // If this is an 'other' or 'immediate' constraint, see if the operand is
    // valid for it. For example, on X86 we might have an 'rI' constraint. If
    // the operand is an integer in the range [0..31] we want to use I (saving a
    // load of a register), otherwise we must use 'r'.
    if ((CType == TargetLowering::C_Other ||
         CType == TargetLowering::C_Immediate) && Op.getNode()) {
      assert(OpInfo.Codes[i].size() == 1 &&
             "Unhandled multi-letter 'other' constraint");
      std::vector<SDValue> ResultOps;
      TLI.LowerAsmOperandForConstraint(Op, OpInfo.Codes[i],
                                       ResultOps, *DAG);
      if (!ResultOps.empty()) {
        BestType = CType;
        BestIdx = i;
        break;
      }
    }

    // Things with matching constraints can only be registers, per gcc
    // documentation.  This mainly affects "g" constraints.
    if (CType == TargetLowering::C_Memory && OpInfo.hasMatchingInput())
      continue;

    // This constraint letter is more general than the previous one, use it.
    int Generality = getConstraintGenerality(CType);
    if (Generality > BestGenerality) {
      BestType = CType;
      BestIdx = i;
      BestGenerality = Generality;
    }
  }

  OpInfo.ConstraintCode = OpInfo.Codes[BestIdx];
  OpInfo.ConstraintType = BestType;
}

/// Determines the constraint code and constraint type to use for the specific
/// AsmOperandInfo, setting OpInfo.ConstraintCode and OpInfo.ConstraintType.
void TargetLowering::ComputeConstraintToUse(AsmOperandInfo &OpInfo,
                                            SDValue Op,
                                            SelectionDAG *DAG) const {
  assert(!OpInfo.Codes.empty() && "Must have at least one constraint");

  // Single-letter constraints ('r') are very common.
  if (OpInfo.Codes.size() == 1) {
    OpInfo.ConstraintCode = OpInfo.Codes[0];
    OpInfo.ConstraintType = getConstraintType(OpInfo.ConstraintCode);
  } else {
    ChooseConstraint(OpInfo, *this, Op, DAG);
  }

  // 'X' matches anything.
  if (OpInfo.ConstraintCode == "X" && OpInfo.CallOperandVal) {
    // Labels and constants are handled elsewhere ('X' is the only thing
    // that matches labels).  For Functions, the type here is the type of
    // the result, which is not what we want to look at; leave them alone.
    Value *v = OpInfo.CallOperandVal;
    if (isa<BasicBlock>(v) || isa<ConstantInt>(v) || isa<Function>(v)) {
      OpInfo.CallOperandVal = v;
      return;
    }

    if (Op.getNode() && Op.getOpcode() == ISD::TargetBlockAddress)
      return;

    // Otherwise, try to resolve it to something we know about by looking at
    // the actual operand type.
    if (const char *Repl = LowerXConstraint(OpInfo.ConstraintVT)) {
      OpInfo.ConstraintCode = Repl;
      OpInfo.ConstraintType = getConstraintType(OpInfo.ConstraintCode);
    }
  }
}

/// Given an exact SDIV by a constant, create a multiplication
/// with the multiplicative inverse of the constant.
static SDValue BuildExactSDIV(const TargetLowering &TLI, SDNode *N,
                              const SDLoc &dl, SelectionDAG &DAG,
                              SmallVectorImpl<SDNode *> &Created) {
  SDValue Op0 = N->getOperand(0);
  SDValue Op1 = N->getOperand(1);
  EVT VT = N->getValueType(0);
  EVT SVT = VT.getScalarType();
  EVT ShVT = TLI.getShiftAmountTy(VT, DAG.getDataLayout());
  EVT ShSVT = ShVT.getScalarType();

  bool UseSRA = false;
  SmallVector<SDValue, 16> Shifts, Factors;

  auto BuildSDIVPattern = [&](ConstantSDNode *C) {
    if (C->isNullValue())
      return false;
    APInt Divisor = C->getAPIntValue();
    unsigned Shift = Divisor.countTrailingZeros();
    if (Shift) {
      Divisor.ashrInPlace(Shift);
      UseSRA = true;
    }
    // Calculate the multiplicative inverse, using Newton's method.
    APInt t;
    APInt Factor = Divisor;
    while ((t = Divisor * Factor) != 1)
      Factor *= APInt(Divisor.getBitWidth(), 2) - t;
    Shifts.push_back(DAG.getConstant(Shift, dl, ShSVT));
    Factors.push_back(DAG.getConstant(Factor, dl, SVT));
    return true;
  };

  // Collect all magic values from the build vector.
  if (!ISD::matchUnaryPredicate(Op1, BuildSDIVPattern))
    return SDValue();

  SDValue Shift, Factor;
  if (VT.isVector()) {
    Shift = DAG.getBuildVector(ShVT, dl, Shifts);
    Factor = DAG.getBuildVector(VT, dl, Factors);
  } else {
    Shift = Shifts[0];
    Factor = Factors[0];
  }

  SDValue Res = Op0;

  // Shift the value upfront if it is even, so the LSB is one.
  if (UseSRA) {
    // TODO: For UDIV use SRL instead of SRA.
    SDNodeFlags Flags;
    Flags.setExact(true);
    Res = DAG.getNode(ISD::SRA, dl, VT, Res, Shift, Flags);
    Created.push_back(Res.getNode());
  }

  return DAG.getNode(ISD::MUL, dl, VT, Res, Factor);
}

SDValue TargetLowering::BuildSDIVPow2(SDNode *N, const APInt &Divisor,
                              SelectionDAG &DAG,
                              SmallVectorImpl<SDNode *> &Created) const {
  AttributeList Attr = DAG.getMachineFunction().getFunction().getAttributes();
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  if (TLI.isIntDivCheap(N->getValueType(0), Attr))
    return SDValue(N, 0); // Lower SDIV as SDIV
  return SDValue();
}

/// Given an ISD::SDIV node expressing a divide by constant,
/// return a DAG expression to select that will generate the same value by
/// multiplying by a magic number.
/// Ref: "Hacker's Delight" or "The PowerPC Compiler Writer's Guide".
SDValue TargetLowering::BuildSDIV(SDNode *N, SelectionDAG &DAG,
                                  bool IsAfterLegalization,
                                  SmallVectorImpl<SDNode *> &Created) const {
  SDLoc dl(N);
  EVT VT = N->getValueType(0);
  EVT SVT = VT.getScalarType();
  EVT ShVT = getShiftAmountTy(VT, DAG.getDataLayout());
  EVT ShSVT = ShVT.getScalarType();
  unsigned EltBits = VT.getScalarSizeInBits();

  // Check to see if we can do this.
  // FIXME: We should be more aggressive here.
  if (!isTypeLegal(VT))
    return SDValue();

  // If the sdiv has an 'exact' bit we can use a simpler lowering.
  if (N->getFlags().hasExact())
    return BuildExactSDIV(*this, N, dl, DAG, Created);

  SmallVector<SDValue, 16> MagicFactors, Factors, Shifts, ShiftMasks;

  auto BuildSDIVPattern = [&](ConstantSDNode *C) {
    if (C->isNullValue())
      return false;

    const APInt &Divisor = C->getAPIntValue();
    APInt::ms magics = Divisor.magic();
    int NumeratorFactor = 0;
    int ShiftMask = -1;

    if (Divisor.isOneValue() || Divisor.isAllOnesValue()) {
      // If d is +1/-1, we just multiply the numerator by +1/-1.
      NumeratorFactor = Divisor.getSExtValue();
      magics.m = 0;
      magics.s = 0;
      ShiftMask = 0;
    } else if (Divisor.isStrictlyPositive() && magics.m.isNegative()) {
      // If d > 0 and m < 0, add the numerator.
      NumeratorFactor = 1;
    } else if (Divisor.isNegative() && magics.m.isStrictlyPositive()) {
      // If d < 0 and m > 0, subtract the numerator.
      NumeratorFactor = -1;
    }

    MagicFactors.push_back(DAG.getConstant(magics.m, dl, SVT));
    Factors.push_back(DAG.getConstant(NumeratorFactor, dl, SVT));
    Shifts.push_back(DAG.getConstant(magics.s, dl, ShSVT));
    ShiftMasks.push_back(DAG.getConstant(ShiftMask, dl, SVT));
    return true;
  };

  SDValue N0 = N->getOperand(0);
  SDValue N1 = N->getOperand(1);

  // Collect the shifts / magic values from each element.
  if (!ISD::matchUnaryPredicate(N1, BuildSDIVPattern))
    return SDValue();

  SDValue MagicFactor, Factor, Shift, ShiftMask;
  if (VT.isVector()) {
    MagicFactor = DAG.getBuildVector(VT, dl, MagicFactors);
    Factor = DAG.getBuildVector(VT, dl, Factors);
    Shift = DAG.getBuildVector(ShVT, dl, Shifts);
    ShiftMask = DAG.getBuildVector(VT, dl, ShiftMasks);
  } else {
    MagicFactor = MagicFactors[0];
    Factor = Factors[0];
    Shift = Shifts[0];
    ShiftMask = ShiftMasks[0];
  }

  // Multiply the numerator (operand 0) by the magic value.
  // FIXME: We should support doing a MUL in a wider type.
  SDValue Q;
  if (IsAfterLegalization ? isOperationLegal(ISD::MULHS, VT)
                          : isOperationLegalOrCustom(ISD::MULHS, VT))
    Q = DAG.getNode(ISD::MULHS, dl, VT, N0, MagicFactor);
  else if (IsAfterLegalization ? isOperationLegal(ISD::SMUL_LOHI, VT)
                               : isOperationLegalOrCustom(ISD::SMUL_LOHI, VT)) {
    SDValue LoHi =
        DAG.getNode(ISD::SMUL_LOHI, dl, DAG.getVTList(VT, VT), N0, MagicFactor);
    Q = SDValue(LoHi.getNode(), 1);
  } else
    return SDValue(); // No mulhs or equivalent.
  Created.push_back(Q.getNode());

  // (Optionally) Add/subtract the numerator using Factor.
  Factor = DAG.getNode(ISD::MUL, dl, VT, N0, Factor);
  Created.push_back(Factor.getNode());
  Q = DAG.getNode(ISD::ADD, dl, VT, Q, Factor);
  Created.push_back(Q.getNode());

  // Shift right algebraic by shift value.
  Q = DAG.getNode(ISD::SRA, dl, VT, Q, Shift);
  Created.push_back(Q.getNode());

  // Extract the sign bit, mask it and add it to the quotient.
  SDValue SignShift = DAG.getConstant(EltBits - 1, dl, ShVT);
  SDValue T = DAG.getNode(ISD::SRL, dl, VT, Q, SignShift);
  Created.push_back(T.getNode());
  T = DAG.getNode(ISD::AND, dl, VT, T, ShiftMask);
  Created.push_back(T.getNode());
  return DAG.getNode(ISD::ADD, dl, VT, Q, T);
}

/// Given an ISD::UDIV node expressing a divide by constant,
/// return a DAG expression to select that will generate the same value by
/// multiplying by a magic number.
/// Ref: "Hacker's Delight" or "The PowerPC Compiler Writer's Guide".
SDValue TargetLowering::BuildUDIV(SDNode *N, SelectionDAG &DAG,
                                  bool IsAfterLegalization,
                                  SmallVectorImpl<SDNode *> &Created) const {
  SDLoc dl(N);
  EVT VT = N->getValueType(0);
  EVT SVT = VT.getScalarType();
  EVT ShVT = getShiftAmountTy(VT, DAG.getDataLayout());
  EVT ShSVT = ShVT.getScalarType();
  unsigned EltBits = VT.getScalarSizeInBits();

  // Check to see if we can do this.
  // FIXME: We should be more aggressive here.
  if (!isTypeLegal(VT))
    return SDValue();

  bool UseNPQ = false;
  SmallVector<SDValue, 16> PreShifts, PostShifts, MagicFactors, NPQFactors;

  auto BuildUDIVPattern = [&](ConstantSDNode *C) {
    if (C->isNullValue())
      return false;
    // FIXME: We should use a narrower constant when the upper
    // bits are known to be zero.
    APInt Divisor = C->getAPIntValue();
    APInt::mu magics = Divisor.magicu();
    unsigned PreShift = 0, PostShift = 0;

    // If the divisor is even, we can avoid using the expensive fixup by
    // shifting the divided value upfront.
    if (magics.a != 0 && !Divisor[0]) {
      PreShift = Divisor.countTrailingZeros();
      // Get magic number for the shifted divisor.
      magics = Divisor.lshr(PreShift).magicu(PreShift);
      assert(magics.a == 0 && "Should use cheap fixup now");
    }

    APInt Magic = magics.m;

    unsigned SelNPQ;
    if (magics.a == 0 || Divisor.isOneValue()) {
      assert(magics.s < Divisor.getBitWidth() &&
             "We shouldn't generate an undefined shift!");
      PostShift = magics.s;
      SelNPQ = false;
    } else {
      PostShift = magics.s - 1;
      SelNPQ = true;
    }

    PreShifts.push_back(DAG.getConstant(PreShift, dl, ShSVT));
    MagicFactors.push_back(DAG.getConstant(Magic, dl, SVT));
    NPQFactors.push_back(
        DAG.getConstant(SelNPQ ? APInt::getOneBitSet(EltBits, EltBits - 1)
                               : APInt::getNullValue(EltBits),
                        dl, SVT));
    PostShifts.push_back(DAG.getConstant(PostShift, dl, ShSVT));
    UseNPQ |= SelNPQ;
    return true;
  };

  SDValue N0 = N->getOperand(0);
  SDValue N1 = N->getOperand(1);

  // Collect the shifts/magic values from each element.
  if (!ISD::matchUnaryPredicate(N1, BuildUDIVPattern))
    return SDValue();

  SDValue PreShift, PostShift, MagicFactor, NPQFactor;
  if (VT.isVector()) {
    PreShift = DAG.getBuildVector(ShVT, dl, PreShifts);
    MagicFactor = DAG.getBuildVector(VT, dl, MagicFactors);
    NPQFactor = DAG.getBuildVector(VT, dl, NPQFactors);
    PostShift = DAG.getBuildVector(ShVT, dl, PostShifts);
  } else {
    PreShift = PreShifts[0];
    MagicFactor = MagicFactors[0];
    PostShift = PostShifts[0];
  }

  SDValue Q = N0;
  Q = DAG.getNode(ISD::SRL, dl, VT, Q, PreShift);
  Created.push_back(Q.getNode());

  // FIXME: We should support doing a MUL in a wider type.
  auto GetMULHU = [&](SDValue X, SDValue Y) {
    if (IsAfterLegalization ? isOperationLegal(ISD::MULHU, VT)
                            : isOperationLegalOrCustom(ISD::MULHU, VT))
      return DAG.getNode(ISD::MULHU, dl, VT, X, Y);
    if (IsAfterLegalization ? isOperationLegal(ISD::UMUL_LOHI, VT)
                            : isOperationLegalOrCustom(ISD::UMUL_LOHI, VT)) {
      SDValue LoHi =
          DAG.getNode(ISD::UMUL_LOHI, dl, DAG.getVTList(VT, VT), X, Y);
      return SDValue(LoHi.getNode(), 1);
    }
    return SDValue(); // No mulhu or equivalent
  };

  // Multiply the numerator (operand 0) by the magic value.
  Q = GetMULHU(Q, MagicFactor);
  if (!Q)
    return SDValue();

  Created.push_back(Q.getNode());

  if (UseNPQ) {
    SDValue NPQ = DAG.getNode(ISD::SUB, dl, VT, N0, Q);
    Created.push_back(NPQ.getNode());

    // For vectors we might have a mix of non-NPQ/NPQ paths, so use
    // MULHU to act as a SRL-by-1 for NPQ, else multiply by zero.
    if (VT.isVector())
      NPQ = GetMULHU(NPQ, NPQFactor);
    else
      NPQ = DAG.getNode(ISD::SRL, dl, VT, NPQ, DAG.getConstant(1, dl, ShVT));

    Created.push_back(NPQ.getNode());

    Q = DAG.getNode(ISD::ADD, dl, VT, NPQ, Q);
    Created.push_back(Q.getNode());
  }

  Q = DAG.getNode(ISD::SRL, dl, VT, Q, PostShift);
  Created.push_back(Q.getNode());

  SDValue One = DAG.getConstant(1, dl, VT);
  SDValue IsOne = DAG.getSetCC(dl, VT, N1, One, ISD::SETEQ);
  return DAG.getSelect(dl, VT, IsOne, N0, Q);
}

/// If all values in Values that *don't* match the predicate are same 'splat'
/// value, then replace all values with that splat value.
/// Else, if AlternativeReplacement was provided, then replace all values that
/// do match predicate with AlternativeReplacement value.
static void
turnVectorIntoSplatVector(MutableArrayRef<SDValue> Values,
                          std::function<bool(SDValue)> Predicate,
                          SDValue AlternativeReplacement = SDValue()) {
  SDValue Replacement;
  // Is there a value for which the Predicate does *NOT* match? What is it?
  auto SplatValue = llvm::find_if_not(Values, Predicate);
  if (SplatValue != Values.end()) {
    // Does Values consist only of SplatValue's and values matching Predicate?
    if (llvm::all_of(Values, [Predicate, SplatValue](SDValue Value) {
          return Value == *SplatValue || Predicate(Value);
        })) // Then we shall replace values matching predicate with SplatValue.
      Replacement = *SplatValue;
  }
  if (!Replacement) {
    // Oops, we did not find the "baseline" splat value.
    if (!AlternativeReplacement)
      return; // Nothing to do.
    // Let's replace with provided value then.
    Replacement = AlternativeReplacement;
  }
  std::replace_if(Values.begin(), Values.end(), Predicate, Replacement);
}

/// Given an ISD::UREM used only by an ISD::SETEQ or ISD::SETNE
/// where the divisor is constant and the comparison target is zero,
/// return a DAG expression that will generate the same comparison result
/// using only multiplications, additions and shifts/rotations.
/// Ref: "Hacker's Delight" 10-17.
SDValue TargetLowering::buildUREMEqFold(EVT SETCCVT, SDValue REMNode,
                                        SDValue CompTargetNode,
                                        ISD::CondCode Cond,
                                        DAGCombinerInfo &DCI,
                                        const SDLoc &DL) const {
  SmallVector<SDNode *, 5> Built;
  if (SDValue Folded = prepareUREMEqFold(SETCCVT, REMNode, CompTargetNode, Cond,
                                         DCI, DL, Built)) {
    for (SDNode *N : Built)
      DCI.AddToWorklist(N);
    return Folded;
  }

  return SDValue();
}

SDValue
TargetLowering::prepareUREMEqFold(EVT SETCCVT, SDValue REMNode,
                                  SDValue CompTargetNode, ISD::CondCode Cond,
                                  DAGCombinerInfo &DCI, const SDLoc &DL,
                                  SmallVectorImpl<SDNode *> &Created) const {
  // fold (seteq/ne (urem N, D), 0) -> (setule/ugt (rotr (mul N, P), K), Q)
  // - D must be constant, with D = D0 * 2^K where D0 is odd
  // - P is the multiplicative inverse of D0 modulo 2^W
  // - Q = floor(((2^W) - 1) / D)
  // where W is the width of the common type of N and D.
  assert((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
         "Only applicable for (in)equality comparisons.");

  SelectionDAG &DAG = DCI.DAG;

  EVT VT = REMNode.getValueType();
  EVT SVT = VT.getScalarType();
  EVT ShVT = getShiftAmountTy(VT, DAG.getDataLayout());
  EVT ShSVT = ShVT.getScalarType();

  // If MUL is unavailable, we cannot proceed in any case.
  if (!isOperationLegalOrCustom(ISD::MUL, VT))
    return SDValue();

  bool ComparingWithAllZeros = true;
  bool AllComparisonsWithNonZerosAreTautological = true;
  bool HadTautologicalLanes = false;
  bool AllLanesAreTautological = true;
  bool HadEvenDivisor = false;
  bool AllDivisorsArePowerOfTwo = true;
  bool HadTautologicalInvertedLanes = false;
  SmallVector<SDValue, 16> PAmts, KAmts, QAmts, IAmts;

  auto BuildUREMPattern = [&](ConstantSDNode *CDiv, ConstantSDNode *CCmp) {
    // Division by 0 is UB. Leave it to be constant-folded elsewhere.
    if (CDiv->isNullValue())
      return false;

    const APInt &D = CDiv->getAPIntValue();
    const APInt &Cmp = CCmp->getAPIntValue();

    ComparingWithAllZeros &= Cmp.isNullValue();

    // x u% C1` is *always* less than C1. So given `x u% C1 == C2`,
    // if C2 is not less than C1, the comparison is always false.
    // But we will only be able to produce the comparison that will give the
    // opposive tautological answer. So this lane would need to be fixed up.
    bool TautologicalInvertedLane = D.ule(Cmp);
    HadTautologicalInvertedLanes |= TautologicalInvertedLane;

    // If all lanes are tautological (either all divisors are ones, or divisor
    // is not greater than the constant we are comparing with),
    // we will prefer to avoid the fold.
    bool TautologicalLane = D.isOneValue() || TautologicalInvertedLane;
    HadTautologicalLanes |= TautologicalLane;
    AllLanesAreTautological &= TautologicalLane;

    // If we are comparing with non-zero, we need'll need  to subtract said
    // comparison value from the LHS. But there is no point in doing that if
    // every lane where we are comparing with non-zero is tautological..
    if (!Cmp.isNullValue())
      AllComparisonsWithNonZerosAreTautological &= TautologicalLane;

    // Decompose D into D0 * 2^K
    unsigned K = D.countTrailingZeros();
    assert((!D.isOneValue() || (K == 0)) && "For divisor '1' we won't rotate.");
    APInt D0 = D.lshr(K);

    // D is even if it has trailing zeros.
    HadEvenDivisor |= (K != 0);
    // D is a power-of-two if D0 is one.
    // If all divisors are power-of-two, we will prefer to avoid the fold.
    AllDivisorsArePowerOfTwo &= D0.isOneValue();

    // P = inv(D0, 2^W)
    // 2^W requires W + 1 bits, so we have to extend and then truncate.
    unsigned W = D.getBitWidth();
    APInt P = D0.zext(W + 1)
                  .multiplicativeInverse(APInt::getSignedMinValue(W + 1))
                  .trunc(W);
    assert(!P.isNullValue() && "No multiplicative inverse!"); // unreachable
    assert((D0 * P).isOneValue() && "Multiplicative inverse sanity check.");

    // Q = floor((2^W - 1) u/ D)
    // R = ((2^W - 1) u% D)
    APInt Q, R;
    APInt::udivrem(APInt::getAllOnesValue(W), D, Q, R);

    // If we are comparing with zero, then that comparison constant is okay,
    // else it may need to be one less than that.
    if (Cmp.ugt(R))
      Q -= 1;

    assert(APInt::getAllOnesValue(ShSVT.getSizeInBits()).ugt(K) &&
           "We are expecting that K is always less than all-ones for ShSVT");

    // If the lane is tautological the result can be constant-folded.
    if (TautologicalLane) {
      // Set P and K amount to a bogus values so we can try to splat them.
      P = 0;
      K = -1;
      // And ensure that comparison constant is tautological,
      // it will always compare true/false.
      Q = -1;
    }

    PAmts.push_back(DAG.getConstant(P, DL, SVT));
    KAmts.push_back(
        DAG.getConstant(APInt(ShSVT.getSizeInBits(), K), DL, ShSVT));
    QAmts.push_back(DAG.getConstant(Q, DL, SVT));
    return true;
  };

  SDValue N = REMNode.getOperand(0);
  SDValue D = REMNode.getOperand(1);

  // Collect the values from each element.
  if (!ISD::matchBinaryPredicate(D, CompTargetNode, BuildUREMPattern))
    return SDValue();

  // If all lanes are tautological, the result can be constant-folded.
  if (AllLanesAreTautological)
    return SDValue();

  // If this is a urem by a powers-of-two, avoid the fold since it can be
  // best implemented as a bit test.
  if (AllDivisorsArePowerOfTwo)
    return SDValue();

  SDValue PVal, KVal, QVal;
  if (VT.isVector()) {
    if (HadTautologicalLanes) {
      // Try to turn PAmts into a splat, since we don't care about the values
      // that are currently '0'. If we can't, just keep '0'`s.
      turnVectorIntoSplatVector(PAmts, isNullConstant);
      // Try to turn KAmts into a splat, since we don't care about the values
      // that are currently '-1'. If we can't, change them to '0'`s.
      turnVectorIntoSplatVector(KAmts, isAllOnesConstant,
                                DAG.getConstant(0, DL, ShSVT));
    }

    PVal = DAG.getBuildVector(VT, DL, PAmts);
    KVal = DAG.getBuildVector(ShVT, DL, KAmts);
    QVal = DAG.getBuildVector(VT, DL, QAmts);
  } else {
    PVal = PAmts[0];
    KVal = KAmts[0];
    QVal = QAmts[0];
  }

  if (!ComparingWithAllZeros && !AllComparisonsWithNonZerosAreTautological) {
    if (!isOperationLegalOrCustom(ISD::SUB, VT))
      return SDValue(); // FIXME: Could/should use `ISD::ADD`?
    assert(CompTargetNode.getValueType() == N.getValueType() &&
           "Expecting that the types on LHS and RHS of comparisons match.");
    N = DAG.getNode(ISD::SUB, DL, VT, N, CompTargetNode);
  }

  // (mul N, P)
  SDValue Op0 = DAG.getNode(ISD::MUL, DL, VT, N, PVal);
  Created.push_back(Op0.getNode());

  // Rotate right only if any divisor was even. We avoid rotates for all-odd
  // divisors as a performance improvement, since rotating by 0 is a no-op.
  if (HadEvenDivisor) {
    // We need ROTR to do this.
    if (!isOperationLegalOrCustom(ISD::ROTR, VT))
      return SDValue();
    SDNodeFlags Flags;
    Flags.setExact(true);
    // UREM: (rotr (mul N, P), K)
    Op0 = DAG.getNode(ISD::ROTR, DL, VT, Op0, KVal, Flags);
    Created.push_back(Op0.getNode());
  }

  // UREM: (setule/setugt (rotr (mul N, P), K), Q)
  SDValue NewCC =
      DAG.getSetCC(DL, SETCCVT, Op0, QVal,
                   ((Cond == ISD::SETEQ) ? ISD::SETULE : ISD::SETUGT));
  if (!HadTautologicalInvertedLanes)
    return NewCC;

  // If any lanes previously compared always-false, the NewCC will give
  // always-true result for them, so we need to fixup those lanes.
  // Or the other way around for inequality predicate.
  assert(VT.isVector() && "Can/should only get here for vectors.");
  Created.push_back(NewCC.getNode());

  // x u% C1` is *always* less than C1. So given `x u% C1 == C2`,
  // if C2 is not less than C1, the comparison is always false.
  // But we have produced the comparison that will give the
  // opposive tautological answer. So these lanes would need to be fixed up.
  SDValue TautologicalInvertedChannels =
      DAG.getSetCC(DL, SETCCVT, D, CompTargetNode, ISD::SETULE);
  Created.push_back(TautologicalInvertedChannels.getNode());

  if (isOperationLegalOrCustom(ISD::VSELECT, SETCCVT)) {
    // If we have a vector select, let's replace the comparison results in the
    // affected lanes with the correct tautological result.
    SDValue Replacement = DAG.getBoolConstant(Cond == ISD::SETEQ ? false : true,
                                              DL, SETCCVT, SETCCVT);
    return DAG.getNode(ISD::VSELECT, DL, SETCCVT, TautologicalInvertedChannels,
                       Replacement, NewCC);
  }

  // Else, we can just invert the comparison result in the appropriate lanes.
  if (isOperationLegalOrCustom(ISD::XOR, SETCCVT))
    return DAG.getNode(ISD::XOR, DL, SETCCVT, NewCC,
                       TautologicalInvertedChannels);

  return SDValue(); // Don't know how to lower.
}

/// Given an ISD::SREM used only by an ISD::SETEQ or ISD::SETNE
/// where the divisor is constant and the comparison target is zero,
/// return a DAG expression that will generate the same comparison result
/// using only multiplications, additions and shifts/rotations.
/// Ref: "Hacker's Delight" 10-17.
SDValue TargetLowering::buildSREMEqFold(EVT SETCCVT, SDValue REMNode,
                                        SDValue CompTargetNode,
                                        ISD::CondCode Cond,
                                        DAGCombinerInfo &DCI,
                                        const SDLoc &DL) const {
  SmallVector<SDNode *, 7> Built;
  if (SDValue Folded = prepareSREMEqFold(SETCCVT, REMNode, CompTargetNode, Cond,
                                         DCI, DL, Built)) {
    assert(Built.size() <= 7 && "Max size prediction failed.");
    for (SDNode *N : Built)
      DCI.AddToWorklist(N);
    return Folded;
  }

  return SDValue();
}

SDValue
TargetLowering::prepareSREMEqFold(EVT SETCCVT, SDValue REMNode,
                                  SDValue CompTargetNode, ISD::CondCode Cond,
                                  DAGCombinerInfo &DCI, const SDLoc &DL,
                                  SmallVectorImpl<SDNode *> &Created) const {
  // Fold:
  //   (seteq/ne (srem N, D), 0)
  // To:
  //   (setule/ugt (rotr (add (mul N, P), A), K), Q)
  //
  // - D must be constant, with D = D0 * 2^K where D0 is odd
  // - P is the multiplicative inverse of D0 modulo 2^W
  // - A = bitwiseand(floor((2^(W - 1) - 1) / D0), (-(2^k)))
  // - Q = floor((2 * A) / (2^K))
  // where W is the width of the common type of N and D.
  assert((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
         "Only applicable for (in)equality comparisons.");

  SelectionDAG &DAG = DCI.DAG;

  EVT VT = REMNode.getValueType();
  EVT SVT = VT.getScalarType();
  EVT ShVT = getShiftAmountTy(VT, DAG.getDataLayout());
  EVT ShSVT = ShVT.getScalarType();

  // If MUL is unavailable, we cannot proceed in any case.
  if (!isOperationLegalOrCustom(ISD::MUL, VT))
    return SDValue();

  // TODO: Could support comparing with non-zero too.
  ConstantSDNode *CompTarget = isConstOrConstSplat(CompTargetNode);
  if (!CompTarget || !CompTarget->isNullValue())
    return SDValue();

  bool HadIntMinDivisor = false;
  bool HadOneDivisor = false;
  bool AllDivisorsAreOnes = true;
  bool HadEvenDivisor = false;
  bool NeedToApplyOffset = false;
  bool AllDivisorsArePowerOfTwo = true;
  SmallVector<SDValue, 16> PAmts, AAmts, KAmts, QAmts;

  auto BuildSREMPattern = [&](ConstantSDNode *C) {
    // Division by 0 is UB. Leave it to be constant-folded elsewhere.
    if (C->isNullValue())
      return false;

    // FIXME: we don't fold `rem %X, -C` to `rem %X, C` in DAGCombine.

    // WARNING: this fold is only valid for positive divisors!
    APInt D = C->getAPIntValue();
    if (D.isNegative())
      D.negate(); //  `rem %X, -C` is equivalent to `rem %X, C`

    HadIntMinDivisor |= D.isMinSignedValue();

    // If all divisors are ones, we will prefer to avoid the fold.
    HadOneDivisor |= D.isOneValue();
    AllDivisorsAreOnes &= D.isOneValue();

    // Decompose D into D0 * 2^K
    unsigned K = D.countTrailingZeros();
    assert((!D.isOneValue() || (K == 0)) && "For divisor '1' we won't rotate.");
    APInt D0 = D.lshr(K);

    if (!D.isMinSignedValue()) {
      // D is even if it has trailing zeros; unless it's INT_MIN, in which case
      // we don't care about this lane in this fold, we'll special-handle it.
      HadEvenDivisor |= (K != 0);
    }

    // D is a power-of-two if D0 is one. This includes INT_MIN.
    // If all divisors are power-of-two, we will prefer to avoid the fold.
    AllDivisorsArePowerOfTwo &= D0.isOneValue();

    // P = inv(D0, 2^W)
    // 2^W requires W + 1 bits, so we have to extend and then truncate.
    unsigned W = D.getBitWidth();
    APInt P = D0.zext(W + 1)
                  .multiplicativeInverse(APInt::getSignedMinValue(W + 1))
                  .trunc(W);
    assert(!P.isNullValue() && "No multiplicative inverse!"); // unreachable
    assert((D0 * P).isOneValue() && "Multiplicative inverse sanity check.");

    // A = floor((2^(W - 1) - 1) / D0) & -2^K
    APInt A = APInt::getSignedMaxValue(W).udiv(D0);
    A.clearLowBits(K);

    if (!D.isMinSignedValue()) {
      // If divisor INT_MIN, then we don't care about this lane in this fold,
      // we'll special-handle it.
      NeedToApplyOffset |= A != 0;
    }

    // Q = floor((2 * A) / (2^K))
    APInt Q = (2 * A).udiv(APInt::getOneBitSet(W, K));

    assert(APInt::getAllOnesValue(SVT.getSizeInBits()).ugt(A) &&
           "We are expecting that A is always less than all-ones for SVT");
    assert(APInt::getAllOnesValue(ShSVT.getSizeInBits()).ugt(K) &&
           "We are expecting that K is always less than all-ones for ShSVT");

    // If the divisor is 1 the result can be constant-folded. Likewise, we
    // don't care about INT_MIN lanes, those can be set to undef if appropriate.
    if (D.isOneValue()) {
      // Set P, A and K to a bogus values so we can try to splat them.
      P = 0;
      A = -1;
      K = -1;

      // x ?% 1 == 0  <-->  true  <-->  x u<= -1
      Q = -1;
    }

    PAmts.push_back(DAG.getConstant(P, DL, SVT));
    AAmts.push_back(DAG.getConstant(A, DL, SVT));
    KAmts.push_back(
        DAG.getConstant(APInt(ShSVT.getSizeInBits(), K), DL, ShSVT));
    QAmts.push_back(DAG.getConstant(Q, DL, SVT));
    return true;
  };

  SDValue N = REMNode.getOperand(0);
  SDValue D = REMNode.getOperand(1);

  // Collect the values from each element.
  if (!ISD::matchUnaryPredicate(D, BuildSREMPattern))
    return SDValue();

  // If this is a srem by a one, avoid the fold since it can be constant-folded.
  if (AllDivisorsAreOnes)
    return SDValue();

  // If this is a srem by a powers-of-two (including INT_MIN), avoid the fold
  // since it can be best implemented as a bit test.
  if (AllDivisorsArePowerOfTwo)
    return SDValue();

  SDValue PVal, AVal, KVal, QVal;
  if (VT.isVector()) {
    if (HadOneDivisor) {
      // Try to turn PAmts into a splat, since we don't care about the values
      // that are currently '0'. If we can't, just keep '0'`s.
      turnVectorIntoSplatVector(PAmts, isNullConstant);
      // Try to turn AAmts into a splat, since we don't care about the
      // values that are currently '-1'. If we can't, change them to '0'`s.
      turnVectorIntoSplatVector(AAmts, isAllOnesConstant,
                                DAG.getConstant(0, DL, SVT));
      // Try to turn KAmts into a splat, since we don't care about the values
      // that are currently '-1'. If we can't, change them to '0'`s.
      turnVectorIntoSplatVector(KAmts, isAllOnesConstant,
                                DAG.getConstant(0, DL, ShSVT));
    }

    PVal = DAG.getBuildVector(VT, DL, PAmts);
    AVal = DAG.getBuildVector(VT, DL, AAmts);
    KVal = DAG.getBuildVector(ShVT, DL, KAmts);
    QVal = DAG.getBuildVector(VT, DL, QAmts);
  } else {
    PVal = PAmts[0];
    AVal = AAmts[0];
    KVal = KAmts[0];
    QVal = QAmts[0];
  }

  // (mul N, P)
  SDValue Op0 = DAG.getNode(ISD::MUL, DL, VT, N, PVal);
  Created.push_back(Op0.getNode());

  if (NeedToApplyOffset) {
    // We need ADD to do this.
    if (!isOperationLegalOrCustom(ISD::ADD, VT))
      return SDValue();

    // (add (mul N, P), A)
    Op0 = DAG.getNode(ISD::ADD, DL, VT, Op0, AVal);
    Created.push_back(Op0.getNode());
  }

  // Rotate right only if any divisor was even. We avoid rotates for all-odd
  // divisors as a performance improvement, since rotating by 0 is a no-op.
  if (HadEvenDivisor) {
    // We need ROTR to do this.
    if (!isOperationLegalOrCustom(ISD::ROTR, VT))
      return SDValue();
    SDNodeFlags Flags;
    Flags.setExact(true);
    // SREM: (rotr (add (mul N, P), A), K)
    Op0 = DAG.getNode(ISD::ROTR, DL, VT, Op0, KVal, Flags);
    Created.push_back(Op0.getNode());
  }

  // SREM: (setule/setugt (rotr (add (mul N, P), A), K), Q)
  SDValue Fold =
      DAG.getSetCC(DL, SETCCVT, Op0, QVal,
                   ((Cond == ISD::SETEQ) ? ISD::SETULE : ISD::SETUGT));

  // If we didn't have lanes with INT_MIN divisor, then we're done.
  if (!HadIntMinDivisor)
    return Fold;

  // That fold is only valid for positive divisors. Which effectively means,
  // it is invalid for INT_MIN divisors. So if we have such a lane,
  // we must fix-up results for said lanes.
  assert(VT.isVector() && "Can/should only get here for vectors.");

  if (!isOperationLegalOrCustom(ISD::SETEQ, VT) ||
      !isOperationLegalOrCustom(ISD::AND, VT) ||
      !isOperationLegalOrCustom(Cond, VT) ||
      !isOperationLegalOrCustom(ISD::VSELECT, VT))
    return SDValue();

  Created.push_back(Fold.getNode());

  SDValue IntMin = DAG.getConstant(
      APInt::getSignedMinValue(SVT.getScalarSizeInBits()), DL, VT);
  SDValue IntMax = DAG.getConstant(
      APInt::getSignedMaxValue(SVT.getScalarSizeInBits()), DL, VT);
  SDValue Zero =
      DAG.getConstant(APInt::getNullValue(SVT.getScalarSizeInBits()), DL, VT);

  // Which lanes had INT_MIN divisors? Divisor is constant, so const-folded.
  SDValue DivisorIsIntMin = DAG.getSetCC(DL, SETCCVT, D, IntMin, ISD::SETEQ);
  Created.push_back(DivisorIsIntMin.getNode());

  // (N s% INT_MIN) ==/!= 0  <-->  (N & INT_MAX) ==/!= 0
  SDValue Masked = DAG.getNode(ISD::AND, DL, VT, N, IntMax);
  Created.push_back(Masked.getNode());
  SDValue MaskedIsZero = DAG.getSetCC(DL, SETCCVT, Masked, Zero, Cond);
  Created.push_back(MaskedIsZero.getNode());

  // To produce final result we need to blend 2 vectors: 'SetCC' and
  // 'MaskedIsZero'. If the divisor for channel was *NOT* INT_MIN, we pick
  // from 'Fold', else pick from 'MaskedIsZero'. Since 'DivisorIsIntMin' is
  // constant-folded, select can get lowered to a shuffle with constant mask.
  SDValue Blended =
      DAG.getNode(ISD::VSELECT, DL, VT, DivisorIsIntMin, MaskedIsZero, Fold);

  return Blended;
}

bool TargetLowering::
verifyReturnAddressArgumentIsConstant(SDValue Op, SelectionDAG &DAG) const {
  if (!isa<ConstantSDNode>(Op.getOperand(0))) {
    DAG.getContext()->emitError("argument to '__builtin_return_address' must "
                                "be a constant integer");
    return true;
  }

  return false;
}

char TargetLowering::isNegatibleForFree(SDValue Op, SelectionDAG &DAG,
                                        bool LegalOperations, bool ForCodeSize,
                                        bool EnableUseCheck,
                                        unsigned Depth) const {
  // fneg is removable even if it has multiple uses.
  if (Op.getOpcode() == ISD::FNEG)
    return 2;

  // If the caller requires checking uses, don't allow anything with multiple
  // uses unless we know it is free.
  EVT VT = Op.getValueType();
  const SDNodeFlags Flags = Op->getFlags();
  const TargetOptions &Options = DAG.getTarget().Options;
  if (EnableUseCheck)
    if (!Op.hasOneUse() && !(Op.getOpcode() == ISD::FP_EXTEND &&
                             isFPExtFree(VT, Op.getOperand(0).getValueType())))
      return 0;

  // Don't recurse exponentially.
  if (Depth > SelectionDAG::MaxRecursionDepth)
    return 0;

  switch (Op.getOpcode()) {
  case ISD::ConstantFP: {
    if (!LegalOperations)
      return 1;

    // Don't invert constant FP values after legalization unless the target says
    // the negated constant is legal.
    return isOperationLegal(ISD::ConstantFP, VT) ||
           isFPImmLegal(neg(cast<ConstantFPSDNode>(Op)->getValueAPF()), VT,
                        ForCodeSize);
  }
  case ISD::BUILD_VECTOR: {
    // Only permit BUILD_VECTOR of constants.
    if (llvm::any_of(Op->op_values(), [&](SDValue N) {
          return !N.isUndef() && !isa<ConstantFPSDNode>(N);
        }))
      return 0;
    if (!LegalOperations)
      return 1;
    if (isOperationLegal(ISD::ConstantFP, VT) &&
        isOperationLegal(ISD::BUILD_VECTOR, VT))
      return 1;
    return llvm::all_of(Op->op_values(), [&](SDValue N) {
      return N.isUndef() ||
             isFPImmLegal(neg(cast<ConstantFPSDNode>(N)->getValueAPF()), VT,
                          ForCodeSize);
    });
  }
  case ISD::FADD:
    if (!Options.NoSignedZerosFPMath && !Flags.hasNoSignedZeros())
      return 0;

    // After operation legalization, it might not be legal to create new FSUBs.
    if (LegalOperations && !isOperationLegalOrCustom(ISD::FSUB, VT))
      return 0;

    // fold (fneg (fadd A, B)) -> (fsub (fneg A), B)
    if (char V = isNegatibleForFree(Op.getOperand(0), DAG, LegalOperations,
                                    ForCodeSize, EnableUseCheck, Depth + 1))
      return V;
    // fold (fneg (fadd A, B)) -> (fsub (fneg B), A)
    return isNegatibleForFree(Op.getOperand(1), DAG, LegalOperations,
                              ForCodeSize, EnableUseCheck, Depth + 1);
  case ISD::FSUB:
    // We can't turn -(A-B) into B-A when we honor signed zeros.
    if (!Options.NoSignedZerosFPMath && !Flags.hasNoSignedZeros())
      return 0;

    // fold (fneg (fsub A, B)) -> (fsub B, A)
    return 1;

  case ISD::FMUL:
  case ISD::FDIV:
    // fold (fneg (fmul X, Y)) -> (fmul (fneg X), Y) or (fmul X, (fneg Y))
    if (char V = isNegatibleForFree(Op.getOperand(0), DAG, LegalOperations,
                                    ForCodeSize, EnableUseCheck, Depth + 1))
      return V;

    // Ignore X * 2.0 because that is expected to be canonicalized to X + X.
    if (auto *C = isConstOrConstSplatFP(Op.getOperand(1)))
      if (C->isExactlyValue(2.0) && Op.getOpcode() == ISD::FMUL)
        return 0;

    return isNegatibleForFree(Op.getOperand(1), DAG, LegalOperations,
                              ForCodeSize, EnableUseCheck, Depth + 1);

  case ISD::FMA:
  case ISD::FMAD: {
    if (!Options.NoSignedZerosFPMath && !Flags.hasNoSignedZeros())
      return 0;

    // fold (fneg (fma X, Y, Z)) -> (fma (fneg X), Y, (fneg Z))
    // fold (fneg (fma X, Y, Z)) -> (fma X, (fneg Y), (fneg Z))
    char V2 = isNegatibleForFree(Op.getOperand(2), DAG, LegalOperations,
                                 ForCodeSize, EnableUseCheck, Depth + 1);
    if (!V2)
      return 0;

    // One of Op0/Op1 must be cheaply negatible, then select the cheapest.
    char V0 = isNegatibleForFree(Op.getOperand(0), DAG, LegalOperations,
                                 ForCodeSize, EnableUseCheck, Depth + 1);
    char V1 = isNegatibleForFree(Op.getOperand(1), DAG, LegalOperations,
                                 ForCodeSize, EnableUseCheck, Depth + 1);
    char V01 = std::max(V0, V1);
    return V01 ? std::max(V01, V2) : 0;
  }

  case ISD::FP_EXTEND:
  case ISD::FP_ROUND:
  case ISD::FSIN:
    return isNegatibleForFree(Op.getOperand(0), DAG, LegalOperations,
                              ForCodeSize, EnableUseCheck, Depth + 1);
  }

  return 0;
}

SDValue TargetLowering::getNegatedExpression(SDValue Op, SelectionDAG &DAG,
                                             bool LegalOperations,
                                             bool ForCodeSize,
                                             unsigned Depth) const {
  // fneg is removable even if it has multiple uses.
  if (Op.getOpcode() == ISD::FNEG)
    return Op.getOperand(0);

  assert(Depth <= SelectionDAG::MaxRecursionDepth &&
         "getNegatedExpression doesn't match isNegatibleForFree");
  const SDNodeFlags Flags = Op->getFlags();

  switch (Op.getOpcode()) {
  case ISD::ConstantFP: {
    APFloat V = cast<ConstantFPSDNode>(Op)->getValueAPF();
    V.changeSign();
    return DAG.getConstantFP(V, SDLoc(Op), Op.getValueType());
  }
  case ISD::BUILD_VECTOR: {
    SmallVector<SDValue, 4> Ops;
    for (SDValue C : Op->op_values()) {
      if (C.isUndef()) {
        Ops.push_back(C);
        continue;
      }
      APFloat V = cast<ConstantFPSDNode>(C)->getValueAPF();
      V.changeSign();
      Ops.push_back(DAG.getConstantFP(V, SDLoc(Op), C.getValueType()));
    }
    return DAG.getBuildVector(Op.getValueType(), SDLoc(Op), Ops);
  }
  case ISD::FADD:
    assert((DAG.getTarget().Options.NoSignedZerosFPMath ||
            Flags.hasNoSignedZeros()) &&
           "Expected NSZ fp-flag");

    // fold (fneg (fadd A, B)) -> (fsub (fneg A), B)
    if (isNegatibleForFree(Op.getOperand(0), DAG, LegalOperations, ForCodeSize,
                           false, Depth + 1))
      return DAG.getNode(ISD::FSUB, SDLoc(Op), Op.getValueType(),
                         getNegatedExpression(Op.getOperand(0), DAG,
                                              LegalOperations, ForCodeSize,
                                              Depth + 1),
                         Op.getOperand(1), Flags);
    // fold (fneg (fadd A, B)) -> (fsub (fneg B), A)
    return DAG.getNode(ISD::FSUB, SDLoc(Op), Op.getValueType(),
                       getNegatedExpression(Op.getOperand(1), DAG,
                                            LegalOperations, ForCodeSize,
                                            Depth + 1),
                       Op.getOperand(0), Flags);
  case ISD::FSUB:
    // fold (fneg (fsub 0, B)) -> B
    if (ConstantFPSDNode *N0CFP =
            isConstOrConstSplatFP(Op.getOperand(0), /*AllowUndefs*/ true))
      if (N0CFP->isZero())
        return Op.getOperand(1);

    // fold (fneg (fsub A, B)) -> (fsub B, A)
    return DAG.getNode(ISD::FSUB, SDLoc(Op), Op.getValueType(),
                       Op.getOperand(1), Op.getOperand(0), Flags);

  case ISD::FMUL:
  case ISD::FDIV:
    // fold (fneg (fmul X, Y)) -> (fmul (fneg X), Y)
    if (isNegatibleForFree(Op.getOperand(0), DAG, LegalOperations, ForCodeSize,
                           false, Depth + 1))
      return DAG.getNode(Op.getOpcode(), SDLoc(Op), Op.getValueType(),
                         getNegatedExpression(Op.getOperand(0), DAG,
                                              LegalOperations, ForCodeSize,
                                              Depth + 1),
                         Op.getOperand(1), Flags);

    // fold (fneg (fmul X, Y)) -> (fmul X, (fneg Y))
    return DAG.getNode(
        Op.getOpcode(), SDLoc(Op), Op.getValueType(), Op.getOperand(0),
        getNegatedExpression(Op.getOperand(1), DAG, LegalOperations,
                             ForCodeSize, Depth + 1),
        Flags);

  case ISD::FMA:
  case ISD::FMAD: {
    assert((DAG.getTarget().Options.NoSignedZerosFPMath ||
            Flags.hasNoSignedZeros()) &&
           "Expected NSZ fp-flag");

    SDValue Neg2 = getNegatedExpression(Op.getOperand(2), DAG, LegalOperations,
                                        ForCodeSize, Depth + 1);

    char V0 = isNegatibleForFree(Op.getOperand(0), DAG, LegalOperations,
                                 ForCodeSize, false, Depth + 1);
    char V1 = isNegatibleForFree(Op.getOperand(1), DAG, LegalOperations,
                                 ForCodeSize, false, Depth + 1);
    if (V0 >= V1) {
      // fold (fneg (fma X, Y, Z)) -> (fma (fneg X), Y, (fneg Z))
      SDValue Neg0 = getNegatedExpression(
          Op.getOperand(0), DAG, LegalOperations, ForCodeSize, Depth + 1);
      return DAG.getNode(Op.getOpcode(), SDLoc(Op), Op.getValueType(), Neg0,
                         Op.getOperand(1), Neg2, Flags);
    }

    // fold (fneg (fma X, Y, Z)) -> (fma X, (fneg Y), (fneg Z))
    SDValue Neg1 = getNegatedExpression(Op.getOperand(1), DAG, LegalOperations,
                                        ForCodeSize, Depth + 1);
    return DAG.getNode(Op.getOpcode(), SDLoc(Op), Op.getValueType(),
                       Op.getOperand(0), Neg1, Neg2, Flags);
  }

  case ISD::FP_EXTEND:
  case ISD::FSIN:
    return DAG.getNode(Op.getOpcode(), SDLoc(Op), Op.getValueType(),
                       getNegatedExpression(Op.getOperand(0), DAG,
                                            LegalOperations, ForCodeSize,
                                            Depth + 1));
  case ISD::FP_ROUND:
    return DAG.getNode(ISD::FP_ROUND, SDLoc(Op), Op.getValueType(),
                       getNegatedExpression(Op.getOperand(0), DAG,
                                            LegalOperations, ForCodeSize,
                                            Depth + 1),
                       Op.getOperand(1));
  }

  llvm_unreachable("Unknown code");
}

//===----------------------------------------------------------------------===//
// Legalization Utilities
//===----------------------------------------------------------------------===//

bool TargetLowering::expandMUL_LOHI(unsigned Opcode, EVT VT, SDLoc dl,
                                    SDValue LHS, SDValue RHS,
                                    SmallVectorImpl<SDValue> &Result,
                                    EVT HiLoVT, SelectionDAG &DAG,
                                    MulExpansionKind Kind, SDValue LL,
                                    SDValue LH, SDValue RL, SDValue RH) const {
  assert(Opcode == ISD::MUL || Opcode == ISD::UMUL_LOHI ||
         Opcode == ISD::SMUL_LOHI);

  bool HasMULHS = (Kind == MulExpansionKind::Always) ||
                  isOperationLegalOrCustom(ISD::MULHS, HiLoVT);
  bool HasMULHU = (Kind == MulExpansionKind::Always) ||
                  isOperationLegalOrCustom(ISD::MULHU, HiLoVT);
  bool HasSMUL_LOHI = (Kind == MulExpansionKind::Always) ||
                      isOperationLegalOrCustom(ISD::SMUL_LOHI, HiLoVT);
  bool HasUMUL_LOHI = (Kind == MulExpansionKind::Always) ||
                      isOperationLegalOrCustom(ISD::UMUL_LOHI, HiLoVT);

  if (!HasMULHU && !HasMULHS && !HasUMUL_LOHI && !HasSMUL_LOHI)
    return false;

  unsigned OuterBitSize = VT.getScalarSizeInBits();
  unsigned InnerBitSize = HiLoVT.getScalarSizeInBits();
  unsigned LHSSB = DAG.ComputeNumSignBits(LHS);
  unsigned RHSSB = DAG.ComputeNumSignBits(RHS);

  // LL, LH, RL, and RH must be either all NULL or all set to a value.
  assert((LL.getNode() && LH.getNode() && RL.getNode() && RH.getNode()) ||
         (!LL.getNode() && !LH.getNode() && !RL.getNode() && !RH.getNode()));

  SDVTList VTs = DAG.getVTList(HiLoVT, HiLoVT);
  auto MakeMUL_LOHI = [&](SDValue L, SDValue R, SDValue &Lo, SDValue &Hi,
                          bool Signed) -> bool {
    if ((Signed && HasSMUL_LOHI) || (!Signed && HasUMUL_LOHI)) {
      Lo = DAG.getNode(Signed ? ISD::SMUL_LOHI : ISD::UMUL_LOHI, dl, VTs, L, R);
      Hi = SDValue(Lo.getNode(), 1);
      return true;
    }
    if ((Signed && HasMULHS) || (!Signed && HasMULHU)) {
      Lo = DAG.getNode(ISD::MUL, dl, HiLoVT, L, R);
      Hi = DAG.getNode(Signed ? ISD::MULHS : ISD::MULHU, dl, HiLoVT, L, R);
      return true;
    }
    return false;
  };

  SDValue Lo, Hi;

  if (!LL.getNode() && !RL.getNode() &&
      isOperationLegalOrCustom(ISD::TRUNCATE, HiLoVT)) {
    LL = DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, LHS);
    RL = DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, RHS);
  }

  if (!LL.getNode())
    return false;

  APInt HighMask = APInt::getHighBitsSet(OuterBitSize, InnerBitSize);
  if (DAG.MaskedValueIsZero(LHS, HighMask) &&
      DAG.MaskedValueIsZero(RHS, HighMask)) {
    // The inputs are both zero-extended.
    if (MakeMUL_LOHI(LL, RL, Lo, Hi, false)) {
      Result.push_back(Lo);
      Result.push_back(Hi);
      if (Opcode != ISD::MUL) {
        SDValue Zero = DAG.getConstant(0, dl, HiLoVT);
        Result.push_back(Zero);
        Result.push_back(Zero);
      }
      return true;
    }
  }

  if (!VT.isVector() && Opcode == ISD::MUL && LHSSB > InnerBitSize &&
      RHSSB > InnerBitSize) {
    // The input values are both sign-extended.
    // TODO non-MUL case?
    if (MakeMUL_LOHI(LL, RL, Lo, Hi, true)) {
      Result.push_back(Lo);
      Result.push_back(Hi);
      return true;
    }
  }

  unsigned ShiftAmount = OuterBitSize - InnerBitSize;
  EVT ShiftAmountTy = getShiftAmountTy(VT, DAG.getDataLayout());
  if (APInt::getMaxValue(ShiftAmountTy.getSizeInBits()).ult(ShiftAmount)) {
    // FIXME getShiftAmountTy does not always return a sensible result when VT
    // is an illegal type, and so the type may be too small to fit the shift
    // amount. Override it with i32. The shift will have to be legalized.
    ShiftAmountTy = MVT::i32;
  }
  SDValue Shift = DAG.getConstant(ShiftAmount, dl, ShiftAmountTy);

  if (!LH.getNode() && !RH.getNode() &&
      isOperationLegalOrCustom(ISD::SRL, VT) &&
      isOperationLegalOrCustom(ISD::TRUNCATE, HiLoVT)) {
    LH = DAG.getNode(ISD::SRL, dl, VT, LHS, Shift);
    LH = DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, LH);
    RH = DAG.getNode(ISD::SRL, dl, VT, RHS, Shift);
    RH = DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, RH);
  }

  if (!LH.getNode())
    return false;

  if (!MakeMUL_LOHI(LL, RL, Lo, Hi, false))
    return false;

  Result.push_back(Lo);

  if (Opcode == ISD::MUL) {
    RH = DAG.getNode(ISD::MUL, dl, HiLoVT, LL, RH);
    LH = DAG.getNode(ISD::MUL, dl, HiLoVT, LH, RL);
    Hi = DAG.getNode(ISD::ADD, dl, HiLoVT, Hi, RH);
    Hi = DAG.getNode(ISD::ADD, dl, HiLoVT, Hi, LH);
    Result.push_back(Hi);
    return true;
  }

  // Compute the full width result.
  auto Merge = [&](SDValue Lo, SDValue Hi) -> SDValue {
    Lo = DAG.getNode(ISD::ZERO_EXTEND, dl, VT, Lo);
    Hi = DAG.getNode(ISD::ZERO_EXTEND, dl, VT, Hi);
    Hi = DAG.getNode(ISD::SHL, dl, VT, Hi, Shift);
    return DAG.getNode(ISD::OR, dl, VT, Lo, Hi);
  };

  SDValue Next = DAG.getNode(ISD::ZERO_EXTEND, dl, VT, Hi);
  if (!MakeMUL_LOHI(LL, RH, Lo, Hi, false))
    return false;

  // This is effectively the add part of a multiply-add of half-sized operands,
  // so it cannot overflow.
  Next = DAG.getNode(ISD::ADD, dl, VT, Next, Merge(Lo, Hi));

  if (!MakeMUL_LOHI(LH, RL, Lo, Hi, false))
    return false;

  SDValue Zero = DAG.getConstant(0, dl, HiLoVT);
  EVT BoolType = getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), VT);

  bool UseGlue = (isOperationLegalOrCustom(ISD::ADDC, VT) &&
                  isOperationLegalOrCustom(ISD::ADDE, VT));
  if (UseGlue)
    Next = DAG.getNode(ISD::ADDC, dl, DAG.getVTList(VT, MVT::Glue), Next,
                       Merge(Lo, Hi));
  else
    Next = DAG.getNode(ISD::ADDCARRY, dl, DAG.getVTList(VT, BoolType), Next,
                       Merge(Lo, Hi), DAG.getConstant(0, dl, BoolType));

  SDValue Carry = Next.getValue(1);
  Result.push_back(DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, Next));
  Next = DAG.getNode(ISD::SRL, dl, VT, Next, Shift);

  if (!MakeMUL_LOHI(LH, RH, Lo, Hi, Opcode == ISD::SMUL_LOHI))
    return false;

  if (UseGlue)
    Hi = DAG.getNode(ISD::ADDE, dl, DAG.getVTList(HiLoVT, MVT::Glue), Hi, Zero,
                     Carry);
  else
    Hi = DAG.getNode(ISD::ADDCARRY, dl, DAG.getVTList(HiLoVT, BoolType), Hi,
                     Zero, Carry);

  Next = DAG.getNode(ISD::ADD, dl, VT, Next, Merge(Lo, Hi));

  if (Opcode == ISD::SMUL_LOHI) {
    SDValue NextSub = DAG.getNode(ISD::SUB, dl, VT, Next,
                                  DAG.getNode(ISD::ZERO_EXTEND, dl, VT, RL));
    Next = DAG.getSelectCC(dl, LH, Zero, NextSub, Next, ISD::SETLT);

    NextSub = DAG.getNode(ISD::SUB, dl, VT, Next,
                          DAG.getNode(ISD::ZERO_EXTEND, dl, VT, LL));
    Next = DAG.getSelectCC(dl, RH, Zero, NextSub, Next, ISD::SETLT);
  }

  Result.push_back(DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, Next));
  Next = DAG.getNode(ISD::SRL, dl, VT, Next, Shift);
  Result.push_back(DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, Next));
  return true;
}

bool TargetLowering::expandMUL(SDNode *N, SDValue &Lo, SDValue &Hi, EVT HiLoVT,
                               SelectionDAG &DAG, MulExpansionKind Kind,
                               SDValue LL, SDValue LH, SDValue RL,
                               SDValue RH) const {
  SmallVector<SDValue, 2> Result;
  bool Ok = expandMUL_LOHI(N->getOpcode(), N->getValueType(0), N,
                           N->getOperand(0), N->getOperand(1), Result, HiLoVT,
                           DAG, Kind, LL, LH, RL, RH);
  if (Ok) {
    assert(Result.size() == 2);
    Lo = Result[0];
    Hi = Result[1];
  }
  return Ok;
}

bool TargetLowering::expandFunnelShift(SDNode *Node, SDValue &Result,
                                       SelectionDAG &DAG) const {
  EVT VT = Node->getValueType(0);

  if (VT.isVector() && (!isOperationLegalOrCustom(ISD::SHL, VT) ||
                        !isOperationLegalOrCustom(ISD::SRL, VT) ||
                        !isOperationLegalOrCustom(ISD::SUB, VT) ||
                        !isOperationLegalOrCustomOrPromote(ISD::OR, VT)))
    return false;

  // fshl: (X << (Z % BW)) | (Y >> (BW - (Z % BW)))
  // fshr: (X << (BW - (Z % BW))) | (Y >> (Z % BW))
  SDValue X = Node->getOperand(0);
  SDValue Y = Node->getOperand(1);
  SDValue Z = Node->getOperand(2);

  unsigned EltSizeInBits = VT.getScalarSizeInBits();
  bool IsFSHL = Node->getOpcode() == ISD::FSHL;
  SDLoc DL(SDValue(Node, 0));

  EVT ShVT = Z.getValueType();
  SDValue BitWidthC = DAG.getConstant(EltSizeInBits, DL, ShVT);
  SDValue Zero = DAG.getConstant(0, DL, ShVT);

  SDValue ShAmt;
  if (isPowerOf2_32(EltSizeInBits)) {
    SDValue Mask = DAG.getConstant(EltSizeInBits - 1, DL, ShVT);
    ShAmt = DAG.getNode(ISD::AND, DL, ShVT, Z, Mask);
  } else {
    ShAmt = DAG.getNode(ISD::UREM, DL, ShVT, Z, BitWidthC);
  }

  SDValue InvShAmt = DAG.getNode(ISD::SUB, DL, ShVT, BitWidthC, ShAmt);
  SDValue ShX = DAG.getNode(ISD::SHL, DL, VT, X, IsFSHL ? ShAmt : InvShAmt);
  SDValue ShY = DAG.getNode(ISD::SRL, DL, VT, Y, IsFSHL ? InvShAmt : ShAmt);
  SDValue Or = DAG.getNode(ISD::OR, DL, VT, ShX, ShY);

  // If (Z % BW == 0), then the opposite direction shift is shift-by-bitwidth,
  // and that is undefined. We must compare and select to avoid UB.
  EVT CCVT = getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), ShVT);

  // For fshl, 0-shift returns the 1st arg (X).
  // For fshr, 0-shift returns the 2nd arg (Y).
  SDValue IsZeroShift = DAG.getSetCC(DL, CCVT, ShAmt, Zero, ISD::SETEQ);
  Result = DAG.getSelect(DL, VT, IsZeroShift, IsFSHL ? X : Y, Or);
  return true;
}

// TODO: Merge with expandFunnelShift.
bool TargetLowering::expandROT(SDNode *Node, SDValue &Result,
                               SelectionDAG &DAG) const {
  EVT VT = Node->getValueType(0);
  unsigned EltSizeInBits = VT.getScalarSizeInBits();
  bool IsLeft = Node->getOpcode() == ISD::ROTL;
  SDValue Op0 = Node->getOperand(0);
  SDValue Op1 = Node->getOperand(1);
  SDLoc DL(SDValue(Node, 0));

  EVT ShVT = Op1.getValueType();
  SDValue BitWidthC = DAG.getConstant(EltSizeInBits, DL, ShVT);

  // If a rotate in the other direction is legal, use it.
  unsigned RevRot = IsLeft ? ISD::ROTR : ISD::ROTL;
  if (isOperationLegal(RevRot, VT)) {
    SDValue Sub = DAG.getNode(ISD::SUB, DL, ShVT, BitWidthC, Op1);
    Result = DAG.getNode(RevRot, DL, VT, Op0, Sub);
    return true;
  }

  if (VT.isVector() && (!isOperationLegalOrCustom(ISD::SHL, VT) ||
                        !isOperationLegalOrCustom(ISD::SRL, VT) ||
                        !isOperationLegalOrCustom(ISD::SUB, VT) ||
                        !isOperationLegalOrCustomOrPromote(ISD::OR, VT) ||
                        !isOperationLegalOrCustomOrPromote(ISD::AND, VT)))
    return false;

  // Otherwise,
  //   (rotl x, c) -> (or (shl x, (and c, w-1)), (srl x, (and w-c, w-1)))
  //   (rotr x, c) -> (or (srl x, (and c, w-1)), (shl x, (and w-c, w-1)))
  //
  assert(isPowerOf2_32(EltSizeInBits) && EltSizeInBits > 1 &&
         "Expecting the type bitwidth to be a power of 2");
  unsigned ShOpc = IsLeft ? ISD::SHL : ISD::SRL;
  unsigned HsOpc = IsLeft ? ISD::SRL : ISD::SHL;
  SDValue BitWidthMinusOneC = DAG.getConstant(EltSizeInBits - 1, DL, ShVT);
  SDValue NegOp1 = DAG.getNode(ISD::SUB, DL, ShVT, BitWidthC, Op1);
  SDValue And0 = DAG.getNode(ISD::AND, DL, ShVT, Op1, BitWidthMinusOneC);
  SDValue And1 = DAG.getNode(ISD::AND, DL, ShVT, NegOp1, BitWidthMinusOneC);
  Result = DAG.getNode(ISD::OR, DL, VT, DAG.getNode(ShOpc, DL, VT, Op0, And0),
                       DAG.getNode(HsOpc, DL, VT, Op0, And1));
  return true;
}

bool TargetLowering::expandFP_TO_SINT(SDNode *Node, SDValue &Result,
                                      SelectionDAG &DAG) const {
  unsigned OpNo = Node->isStrictFPOpcode() ? 1 : 0;
  SDValue Src = Node->getOperand(OpNo);
  EVT SrcVT = Src.getValueType();
  EVT DstVT = Node->getValueType(0);
  SDLoc dl(SDValue(Node, 0));

  // FIXME: Only f32 to i64 conversions are supported.
  if (SrcVT != MVT::f32 || DstVT != MVT::i64)
    return false;

  if (Node->isStrictFPOpcode())
    // When a NaN is converted to an integer a trap is allowed. We can't
    // use this expansion here because it would eliminate that trap. Other
    // traps are also allowed and cannot be eliminated. See 
    // IEEE 754-2008 sec 5.8.
    return false;

  // Expand f32 -> i64 conversion
  // This algorithm comes from compiler-rt's implementation of fixsfdi:
  // https://github.com/llvm/llvm-project/blob/master/compiler-rt/lib/builtins/fixsfdi.c
  unsigned SrcEltBits = SrcVT.getScalarSizeInBits();
  EVT IntVT = SrcVT.changeTypeToInteger();
  EVT IntShVT = getShiftAmountTy(IntVT, DAG.getDataLayout());

  SDValue ExponentMask = DAG.getConstant(0x7F800000, dl, IntVT);
  SDValue ExponentLoBit = DAG.getConstant(23, dl, IntVT);
  SDValue Bias = DAG.getConstant(127, dl, IntVT);
  SDValue SignMask = DAG.getConstant(APInt::getSignMask(SrcEltBits), dl, IntVT);
  SDValue SignLowBit = DAG.getConstant(SrcEltBits - 1, dl, IntVT);
  SDValue MantissaMask = DAG.getConstant(0x007FFFFF, dl, IntVT);

  SDValue Bits = DAG.getNode(ISD::BITCAST, dl, IntVT, Src);

  SDValue ExponentBits = DAG.getNode(
      ISD::SRL, dl, IntVT, DAG.getNode(ISD::AND, dl, IntVT, Bits, ExponentMask),
      DAG.getZExtOrTrunc(ExponentLoBit, dl, IntShVT));
  SDValue Exponent = DAG.getNode(ISD::SUB, dl, IntVT, ExponentBits, Bias);

  SDValue Sign = DAG.getNode(ISD::SRA, dl, IntVT,
                             DAG.getNode(ISD::AND, dl, IntVT, Bits, SignMask),
                             DAG.getZExtOrTrunc(SignLowBit, dl, IntShVT));
  Sign = DAG.getSExtOrTrunc(Sign, dl, DstVT);

  SDValue R = DAG.getNode(ISD::OR, dl, IntVT,
                          DAG.getNode(ISD::AND, dl, IntVT, Bits, MantissaMask),
                          DAG.getConstant(0x00800000, dl, IntVT));

  R = DAG.getZExtOrTrunc(R, dl, DstVT);

  R = DAG.getSelectCC(
      dl, Exponent, ExponentLoBit,
      DAG.getNode(ISD::SHL, dl, DstVT, R,
                  DAG.getZExtOrTrunc(
                      DAG.getNode(ISD::SUB, dl, IntVT, Exponent, ExponentLoBit),
                      dl, IntShVT)),
      DAG.getNode(ISD::SRL, dl, DstVT, R,
                  DAG.getZExtOrTrunc(
                      DAG.getNode(ISD::SUB, dl, IntVT, ExponentLoBit, Exponent),
                      dl, IntShVT)),
      ISD::SETGT);

  SDValue Ret = DAG.getNode(ISD::SUB, dl, DstVT,
                            DAG.getNode(ISD::XOR, dl, DstVT, R, Sign), Sign);

  Result = DAG.getSelectCC(dl, Exponent, DAG.getConstant(0, dl, IntVT),
                           DAG.getConstant(0, dl, DstVT), Ret, ISD::SETLT);
  return true;
}

bool TargetLowering::expandFP_TO_UINT(SDNode *Node, SDValue &Result,
                                      SDValue &Chain,
                                      SelectionDAG &DAG) const {
  SDLoc dl(SDValue(Node, 0));
  unsigned OpNo = Node->isStrictFPOpcode() ? 1 : 0;
  SDValue Src = Node->getOperand(OpNo);

  EVT SrcVT = Src.getValueType();
  EVT DstVT = Node->getValueType(0);
  EVT SetCCVT =
      getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), SrcVT);

  // Only expand vector types if we have the appropriate vector bit operations.
  unsigned SIntOpcode = Node->isStrictFPOpcode() ? ISD::STRICT_FP_TO_SINT : 
                                                   ISD::FP_TO_SINT;
  if (DstVT.isVector() && (!isOperationLegalOrCustom(SIntOpcode, DstVT) ||
                           !isOperationLegalOrCustomOrPromote(ISD::XOR, SrcVT)))
    return false;

  // If the maximum float value is smaller then the signed integer range,
  // the destination signmask can't be represented by the float, so we can
  // just use FP_TO_SINT directly.
  const fltSemantics &APFSem = DAG.EVTToAPFloatSemantics(SrcVT);
  APFloat APF(APFSem, APInt::getNullValue(SrcVT.getScalarSizeInBits()));
  APInt SignMask = APInt::getSignMask(DstVT.getScalarSizeInBits());
  if (APFloat::opOverflow &
      APF.convertFromAPInt(SignMask, false, APFloat::rmNearestTiesToEven)) {
    if (Node->isStrictFPOpcode()) {
      Result = DAG.getNode(ISD::STRICT_FP_TO_SINT, dl, { DstVT, MVT::Other }, 
                           { Node->getOperand(0), Src }); 
      Chain = Result.getValue(1);
    } else
      Result = DAG.getNode(ISD::FP_TO_SINT, dl, DstVT, Src);
    return true;
  }

  SDValue Cst = DAG.getConstantFP(APF, dl, SrcVT);
  SDValue Sel = DAG.getSetCC(dl, SetCCVT, Src, Cst, ISD::SETLT);

  bool Strict = Node->isStrictFPOpcode() ||
                shouldUseStrictFP_TO_INT(SrcVT, DstVT, /*IsSigned*/ false);

  if (Strict) {
    // Expand based on maximum range of FP_TO_SINT, if the value exceeds the
    // signmask then offset (the result of which should be fully representable).
    // Sel = Src < 0x8000000000000000
    // FltOfs = select Sel, 0, 0x8000000000000000
    // IntOfs = select Sel, 0, 0x8000000000000000
    // Result = fp_to_sint(Src - FltOfs) ^ IntOfs

    // TODO: Should any fast-math-flags be set for the FSUB?
    SDValue FltOfs = DAG.getSelect(dl, SrcVT, Sel,
                                   DAG.getConstantFP(0.0, dl, SrcVT), Cst);
    SDValue IntOfs = DAG.getSelect(dl, DstVT, Sel,
                                   DAG.getConstant(0, dl, DstVT),
                                   DAG.getConstant(SignMask, dl, DstVT));
    SDValue SInt;
    if (Node->isStrictFPOpcode()) {
      SDValue Val = DAG.getNode(ISD::STRICT_FSUB, dl, { SrcVT, MVT::Other }, 
                                { Node->getOperand(0), Src, FltOfs });
      SInt = DAG.getNode(ISD::STRICT_FP_TO_SINT, dl, { DstVT, MVT::Other }, 
                         { Val.getValue(1), Val });
      Chain = SInt.getValue(1);
    } else {
      SDValue Val = DAG.getNode(ISD::FSUB, dl, SrcVT, Src, FltOfs);
      SInt = DAG.getNode(ISD::FP_TO_SINT, dl, DstVT, Val);
    }
    Result = DAG.getNode(ISD::XOR, dl, DstVT, SInt, IntOfs);
  } else {
    // Expand based on maximum range of FP_TO_SINT:
    // True = fp_to_sint(Src)
    // False = 0x8000000000000000 + fp_to_sint(Src - 0x8000000000000000)
    // Result = select (Src < 0x8000000000000000), True, False

    SDValue True = DAG.getNode(ISD::FP_TO_SINT, dl, DstVT, Src);
    // TODO: Should any fast-math-flags be set for the FSUB?
    SDValue False = DAG.getNode(ISD::FP_TO_SINT, dl, DstVT,
                                DAG.getNode(ISD::FSUB, dl, SrcVT, Src, Cst));
    False = DAG.getNode(ISD::XOR, dl, DstVT, False,
                        DAG.getConstant(SignMask, dl, DstVT));
    Result = DAG.getSelect(dl, DstVT, Sel, True, False);
  }
  return true;
}

bool TargetLowering::expandUINT_TO_FP(SDNode *Node, SDValue &Result,
                                      SelectionDAG &DAG) const {
  SDValue Src = Node->getOperand(0);
  EVT SrcVT = Src.getValueType();
  EVT DstVT = Node->getValueType(0);

  if (SrcVT.getScalarType() != MVT::i64)
    return false;

  SDLoc dl(SDValue(Node, 0));
  EVT ShiftVT = getShiftAmountTy(SrcVT, DAG.getDataLayout());

  if (DstVT.getScalarType() == MVT::f32) {
    // Only expand vector types if we have the appropriate vector bit
    // operations.
    if (SrcVT.isVector() &&
        (!isOperationLegalOrCustom(ISD::SRL, SrcVT) ||
         !isOperationLegalOrCustom(ISD::FADD, DstVT) ||
         !isOperationLegalOrCustom(ISD::SINT_TO_FP, SrcVT) ||
         !isOperationLegalOrCustomOrPromote(ISD::OR, SrcVT) ||
         !isOperationLegalOrCustomOrPromote(ISD::AND, SrcVT)))
      return false;

    // For unsigned conversions, convert them to signed conversions using the
    // algorithm from the x86_64 __floatundidf in compiler_rt.
    SDValue Fast = DAG.getNode(ISD::SINT_TO_FP, dl, DstVT, Src);

    SDValue ShiftConst = DAG.getConstant(1, dl, ShiftVT);
    SDValue Shr = DAG.getNode(ISD::SRL, dl, SrcVT, Src, ShiftConst);
    SDValue AndConst = DAG.getConstant(1, dl, SrcVT);
    SDValue And = DAG.getNode(ISD::AND, dl, SrcVT, Src, AndConst);
    SDValue Or = DAG.getNode(ISD::OR, dl, SrcVT, And, Shr);

    SDValue SignCvt = DAG.getNode(ISD::SINT_TO_FP, dl, DstVT, Or);
    SDValue Slow = DAG.getNode(ISD::FADD, dl, DstVT, SignCvt, SignCvt);

    // TODO: This really should be implemented using a branch rather than a
    // select.  We happen to get lucky and machinesink does the right
    // thing most of the time.  This would be a good candidate for a
    // pseudo-op, or, even better, for whole-function isel.
    EVT SetCCVT =
        getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), SrcVT);

    SDValue SignBitTest = DAG.getSetCC(
        dl, SetCCVT, Src, DAG.getConstant(0, dl, SrcVT), ISD::SETLT);
    Result = DAG.getSelect(dl, DstVT, SignBitTest, Slow, Fast);
    return true;
  }

  if (DstVT.getScalarType() == MVT::f64) {
    // Only expand vector types if we have the appropriate vector bit
    // operations.
    if (SrcVT.isVector() &&
        (!isOperationLegalOrCustom(ISD::SRL, SrcVT) ||
         !isOperationLegalOrCustom(ISD::FADD, DstVT) ||
         !isOperationLegalOrCustom(ISD::FSUB, DstVT) ||
         !isOperationLegalOrCustomOrPromote(ISD::OR, SrcVT) ||
         !isOperationLegalOrCustomOrPromote(ISD::AND, SrcVT)))
      return false;

    // Implementation of unsigned i64 to f64 following the algorithm in
    // __floatundidf in compiler_rt. This implementation has the advantage
    // of performing rounding correctly, both in the default rounding mode
    // and in all alternate rounding modes.
    SDValue TwoP52 = DAG.getConstant(UINT64_C(0x4330000000000000), dl, SrcVT);
    SDValue TwoP84PlusTwoP52 = DAG.getConstantFP(
        BitsToDouble(UINT64_C(0x4530000000100000)), dl, DstVT);
    SDValue TwoP84 = DAG.getConstant(UINT64_C(0x4530000000000000), dl, SrcVT);
    SDValue LoMask = DAG.getConstant(UINT64_C(0x00000000FFFFFFFF), dl, SrcVT);
    SDValue HiShift = DAG.getConstant(32, dl, ShiftVT);

    SDValue Lo = DAG.getNode(ISD::AND, dl, SrcVT, Src, LoMask);
    SDValue Hi = DAG.getNode(ISD::SRL, dl, SrcVT, Src, HiShift);
    SDValue LoOr = DAG.getNode(ISD::OR, dl, SrcVT, Lo, TwoP52);
    SDValue HiOr = DAG.getNode(ISD::OR, dl, SrcVT, Hi, TwoP84);
    SDValue LoFlt = DAG.getBitcast(DstVT, LoOr);
    SDValue HiFlt = DAG.getBitcast(DstVT, HiOr);
    SDValue HiSub = DAG.getNode(ISD::FSUB, dl, DstVT, HiFlt, TwoP84PlusTwoP52);
    Result = DAG.getNode(ISD::FADD, dl, DstVT, LoFlt, HiSub);
    return true;
  }

  return false;
}

SDValue TargetLowering::expandFMINNUM_FMAXNUM(SDNode *Node,
                                              SelectionDAG &DAG) const {
  SDLoc dl(Node);
  unsigned NewOp = Node->getOpcode() == ISD::FMINNUM ?
    ISD::FMINNUM_IEEE : ISD::FMAXNUM_IEEE;
  EVT VT = Node->getValueType(0);
  if (isOperationLegalOrCustom(NewOp, VT)) {
    SDValue Quiet0 = Node->getOperand(0);
    SDValue Quiet1 = Node->getOperand(1);

    if (!Node->getFlags().hasNoNaNs()) {
      // Insert canonicalizes if it's possible we need to quiet to get correct
      // sNaN behavior.
      if (!DAG.isKnownNeverSNaN(Quiet0)) {
        Quiet0 = DAG.getNode(ISD::FCANONICALIZE, dl, VT, Quiet0,
                             Node->getFlags());
      }
      if (!DAG.isKnownNeverSNaN(Quiet1)) {
        Quiet1 = DAG.getNode(ISD::FCANONICALIZE, dl, VT, Quiet1,
                             Node->getFlags());
      }
    }

    return DAG.getNode(NewOp, dl, VT, Quiet0, Quiet1, Node->getFlags());
  }

  // If the target has FMINIMUM/FMAXIMUM but not FMINNUM/FMAXNUM use that
  // instead if there are no NaNs.
  if (Node->getFlags().hasNoNaNs()) {
    unsigned IEEE2018Op =
        Node->getOpcode() == ISD::FMINNUM ? ISD::FMINIMUM : ISD::FMAXIMUM;
    if (isOperationLegalOrCustom(IEEE2018Op, VT)) {
      return DAG.getNode(IEEE2018Op, dl, VT, Node->getOperand(0),
                         Node->getOperand(1), Node->getFlags());
    }
  }

  // If none of the above worked, but there are no NaNs, then expand to
  // a compare/select sequence.  This is required for correctness since
  // InstCombine might have canonicalized a fcmp+select sequence to a
  // FMINNUM/FMAXNUM node.  If we were to fall through to the default
  // expansion to libcall, we might introduce a link-time dependency
  // on libm into a file that originally did not have one.
  if (Node->getFlags().hasNoNaNs()) {
    ISD::CondCode Pred =
        Node->getOpcode() == ISD::FMINNUM ? ISD::SETLT : ISD::SETGT;
    SDValue Op1 = Node->getOperand(0);
    SDValue Op2 = Node->getOperand(1);
    SDValue SelCC = DAG.getSelectCC(dl, Op1, Op2, Op1, Op2, Pred);
    // Copy FMF flags, but always set the no-signed-zeros flag
    // as this is implied by the FMINNUM/FMAXNUM semantics.
    SDNodeFlags Flags = Node->getFlags();
    Flags.setNoSignedZeros(true);
    SelCC->setFlags(Flags);
    return SelCC;
  }

  return SDValue();
}

bool TargetLowering::expandCTPOP(SDNode *Node, SDValue &Result,
                                 SelectionDAG &DAG) const {
  SDLoc dl(Node);
  EVT VT = Node->getValueType(0);
  EVT ShVT = getShiftAmountTy(VT, DAG.getDataLayout());
  SDValue Op = Node->getOperand(0);
  unsigned Len = VT.getScalarSizeInBits();
  assert(VT.isInteger() && "CTPOP not implemented for this type.");

  // TODO: Add support for irregular type lengths.
  if (!(Len <= 128 && Len % 8 == 0))
    return false;

  // Only expand vector types if we have the appropriate vector bit operations.
  if (VT.isVector() && (!isOperationLegalOrCustom(ISD::ADD, VT) ||
                        !isOperationLegalOrCustom(ISD::SUB, VT) ||
                        !isOperationLegalOrCustom(ISD::SRL, VT) ||
                        (Len != 8 && !isOperationLegalOrCustom(ISD::MUL, VT)) ||
                        !isOperationLegalOrCustomOrPromote(ISD::AND, VT)))
    return false;

  // This is the "best" algorithm from
  // http://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel
  SDValue Mask55 =
      DAG.getConstant(APInt::getSplat(Len, APInt(8, 0x55)), dl, VT);
  SDValue Mask33 =
      DAG.getConstant(APInt::getSplat(Len, APInt(8, 0x33)), dl, VT);
  SDValue Mask0F =
      DAG.getConstant(APInt::getSplat(Len, APInt(8, 0x0F)), dl, VT);
  SDValue Mask01 =
      DAG.getConstant(APInt::getSplat(Len, APInt(8, 0x01)), dl, VT);

  // v = v - ((v >> 1) & 0x55555555...)
  Op = DAG.getNode(ISD::SUB, dl, VT, Op,
                   DAG.getNode(ISD::AND, dl, VT,
                               DAG.getNode(ISD::SRL, dl, VT, Op,
                                           DAG.getConstant(1, dl, ShVT)),
                               Mask55));
  // v = (v & 0x33333333...) + ((v >> 2) & 0x33333333...)
  Op = DAG.getNode(ISD::ADD, dl, VT, DAG.getNode(ISD::AND, dl, VT, Op, Mask33),
                   DAG.getNode(ISD::AND, dl, VT,
                               DAG.getNode(ISD::SRL, dl, VT, Op,
                                           DAG.getConstant(2, dl, ShVT)),
                               Mask33));
  // v = (v + (v >> 4)) & 0x0F0F0F0F...
  Op = DAG.getNode(ISD::AND, dl, VT,
                   DAG.getNode(ISD::ADD, dl, VT, Op,
                               DAG.getNode(ISD::SRL, dl, VT, Op,
                                           DAG.getConstant(4, dl, ShVT))),
                   Mask0F);
  // v = (v * 0x01010101...) >> (Len - 8)
  if (Len > 8)
    Op =
        DAG.getNode(ISD::SRL, dl, VT, DAG.getNode(ISD::MUL, dl, VT, Op, Mask01),
                    DAG.getConstant(Len - 8, dl, ShVT));

  Result = Op;
  return true;
}

bool TargetLowering::expandCTLZ(SDNode *Node, SDValue &Result,
                                SelectionDAG &DAG) const {
  SDLoc dl(Node);
  EVT VT = Node->getValueType(0);
  EVT ShVT = getShiftAmountTy(VT, DAG.getDataLayout());
  SDValue Op = Node->getOperand(0);
  unsigned NumBitsPerElt = VT.getScalarSizeInBits();

  // If the non-ZERO_UNDEF version is supported we can use that instead.
  if (Node->getOpcode() == ISD::CTLZ_ZERO_UNDEF &&
      isOperationLegalOrCustom(ISD::CTLZ, VT)) {
    Result = DAG.getNode(ISD::CTLZ, dl, VT, Op);
    return true;
  }

  // If the ZERO_UNDEF version is supported use that and handle the zero case.
  if (isOperationLegalOrCustom(ISD::CTLZ_ZERO_UNDEF, VT)) {
    EVT SetCCVT =
        getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), VT);
    SDValue CTLZ = DAG.getNode(ISD::CTLZ_ZERO_UNDEF, dl, VT, Op);
    SDValue Zero = DAG.getConstant(0, dl, VT);
    SDValue SrcIsZero = DAG.getSetCC(dl, SetCCVT, Op, Zero, ISD::SETEQ);
    Result = DAG.getNode(ISD::SELECT, dl, VT, SrcIsZero,
                         DAG.getConstant(NumBitsPerElt, dl, VT), CTLZ);
    return true;
  }

  // Only expand vector types if we have the appropriate vector bit operations.
  if (VT.isVector() && (!isPowerOf2_32(NumBitsPerElt) ||
                        !isOperationLegalOrCustom(ISD::CTPOP, VT) ||
                        !isOperationLegalOrCustom(ISD::SRL, VT) ||
                        !isOperationLegalOrCustomOrPromote(ISD::OR, VT)))
    return false;

  // for now, we do this:
  // x = x | (x >> 1);
  // x = x | (x >> 2);
  // ...
  // x = x | (x >>16);
  // x = x | (x >>32); // for 64-bit input
  // return popcount(~x);
  //
  // Ref: "Hacker's Delight" by Henry Warren
  for (unsigned i = 0; (1U << i) <= (NumBitsPerElt / 2); ++i) {
    SDValue Tmp = DAG.getConstant(1ULL << i, dl, ShVT);
    Op = DAG.getNode(ISD::OR, dl, VT, Op,
                     DAG.getNode(ISD::SRL, dl, VT, Op, Tmp));
  }
  Op = DAG.getNOT(dl, Op, VT);
  Result = DAG.getNode(ISD::CTPOP, dl, VT, Op);
  return true;
}

bool TargetLowering::expandCTTZ(SDNode *Node, SDValue &Result,
                                SelectionDAG &DAG) const {
  SDLoc dl(Node);
  EVT VT = Node->getValueType(0);
  SDValue Op = Node->getOperand(0);
  unsigned NumBitsPerElt = VT.getScalarSizeInBits();

  // If the non-ZERO_UNDEF version is supported we can use that instead.
  if (Node->getOpcode() == ISD::CTTZ_ZERO_UNDEF &&
      isOperationLegalOrCustom(ISD::CTTZ, VT)) {
    Result = DAG.getNode(ISD::CTTZ, dl, VT, Op);
    return true;
  }

  // If the ZERO_UNDEF version is supported use that and handle the zero case.
  if (isOperationLegalOrCustom(ISD::CTTZ_ZERO_UNDEF, VT)) {
    EVT SetCCVT =
        getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), VT);
    SDValue CTTZ = DAG.getNode(ISD::CTTZ_ZERO_UNDEF, dl, VT, Op);
    SDValue Zero = DAG.getConstant(0, dl, VT);
    SDValue SrcIsZero = DAG.getSetCC(dl, SetCCVT, Op, Zero, ISD::SETEQ);
    Result = DAG.getNode(ISD::SELECT, dl, VT, SrcIsZero,
                         DAG.getConstant(NumBitsPerElt, dl, VT), CTTZ);
    return true;
  }

  // Only expand vector types if we have the appropriate vector bit operations.
  if (VT.isVector() && (!isPowerOf2_32(NumBitsPerElt) ||
                        (!isOperationLegalOrCustom(ISD::CTPOP, VT) &&
                         !isOperationLegalOrCustom(ISD::CTLZ, VT)) ||
                        !isOperationLegalOrCustom(ISD::SUB, VT) ||
                        !isOperationLegalOrCustomOrPromote(ISD::AND, VT) ||
                        !isOperationLegalOrCustomOrPromote(ISD::XOR, VT)))
    return false;

  // for now, we use: { return popcount(~x & (x - 1)); }
  // unless the target has ctlz but not ctpop, in which case we use:
  // { return 32 - nlz(~x & (x-1)); }
  // Ref: "Hacker's Delight" by Henry Warren
  SDValue Tmp = DAG.getNode(
      ISD::AND, dl, VT, DAG.getNOT(dl, Op, VT),
      DAG.getNode(ISD::SUB, dl, VT, Op, DAG.getConstant(1, dl, VT)));

  // If ISD::CTLZ is legal and CTPOP isn't, then do that instead.
  if (isOperationLegal(ISD::CTLZ, VT) && !isOperationLegal(ISD::CTPOP, VT)) {
    Result =
        DAG.getNode(ISD::SUB, dl, VT, DAG.getConstant(NumBitsPerElt, dl, VT),
                    DAG.getNode(ISD::CTLZ, dl, VT, Tmp));
    return true;
  }

  Result = DAG.getNode(ISD::CTPOP, dl, VT, Tmp);
  return true;
}

bool TargetLowering::expandABS(SDNode *N, SDValue &Result,
                               SelectionDAG &DAG) const {
  SDLoc dl(N);
  EVT VT = N->getValueType(0);
  EVT ShVT = getShiftAmountTy(VT, DAG.getDataLayout());
  SDValue Op = N->getOperand(0);

  // Only expand vector types if we have the appropriate vector operations.
  if (VT.isVector() && (!isOperationLegalOrCustom(ISD::SRA, VT) ||
                        !isOperationLegalOrCustom(ISD::ADD, VT) ||
                        !isOperationLegalOrCustomOrPromote(ISD::XOR, VT)))
    return false;

  SDValue Shift =
      DAG.getNode(ISD::SRA, dl, VT, Op,
                  DAG.getConstant(VT.getScalarSizeInBits() - 1, dl, ShVT));
  SDValue Add = DAG.getNode(ISD::ADD, dl, VT, Op, Shift);
  Result = DAG.getNode(ISD::XOR, dl, VT, Add, Shift);
  return true;
}

SDValue TargetLowering::scalarizeVectorLoad(LoadSDNode *LD,
                                            SelectionDAG &DAG) const {
  SDLoc SL(LD);
  SDValue Chain = LD->getChain();
  SDValue BasePTR = LD->getBasePtr();
  EVT SrcVT = LD->getMemoryVT();
  ISD::LoadExtType ExtType = LD->getExtensionType();

  unsigned NumElem = SrcVT.getVectorNumElements();

  EVT SrcEltVT = SrcVT.getScalarType();
  EVT DstEltVT = LD->getValueType(0).getScalarType();

  unsigned Stride = SrcEltVT.getSizeInBits() / 8;
  assert(SrcEltVT.isByteSized());

  SmallVector<SDValue, 8> Vals;
  SmallVector<SDValue, 8> LoadChains;

  for (unsigned Idx = 0; Idx < NumElem; ++Idx) {
    SDValue ScalarLoad =
        DAG.getExtLoad(ExtType, SL, DstEltVT, Chain, BasePTR,
                       LD->getPointerInfo().getWithOffset(Idx * Stride),
                       SrcEltVT, MinAlign(LD->getAlignment(), Idx * Stride),
                       LD->getMemOperand()->getFlags(), LD->getAAInfo());

    BasePTR = DAG.getObjectPtrOffset(SL, BasePTR, Stride);

    Vals.push_back(ScalarLoad.getValue(0));
    LoadChains.push_back(ScalarLoad.getValue(1));
  }

  SDValue NewChain = DAG.getNode(ISD::TokenFactor, SL, MVT::Other, LoadChains);
  SDValue Value = DAG.getBuildVector(LD->getValueType(0), SL, Vals);

  return DAG.getMergeValues({Value, NewChain}, SL);
}

SDValue TargetLowering::scalarizeVectorStore(StoreSDNode *ST,
                                             SelectionDAG &DAG) const {
  SDLoc SL(ST);

  SDValue Chain = ST->getChain();
  SDValue BasePtr = ST->getBasePtr();
  SDValue Value = ST->getValue();
  EVT StVT = ST->getMemoryVT();

  // The type of the data we want to save
  EVT RegVT = Value.getValueType();
  EVT RegSclVT = RegVT.getScalarType();

  // The type of data as saved in memory.
  EVT MemSclVT = StVT.getScalarType();

  EVT IdxVT = getVectorIdxTy(DAG.getDataLayout());
  unsigned NumElem = StVT.getVectorNumElements();

  // A vector must always be stored in memory as-is, i.e. without any padding
  // between the elements, since various code depend on it, e.g. in the
  // handling of a bitcast of a vector type to int, which may be done with a
  // vector store followed by an integer load. A vector that does not have
  // elements that are byte-sized must therefore be stored as an integer
  // built out of the extracted vector elements.
  if (!MemSclVT.isByteSized()) {
    unsigned NumBits = StVT.getSizeInBits();
    EVT IntVT = EVT::getIntegerVT(*DAG.getContext(), NumBits);

    SDValue CurrVal = DAG.getConstant(0, SL, IntVT);

    for (unsigned Idx = 0; Idx < NumElem; ++Idx) {
      SDValue Elt = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, SL, RegSclVT, Value,
                                DAG.getConstant(Idx, SL, IdxVT));
      SDValue Trunc = DAG.getNode(ISD::TRUNCATE, SL, MemSclVT, Elt);
      SDValue ExtElt = DAG.getNode(ISD::ZERO_EXTEND, SL, IntVT, Trunc);
      unsigned ShiftIntoIdx =
          (DAG.getDataLayout().isBigEndian() ? (NumElem - 1) - Idx : Idx);
      SDValue ShiftAmount =
          DAG.getConstant(ShiftIntoIdx * MemSclVT.getSizeInBits(), SL, IntVT);
      SDValue ShiftedElt =
          DAG.getNode(ISD::SHL, SL, IntVT, ExtElt, ShiftAmount);
      CurrVal = DAG.getNode(ISD::OR, SL, IntVT, CurrVal, ShiftedElt);
    }

    return DAG.getStore(Chain, SL, CurrVal, BasePtr, ST->getPointerInfo(),
                        ST->getAlignment(), ST->getMemOperand()->getFlags(),
                        ST->getAAInfo());
  }

  // Store Stride in bytes
  unsigned Stride = MemSclVT.getSizeInBits() / 8;
  assert(Stride && "Zero stride!");
  // Extract each of the elements from the original vector and save them into
  // memory individually.
  SmallVector<SDValue, 8> Stores;
  for (unsigned Idx = 0; Idx < NumElem; ++Idx) {
    SDValue Elt = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, SL, RegSclVT, Value,
                              DAG.getConstant(Idx, SL, IdxVT));

    SDValue Ptr = DAG.getObjectPtrOffset(SL, BasePtr, Idx * Stride);

    // This scalar TruncStore may be illegal, but we legalize it later.
    SDValue Store = DAG.getTruncStore(
        Chain, SL, Elt, Ptr, ST->getPointerInfo().getWithOffset(Idx * Stride),
        MemSclVT, MinAlign(ST->getAlignment(), Idx * Stride),
        ST->getMemOperand()->getFlags(), ST->getAAInfo());

    Stores.push_back(Store);
  }

  return DAG.getNode(ISD::TokenFactor, SL, MVT::Other, Stores);
}

std::pair<SDValue, SDValue>
TargetLowering::expandUnalignedLoad(LoadSDNode *LD, SelectionDAG &DAG) const {
  assert(LD->getAddressingMode() == ISD::UNINDEXED &&
         "unaligned indexed loads not implemented!");
  SDValue Chain = LD->getChain();
  SDValue Ptr = LD->getBasePtr();
  EVT VT = LD->getValueType(0);
  EVT LoadedVT = LD->getMemoryVT();
  SDLoc dl(LD);
  auto &MF = DAG.getMachineFunction();

  if (VT.isFloatingPoint() || VT.isVector()) {
    EVT intVT = EVT::getIntegerVT(*DAG.getContext(), LoadedVT.getSizeInBits());
    if (isTypeLegal(intVT) && isTypeLegal(LoadedVT)) {
      if (!isOperationLegalOrCustom(ISD::LOAD, intVT) &&
          LoadedVT.isVector()) {
        // Scalarize the load and let the individual components be handled.
        SDValue Scalarized = scalarizeVectorLoad(LD, DAG);
        if (Scalarized->getOpcode() == ISD::MERGE_VALUES)
          return std::make_pair(Scalarized.getOperand(0), Scalarized.getOperand(1));
        return std::make_pair(Scalarized.getValue(0), Scalarized.getValue(1));
      }

      // Expand to a (misaligned) integer load of the same size,
      // then bitconvert to floating point or vector.
      SDValue newLoad = DAG.getLoad(intVT, dl, Chain, Ptr,
                                    LD->getMemOperand());
      SDValue Result = DAG.getNode(ISD::BITCAST, dl, LoadedVT, newLoad);
      if (LoadedVT != VT)
        Result = DAG.getNode(VT.isFloatingPoint() ? ISD::FP_EXTEND :
                             ISD::ANY_EXTEND, dl, VT, Result);

      return std::make_pair(Result, newLoad.getValue(1));
    }

    // Copy the value to a (aligned) stack slot using (unaligned) integer
    // loads and stores, then do a (aligned) load from the stack slot.
    MVT RegVT = getRegisterType(*DAG.getContext(), intVT);
    unsigned LoadedBytes = LoadedVT.getStoreSize();
    unsigned RegBytes = RegVT.getSizeInBits() / 8;
    unsigned NumRegs = (LoadedBytes + RegBytes - 1) / RegBytes;

    // Make sure the stack slot is also aligned for the register type.
    SDValue StackBase = DAG.CreateStackTemporary(LoadedVT, RegVT);
    auto FrameIndex = cast<FrameIndexSDNode>(StackBase.getNode())->getIndex();
    SmallVector<SDValue, 8> Stores;
    SDValue StackPtr = StackBase;
    unsigned Offset = 0;

    EVT PtrVT = Ptr.getValueType();
    EVT StackPtrVT = StackPtr.getValueType();

    SDValue PtrIncrement = DAG.getConstant(RegBytes, dl, PtrVT);
    SDValue StackPtrIncrement = DAG.getConstant(RegBytes, dl, StackPtrVT);

    // Do all but one copies using the full register width.
    for (unsigned i = 1; i < NumRegs; i++) {
      // Load one integer register's worth from the original location.
      SDValue Load = DAG.getLoad(
          RegVT, dl, Chain, Ptr, LD->getPointerInfo().getWithOffset(Offset),
          MinAlign(LD->getAlignment(), Offset), LD->getMemOperand()->getFlags(),
          LD->getAAInfo());
      // Follow the load with a store to the stack slot.  Remember the store.
      Stores.push_back(DAG.getStore(
          Load.getValue(1), dl, Load, StackPtr,
          MachinePointerInfo::getFixedStack(MF, FrameIndex, Offset)));
      // Increment the pointers.
      Offset += RegBytes;

      Ptr = DAG.getObjectPtrOffset(dl, Ptr, PtrIncrement);
      StackPtr = DAG.getObjectPtrOffset(dl, StackPtr, StackPtrIncrement);
    }

    // The last copy may be partial.  Do an extending load.
    EVT MemVT = EVT::getIntegerVT(*DAG.getContext(),
                                  8 * (LoadedBytes - Offset));
    SDValue Load =
        DAG.getExtLoad(ISD::EXTLOAD, dl, RegVT, Chain, Ptr,
                       LD->getPointerInfo().getWithOffset(Offset), MemVT,
                       MinAlign(LD->getAlignment(), Offset),
                       LD->getMemOperand()->getFlags(), LD->getAAInfo());
    // Follow the load with a store to the stack slot.  Remember the store.
    // On big-endian machines this requires a truncating store to ensure
    // that the bits end up in the right place.
    Stores.push_back(DAG.getTruncStore(
        Load.getValue(1), dl, Load, StackPtr,
        MachinePointerInfo::getFixedStack(MF, FrameIndex, Offset), MemVT));

    // The order of the stores doesn't matter - say it with a TokenFactor.
    SDValue TF = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Stores);

    // Finally, perform the original load only redirected to the stack slot.
    Load = DAG.getExtLoad(LD->getExtensionType(), dl, VT, TF, StackBase,
                          MachinePointerInfo::getFixedStack(MF, FrameIndex, 0),
                          LoadedVT);

    // Callers expect a MERGE_VALUES node.
    return std::make_pair(Load, TF);
  }

  assert(LoadedVT.isInteger() && !LoadedVT.isVector() &&
         "Unaligned load of unsupported type.");

  // Compute the new VT that is half the size of the old one.  This is an
  // integer MVT.
  unsigned NumBits = LoadedVT.getSizeInBits();
  EVT NewLoadedVT;
  NewLoadedVT = EVT::getIntegerVT(*DAG.getContext(), NumBits/2);
  NumBits >>= 1;

  unsigned Alignment = LD->getAlignment();
  unsigned IncrementSize = NumBits / 8;
  ISD::LoadExtType HiExtType = LD->getExtensionType();

  // If the original load is NON_EXTLOAD, the hi part load must be ZEXTLOAD.
  if (HiExtType == ISD::NON_EXTLOAD)
    HiExtType = ISD::ZEXTLOAD;

  // Load the value in two parts
  SDValue Lo, Hi;
  if (DAG.getDataLayout().isLittleEndian()) {
    Lo = DAG.getExtLoad(ISD::ZEXTLOAD, dl, VT, Chain, Ptr, LD->getPointerInfo(),
                        NewLoadedVT, Alignment, LD->getMemOperand()->getFlags(),
                        LD->getAAInfo());

    Ptr = DAG.getObjectPtrOffset(dl, Ptr, IncrementSize);
    Hi = DAG.getExtLoad(HiExtType, dl, VT, Chain, Ptr,
                        LD->getPointerInfo().getWithOffset(IncrementSize),
                        NewLoadedVT, MinAlign(Alignment, IncrementSize),
                        LD->getMemOperand()->getFlags(), LD->getAAInfo());
  } else {
    Hi = DAG.getExtLoad(HiExtType, dl, VT, Chain, Ptr, LD->getPointerInfo(),
                        NewLoadedVT, Alignment, LD->getMemOperand()->getFlags(),
                        LD->getAAInfo());

    Ptr = DAG.getObjectPtrOffset(dl, Ptr, IncrementSize);
    Lo = DAG.getExtLoad(ISD::ZEXTLOAD, dl, VT, Chain, Ptr,
                        LD->getPointerInfo().getWithOffset(IncrementSize),
                        NewLoadedVT, MinAlign(Alignment, IncrementSize),
                        LD->getMemOperand()->getFlags(), LD->getAAInfo());
  }

  // aggregate the two parts
  SDValue ShiftAmount =
      DAG.getConstant(NumBits, dl, getShiftAmountTy(Hi.getValueType(),
                                                    DAG.getDataLayout()));
  SDValue Result = DAG.getNode(ISD::SHL, dl, VT, Hi, ShiftAmount);
  Result = DAG.getNode(ISD::OR, dl, VT, Result, Lo);

  SDValue TF = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Lo.getValue(1),
                             Hi.getValue(1));

  return std::make_pair(Result, TF);
}

SDValue TargetLowering::expandUnalignedStore(StoreSDNode *ST,
                                             SelectionDAG &DAG) const {
  assert(ST->getAddressingMode() == ISD::UNINDEXED &&
         "unaligned indexed stores not implemented!");
  SDValue Chain = ST->getChain();
  SDValue Ptr = ST->getBasePtr();
  SDValue Val = ST->getValue();
  EVT VT = Val.getValueType();
  int Alignment = ST->getAlignment();
  auto &MF = DAG.getMachineFunction();
  EVT StoreMemVT = ST->getMemoryVT();

  SDLoc dl(ST);
  if (StoreMemVT.isFloatingPoint() || StoreMemVT.isVector()) {
    EVT intVT = EVT::getIntegerVT(*DAG.getContext(), VT.getSizeInBits());
    if (isTypeLegal(intVT)) {
      if (!isOperationLegalOrCustom(ISD::STORE, intVT) &&
          StoreMemVT.isVector()) {
        // Scalarize the store and let the individual components be handled.
        SDValue Result = scalarizeVectorStore(ST, DAG);
        return Result;
      }
      // Expand to a bitconvert of the value to the integer type of the
      // same size, then a (misaligned) int store.
      // FIXME: Does not handle truncating floating point stores!
      SDValue Result = DAG.getNode(ISD::BITCAST, dl, intVT, Val);
      Result = DAG.getStore(Chain, dl, Result, Ptr, ST->getPointerInfo(),
                            Alignment, ST->getMemOperand()->getFlags());
      return Result;
    }
    // Do a (aligned) store to a stack slot, then copy from the stack slot
    // to the final destination using (unaligned) integer loads and stores.
    MVT RegVT = getRegisterType(
        *DAG.getContext(),
        EVT::getIntegerVT(*DAG.getContext(), StoreMemVT.getSizeInBits()));
    EVT PtrVT = Ptr.getValueType();
    unsigned StoredBytes = StoreMemVT.getStoreSize();
    unsigned RegBytes = RegVT.getSizeInBits() / 8;
    unsigned NumRegs = (StoredBytes + RegBytes - 1) / RegBytes;

    // Make sure the stack slot is also aligned for the register type.
    SDValue StackPtr = DAG.CreateStackTemporary(StoreMemVT, RegVT);
    auto FrameIndex = cast<FrameIndexSDNode>(StackPtr.getNode())->getIndex();

    // Perform the original store, only redirected to the stack slot.
    SDValue Store = DAG.getTruncStore(
        Chain, dl, Val, StackPtr,
        MachinePointerInfo::getFixedStack(MF, FrameIndex, 0), StoreMemVT);

    EVT StackPtrVT = StackPtr.getValueType();

    SDValue PtrIncrement = DAG.getConstant(RegBytes, dl, PtrVT);
    SDValue StackPtrIncrement = DAG.getConstant(RegBytes, dl, StackPtrVT);
    SmallVector<SDValue, 8> Stores;
    unsigned Offset = 0;

    // Do all but one copies using the full register width.
    for (unsigned i = 1; i < NumRegs; i++) {
      // Load one integer register's worth from the stack slot.
      SDValue Load = DAG.getLoad(
          RegVT, dl, Store, StackPtr,
          MachinePointerInfo::getFixedStack(MF, FrameIndex, Offset));
      // Store it to the final location.  Remember the store.
      Stores.push_back(DAG.getStore(Load.getValue(1), dl, Load, Ptr,
                                    ST->getPointerInfo().getWithOffset(Offset),
                                    MinAlign(ST->getAlignment(), Offset),
                                    ST->getMemOperand()->getFlags()));
      // Increment the pointers.
      Offset += RegBytes;
      StackPtr = DAG.getObjectPtrOffset(dl, StackPtr, StackPtrIncrement);
      Ptr = DAG.getObjectPtrOffset(dl, Ptr, PtrIncrement);
    }

    // The last store may be partial.  Do a truncating store.  On big-endian
    // machines this requires an extending load from the stack slot to ensure
    // that the bits are in the right place.
    EVT LoadMemVT =
        EVT::getIntegerVT(*DAG.getContext(), 8 * (StoredBytes - Offset));

    // Load from the stack slot.
    SDValue Load = DAG.getExtLoad(
        ISD::EXTLOAD, dl, RegVT, Store, StackPtr,
        MachinePointerInfo::getFixedStack(MF, FrameIndex, Offset), LoadMemVT);

    Stores.push_back(
        DAG.getTruncStore(Load.getValue(1), dl, Load, Ptr,
                          ST->getPointerInfo().getWithOffset(Offset), LoadMemVT,
                          MinAlign(ST->getAlignment(), Offset),
                          ST->getMemOperand()->getFlags(), ST->getAAInfo()));
    // The order of the stores doesn't matter - say it with a TokenFactor.
    SDValue Result = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Stores);
    return Result;
  }

  assert(StoreMemVT.isInteger() && !StoreMemVT.isVector() &&
         "Unaligned store of unknown type.");
  // Get the half-size VT
  EVT NewStoredVT = StoreMemVT.getHalfSizedIntegerVT(*DAG.getContext());
  int NumBits = NewStoredVT.getSizeInBits();
  int IncrementSize = NumBits / 8;

  // Divide the stored value in two parts.
  SDValue ShiftAmount = DAG.getConstant(
      NumBits, dl, getShiftAmountTy(Val.getValueType(), DAG.getDataLayout()));
  SDValue Lo = Val;
  SDValue Hi = DAG.getNode(ISD::SRL, dl, VT, Val, ShiftAmount);

  // Store the two parts
  SDValue Store1, Store2;
  Store1 = DAG.getTruncStore(Chain, dl,
                             DAG.getDataLayout().isLittleEndian() ? Lo : Hi,
                             Ptr, ST->getPointerInfo(), NewStoredVT, Alignment,
                             ST->getMemOperand()->getFlags());

  Ptr = DAG.getObjectPtrOffset(dl, Ptr, IncrementSize);
  Alignment = MinAlign(Alignment, IncrementSize);
  Store2 = DAG.getTruncStore(
      Chain, dl, DAG.getDataLayout().isLittleEndian() ? Hi : Lo, Ptr,
      ST->getPointerInfo().getWithOffset(IncrementSize), NewStoredVT, Alignment,
      ST->getMemOperand()->getFlags(), ST->getAAInfo());

  SDValue Result =
      DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Store1, Store2);
  return Result;
}

SDValue
TargetLowering::IncrementMemoryAddress(SDValue Addr, SDValue Mask,
                                       const SDLoc &DL, EVT DataVT,
                                       SelectionDAG &DAG,
                                       bool IsCompressedMemory) const {
  SDValue Increment;
  EVT AddrVT = Addr.getValueType();
  EVT MaskVT = Mask.getValueType();
  assert(DataVT.getVectorNumElements() == MaskVT.getVectorNumElements() &&
         "Incompatible types of Data and Mask");
  if (IsCompressedMemory) {
    // Incrementing the pointer according to number of '1's in the mask.
    EVT MaskIntVT = EVT::getIntegerVT(*DAG.getContext(), MaskVT.getSizeInBits());
    SDValue MaskInIntReg = DAG.getBitcast(MaskIntVT, Mask);
    if (MaskIntVT.getSizeInBits() < 32) {
      MaskInIntReg = DAG.getNode(ISD::ZERO_EXTEND, DL, MVT::i32, MaskInIntReg);
      MaskIntVT = MVT::i32;
    }

    // Count '1's with POPCNT.
    Increment = DAG.getNode(ISD::CTPOP, DL, MaskIntVT, MaskInIntReg);
    Increment = DAG.getZExtOrTrunc(Increment, DL, AddrVT);
    // Scale is an element size in bytes.
    SDValue Scale = DAG.getConstant(DataVT.getScalarSizeInBits() / 8, DL,
                                    AddrVT);
    Increment = DAG.getNode(ISD::MUL, DL, AddrVT, Increment, Scale);
  } else
    Increment = DAG.getConstant(DataVT.getStoreSize(), DL, AddrVT);

  return DAG.getNode(ISD::ADD, DL, AddrVT, Addr, Increment);
}

static SDValue clampDynamicVectorIndex(SelectionDAG &DAG,
                                       SDValue Idx,
                                       EVT VecVT,
                                       const SDLoc &dl) {
  if (isa<ConstantSDNode>(Idx))
    return Idx;

  EVT IdxVT = Idx.getValueType();
  unsigned NElts = VecVT.getVectorNumElements();
  if (isPowerOf2_32(NElts)) {
    APInt Imm = APInt::getLowBitsSet(IdxVT.getSizeInBits(),
                                     Log2_32(NElts));
    return DAG.getNode(ISD::AND, dl, IdxVT, Idx,
                       DAG.getConstant(Imm, dl, IdxVT));
  }

  return DAG.getNode(ISD::UMIN, dl, IdxVT, Idx,
                     DAG.getConstant(NElts - 1, dl, IdxVT));
}

SDValue TargetLowering::getVectorElementPointer(SelectionDAG &DAG,
                                                SDValue VecPtr, EVT VecVT,
                                                SDValue Index) const {
  SDLoc dl(Index);
  // Make sure the index type is big enough to compute in.
  Index = DAG.getZExtOrTrunc(Index, dl, VecPtr.getValueType());

  EVT EltVT = VecVT.getVectorElementType();

  // Calculate the element offset and add it to the pointer.
  unsigned EltSize = EltVT.getSizeInBits() / 8; // FIXME: should be ABI size.
  assert(EltSize * 8 == EltVT.getSizeInBits() &&
         "Converting bits to bytes lost precision");

  Index = clampDynamicVectorIndex(DAG, Index, VecVT, dl);

  EVT IdxVT = Index.getValueType();

  Index = DAG.getNode(ISD::MUL, dl, IdxVT, Index,
                      DAG.getConstant(EltSize, dl, IdxVT));
  return DAG.getNode(ISD::ADD, dl, IdxVT, VecPtr, Index);
}

//===----------------------------------------------------------------------===//
// Implementation of Emulated TLS Model
//===----------------------------------------------------------------------===//

SDValue TargetLowering::LowerToTLSEmulatedModel(const GlobalAddressSDNode *GA,
                                                SelectionDAG &DAG) const {
  // Access to address of TLS varialbe xyz is lowered to a function call:
  //   __emutls_get_address( address of global variable named "__emutls_v.xyz" )
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  PointerType *VoidPtrType = Type::getInt8PtrTy(*DAG.getContext());
  SDLoc dl(GA);

  ArgListTy Args;
  ArgListEntry Entry;
  std::string NameString = ("__emutls_v." + GA->getGlobal()->getName()).str();
  Module *VariableModule = const_cast<Module*>(GA->getGlobal()->getParent());
  StringRef EmuTlsVarName(NameString);
  GlobalVariable *EmuTlsVar = VariableModule->getNamedGlobal(EmuTlsVarName);
  assert(EmuTlsVar && "Cannot find EmuTlsVar ");
  Entry.Node = DAG.getGlobalAddress(EmuTlsVar, dl, PtrVT);
  Entry.Ty = VoidPtrType;
  Args.push_back(Entry);

  SDValue EmuTlsGetAddr = DAG.getExternalSymbol("__emutls_get_address", PtrVT);

  TargetLowering::CallLoweringInfo CLI(DAG);
  CLI.setDebugLoc(dl).setChain(DAG.getEntryNode());
  CLI.setLibCallee(CallingConv::C, VoidPtrType, EmuTlsGetAddr, std::move(Args));
  std::pair<SDValue, SDValue> CallResult = LowerCallTo(CLI);

  // TLSADDR will be codegen'ed as call. Inform MFI that function has calls.
  // At last for X86 targets, maybe good for other targets too?
  MachineFrameInfo &MFI = DAG.getMachineFunction().getFrameInfo();
  MFI.setAdjustsStack(true); // Is this only for X86 target?
  MFI.setHasCalls(true);

  assert((GA->getOffset() == 0) &&
         "Emulated TLS must have zero offset in GlobalAddressSDNode");
  return CallResult.first;
}

SDValue TargetLowering::lowerCmpEqZeroToCtlzSrl(SDValue Op,
                                                SelectionDAG &DAG) const {
  assert((Op->getOpcode() == ISD::SETCC) && "Input has to be a SETCC node.");
  if (!isCtlzFast())
    return SDValue();
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(2))->get();
  SDLoc dl(Op);
  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op.getOperand(1))) {
    if (C->isNullValue() && CC == ISD::SETEQ) {
      EVT VT = Op.getOperand(0).getValueType();
      SDValue Zext = Op.getOperand(0);
      if (VT.bitsLT(MVT::i32)) {
        VT = MVT::i32;
        Zext = DAG.getNode(ISD::ZERO_EXTEND, dl, VT, Op.getOperand(0));
      }
      unsigned Log2b = Log2_32(VT.getSizeInBits());
      SDValue Clz = DAG.getNode(ISD::CTLZ, dl, VT, Zext);
      SDValue Scc = DAG.getNode(ISD::SRL, dl, VT, Clz,
                                DAG.getConstant(Log2b, dl, MVT::i32));
      return DAG.getNode(ISD::TRUNCATE, dl, MVT::i32, Scc);
    }
  }
  return SDValue();
}

SDValue TargetLowering::expandAddSubSat(SDNode *Node, SelectionDAG &DAG) const {
  unsigned Opcode = Node->getOpcode();
  SDValue LHS = Node->getOperand(0);
  SDValue RHS = Node->getOperand(1);
  EVT VT = LHS.getValueType();
  SDLoc dl(Node);

  assert(VT == RHS.getValueType() && "Expected operands to be the same type");
  assert(VT.isInteger() && "Expected operands to be integers");

  // usub.sat(a, b) -> umax(a, b) - b
  if (Opcode == ISD::USUBSAT && isOperationLegalOrCustom(ISD::UMAX, VT)) {
    SDValue Max = DAG.getNode(ISD::UMAX, dl, VT, LHS, RHS);
    return DAG.getNode(ISD::SUB, dl, VT, Max, RHS);
  }

  if (Opcode == ISD::UADDSAT && isOperationLegalOrCustom(ISD::UMIN, VT)) {
    SDValue InvRHS = DAG.getNOT(dl, RHS, VT);
    SDValue Min = DAG.getNode(ISD::UMIN, dl, VT, LHS, InvRHS);
    return DAG.getNode(ISD::ADD, dl, VT, Min, RHS);
  }

  unsigned OverflowOp;
  switch (Opcode) {
  case ISD::SADDSAT:
    OverflowOp = ISD::SADDO;
    break;
  case ISD::UADDSAT:
    OverflowOp = ISD::UADDO;
    break;
  case ISD::SSUBSAT:
    OverflowOp = ISD::SSUBO;
    break;
  case ISD::USUBSAT:
    OverflowOp = ISD::USUBO;
    break;
  default:
    llvm_unreachable("Expected method to receive signed or unsigned saturation "
                     "addition or subtraction node.");
  }

  unsigned BitWidth = LHS.getScalarValueSizeInBits();
  EVT BoolVT = getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), VT);
  SDValue Result = DAG.getNode(OverflowOp, dl, DAG.getVTList(VT, BoolVT),
                               LHS, RHS);
  SDValue SumDiff = Result.getValue(0);
  SDValue Overflow = Result.getValue(1);
  SDValue Zero = DAG.getConstant(0, dl, VT);
  SDValue AllOnes = DAG.getAllOnesConstant(dl, VT);

  if (Opcode == ISD::UADDSAT) {
    if (getBooleanContents(VT) == ZeroOrNegativeOneBooleanContent) {
      // (LHS + RHS) | OverflowMask
      SDValue OverflowMask = DAG.getSExtOrTrunc(Overflow, dl, VT);
      return DAG.getNode(ISD::OR, dl, VT, SumDiff, OverflowMask);
    }
    // Overflow ? 0xffff.... : (LHS + RHS)
    return DAG.getSelect(dl, VT, Overflow, AllOnes, SumDiff);
  } else if (Opcode == ISD::USUBSAT) {
    if (getBooleanContents(VT) == ZeroOrNegativeOneBooleanContent) {
      // (LHS - RHS) & ~OverflowMask
      SDValue OverflowMask = DAG.getSExtOrTrunc(Overflow, dl, VT);
      SDValue Not = DAG.getNOT(dl, OverflowMask, VT);
      return DAG.getNode(ISD::AND, dl, VT, SumDiff, Not);
    }
    // Overflow ? 0 : (LHS - RHS)
    return DAG.getSelect(dl, VT, Overflow, Zero, SumDiff);
  } else {
    // SatMax -> Overflow && SumDiff < 0
    // SatMin -> Overflow && SumDiff >= 0
    APInt MinVal = APInt::getSignedMinValue(BitWidth);
    APInt MaxVal = APInt::getSignedMaxValue(BitWidth);
    SDValue SatMin = DAG.getConstant(MinVal, dl, VT);
    SDValue SatMax = DAG.getConstant(MaxVal, dl, VT);
    SDValue SumNeg = DAG.getSetCC(dl, BoolVT, SumDiff, Zero, ISD::SETLT);
    Result = DAG.getSelect(dl, VT, SumNeg, SatMax, SatMin);
    return DAG.getSelect(dl, VT, Overflow, Result, SumDiff);
  }
}

SDValue
TargetLowering::expandFixedPointMul(SDNode *Node, SelectionDAG &DAG) const {
  assert((Node->getOpcode() == ISD::SMULFIX ||
          Node->getOpcode() == ISD::UMULFIX ||
          Node->getOpcode() == ISD::SMULFIXSAT ||
          Node->getOpcode() == ISD::UMULFIXSAT) &&
         "Expected a fixed point multiplication opcode");

  SDLoc dl(Node);
  SDValue LHS = Node->getOperand(0);
  SDValue RHS = Node->getOperand(1);
  EVT VT = LHS.getValueType();
  unsigned Scale = Node->getConstantOperandVal(2);
  bool Saturating = (Node->getOpcode() == ISD::SMULFIXSAT ||
                     Node->getOpcode() == ISD::UMULFIXSAT);
  bool Signed = (Node->getOpcode() == ISD::SMULFIX ||
                 Node->getOpcode() == ISD::SMULFIXSAT);
  EVT BoolVT = getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), VT);
  unsigned VTSize = VT.getScalarSizeInBits();

  if (!Scale) {
    // [us]mul.fix(a, b, 0) -> mul(a, b)
    if (!Saturating) {
      if (isOperationLegalOrCustom(ISD::MUL, VT))
        return DAG.getNode(ISD::MUL, dl, VT, LHS, RHS);
    } else if (Signed && isOperationLegalOrCustom(ISD::SMULO, VT)) {
      SDValue Result =
          DAG.getNode(ISD::SMULO, dl, DAG.getVTList(VT, BoolVT), LHS, RHS);
      SDValue Product = Result.getValue(0);
      SDValue Overflow = Result.getValue(1);
      SDValue Zero = DAG.getConstant(0, dl, VT);

      APInt MinVal = APInt::getSignedMinValue(VTSize);
      APInt MaxVal = APInt::getSignedMaxValue(VTSize);
      SDValue SatMin = DAG.getConstant(MinVal, dl, VT);
      SDValue SatMax = DAG.getConstant(MaxVal, dl, VT);
      SDValue ProdNeg = DAG.getSetCC(dl, BoolVT, Product, Zero, ISD::SETLT);
      Result = DAG.getSelect(dl, VT, ProdNeg, SatMax, SatMin);
      return DAG.getSelect(dl, VT, Overflow, Result, Product);
    } else if (!Signed && isOperationLegalOrCustom(ISD::UMULO, VT)) {
      SDValue Result =
          DAG.getNode(ISD::UMULO, dl, DAG.getVTList(VT, BoolVT), LHS, RHS);
      SDValue Product = Result.getValue(0);
      SDValue Overflow = Result.getValue(1);

      APInt MaxVal = APInt::getMaxValue(VTSize);
      SDValue SatMax = DAG.getConstant(MaxVal, dl, VT);
      return DAG.getSelect(dl, VT, Overflow, SatMax, Product);
    }
  }

  assert(((Signed && Scale < VTSize) || (!Signed && Scale <= VTSize)) &&
         "Expected scale to be less than the number of bits if signed or at "
         "most the number of bits if unsigned.");
  assert(LHS.getValueType() == RHS.getValueType() &&
         "Expected both operands to be the same type");

  // Get the upper and lower bits of the result.
  SDValue Lo, Hi;
  unsigned LoHiOp = Signed ? ISD::SMUL_LOHI : ISD::UMUL_LOHI;
  unsigned HiOp = Signed ? ISD::MULHS : ISD::MULHU;
  if (isOperationLegalOrCustom(LoHiOp, VT)) {
    SDValue Result = DAG.getNode(LoHiOp, dl, DAG.getVTList(VT, VT), LHS, RHS);
    Lo = Result.getValue(0);
    Hi = Result.getValue(1);
  } else if (isOperationLegalOrCustom(HiOp, VT)) {
    Lo = DAG.getNode(ISD::MUL, dl, VT, LHS, RHS);
    Hi = DAG.getNode(HiOp, dl, VT, LHS, RHS);
  } else if (VT.isVector()) {
    return SDValue();
  } else {
    report_fatal_error("Unable to expand fixed point multiplication.");
  }

  if (Scale == VTSize)
    // Result is just the top half since we'd be shifting by the width of the
    // operand. Overflow impossible so this works for both UMULFIX and
    // UMULFIXSAT.
    return Hi;

  // The result will need to be shifted right by the scale since both operands
  // are scaled. The result is given to us in 2 halves, so we only want part of
  // both in the result.
  EVT ShiftTy = getShiftAmountTy(VT, DAG.getDataLayout());
  SDValue Result = DAG.getNode(ISD::FSHR, dl, VT, Hi, Lo,
                               DAG.getConstant(Scale, dl, ShiftTy));
  if (!Saturating)
    return Result;

  if (!Signed) {
    // Unsigned overflow happened if the upper (VTSize - Scale) bits (of the
    // widened multiplication) aren't all zeroes.

    // Saturate to max if ((Hi >> Scale) != 0),
    // which is the same as if (Hi > ((1 << Scale) - 1))
    APInt MaxVal = APInt::getMaxValue(VTSize);
    SDValue LowMask = DAG.getConstant(APInt::getLowBitsSet(VTSize, Scale),
                                      dl, VT);
    Result = DAG.getSelectCC(dl, Hi, LowMask,
                             DAG.getConstant(MaxVal, dl, VT), Result,
                             ISD::SETUGT);

    return Result;
  }

  // Signed overflow happened if the upper (VTSize - Scale + 1) bits (of the
  // widened multiplication) aren't all ones or all zeroes.

  SDValue SatMin = DAG.getConstant(APInt::getSignedMinValue(VTSize), dl, VT);
  SDValue SatMax = DAG.getConstant(APInt::getSignedMaxValue(VTSize), dl, VT);

  if (Scale == 0) {
    SDValue Sign = DAG.getNode(ISD::SRA, dl, VT, Lo,
                               DAG.getConstant(VTSize - 1, dl, ShiftTy));
    SDValue Overflow = DAG.getSetCC(dl, BoolVT, Hi, Sign, ISD::SETNE);
    // Saturated to SatMin if wide product is negative, and SatMax if wide
    // product is positive ...
    SDValue Zero = DAG.getConstant(0, dl, VT);
    SDValue ResultIfOverflow = DAG.getSelectCC(dl, Hi, Zero, SatMin, SatMax,
                                               ISD::SETLT);
    // ... but only if we overflowed.
    return DAG.getSelect(dl, VT, Overflow, ResultIfOverflow, Result);
  }

  //  We handled Scale==0 above so all the bits to examine is in Hi.

  // Saturate to max if ((Hi >> (Scale - 1)) > 0),
  // which is the same as if (Hi > (1 << (Scale - 1)) - 1)
  SDValue LowMask = DAG.getConstant(APInt::getLowBitsSet(VTSize, Scale - 1),
                                    dl, VT);
  Result = DAG.getSelectCC(dl, Hi, LowMask, SatMax, Result, ISD::SETGT);
  // Saturate to min if (Hi >> (Scale - 1)) < -1),
  // which is the same as if (HI < (-1 << (Scale - 1))
  SDValue HighMask =
      DAG.getConstant(APInt::getHighBitsSet(VTSize, VTSize - Scale + 1),
                      dl, VT);
  Result = DAG.getSelectCC(dl, Hi, HighMask, SatMin, Result, ISD::SETLT);
  return Result;
}

void TargetLowering::expandUADDSUBO(
    SDNode *Node, SDValue &Result, SDValue &Overflow, SelectionDAG &DAG) const {
  SDLoc dl(Node);
  SDValue LHS = Node->getOperand(0);
  SDValue RHS = Node->getOperand(1);
  bool IsAdd = Node->getOpcode() == ISD::UADDO;

  // If ADD/SUBCARRY is legal, use that instead.
  unsigned OpcCarry = IsAdd ? ISD::ADDCARRY : ISD::SUBCARRY;
  if (isOperationLegalOrCustom(OpcCarry, Node->getValueType(0))) {
    SDValue CarryIn = DAG.getConstant(0, dl, Node->getValueType(1));
    SDValue NodeCarry = DAG.getNode(OpcCarry, dl, Node->getVTList(),
                                    { LHS, RHS, CarryIn });
    Result = SDValue(NodeCarry.getNode(), 0);
    Overflow = SDValue(NodeCarry.getNode(), 1);
    return;
  }

  Result = DAG.getNode(IsAdd ? ISD::ADD : ISD::SUB, dl,
                            LHS.getValueType(), LHS, RHS);

  EVT ResultType = Node->getValueType(1);
  EVT SetCCType = getSetCCResultType(
      DAG.getDataLayout(), *DAG.getContext(), Node->getValueType(0));
  ISD::CondCode CC = IsAdd ? ISD::SETULT : ISD::SETUGT;
  SDValue SetCC = DAG.getSetCC(dl, SetCCType, Result, LHS, CC);
  Overflow = DAG.getBoolExtOrTrunc(SetCC, dl, ResultType, ResultType);
}

void TargetLowering::expandSADDSUBO(
    SDNode *Node, SDValue &Result, SDValue &Overflow, SelectionDAG &DAG) const {
  SDLoc dl(Node);
  SDValue LHS = Node->getOperand(0);
  SDValue RHS = Node->getOperand(1);
  bool IsAdd = Node->getOpcode() == ISD::SADDO;

  Result = DAG.getNode(IsAdd ? ISD::ADD : ISD::SUB, dl,
                            LHS.getValueType(), LHS, RHS);

  EVT ResultType = Node->getValueType(1);
  EVT OType = getSetCCResultType(
      DAG.getDataLayout(), *DAG.getContext(), Node->getValueType(0));

  // If SADDSAT/SSUBSAT is legal, compare results to detect overflow.
  unsigned OpcSat = IsAdd ? ISD::SADDSAT : ISD::SSUBSAT;
  if (isOperationLegalOrCustom(OpcSat, LHS.getValueType())) {
    SDValue Sat = DAG.getNode(OpcSat, dl, LHS.getValueType(), LHS, RHS);
    SDValue SetCC = DAG.getSetCC(dl, OType, Result, Sat, ISD::SETNE);
    Overflow = DAG.getBoolExtOrTrunc(SetCC, dl, ResultType, ResultType);
    return;
  }

  SDValue Zero = DAG.getConstant(0, dl, LHS.getValueType());

  // For an addition, the result should be less than one of the operands (LHS)
  // if and only if the other operand (RHS) is negative, otherwise there will
  // be overflow.
  // For a subtraction, the result should be less than one of the operands
  // (LHS) if and only if the other operand (RHS) is (non-zero) positive,
  // otherwise there will be overflow.
  SDValue ResultLowerThanLHS = DAG.getSetCC(dl, OType, Result, LHS, ISD::SETLT);
  SDValue ConditionRHS =
      DAG.getSetCC(dl, OType, RHS, Zero, IsAdd ? ISD::SETLT : ISD::SETGT);

  Overflow = DAG.getBoolExtOrTrunc(
      DAG.getNode(ISD::XOR, dl, OType, ConditionRHS, ResultLowerThanLHS), dl,
      ResultType, ResultType);
}

bool TargetLowering::expandMULO(SDNode *Node, SDValue &Result,
                                SDValue &Overflow, SelectionDAG &DAG) const {
  SDLoc dl(Node);
  EVT VT = Node->getValueType(0);
  EVT SetCCVT = getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), VT);
  SDValue LHS = Node->getOperand(0);
  SDValue RHS = Node->getOperand(1);
  bool isSigned = Node->getOpcode() == ISD::SMULO;

  // For power-of-two multiplications we can use a simpler shift expansion.
  if (ConstantSDNode *RHSC = isConstOrConstSplat(RHS)) {
    const APInt &C = RHSC->getAPIntValue();
    // mulo(X, 1 << S) -> { X << S, (X << S) >> S != X }
    if (C.isPowerOf2()) {
      // smulo(x, signed_min) is same as umulo(x, signed_min).
      bool UseArithShift = isSigned && !C.isMinSignedValue();
      EVT ShiftAmtTy = getShiftAmountTy(VT, DAG.getDataLayout());
      SDValue ShiftAmt = DAG.getConstant(C.logBase2(), dl, ShiftAmtTy);
      Result = DAG.getNode(ISD::SHL, dl, VT, LHS, ShiftAmt);
      Overflow = DAG.getSetCC(dl, SetCCVT,
          DAG.getNode(UseArithShift ? ISD::SRA : ISD::SRL,
                      dl, VT, Result, ShiftAmt),
          LHS, ISD::SETNE);
      return true;
    }
  }

  EVT WideVT = EVT::getIntegerVT(*DAG.getContext(), VT.getScalarSizeInBits() * 2);
  if (VT.isVector())
    WideVT = EVT::getVectorVT(*DAG.getContext(), WideVT,
                              VT.getVectorNumElements());

  SDValue BottomHalf;
  SDValue TopHalf;
  static const unsigned Ops[2][3] =
      { { ISD::MULHU, ISD::UMUL_LOHI, ISD::ZERO_EXTEND },
        { ISD::MULHS, ISD::SMUL_LOHI, ISD::SIGN_EXTEND }};
  if (isOperationLegalOrCustom(Ops[isSigned][0], VT)) {
    BottomHalf = DAG.getNode(ISD::MUL, dl, VT, LHS, RHS);
    TopHalf = DAG.getNode(Ops[isSigned][0], dl, VT, LHS, RHS);
  } else if (isOperationLegalOrCustom(Ops[isSigned][1], VT)) {
    BottomHalf = DAG.getNode(Ops[isSigned][1], dl, DAG.getVTList(VT, VT), LHS,
                             RHS);
    TopHalf = BottomHalf.getValue(1);
  } else if (isTypeLegal(WideVT)) {
    LHS = DAG.getNode(Ops[isSigned][2], dl, WideVT, LHS);
    RHS = DAG.getNode(Ops[isSigned][2], dl, WideVT, RHS);
    SDValue Mul = DAG.getNode(ISD::MUL, dl, WideVT, LHS, RHS);
    BottomHalf = DAG.getNode(ISD::TRUNCATE, dl, VT, Mul);
    SDValue ShiftAmt = DAG.getConstant(VT.getScalarSizeInBits(), dl,
        getShiftAmountTy(WideVT, DAG.getDataLayout()));
    TopHalf = DAG.getNode(ISD::TRUNCATE, dl, VT,
                          DAG.getNode(ISD::SRL, dl, WideVT, Mul, ShiftAmt));
  } else {
    if (VT.isVector())
      return false;

    // We can fall back to a libcall with an illegal type for the MUL if we
    // have a libcall big enough.
    // Also, we can fall back to a division in some cases, but that's a big
    // performance hit in the general case.
    RTLIB::Libcall LC = RTLIB::UNKNOWN_LIBCALL;
    if (WideVT == MVT::i16)
      LC = RTLIB::MUL_I16;
    else if (WideVT == MVT::i32)
      LC = RTLIB::MUL_I32;
    else if (WideVT == MVT::i64)
      LC = RTLIB::MUL_I64;
    else if (WideVT == MVT::i128)
      LC = RTLIB::MUL_I128;
    assert(LC != RTLIB::UNKNOWN_LIBCALL && "Cannot expand this operation!");

    SDValue HiLHS;
    SDValue HiRHS;
    if (isSigned) {
      // The high part is obtained by SRA'ing all but one of the bits of low
      // part.
      unsigned LoSize = VT.getSizeInBits();
      HiLHS =
          DAG.getNode(ISD::SRA, dl, VT, LHS,
                      DAG.getConstant(LoSize - 1, dl,
                                      getPointerTy(DAG.getDataLayout())));
      HiRHS =
          DAG.getNode(ISD::SRA, dl, VT, RHS,
                      DAG.getConstant(LoSize - 1, dl,
                                      getPointerTy(DAG.getDataLayout())));
    } else {
        HiLHS = DAG.getConstant(0, dl, VT);
        HiRHS = DAG.getConstant(0, dl, VT);
    }

    // Here we're passing the 2 arguments explicitly as 4 arguments that are
    // pre-lowered to the correct types. This all depends upon WideVT not
    // being a legal type for the architecture and thus has to be split to
    // two arguments.
    SDValue Ret;
    TargetLowering::MakeLibCallOptions CallOptions;
    CallOptions.setSExt(isSigned);
    CallOptions.setIsPostTypeLegalization(true);
    if (shouldSplitFunctionArgumentsAsLittleEndian(DAG.getDataLayout())) {
      // Halves of WideVT are packed into registers in different order
      // depending on platform endianness. This is usually handled by
      // the C calling convention, but we can't defer to it in
      // the legalizer.
      SDValue Args[] = { LHS, HiLHS, RHS, HiRHS };
      Ret = makeLibCall(DAG, LC, WideVT, Args, CallOptions, dl).first;
    } else {
      SDValue Args[] = { HiLHS, LHS, HiRHS, RHS };
      Ret = makeLibCall(DAG, LC, WideVT, Args, CallOptions, dl).first;
    }
    assert(Ret.getOpcode() == ISD::MERGE_VALUES &&
           "Ret value is a collection of constituent nodes holding result.");
    if (DAG.getDataLayout().isLittleEndian()) {
      // Same as above.
      BottomHalf = Ret.getOperand(0);
      TopHalf = Ret.getOperand(1);
    } else {
      BottomHalf = Ret.getOperand(1);
      TopHalf = Ret.getOperand(0);
    }
  }

  Result = BottomHalf;
  if (isSigned) {
    SDValue ShiftAmt = DAG.getConstant(
        VT.getScalarSizeInBits() - 1, dl,
        getShiftAmountTy(BottomHalf.getValueType(), DAG.getDataLayout()));
    SDValue Sign = DAG.getNode(ISD::SRA, dl, VT, BottomHalf, ShiftAmt);
    Overflow = DAG.getSetCC(dl, SetCCVT, TopHalf, Sign, ISD::SETNE);
  } else {
    Overflow = DAG.getSetCC(dl, SetCCVT, TopHalf,
                            DAG.getConstant(0, dl, VT), ISD::SETNE);
  }

  // Truncate the result if SetCC returns a larger type than needed.
  EVT RType = Node->getValueType(1);
  if (RType.getSizeInBits() < Overflow.getValueSizeInBits())
    Overflow = DAG.getNode(ISD::TRUNCATE, dl, RType, Overflow);

  assert(RType.getSizeInBits() == Overflow.getValueSizeInBits() &&
         "Unexpected result type for S/UMULO legalization");
  return true;
}

SDValue TargetLowering::expandVecReduce(SDNode *Node, SelectionDAG &DAG) const {
  SDLoc dl(Node);
  bool NoNaN = Node->getFlags().hasNoNaNs();
  unsigned BaseOpcode = 0;
  switch (Node->getOpcode()) {
  default: llvm_unreachable("Expected VECREDUCE opcode");
  case ISD::VECREDUCE_FADD: BaseOpcode = ISD::FADD; break;
  case ISD::VECREDUCE_FMUL: BaseOpcode = ISD::FMUL; break;
  case ISD::VECREDUCE_ADD:  BaseOpcode = ISD::ADD; break;
  case ISD::VECREDUCE_MUL:  BaseOpcode = ISD::MUL; break;
  case ISD::VECREDUCE_AND:  BaseOpcode = ISD::AND; break;
  case ISD::VECREDUCE_OR:   BaseOpcode = ISD::OR; break;
  case ISD::VECREDUCE_XOR:  BaseOpcode = ISD::XOR; break;
  case ISD::VECREDUCE_SMAX: BaseOpcode = ISD::SMAX; break;
  case ISD::VECREDUCE_SMIN: BaseOpcode = ISD::SMIN; break;
  case ISD::VECREDUCE_UMAX: BaseOpcode = ISD::UMAX; break;
  case ISD::VECREDUCE_UMIN: BaseOpcode = ISD::UMIN; break;
  case ISD::VECREDUCE_FMAX:
    BaseOpcode = NoNaN ? ISD::FMAXNUM : ISD::FMAXIMUM;
    break;
  case ISD::VECREDUCE_FMIN:
    BaseOpcode = NoNaN ? ISD::FMINNUM : ISD::FMINIMUM;
    break;
  }

  SDValue Op = Node->getOperand(0);
  EVT VT = Op.getValueType();

  // Try to use a shuffle reduction for power of two vectors.
  if (VT.isPow2VectorType()) {
    while (VT.getVectorNumElements() > 1) {
      EVT HalfVT = VT.getHalfNumVectorElementsVT(*DAG.getContext());
      if (!isOperationLegalOrCustom(BaseOpcode, HalfVT))
        break;

      SDValue Lo, Hi;
      std::tie(Lo, Hi) = DAG.SplitVector(Op, dl);
      Op = DAG.getNode(BaseOpcode, dl, HalfVT, Lo, Hi);
      VT = HalfVT;
    }
  }

  EVT EltVT = VT.getVectorElementType();
  unsigned NumElts = VT.getVectorNumElements();

  SmallVector<SDValue, 8> Ops;
  DAG.ExtractVectorElements(Op, Ops, 0, NumElts);

  SDValue Res = Ops[0];
  for (unsigned i = 1; i < NumElts; i++)
    Res = DAG.getNode(BaseOpcode, dl, EltVT, Res, Ops[i], Node->getFlags());

  // Result type may be wider than element type.
  if (EltVT != Node->getValueType(0))
    Res = DAG.getNode(ISD::ANY_EXTEND, dl, Node->getValueType(0), Res);
  return Res;
}
