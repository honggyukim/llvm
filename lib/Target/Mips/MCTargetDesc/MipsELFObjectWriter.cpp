//===-- MipsELFObjectWriter.cpp - Mips ELF Writer -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <list>
#include "MCTargetDesc/MipsBaseInfo.h"
#include "MCTargetDesc/MipsFixupKinds.h"
#include "MCTargetDesc/MipsMCExpr.h"
#include "MCTargetDesc/MipsMCTargetDesc.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSymbolELF.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

#define DEBUG_TYPE "mips-elf-object-writer"

using namespace llvm;

namespace {
/// Holds additional information needed by the relocation ordering algorithm.
struct MipsRelocationEntry {
  const ELFRelocationEntry R; ///< The relocation.
  bool Matched;               ///< Is this relocation part of a match.

  MipsRelocationEntry(const ELFRelocationEntry &R) : R(R), Matched(false) {}

  void print(raw_ostream &Out) const {
    R.print(Out);
    Out << ", Matched=" << Matched;
  }
};

#ifndef NDEBUG
raw_ostream &operator<<(raw_ostream &OS, const MipsRelocationEntry &RHS) {
  RHS.print(OS);
  return OS;
}
#endif

class MipsELFObjectWriter : public MCELFObjectTargetWriter {
public:
  MipsELFObjectWriter(bool _is64Bit, uint8_t OSABI, bool _isN64,
                      bool IsLittleEndian);

  ~MipsELFObjectWriter() override;

  unsigned getRelocType(MCContext &Ctx, const MCValue &Target,
                        const MCFixup &Fixup, bool IsPCRel) const override;
  bool needsRelocateWithSymbol(const MCSymbol &Sym,
                               unsigned Type) const override;
  virtual void sortRelocs(const MCAssembler &Asm,
                          std::vector<ELFRelocationEntry> &Relocs) override;
};

/// Copy elements in the range [First, Last) to d1 when the predicate is true or
/// d2 when the predicate is false. This is essentially both std::copy_if and
/// std::remove_copy_if combined into a single pass.
template <class InputIt, class OutputIt1, class OutputIt2, class UnaryPredicate>
std::pair<OutputIt1, OutputIt2> copy_if_else(InputIt First, InputIt Last,
                                             OutputIt1 d1, OutputIt2 d2,
                                             UnaryPredicate Predicate) {
  for (InputIt I = First; I != Last; ++I) {
    if (Predicate(*I)) {
      *d1 = *I;
      d1++;
    } else {
      *d2 = *I;
      d2++;
    }
  }

  return std::make_pair(d1, d2);
}

/// The possible results of the Predicate function used by find_best.
enum FindBestPredicateResult {
  FindBest_NoMatch = 0,  ///< The current element is not a match.
  FindBest_Match,        ///< The current element is a match but better ones are
                         ///  possible.
  FindBest_PerfectMatch, ///< The current element is an unbeatable match.
};

/// Find the best match in the range [First, Last).
///
/// An element matches when Predicate(X) returns FindBest_Match or
/// FindBest_PerfectMatch. A value of FindBest_PerfectMatch also terminates
/// the search. BetterThan(A, B) is a comparator that returns true when A is a
/// better match than B. The return value is the position of the best match.
///
/// This is similar to std::find_if but finds the best of multiple possible
/// matches.
template <class InputIt, class UnaryPredicate, class Comparator>
InputIt find_best(InputIt First, InputIt Last, UnaryPredicate Predicate,
                  Comparator BetterThan) {
  InputIt Best = Last;

  for (InputIt I = First; I != Last; ++I) {
    unsigned Matched = Predicate(*I);
    if (Matched != FindBest_NoMatch) {
      DEBUG(dbgs() << std::distance(First, I) << " is a match (";
            I->print(dbgs()); dbgs() << ")\n");
      if (Best == Last || BetterThan(*I, *Best)) {
        DEBUG(dbgs() << ".. and it beats the last one\n");
        Best = I;
      }
    }
    if (Matched == FindBest_PerfectMatch) {
      DEBUG(dbgs() << ".. and it is unbeatable\n");
      break;
    }
  }

  return Best;
}

/// Determine the low relocation that matches the given relocation.
/// If the relocation does not need a low relocation then the return value
/// is ELF::R_MIPS_NONE.
///
/// The relocations that need a matching low part are
/// R_(MIPS|MICROMIPS|MIPS16)_HI16 for all symbols and
/// R_(MIPS|MICROMIPS|MIPS16)_GOT16 for local symbols only.
static unsigned getMatchingLoType(const ELFRelocationEntry &Reloc) {
  unsigned Type = Reloc.Type;
  if (Type == ELF::R_MIPS_HI16)
    return ELF::R_MIPS_LO16;
  if (Type == ELF::R_MICROMIPS_HI16)
    return ELF::R_MICROMIPS_LO16;
  if (Type == ELF::R_MIPS16_HI16)
    return ELF::R_MIPS16_LO16;

  if (Reloc.OriginalSymbol->getBinding() != ELF::STB_LOCAL)
    return ELF::R_MIPS_NONE;

  if (Type == ELF::R_MIPS_GOT16)
    return ELF::R_MIPS_LO16;
  if (Type == ELF::R_MICROMIPS_GOT16)
    return ELF::R_MICROMIPS_LO16;
  if (Type == ELF::R_MIPS16_GOT16)
    return ELF::R_MIPS16_LO16;

  return ELF::R_MIPS_NONE;
}

/// Determine whether a relocation (X) matches the one given in R.
///
/// A relocation matches if:
/// - It's type matches that of a corresponding low part. This is provided in
///   MatchingType for efficiency.
/// - It's based on the same symbol.
/// - It's offset of greater or equal to that of the one given in R.
///   It should be noted that this rule assumes the programmer does not use
///   offsets that exceed the alignment of the symbol. The carry-bit will be
///   incorrect if this is not true.
///
/// A matching relocation is unbeatable if:
/// - It is not already involved in a match.
/// - It's offset is exactly that of the one given in R.
static FindBestPredicateResult isMatchingReloc(const MipsRelocationEntry &X,
                                               const ELFRelocationEntry &R,
                                               unsigned MatchingType) {
  if (X.R.Type == MatchingType && X.R.OriginalSymbol == R.OriginalSymbol) {
    if (!X.Matched &&
        X.R.OriginalAddend == R.OriginalAddend)
      return FindBest_PerfectMatch;
    else if (X.R.OriginalAddend >= R.OriginalAddend)
      return FindBest_Match;
  }
  return FindBest_NoMatch;
}

/// Determine whether Candidate or PreviousBest is the better match.
/// The return value is true if Candidate is the better match.
///
/// A matching relocation is a better match if:
/// - It has a smaller addend.
/// - It is not already involved in a match.
static bool compareMatchingRelocs(const MipsRelocationEntry &Candidate,
                                  const MipsRelocationEntry &PreviousBest) {
  if (Candidate.R.OriginalAddend != PreviousBest.R.OriginalAddend)
    return Candidate.R.OriginalAddend < PreviousBest.R.OriginalAddend;
  return PreviousBest.Matched && !Candidate.Matched;
}

#ifndef NDEBUG
/// Print all the relocations.
template <class Container>
static void dumpRelocs(const char *Prefix, const Container &Relocs) {
  for (const auto &R : Relocs)
    dbgs() << Prefix << R << "\n";
}
#endif

} // end anonymous namespace

MipsELFObjectWriter::MipsELFObjectWriter(bool _is64Bit, uint8_t OSABI,
                                         bool _isN64, bool IsLittleEndian)
    : MCELFObjectTargetWriter(_is64Bit, OSABI, ELF::EM_MIPS,
                              /*HasRelocationAddend*/ _isN64,
                              /*IsN64*/ _isN64) {}

MipsELFObjectWriter::~MipsELFObjectWriter() {}

unsigned MipsELFObjectWriter::getRelocType(MCContext &Ctx,
                                           const MCValue &Target,
                                           const MCFixup &Fixup,
                                           bool IsPCRel) const {
  // Determine the type of the relocation.
  unsigned Kind = (unsigned)Fixup.getKind();

  switch (Kind) {
  case Mips::fixup_Mips_NONE:
    return ELF::R_MIPS_NONE;
  case Mips::fixup_Mips_16:
  case FK_Data_2:
    return IsPCRel ? ELF::R_MIPS_PC16 : ELF::R_MIPS_16;
  case Mips::fixup_Mips_32:
  case FK_Data_4:
    return IsPCRel ? ELF::R_MIPS_PC32 : ELF::R_MIPS_32;
  }

  if (IsPCRel) {
    switch (Kind) {
    case Mips::fixup_Mips_Branch_PCRel:
    case Mips::fixup_Mips_PC16:
      return ELF::R_MIPS_PC16;
    case Mips::fixup_MICROMIPS_PC7_S1:
      return ELF::R_MICROMIPS_PC7_S1;
    case Mips::fixup_MICROMIPS_PC10_S1:
      return ELF::R_MICROMIPS_PC10_S1;
    case Mips::fixup_MICROMIPS_PC16_S1:
      return ELF::R_MICROMIPS_PC16_S1;
    case Mips::fixup_MICROMIPS_PC26_S1:
      return ELF::R_MICROMIPS_PC26_S1;
    case Mips::fixup_MICROMIPS_PC19_S2:
      return ELF::R_MICROMIPS_PC19_S2;
    case Mips::fixup_MICROMIPS_PC18_S3:
      return ELF::R_MICROMIPS_PC18_S3;
    case Mips::fixup_MICROMIPS_PC21_S1:
      return ELF::R_MICROMIPS_PC21_S1;
    case Mips::fixup_MIPS_PC19_S2:
      return ELF::R_MIPS_PC19_S2;
    case Mips::fixup_MIPS_PC18_S3:
      return ELF::R_MIPS_PC18_S3;
    case Mips::fixup_MIPS_PC21_S2:
      return ELF::R_MIPS_PC21_S2;
    case Mips::fixup_MIPS_PC26_S2:
      return ELF::R_MIPS_PC26_S2;
    case Mips::fixup_MIPS_PCHI16:
      return ELF::R_MIPS_PCHI16;
    case Mips::fixup_MIPS_PCLO16:
      return ELF::R_MIPS_PCLO16;
    }

    llvm_unreachable("invalid PC-relative fixup kind!");
  }

  switch (Kind) {
  case Mips::fixup_Mips_64:
  case FK_Data_8:
    return ELF::R_MIPS_64;
  case FK_GPRel_4:
    if (isN64()) {
      unsigned Type = (unsigned)ELF::R_MIPS_NONE;
      Type = setRType((unsigned)ELF::R_MIPS_GPREL32, Type);
      Type = setRType2((unsigned)ELF::R_MIPS_64, Type);
      Type = setRType3((unsigned)ELF::R_MIPS_NONE, Type);
      return Type;
    }
    return ELF::R_MIPS_GPREL32;
  case Mips::fixup_Mips_GPREL16:
    return ELF::R_MIPS_GPREL16;
  case Mips::fixup_Mips_26:
    return ELF::R_MIPS_26;
  case Mips::fixup_Mips_CALL16:
    return ELF::R_MIPS_CALL16;
  case Mips::fixup_Mips_GOT:
    return ELF::R_MIPS_GOT16;
  case Mips::fixup_Mips_HI16:
    return ELF::R_MIPS_HI16;
  case Mips::fixup_Mips_LO16:
    return ELF::R_MIPS_LO16;
  case Mips::fixup_Mips_TLSGD:
    return ELF::R_MIPS_TLS_GD;
  case Mips::fixup_Mips_GOTTPREL:
    return ELF::R_MIPS_TLS_GOTTPREL;
  case Mips::fixup_Mips_TPREL_HI:
    return ELF::R_MIPS_TLS_TPREL_HI16;
  case Mips::fixup_Mips_TPREL_LO:
    return ELF::R_MIPS_TLS_TPREL_LO16;
  case Mips::fixup_Mips_TLSLDM:
    return ELF::R_MIPS_TLS_LDM;
  case Mips::fixup_Mips_DTPREL_HI:
    return ELF::R_MIPS_TLS_DTPREL_HI16;
  case Mips::fixup_Mips_DTPREL_LO:
    return ELF::R_MIPS_TLS_DTPREL_LO16;
  case Mips::fixup_Mips_GOT_PAGE:
    return ELF::R_MIPS_GOT_PAGE;
  case Mips::fixup_Mips_GOT_OFST:
    return ELF::R_MIPS_GOT_OFST;
  case Mips::fixup_Mips_GOT_DISP:
    return ELF::R_MIPS_GOT_DISP;
  case Mips::fixup_Mips_GPOFF_HI: {
    unsigned Type = (unsigned)ELF::R_MIPS_NONE;
    Type = setRType((unsigned)ELF::R_MIPS_GPREL16, Type);
    Type = setRType2((unsigned)ELF::R_MIPS_SUB, Type);
    Type = setRType3((unsigned)ELF::R_MIPS_HI16, Type);
    return Type;
  }
  case Mips::fixup_Mips_GPOFF_LO: {
    unsigned Type = (unsigned)ELF::R_MIPS_NONE;
    Type = setRType((unsigned)ELF::R_MIPS_GPREL16, Type);
    Type = setRType2((unsigned)ELF::R_MIPS_SUB, Type);
    Type = setRType3((unsigned)ELF::R_MIPS_LO16, Type);
    return Type;
  }
  case Mips::fixup_Mips_HIGHER:
    return ELF::R_MIPS_HIGHER;
  case Mips::fixup_Mips_HIGHEST:
    return ELF::R_MIPS_HIGHEST;
  case Mips::fixup_Mips_GOT_HI16:
    return ELF::R_MIPS_GOT_HI16;
  case Mips::fixup_Mips_GOT_LO16:
    return ELF::R_MIPS_GOT_LO16;
  case Mips::fixup_Mips_CALL_HI16:
    return ELF::R_MIPS_CALL_HI16;
  case Mips::fixup_Mips_CALL_LO16:
    return ELF::R_MIPS_CALL_LO16;
  case Mips::fixup_MICROMIPS_26_S1:
    return ELF::R_MICROMIPS_26_S1;
  case Mips::fixup_MICROMIPS_HI16:
    return ELF::R_MICROMIPS_HI16;
  case Mips::fixup_MICROMIPS_LO16:
    return ELF::R_MICROMIPS_LO16;
  case Mips::fixup_MICROMIPS_GOT16:
    return ELF::R_MICROMIPS_GOT16;
  case Mips::fixup_MICROMIPS_CALL16:
    return ELF::R_MICROMIPS_CALL16;
  case Mips::fixup_MICROMIPS_GOT_DISP:
    return ELF::R_MICROMIPS_GOT_DISP;
  case Mips::fixup_MICROMIPS_GOT_PAGE:
    return ELF::R_MICROMIPS_GOT_PAGE;
  case Mips::fixup_MICROMIPS_GOT_OFST:
    return ELF::R_MICROMIPS_GOT_OFST;
  case Mips::fixup_MICROMIPS_TLS_GD:
    return ELF::R_MICROMIPS_TLS_GD;
  case Mips::fixup_MICROMIPS_TLS_LDM:
    return ELF::R_MICROMIPS_TLS_LDM;
  case Mips::fixup_MICROMIPS_TLS_DTPREL_HI16:
    return ELF::R_MICROMIPS_TLS_DTPREL_HI16;
  case Mips::fixup_MICROMIPS_TLS_DTPREL_LO16:
    return ELF::R_MICROMIPS_TLS_DTPREL_LO16;
  case Mips::fixup_MICROMIPS_TLS_TPREL_HI16:
    return ELF::R_MICROMIPS_TLS_TPREL_HI16;
  case Mips::fixup_MICROMIPS_TLS_TPREL_LO16:
    return ELF::R_MICROMIPS_TLS_TPREL_LO16;
  }

  llvm_unreachable("invalid fixup kind!");
}

/// Sort relocation table entries by offset except where another order is
/// required by the MIPS ABI.
///
/// MIPS has a few relocations that have an AHL component in the expression used
/// to evaluate them. This AHL component is an addend with the same number of
/// bits as a symbol value but not all of our ABI's are able to supply a
/// sufficiently sized addend in a single relocation.
///
/// The O32 ABI for example, uses REL relocations which store the addend in the
/// section data. All the relocations with AHL components affect 16-bit fields
/// so the addend for a single relocation is limited to 16-bit. This ABI
/// resolves the limitation by linking relocations (e.g. R_MIPS_HI16 and
/// R_MIPS_LO16) and distributing the addend between the linked relocations. The
/// ABI mandates that such relocations must be next to each other in a
/// particular order (e.g. R_MIPS_HI16 must be immediately followed by a
/// matching R_MIPS_LO16) but the rule is less strict in practice.
///
/// The de facto standard is lenient in the following ways:
/// - 'Immediately following' does not refer to the next relocation entry but
///   the next matching relocation.
/// - There may be multiple high parts relocations for one low part relocation.
/// - There may be multiple low part relocations for one high part relocation.
/// - The AHL addend in each part does not have to be exactly equal as long as
///   the difference does not affect the carry bit from bit 15 into 16. This is
///   to allow, for example, the use of %lo(foo) and %lo(foo+4) when loading
///   both halves of a long long.
///
/// See getMatchingLoType() for a description of which high part relocations
/// match which low part relocations. One particular thing to note is that
/// R_MIPS_GOT16 and similar only have AHL addends if they refer to local
/// symbols.
///
/// It should also be noted that this function is not affected by whether
/// the symbol was kept or rewritten into a section-relative equivalent. We
/// always match using the expressions from the source.
void MipsELFObjectWriter::sortRelocs(const MCAssembler &Asm,
                                     std::vector<ELFRelocationEntry> &Relocs) {
  if (Relocs.size() < 2)
    return;

  // Sort relocations by the address they are applied to.
  std::sort(Relocs.begin(), Relocs.end(),
            [](const ELFRelocationEntry &A, const ELFRelocationEntry &B) {
              return A.Offset < B.Offset;
            });

  std::list<MipsRelocationEntry> Sorted;
  std::list<ELFRelocationEntry> Remainder;

  DEBUG(dumpRelocs("R: ", Relocs));

  // Separate the movable relocations (AHL relocations using the high bits) from
  // the immobile relocations (everything else). This does not preserve high/low
  // matches that already existed in the input.
  copy_if_else(Relocs.begin(), Relocs.end(), std::back_inserter(Remainder),
               std::back_inserter(Sorted), [](const ELFRelocationEntry &Reloc) {
                 return getMatchingLoType(Reloc) != ELF::R_MIPS_NONE;
               });

  for (auto &R : Remainder) {
    DEBUG(dbgs() << "Matching: " << R << "\n");

    unsigned MatchingType = getMatchingLoType(R);
    assert(MatchingType != ELF::R_MIPS_NONE &&
           "Wrong list for reloc that doesn't need a match");

    // Find the best matching relocation for the current high part.
    // See isMatchingReloc for a description of a matching relocation and
    // compareMatchingRelocs for a description of what 'best' means.
    auto InsertionPoint =
        find_best(Sorted.begin(), Sorted.end(),
                  [&R, &MatchingType](const MipsRelocationEntry &X) {
                    return isMatchingReloc(X, R, MatchingType);
                  },
                  compareMatchingRelocs);

    // If we matched then insert the high part in front of the match and mark
    // both relocations as being involved in a match. We only mark the high
    // part for cosmetic reasons in the debug output.
    //
    // If we failed to find a match then the high part is orphaned. This is not
    // permitted since the relocation cannot be evaluated without knowing the
    // carry-in. We can sometimes handle this using a matching low part that is
    // already used in a match but we already cover that case in
    // isMatchingReloc and compareMatchingRelocs. For the remaining cases we
    // should insert the high part at the end of the list. This will cause the
    // linker to fail but the alternative is to cause the linker to bind the
    // high part to a semi-matching low part and silently calculate the wrong
    // value. Unfortunately we have no means to warn the user that we did this
    // so leave it up to the linker to complain about it.
    if (InsertionPoint != Sorted.end())
      InsertionPoint->Matched = true;
    Sorted.insert(InsertionPoint, R)->Matched = true;
  }

  DEBUG(dumpRelocs("S: ", Sorted));

  assert(Relocs.size() == Sorted.size() && "Some relocs were not consumed");

  // Overwrite the original vector with the sorted elements. The caller expects
  // them in reverse order.
  unsigned CopyTo = 0;
  for (const auto &R : reverse(Sorted))
    Relocs[CopyTo++] = R.R;
}

bool MipsELFObjectWriter::needsRelocateWithSymbol(const MCSymbol &Sym,
                                                  unsigned Type) const {
  // If it's a compound relocation for N64 then we need the relocation if any
  // sub-relocation needs it.
  if (!isUInt<8>(Type))
    return needsRelocateWithSymbol(Sym, Type & 0xff) ||
           needsRelocateWithSymbol(Sym, (Type >> 8) & 0xff) ||
           needsRelocateWithSymbol(Sym, (Type >> 16) & 0xff);

  switch (Type) {
  default:
    errs() << Type << "\n";
    llvm_unreachable("Unexpected relocation");
    return true;

  // This relocation doesn't affect the section data.
  case ELF::R_MIPS_NONE:
    return false;

  // On REL ABI's (e.g. O32), these relocations form pairs. The pairing is done
  // by the static linker by matching the symbol and offset.
  // We only see one relocation at a time but it's still safe to relocate with
  // the section so long as both relocations make the same decision.
  //
  // Some older linkers may require the symbol for particular cases. Such cases
  // are not supported yet but can be added as required.
  case ELF::R_MIPS_GOT16:
  case ELF::R_MIPS16_GOT16:
  case ELF::R_MICROMIPS_GOT16:
  case ELF::R_MIPS_HI16:
  case ELF::R_MIPS16_HI16:
  case ELF::R_MICROMIPS_HI16:
  case ELF::R_MIPS_LO16:
  case ELF::R_MIPS16_LO16:
  case ELF::R_MICROMIPS_LO16:
    // FIXME: It should be safe to return false for the STO_MIPS_MICROMIPS but
    //        we neglect to handle the adjustment to the LSB of the addend that
    //        it causes in applyFixup() and similar.
    if (cast<MCSymbolELF>(Sym).getOther() & ELF::STO_MIPS_MICROMIPS)
      return true;
    return false;

  case ELF::R_MIPS_GOT_PAGE:
  case ELF::R_MICROMIPS_GOT_PAGE:
  case ELF::R_MIPS_GOT_OFST:
  case ELF::R_MICROMIPS_GOT_OFST:
  case ELF::R_MIPS_16:
  case ELF::R_MIPS_32:
  case ELF::R_MIPS_GPREL32:
    if (cast<MCSymbolELF>(Sym).getOther() & ELF::STO_MIPS_MICROMIPS)
      return true;
    // fallthrough
  case ELF::R_MIPS_26:
  case ELF::R_MIPS_64:
  case ELF::R_MIPS_GPREL16:
  case ELF::R_MIPS_PC16:
  case ELF::R_MIPS_SUB:
    return false;

  // FIXME: Many of these relocations should probably return false but this
  //        hasn't been confirmed to be safe yet.
  case ELF::R_MIPS_REL32:
  case ELF::R_MIPS_LITERAL:
  case ELF::R_MIPS_CALL16:
  case ELF::R_MIPS_SHIFT5:
  case ELF::R_MIPS_SHIFT6:
  case ELF::R_MIPS_GOT_DISP:
  case ELF::R_MIPS_GOT_HI16:
  case ELF::R_MIPS_GOT_LO16:
  case ELF::R_MIPS_INSERT_A:
  case ELF::R_MIPS_INSERT_B:
  case ELF::R_MIPS_DELETE:
  case ELF::R_MIPS_HIGHER:
  case ELF::R_MIPS_HIGHEST:
  case ELF::R_MIPS_CALL_HI16:
  case ELF::R_MIPS_CALL_LO16:
  case ELF::R_MIPS_SCN_DISP:
  case ELF::R_MIPS_REL16:
  case ELF::R_MIPS_ADD_IMMEDIATE:
  case ELF::R_MIPS_PJUMP:
  case ELF::R_MIPS_RELGOT:
  case ELF::R_MIPS_JALR:
  case ELF::R_MIPS_TLS_DTPMOD32:
  case ELF::R_MIPS_TLS_DTPREL32:
  case ELF::R_MIPS_TLS_DTPMOD64:
  case ELF::R_MIPS_TLS_DTPREL64:
  case ELF::R_MIPS_TLS_GD:
  case ELF::R_MIPS_TLS_LDM:
  case ELF::R_MIPS_TLS_DTPREL_HI16:
  case ELF::R_MIPS_TLS_DTPREL_LO16:
  case ELF::R_MIPS_TLS_GOTTPREL:
  case ELF::R_MIPS_TLS_TPREL32:
  case ELF::R_MIPS_TLS_TPREL64:
  case ELF::R_MIPS_TLS_TPREL_HI16:
  case ELF::R_MIPS_TLS_TPREL_LO16:
  case ELF::R_MIPS_GLOB_DAT:
  case ELF::R_MIPS_PC21_S2:
  case ELF::R_MIPS_PC26_S2:
  case ELF::R_MIPS_PC18_S3:
  case ELF::R_MIPS_PC19_S2:
  case ELF::R_MIPS_PCHI16:
  case ELF::R_MIPS_PCLO16:
  case ELF::R_MIPS_COPY:
  case ELF::R_MIPS_JUMP_SLOT:
  case ELF::R_MIPS_NUM:
  case ELF::R_MIPS_PC32:
  case ELF::R_MIPS_EH:
  case ELF::R_MICROMIPS_26_S1:
  case ELF::R_MICROMIPS_GPREL16:
  case ELF::R_MICROMIPS_LITERAL:
  case ELF::R_MICROMIPS_PC7_S1:
  case ELF::R_MICROMIPS_PC10_S1:
  case ELF::R_MICROMIPS_PC16_S1:
  case ELF::R_MICROMIPS_CALL16:
  case ELF::R_MICROMIPS_GOT_DISP:
  case ELF::R_MICROMIPS_GOT_HI16:
  case ELF::R_MICROMIPS_GOT_LO16:
  case ELF::R_MICROMIPS_SUB:
  case ELF::R_MICROMIPS_HIGHER:
  case ELF::R_MICROMIPS_HIGHEST:
  case ELF::R_MICROMIPS_CALL_HI16:
  case ELF::R_MICROMIPS_CALL_LO16:
  case ELF::R_MICROMIPS_SCN_DISP:
  case ELF::R_MICROMIPS_JALR:
  case ELF::R_MICROMIPS_HI0_LO16:
  case ELF::R_MICROMIPS_TLS_GD:
  case ELF::R_MICROMIPS_TLS_LDM:
  case ELF::R_MICROMIPS_TLS_DTPREL_HI16:
  case ELF::R_MICROMIPS_TLS_DTPREL_LO16:
  case ELF::R_MICROMIPS_TLS_GOTTPREL:
  case ELF::R_MICROMIPS_TLS_TPREL_HI16:
  case ELF::R_MICROMIPS_TLS_TPREL_LO16:
  case ELF::R_MICROMIPS_GPREL7_S2:
  case ELF::R_MICROMIPS_PC23_S2:
  case ELF::R_MICROMIPS_PC21_S1:
  case ELF::R_MICROMIPS_PC26_S1:
  case ELF::R_MICROMIPS_PC18_S3:
  case ELF::R_MICROMIPS_PC19_S2:
    return true;

  // FIXME: Many of these should probably return false but MIPS16 isn't
  //        supported by the integrated assembler.
  case ELF::R_MIPS16_26:
  case ELF::R_MIPS16_GPREL:
  case ELF::R_MIPS16_CALL16:
  case ELF::R_MIPS16_TLS_GD:
  case ELF::R_MIPS16_TLS_LDM:
  case ELF::R_MIPS16_TLS_DTPREL_HI16:
  case ELF::R_MIPS16_TLS_DTPREL_LO16:
  case ELF::R_MIPS16_TLS_GOTTPREL:
  case ELF::R_MIPS16_TLS_TPREL_HI16:
  case ELF::R_MIPS16_TLS_TPREL_LO16:
    llvm_unreachable("Unsupported MIPS16 relocation");
    return true;
  }
}

MCObjectWriter *llvm::createMipsELFObjectWriter(raw_pwrite_stream &OS,
                                                uint8_t OSABI,
                                                bool IsLittleEndian,
                                                bool Is64Bit) {
  MCELFObjectTargetWriter *MOTW =
      new MipsELFObjectWriter(Is64Bit, OSABI, Is64Bit, IsLittleEndian);
  return createELFObjectWriter(MOTW, OS, IsLittleEndian);
}
