//===- MCAssembler.h - Object File Generation -------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCASSEMBLER_H
#define LLVM_MC_MCASSEMBLER_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/iterator.h"
#include "llvm/MC/MCDirectives.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCLinkerOptimizationHint.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/DataTypes.h"
#include <algorithm>
#include <vector> // FIXME: Shouldn't be needed.

namespace llvm {
class raw_ostream;
class MCAsmLayout;
class MCAssembler;
class MCContext;
class MCCodeEmitter;
class MCExpr;
class MCFragment;
class MCObjectWriter;
class MCSection;
class MCSubtargetInfo;
class MCValue;
class MCAsmBackend;

class MCFragment : public ilist_node<MCFragment> {
  friend class MCAsmLayout;

  MCFragment(const MCFragment &) = delete;
  void operator=(const MCFragment &) = delete;

public:
  enum FragmentType {
    FT_Align,
    FT_Data,
    FT_CompactEncodedInst,
    FT_Fill,
    FT_Relaxable,
    FT_Org,
    FT_Dwarf,
    FT_DwarfFrame,
    FT_LEB
  };

private:
  FragmentType Kind;

  /// The data for the section this fragment is in.
  MCSection *Parent;

  /// Atom - The atom this fragment is in, as represented by it's defining
  /// symbol.
  const MCSymbol *Atom;

  /// \name Assembler Backend Data
  /// @{
  //
  // FIXME: This could all be kept private to the assembler implementation.

  /// Offset - The offset of this fragment in its section. This is ~0 until
  /// initialized.
  uint64_t Offset;

  /// LayoutOrder - The layout order of this fragment.
  unsigned LayoutOrder;

  /// @}

protected:
  MCFragment(FragmentType Kind, MCSection *Parent = nullptr);

public:
  // Only for sentinel.
  MCFragment();
  virtual ~MCFragment();

  FragmentType getKind() const { return Kind; }

  MCSection *getParent() const { return Parent; }
  void setParent(MCSection *Value) { Parent = Value; }

  const MCSymbol *getAtom() const { return Atom; }
  void setAtom(const MCSymbol *Value) { Atom = Value; }

  unsigned getLayoutOrder() const { return LayoutOrder; }
  void setLayoutOrder(unsigned Value) { LayoutOrder = Value; }

  /// \brief Does this fragment have instructions emitted into it? By default
  /// this is false, but specific fragment types may set it to true.
  virtual bool hasInstructions() const { return false; }

  /// \brief Should this fragment be placed at the end of an aligned bundle?
  virtual bool alignToBundleEnd() const { return false; }
  virtual void setAlignToBundleEnd(bool V) {}

  /// \brief Get the padding size that must be inserted before this fragment.
  /// Used for bundling. By default, no padding is inserted.
  /// Note that padding size is restricted to 8 bits. This is an optimization
  /// to reduce the amount of space used for each fragment. In practice, larger
  /// padding should never be required.
  virtual uint8_t getBundlePadding() const { return 0; }

  /// \brief Set the padding size for this fragment. By default it's a no-op,
  /// and only some fragments have a meaningful implementation.
  virtual void setBundlePadding(uint8_t N) {}

  void dump();
};

/// Interface implemented by fragments that contain encoded instructions and/or
/// data.
///
class MCEncodedFragment : public MCFragment {
  virtual void anchor();

  uint8_t BundlePadding;

public:
  MCEncodedFragment(MCFragment::FragmentType FType, MCSection *Sec = nullptr)
      : MCFragment(FType, Sec), BundlePadding(0) {}
  ~MCEncodedFragment() override;

  virtual SmallVectorImpl<char> &getContents() = 0;
  virtual const SmallVectorImpl<char> &getContents() const = 0;

  uint8_t getBundlePadding() const override { return BundlePadding; }

  void setBundlePadding(uint8_t N) override { BundlePadding = N; }

  static bool classof(const MCFragment *F) {
    MCFragment::FragmentType Kind = F->getKind();
    switch (Kind) {
    default:
      return false;
    case MCFragment::FT_Relaxable:
    case MCFragment::FT_CompactEncodedInst:
    case MCFragment::FT_Data:
      return true;
    }
  }
};

/// Interface implemented by fragments that contain encoded instructions and/or
/// data and also have fixups registered.
///
class MCEncodedFragmentWithFixups : public MCEncodedFragment {
  void anchor() override;

public:
  MCEncodedFragmentWithFixups(MCFragment::FragmentType FType,
                              MCSection *Sec = nullptr)
      : MCEncodedFragment(FType, Sec) {}

  ~MCEncodedFragmentWithFixups() override;

  typedef SmallVectorImpl<MCFixup>::const_iterator const_fixup_iterator;
  typedef SmallVectorImpl<MCFixup>::iterator fixup_iterator;

  virtual SmallVectorImpl<MCFixup> &getFixups() = 0;
  virtual const SmallVectorImpl<MCFixup> &getFixups() const = 0;

  virtual fixup_iterator fixup_begin() = 0;
  virtual const_fixup_iterator fixup_begin() const = 0;
  virtual fixup_iterator fixup_end() = 0;
  virtual const_fixup_iterator fixup_end() const = 0;

  static bool classof(const MCFragment *F) {
    MCFragment::FragmentType Kind = F->getKind();
    return Kind == MCFragment::FT_Relaxable || Kind == MCFragment::FT_Data;
  }
};

/// Fragment for data and encoded instructions.
///
class MCDataFragment : public MCEncodedFragmentWithFixups {
  void anchor() override;

  /// \brief Does this fragment contain encoded instructions anywhere in it?
  bool HasInstructions;

  /// \brief Should this fragment be aligned to the end of a bundle?
  bool AlignToBundleEnd;

  SmallVector<char, 32> Contents;

  /// Fixups - The list of fixups in this fragment.
  SmallVector<MCFixup, 4> Fixups;

public:
  MCDataFragment(MCSection *Sec = nullptr)
      : MCEncodedFragmentWithFixups(FT_Data, Sec), HasInstructions(false),
        AlignToBundleEnd(false) {}

  SmallVectorImpl<char> &getContents() override { return Contents; }
  const SmallVectorImpl<char> &getContents() const override { return Contents; }

  SmallVectorImpl<MCFixup> &getFixups() override { return Fixups; }

  const SmallVectorImpl<MCFixup> &getFixups() const override { return Fixups; }

  bool hasInstructions() const override { return HasInstructions; }
  virtual void setHasInstructions(bool V) { HasInstructions = V; }

  bool alignToBundleEnd() const override { return AlignToBundleEnd; }
  void setAlignToBundleEnd(bool V) override { AlignToBundleEnd = V; }

  fixup_iterator fixup_begin() override { return Fixups.begin(); }
  const_fixup_iterator fixup_begin() const override { return Fixups.begin(); }

  fixup_iterator fixup_end() override { return Fixups.end(); }
  const_fixup_iterator fixup_end() const override { return Fixups.end(); }

  static bool classof(const MCFragment *F) {
    return F->getKind() == MCFragment::FT_Data;
  }
};

/// This is a compact (memory-size-wise) fragment for holding an encoded
/// instruction (non-relaxable) that has no fixups registered. When applicable,
/// it can be used instead of MCDataFragment and lead to lower memory
/// consumption.
///
class MCCompactEncodedInstFragment : public MCEncodedFragment {
  void anchor() override;

  /// \brief Should this fragment be aligned to the end of a bundle?
  bool AlignToBundleEnd;

  SmallVector<char, 4> Contents;

public:
  MCCompactEncodedInstFragment(MCSection *Sec = nullptr)
      : MCEncodedFragment(FT_CompactEncodedInst, Sec), AlignToBundleEnd(false) {
  }

  bool hasInstructions() const override { return true; }

  SmallVectorImpl<char> &getContents() override { return Contents; }
  const SmallVectorImpl<char> &getContents() const override { return Contents; }

  bool alignToBundleEnd() const override { return AlignToBundleEnd; }
  void setAlignToBundleEnd(bool V) override { AlignToBundleEnd = V; }

  static bool classof(const MCFragment *F) {
    return F->getKind() == MCFragment::FT_CompactEncodedInst;
  }
};

/// A relaxable fragment holds on to its MCInst, since it may need to be
/// relaxed during the assembler layout and relaxation stage.
///
class MCRelaxableFragment : public MCEncodedFragmentWithFixups {
  void anchor() override;

  /// Inst - The instruction this is a fragment for.
  MCInst Inst;

  /// STI - The MCSubtargetInfo in effect when the instruction was encoded.
  /// Keep a copy instead of a reference to make sure that updates to STI
  /// in the assembler are not seen here.
  const MCSubtargetInfo STI;

  /// Contents - Binary data for the currently encoded instruction.
  SmallVector<char, 8> Contents;

  /// Fixups - The list of fixups in this fragment.
  SmallVector<MCFixup, 1> Fixups;

public:
  MCRelaxableFragment(const MCInst &Inst, const MCSubtargetInfo &STI,
                      MCSection *Sec = nullptr)
      : MCEncodedFragmentWithFixups(FT_Relaxable, Sec), Inst(Inst), STI(STI) {}

  SmallVectorImpl<char> &getContents() override { return Contents; }
  const SmallVectorImpl<char> &getContents() const override { return Contents; }

  const MCInst &getInst() const { return Inst; }
  void setInst(const MCInst &Value) { Inst = Value; }

  const MCSubtargetInfo &getSubtargetInfo() { return STI; }

  SmallVectorImpl<MCFixup> &getFixups() override { return Fixups; }

  const SmallVectorImpl<MCFixup> &getFixups() const override { return Fixups; }

  bool hasInstructions() const override { return true; }

  fixup_iterator fixup_begin() override { return Fixups.begin(); }
  const_fixup_iterator fixup_begin() const override { return Fixups.begin(); }

  fixup_iterator fixup_end() override { return Fixups.end(); }
  const_fixup_iterator fixup_end() const override { return Fixups.end(); }

  static bool classof(const MCFragment *F) {
    return F->getKind() == MCFragment::FT_Relaxable;
  }
};

class MCAlignFragment : public MCFragment {
  virtual void anchor();

  /// Alignment - The alignment to ensure, in bytes.
  unsigned Alignment;

  /// Value - Value to use for filling padding bytes.
  int64_t Value;

  /// ValueSize - The size of the integer (in bytes) of \p Value.
  unsigned ValueSize;

  /// MaxBytesToEmit - The maximum number of bytes to emit; if the alignment
  /// cannot be satisfied in this width then this fragment is ignored.
  unsigned MaxBytesToEmit;

  /// EmitNops - Flag to indicate that (optimal) NOPs should be emitted instead
  /// of using the provided value. The exact interpretation of this flag is
  /// target dependent.
  bool EmitNops : 1;

public:
  MCAlignFragment(unsigned Alignment, int64_t Value, unsigned ValueSize,
                  unsigned MaxBytesToEmit, MCSection *Sec = nullptr)
      : MCFragment(FT_Align, Sec), Alignment(Alignment), Value(Value),
        ValueSize(ValueSize), MaxBytesToEmit(MaxBytesToEmit), EmitNops(false) {}

  /// \name Accessors
  /// @{

  unsigned getAlignment() const { return Alignment; }

  int64_t getValue() const { return Value; }

  unsigned getValueSize() const { return ValueSize; }

  unsigned getMaxBytesToEmit() const { return MaxBytesToEmit; }

  bool hasEmitNops() const { return EmitNops; }
  void setEmitNops(bool Value) { EmitNops = Value; }

  /// @}

  static bool classof(const MCFragment *F) {
    return F->getKind() == MCFragment::FT_Align;
  }
};

class MCFillFragment : public MCFragment {
  virtual void anchor();

  /// Value - Value to use for filling bytes.
  int64_t Value;

  /// ValueSize - The size (in bytes) of \p Value to use when filling, or 0 if
  /// this is a virtual fill fragment.
  unsigned ValueSize;

  /// Size - The number of bytes to insert.
  uint64_t Size;

public:
  MCFillFragment(int64_t Value, unsigned ValueSize, uint64_t Size,
                 MCSection *Sec = nullptr)
      : MCFragment(FT_Fill, Sec), Value(Value), ValueSize(ValueSize),
        Size(Size) {
    assert((!ValueSize || (Size % ValueSize) == 0) &&
           "Fill size must be a multiple of the value size!");
  }

  /// \name Accessors
  /// @{

  int64_t getValue() const { return Value; }

  unsigned getValueSize() const { return ValueSize; }

  uint64_t getSize() const { return Size; }

  /// @}

  static bool classof(const MCFragment *F) {
    return F->getKind() == MCFragment::FT_Fill;
  }
};

class MCOrgFragment : public MCFragment {
  virtual void anchor();

  /// Offset - The offset this fragment should start at.
  const MCExpr *Offset;

  /// Value - Value to use for filling bytes.
  int8_t Value;

public:
  MCOrgFragment(const MCExpr &Offset, int8_t Value, MCSection *Sec = nullptr)
      : MCFragment(FT_Org, Sec), Offset(&Offset), Value(Value) {}

  /// \name Accessors
  /// @{

  const MCExpr &getOffset() const { return *Offset; }

  uint8_t getValue() const { return Value; }

  /// @}

  static bool classof(const MCFragment *F) {
    return F->getKind() == MCFragment::FT_Org;
  }
};

class MCLEBFragment : public MCFragment {
  virtual void anchor();

  /// Value - The value this fragment should contain.
  const MCExpr *Value;

  /// IsSigned - True if this is a sleb128, false if uleb128.
  bool IsSigned;

  SmallString<8> Contents;

public:
  MCLEBFragment(const MCExpr &Value_, bool IsSigned_, MCSection *Sec = nullptr)
      : MCFragment(FT_LEB, Sec), Value(&Value_), IsSigned(IsSigned_) {
    Contents.push_back(0);
  }

  /// \name Accessors
  /// @{

  const MCExpr &getValue() const { return *Value; }

  bool isSigned() const { return IsSigned; }

  SmallString<8> &getContents() { return Contents; }
  const SmallString<8> &getContents() const { return Contents; }

  /// @}

  static bool classof(const MCFragment *F) {
    return F->getKind() == MCFragment::FT_LEB;
  }
};

class MCDwarfLineAddrFragment : public MCFragment {
  virtual void anchor();

  /// LineDelta - the value of the difference between the two line numbers
  /// between two .loc dwarf directives.
  int64_t LineDelta;

  /// AddrDelta - The expression for the difference of the two symbols that
  /// make up the address delta between two .loc dwarf directives.
  const MCExpr *AddrDelta;

  SmallString<8> Contents;

public:
  MCDwarfLineAddrFragment(int64_t LineDelta, const MCExpr &AddrDelta,
                          MCSection *Sec = nullptr)
      : MCFragment(FT_Dwarf, Sec), LineDelta(LineDelta), AddrDelta(&AddrDelta) {
    Contents.push_back(0);
  }

  /// \name Accessors
  /// @{

  int64_t getLineDelta() const { return LineDelta; }

  const MCExpr &getAddrDelta() const { return *AddrDelta; }

  SmallString<8> &getContents() { return Contents; }
  const SmallString<8> &getContents() const { return Contents; }

  /// @}

  static bool classof(const MCFragment *F) {
    return F->getKind() == MCFragment::FT_Dwarf;
  }
};

class MCDwarfCallFrameFragment : public MCFragment {
  virtual void anchor();

  /// AddrDelta - The expression for the difference of the two symbols that
  /// make up the address delta between two .cfi_* dwarf directives.
  const MCExpr *AddrDelta;

  SmallString<8> Contents;

public:
  MCDwarfCallFrameFragment(const MCExpr &AddrDelta, MCSection *Sec = nullptr)
      : MCFragment(FT_DwarfFrame, Sec), AddrDelta(&AddrDelta) {
    Contents.push_back(0);
  }

  /// \name Accessors
  /// @{

  const MCExpr &getAddrDelta() const { return *AddrDelta; }

  SmallString<8> &getContents() { return Contents; }
  const SmallString<8> &getContents() const { return Contents; }

  /// @}

  static bool classof(const MCFragment *F) {
    return F->getKind() == MCFragment::FT_DwarfFrame;
  }
};

// FIXME: This really doesn't belong here. See comments below.
struct IndirectSymbolData {
  MCSymbol *Symbol;
  MCSection *Section;
};

// FIXME: Ditto this. Purely so the Streamer and the ObjectWriter can talk
// to one another.
struct DataRegionData {
  // This enum should be kept in sync w/ the mach-o definition in
  // llvm/Object/MachOFormat.h.
  enum KindTy { Data = 1, JumpTable8, JumpTable16, JumpTable32 } Kind;
  MCSymbol *Start;
  MCSymbol *End;
};

class MCAssembler {
  friend class MCAsmLayout;

public:
  typedef SetVector<MCSection *> SectionListType;
  typedef std::vector<const MCSymbol *> SymbolDataListType;

  typedef pointee_iterator<SectionListType::const_iterator> const_iterator;
  typedef pointee_iterator<SectionListType::iterator> iterator;

  typedef pointee_iterator<SymbolDataListType::const_iterator>
  const_symbol_iterator;
  typedef pointee_iterator<SymbolDataListType::iterator> symbol_iterator;

  typedef iterator_range<symbol_iterator> symbol_range;
  typedef iterator_range<const_symbol_iterator> const_symbol_range;

  typedef std::vector<std::string> FileNameVectorType;
  typedef FileNameVectorType::const_iterator const_file_name_iterator;

  typedef std::vector<IndirectSymbolData>::const_iterator
      const_indirect_symbol_iterator;
  typedef std::vector<IndirectSymbolData>::iterator indirect_symbol_iterator;

  typedef std::vector<DataRegionData>::const_iterator
      const_data_region_iterator;
  typedef std::vector<DataRegionData>::iterator data_region_iterator;

  /// MachO specific deployment target version info.
  // A Major version of 0 indicates that no version information was supplied
  // and so the corresponding load command should not be emitted.
  typedef struct {
    MCVersionMinType Kind;
    unsigned Major;
    unsigned Minor;
    unsigned Update;
  } VersionMinInfoType;

private:
  MCAssembler(const MCAssembler &) = delete;
  void operator=(const MCAssembler &) = delete;

  MCContext &Context;

  MCAsmBackend &Backend;

  MCCodeEmitter &Emitter;

  MCObjectWriter &Writer;

  raw_ostream &OS;

  SectionListType Sections;

  SymbolDataListType Symbols;

  DenseSet<const MCSymbol *> LocalsUsedInReloc;

  std::vector<IndirectSymbolData> IndirectSymbols;

  std::vector<DataRegionData> DataRegions;

  /// The list of linker options to propagate into the object file.
  std::vector<std::vector<std::string>> LinkerOptions;

  /// List of declared file names
  FileNameVectorType FileNames;

  /// The set of function symbols for which a .thumb_func directive has
  /// been seen.
  //
  // FIXME: We really would like this in target specific code rather than
  // here. Maybe when the relocation stuff moves to target specific,
  // this can go with it? The streamer would need some target specific
  // refactoring too.
  mutable SmallPtrSet<const MCSymbol *, 64> ThumbFuncs;

  /// \brief The bundle alignment size currently set in the assembler.
  ///
  /// By default it's 0, which means bundling is disabled.
  unsigned BundleAlignSize;

  unsigned RelaxAll : 1;
  unsigned SubsectionsViaSymbols : 1;

  /// ELF specific e_header flags
  // It would be good if there were an MCELFAssembler class to hold this.
  // ELF header flags are used both by the integrated and standalone assemblers.
  // Access to the flags is necessary in cases where assembler directives affect
  // which flags to be set.
  unsigned ELFHeaderEFlags;

  /// Used to communicate Linker Optimization Hint information between
  /// the Streamer and the .o writer
  MCLOHContainer LOHContainer;

  VersionMinInfoType VersionMinInfo;

private:
  /// Evaluate a fixup to a relocatable expression and the value which should be
  /// placed into the fixup.
  ///
  /// \param Layout The layout to use for evaluation.
  /// \param Fixup The fixup to evaluate.
  /// \param DF The fragment the fixup is inside.
  /// \param Target [out] On return, the relocatable expression the fixup
  /// evaluates to.
  /// \param Value [out] On return, the value of the fixup as currently laid
  /// out.
  /// \return Whether the fixup value was fully resolved. This is true if the
  /// \p Value result is fixed, otherwise the value may change due to
  /// relocation.
  bool evaluateFixup(const MCAsmLayout &Layout, const MCFixup &Fixup,
                     const MCFragment *DF, MCValue &Target,
                     uint64_t &Value) const;

  /// Check whether a fixup can be satisfied, or whether it needs to be relaxed
  /// (increased in size, in order to hold its value correctly).
  bool fixupNeedsRelaxation(const MCFixup &Fixup, const MCRelaxableFragment *DF,
                            const MCAsmLayout &Layout) const;

  /// Check whether the given fragment needs relaxation.
  bool fragmentNeedsRelaxation(const MCRelaxableFragment *IF,
                               const MCAsmLayout &Layout) const;

  /// \brief Perform one layout iteration and return true if any offsets
  /// were adjusted.
  bool layoutOnce(MCAsmLayout &Layout);

  /// \brief Perform one layout iteration of the given section and return true
  /// if any offsets were adjusted.
  bool layoutSectionOnce(MCAsmLayout &Layout, MCSection &Sec);

  bool relaxInstruction(MCAsmLayout &Layout, MCRelaxableFragment &IF);

  bool relaxLEB(MCAsmLayout &Layout, MCLEBFragment &IF);

  bool relaxDwarfLineAddr(MCAsmLayout &Layout, MCDwarfLineAddrFragment &DF);
  bool relaxDwarfCallFrameFragment(MCAsmLayout &Layout,
                                   MCDwarfCallFrameFragment &DF);

  /// finishLayout - Finalize a layout, including fragment lowering.
  void finishLayout(MCAsmLayout &Layout);

  std::pair<uint64_t, bool> handleFixup(const MCAsmLayout &Layout,
                                        MCFragment &F, const MCFixup &Fixup);

public:
  void addLocalUsedInReloc(const MCSymbol &Sym);
  bool isLocalUsedInReloc(const MCSymbol &Sym) const;

  /// Compute the effective fragment size assuming it is laid out at the given
  /// \p SectionAddress and \p FragmentOffset.
  uint64_t computeFragmentSize(const MCAsmLayout &Layout,
                               const MCFragment &F) const;

  /// Find the symbol which defines the atom containing the given symbol, or
  /// null if there is no such symbol.
  const MCSymbol *getAtom(const MCSymbol &S) const;

  /// Check whether a particular symbol is visible to the linker and is required
  /// in the symbol table, or whether it can be discarded by the assembler. This
  /// also effects whether the assembler treats the label as potentially
  /// defining a separate atom.
  bool isSymbolLinkerVisible(const MCSymbol &SD) const;

  /// Emit the section contents using the given object writer.
  void writeSectionData(const MCSection *Section,
                        const MCAsmLayout &Layout) const;

  /// Check whether a given symbol has been flagged with .thumb_func.
  bool isThumbFunc(const MCSymbol *Func) const;

  /// Flag a function symbol as the target of a .thumb_func directive.
  void setIsThumbFunc(const MCSymbol *Func) { ThumbFuncs.insert(Func); }

  /// ELF e_header flags
  unsigned getELFHeaderEFlags() const { return ELFHeaderEFlags; }
  void setELFHeaderEFlags(unsigned Flags) { ELFHeaderEFlags = Flags; }

  /// MachO deployment target version information.
  const VersionMinInfoType &getVersionMinInfo() const { return VersionMinInfo; }
  void setVersionMinInfo(MCVersionMinType Kind, unsigned Major, unsigned Minor,
                         unsigned Update) {
    VersionMinInfo.Kind = Kind;
    VersionMinInfo.Major = Major;
    VersionMinInfo.Minor = Minor;
    VersionMinInfo.Update = Update;
  }

public:
  /// Construct a new assembler instance.
  ///
  /// \param OS The stream to output to.
  //
  // FIXME: How are we going to parameterize this? Two obvious options are stay
  // concrete and require clients to pass in a target like object. The other
  // option is to make this abstract, and have targets provide concrete
  // implementations as we do with AsmParser.
  MCAssembler(MCContext &Context_, MCAsmBackend &Backend_,
              MCCodeEmitter &Emitter_, MCObjectWriter &Writer_,
              raw_ostream &OS);
  ~MCAssembler();

  /// Reuse an assembler instance
  ///
  void reset();

  MCContext &getContext() const { return Context; }

  MCAsmBackend &getBackend() const { return Backend; }

  MCCodeEmitter &getEmitter() const { return Emitter; }

  MCObjectWriter &getWriter() const { return Writer; }

  /// Finish - Do final processing and write the object to the output stream.
  /// \p Writer is used for custom object writer (as the MCJIT does),
  /// if not specified it is automatically created from backend.
  void Finish();

  // FIXME: This does not belong here.
  bool getSubsectionsViaSymbols() const { return SubsectionsViaSymbols; }
  void setSubsectionsViaSymbols(bool Value) { SubsectionsViaSymbols = Value; }

  bool getRelaxAll() const { return RelaxAll; }
  void setRelaxAll(bool Value) { RelaxAll = Value; }

  bool isBundlingEnabled() const { return BundleAlignSize != 0; }

  unsigned getBundleAlignSize() const { return BundleAlignSize; }

  void setBundleAlignSize(unsigned Size) {
    assert((Size == 0 || !(Size & (Size - 1))) &&
           "Expect a power-of-two bundle align size");
    BundleAlignSize = Size;
  }

  /// \name Section List Access
  /// @{

  iterator begin() { return Sections.begin(); }
  const_iterator begin() const { return Sections.begin(); }

  iterator end() { return Sections.end(); }
  const_iterator end() const { return Sections.end(); }

  size_t size() const { return Sections.size(); }

  /// @}
  /// \name Symbol List Access
  /// @{
  symbol_iterator symbol_begin() { return Symbols.begin(); }
  const_symbol_iterator symbol_begin() const { return Symbols.begin(); }

  symbol_iterator symbol_end() { return Symbols.end(); }
  const_symbol_iterator symbol_end() const { return Symbols.end(); }

  symbol_range symbols() { return make_range(symbol_begin(), symbol_end()); }
  const_symbol_range symbols() const {
    return make_range(symbol_begin(), symbol_end());
  }

  size_t symbol_size() const { return Symbols.size(); }

  /// @}
  /// \name Indirect Symbol List Access
  /// @{

  // FIXME: This is a total hack, this should not be here. Once things are
  // factored so that the streamer has direct access to the .o writer, it can
  // disappear.
  std::vector<IndirectSymbolData> &getIndirectSymbols() {
    return IndirectSymbols;
  }

  indirect_symbol_iterator indirect_symbol_begin() {
    return IndirectSymbols.begin();
  }
  const_indirect_symbol_iterator indirect_symbol_begin() const {
    return IndirectSymbols.begin();
  }

  indirect_symbol_iterator indirect_symbol_end() {
    return IndirectSymbols.end();
  }
  const_indirect_symbol_iterator indirect_symbol_end() const {
    return IndirectSymbols.end();
  }

  size_t indirect_symbol_size() const { return IndirectSymbols.size(); }

  /// @}
  /// \name Linker Option List Access
  /// @{

  std::vector<std::vector<std::string>> &getLinkerOptions() {
    return LinkerOptions;
  }

  /// @}
  /// \name Data Region List Access
  /// @{

  // FIXME: This is a total hack, this should not be here. Once things are
  // factored so that the streamer has direct access to the .o writer, it can
  // disappear.
  std::vector<DataRegionData> &getDataRegions() { return DataRegions; }

  data_region_iterator data_region_begin() { return DataRegions.begin(); }
  const_data_region_iterator data_region_begin() const {
    return DataRegions.begin();
  }

  data_region_iterator data_region_end() { return DataRegions.end(); }
  const_data_region_iterator data_region_end() const {
    return DataRegions.end();
  }

  size_t data_region_size() const { return DataRegions.size(); }

  /// @}
  /// \name Data Region List Access
  /// @{

  // FIXME: This is a total hack, this should not be here. Once things are
  // factored so that the streamer has direct access to the .o writer, it can
  // disappear.
  MCLOHContainer &getLOHContainer() { return LOHContainer; }
  const MCLOHContainer &getLOHContainer() const {
    return const_cast<MCAssembler *>(this)->getLOHContainer();
  }
  /// @}
  /// \name Backend Data Access
  /// @{

  bool registerSection(MCSection &Section) { return Sections.insert(&Section); }

  bool hasSymbolData(const MCSymbol &Symbol) const { return Symbol.hasData(); }

  MCSymbolData &getSymbolData(const MCSymbol &Symbol) {
    return const_cast<MCSymbolData &>(
        static_cast<const MCAssembler &>(*this).getSymbolData(Symbol));
  }

  const MCSymbolData &getSymbolData(const MCSymbol &Symbol) const {
    return Symbol.getData();
  }

  MCSymbolData &getOrCreateSymbolData(const MCSymbol &Symbol,
                                      bool *Created = nullptr) {
    if (Created)
      *Created = !hasSymbolData(Symbol);
    if (!hasSymbolData(Symbol)) {
      Symbol.initializeData();
      Symbols.push_back(&Symbol);
    }
    return Symbol.getData();
  }

  const_file_name_iterator file_names_begin() const {
    return FileNames.begin();
  }

  const_file_name_iterator file_names_end() const { return FileNames.end(); }

  void addFileName(StringRef FileName) {
    if (std::find(file_names_begin(), file_names_end(), FileName) ==
        file_names_end())
      FileNames.push_back(FileName);
  }

  /// \brief Write the necessary bundle padding to the given object writer.
  /// Expects a fragment \p F containing instructions and its size \p FSize.
  void writeFragmentPadding(const MCFragment &F, uint64_t FSize,
                            MCObjectWriter *OW) const;

  /// @}

  void dump();
};

/// \brief Compute the amount of padding required before the fragment \p F to
/// obey bundling restrictions, where \p FOffset is the fragment's offset in
/// its section and \p FSize is the fragment's size.
uint64_t computeBundlePadding(const MCAssembler &Assembler, const MCFragment *F,
                              uint64_t FOffset, uint64_t FSize);

} // end namespace llvm

#endif
