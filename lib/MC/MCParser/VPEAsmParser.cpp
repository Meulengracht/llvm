//===- VPEAsmParser.cpp - VPE COFF Assembly Parser ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDirectives.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCParser/MCAsmLexer.h"
#include "llvm/MC/MCParser/MCAsmParserExtension.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSectionCOFF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/SectionKind.h"
#include "llvm/Support/SMLoc.h"
#include <cassert>
#include <cstdint>
#include <limits>
#include <utility>

using namespace llvm;

namespace {

class VPEAsmParser : public MCAsmParserExtension {
  template<bool (VPEAsmParser::*HandlerMethod)(StringRef, SMLoc)>
  void addDirectiveHandler(StringRef Directive) {
    MCAsmParser::ExtensionDirectiveHandler Handler = std::make_pair(
        this, HandleDirective<VPEAsmParser, HandlerMethod>);
    getParser().addDirectiveHandler(Directive, Handler);
  }

  bool ParseSectionSwitch(StringRef Section,
                          unsigned Characteristics,
                          SectionKind Kind);

  bool ParseSectionSwitch(StringRef Section, unsigned Characteristics,
                          SectionKind Kind, StringRef COMDATSymName,
                          COFF::COMDATType Type);

  bool ParseSectionName(StringRef &SectionName);
  bool ParseSectionFlags(StringRef SectionName, StringRef FlagsString,
                         unsigned *Flags);

  void Initialize(MCAsmParser &Parser) override {
    // Call the base implementation.
    MCAsmParserExtension::Initialize(Parser);

    addDirectiveHandler<&VPEAsmParser::ParseSectionDirectiveText>(".text");
    addDirectiveHandler<&VPEAsmParser::ParseSectionDirectiveData>(".data");
    addDirectiveHandler<&VPEAsmParser::ParseSectionDirectiveBSS>(".bss");
    addDirectiveHandler<&VPEAsmParser::ParseSectionDirectiveEhFrame>(".eh_frame");

    addDirectiveHandler<&VPEAsmParser::ParseDirectiveSection>(".section");
    addDirectiveHandler<&VPEAsmParser::ParseDirectiveDef>(".def");
    addDirectiveHandler<&VPEAsmParser::ParseDirectiveSecRel32>(".secrel32");
    addDirectiveHandler<&VPEAsmParser::ParseDirectiveSymIdx>(".symidx");
    addDirectiveHandler<&VPEAsmParser::ParseDirectiveSecIdx>(".secidx");
    addDirectiveHandler<&VPEAsmParser::ParseDirectiveLinkOnce>(".linkonce");
    addDirectiveHandler<&VPEAsmParser::ParseDirectiveSymbolAttribute>(".weak");
  }

  bool ParseSectionDirectiveText(StringRef, SMLoc) {
    return ParseSectionSwitch(".text",
                              COFF::IMAGE_SCN_CNT_CODE
                            | COFF::IMAGE_SCN_MEM_EXECUTE
                            | COFF::IMAGE_SCN_MEM_READ,
                              SectionKind::getText());
  }

  bool ParseSectionDirectiveData(StringRef, SMLoc) {
    return ParseSectionSwitch(".data", COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
                                           COFF::IMAGE_SCN_MEM_READ |
                                           COFF::IMAGE_SCN_MEM_WRITE,
                              SectionKind::getData());
  }

  bool ParseSectionDirectiveBSS(StringRef, SMLoc) {
    return ParseSectionSwitch(".bss",
                              COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA
                            | COFF::IMAGE_SCN_MEM_READ
                            | COFF::IMAGE_SCN_MEM_WRITE,
                              SectionKind::getBSS());
  }
  bool ParseSectionDirectiveEhFrame(StringRef, SMLoc) {
    return ParseSectionSwitch(".eh_frame",
                              COFF::IMAGE_SCN_CNT_INITIALIZED_DATA
                            | COFF::IMAGE_SCN_MEM_READ
                            | COFF::IMAGE_SCN_MEM_WRITE,
                              SectionKind::getData());
  }
  bool ParseDirectiveSection(StringRef, SMLoc);
  bool ParseDirectiveDef(StringRef, SMLoc);
  bool ParseDirectiveSecRel32(StringRef, SMLoc);
  bool ParseDirectiveSecIdx(StringRef, SMLoc);
  bool ParseDirectiveSymIdx(StringRef, SMLoc);
  bool parseCOMDATType(COFF::COMDATType &Type);
  bool ParseDirectiveLinkOnce(StringRef, SMLoc);
  bool ParseDirectiveSymbolAttribute(StringRef Directive, SMLoc);

public:
  VPEAsmParser() = default;
};

} // end annonomous namespace.

static SectionKind computeSectionKind(unsigned Flags) {
  if (Flags & COFF::IMAGE_SCN_MEM_EXECUTE)
    return SectionKind::getText();
  if (Flags & COFF::IMAGE_SCN_MEM_READ &&
      (Flags & COFF::IMAGE_SCN_MEM_WRITE) == 0)
    return SectionKind::getReadOnly();
  return SectionKind::getData();
}

bool VPEAsmParser::ParseSectionFlags(StringRef SectionName,
                                     StringRef FlagsString, unsigned *Flags) {
  enum {
    None        = 0,
    Alloc       = 1 << 0,
    Code        = 1 << 1,
    Load        = 1 << 2,
    InitData    = 1 << 3,
    Shared      = 1 << 4,
    NoLoad      = 1 << 5,
    NoRead      = 1 << 6,
    NoWrite     = 1 << 7,
    Discardable = 1 << 8,
  };

  bool ReadOnlyRemoved = false;
  unsigned SecFlags = None;

  for (char FlagChar : FlagsString) {
    switch (FlagChar) {
    case 'a':
      // Ignored.
      break;

    case 'b': // bss section
      SecFlags |= Alloc;
      if (SecFlags & InitData)
        return TokError("conflicting section flags 'b' and 'd'.");
      SecFlags &= ~Load;
      break;

    case 'd': // data section
      SecFlags |= InitData;
      if (SecFlags & Alloc)
        return TokError("conflicting section flags 'b' and 'd'.");
      SecFlags &= ~NoWrite;
      if ((SecFlags & NoLoad) == 0)
        SecFlags |= Load;
      break;

    case 'n': // section is not loaded
      SecFlags |= NoLoad;
      SecFlags &= ~Load;
      break;

    case 'D': // discardable
      SecFlags |= Discardable;
      break;

    case 'r': // read-only
      ReadOnlyRemoved = false;
      SecFlags |= NoWrite;
      if ((SecFlags & Code) == 0)
        SecFlags |= InitData;
      if ((SecFlags & NoLoad) == 0)
        SecFlags |= Load;
      break;

    case 's': // shared section
      SecFlags |= Shared | InitData;
      SecFlags &= ~NoWrite;
      if ((SecFlags & NoLoad) == 0)
        SecFlags |= Load;
      break;

    case 'w': // writable
      SecFlags &= ~NoWrite;
      ReadOnlyRemoved = true;
      break;

    case 'x': // executable section
      SecFlags |= Code;
      if ((SecFlags & NoLoad) == 0)
        SecFlags |= Load;
      if (!ReadOnlyRemoved)
        SecFlags |= NoWrite;
      break;

    case 'y': // not readable
      SecFlags |= NoRead | NoWrite;
      break;

    default:
      return TokError("unknown flag");
    }
  }

  *Flags = 0;

  if (SecFlags == None)
    SecFlags = InitData;

  if (SecFlags & Code)
    *Flags |= COFF::IMAGE_SCN_CNT_CODE | COFF::IMAGE_SCN_MEM_EXECUTE;
  if (SecFlags & InitData)
    *Flags |= COFF::IMAGE_SCN_CNT_INITIALIZED_DATA;
  if ((SecFlags & Alloc) && (SecFlags & Load) == 0)
    *Flags |= COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA;
  if (SecFlags & NoLoad)
    *Flags |= COFF::IMAGE_SCN_LNK_REMOVE;
  if ((SecFlags & Discardable) ||
      MCSectionCOFF::isImplicitlyDiscardable(SectionName))
    *Flags |= COFF::IMAGE_SCN_MEM_DISCARDABLE;
  if ((SecFlags & NoRead) == 0)
    *Flags |= COFF::IMAGE_SCN_MEM_READ;
  if ((SecFlags & NoWrite) == 0)
    *Flags |= COFF::IMAGE_SCN_MEM_WRITE;
  if (SecFlags & Shared)
    *Flags |= COFF::IMAGE_SCN_MEM_SHARED;

  return false;
}

/// ParseDirectiveSymbolAttribute
///  ::= { ".weak", ... } [ identifier ( , identifier )* ]
bool VPEAsmParser::ParseDirectiveSymbolAttribute(StringRef Directive, SMLoc) {
  MCSymbolAttr Attr = StringSwitch<MCSymbolAttr>(Directive)
    .Case(".weak", MCSA_Weak)
    .Default(MCSA_Invalid);
  assert(Attr != MCSA_Invalid && "unexpected symbol attribute directive!");
  if (getLexer().isNot(AsmToken::EndOfStatement)) {
    while (true) {
      StringRef Name;

      if (getParser().parseIdentifier(Name))
        return TokError("expected identifier in directive");

      MCSymbol *Sym = getContext().getOrCreateSymbol(Name);

      getStreamer().EmitSymbolAttribute(Sym, Attr);

      if (getLexer().is(AsmToken::EndOfStatement))
        break;

      if (getLexer().isNot(AsmToken::Comma))
        return TokError("unexpected token in directive");
      Lex();
    }
  }

  Lex();
  return false;
}

bool VPEAsmParser::ParseSectionSwitch(StringRef Section,
                                       unsigned Characteristics,
                                       SectionKind Kind) {
  return ParseSectionSwitch(Section, Characteristics, Kind, "", (COFF::COMDATType)0);
}

bool VPEAsmParser::ParseSectionSwitch(StringRef Section,
                                       unsigned Characteristics,
                                       SectionKind Kind,
                                       StringRef COMDATSymName,
                                       COFF::COMDATType Type) {
  if (getLexer().isNot(AsmToken::EndOfStatement))
    return TokError("unexpected token in section switching directive");
  Lex();

  getStreamer().SwitchSection(getContext().getCOFFSection(
      Section, Characteristics, Kind, COMDATSymName, Type));

  return false;
}

bool VPEAsmParser::ParseSectionName(StringRef &SectionName) {
  if (!getLexer().is(AsmToken::Identifier))
    return true;

  SectionName = getTok().getIdentifier();
  Lex();
  return false;
}

// .section name [, "flags"] [, identifier [ identifier ], identifier]
//
// Supported flags:
//   a: Ignored.
//   b: BSS section (uninitialized data)
//   d: data section (initialized data)
//   n: "noload" section (removed by linker)
//   D: Discardable section
//   r: Readable section
//   s: Shared section
//   w: Writable section
//   x: Executable section
//   y: Not-readable section (clears 'r')
//
// Subsections are not supported.
bool VPEAsmParser::ParseDirectiveSection(StringRef, SMLoc) {
  StringRef SectionName;

  if (ParseSectionName(SectionName))
    return TokError("expected identifier in directive");

  unsigned Flags = COFF::IMAGE_SCN_CNT_INITIALIZED_DATA |
                   COFF::IMAGE_SCN_MEM_READ |
                   COFF::IMAGE_SCN_MEM_WRITE;

  if (getLexer().is(AsmToken::Comma)) {
    Lex();

    if (getLexer().isNot(AsmToken::String))
      return TokError("expected string in directive");

    StringRef FlagsStr = getTok().getStringContents();
    Lex();

    if (ParseSectionFlags(SectionName, FlagsStr, &Flags))
      return true;
  }

  COFF::COMDATType Type = (COFF::COMDATType)0;
  StringRef COMDATSymName;
  if (getLexer().is(AsmToken::Comma)) {
    Type = COFF::IMAGE_COMDAT_SELECT_ANY;
    Lex();

    Flags |= COFF::IMAGE_SCN_LNK_COMDAT;

    if (!getLexer().is(AsmToken::Identifier))
      return TokError("expected comdat type such as 'discard' or 'largest' "
                      "after protection bits");

    if (parseCOMDATType(Type))
      return true;

    if (getLexer().isNot(AsmToken::Comma))
      return TokError("expected comma in directive");
    Lex();

    if (getParser().parseIdentifier(COMDATSymName))
      return TokError("expected identifier in directive");
  }

  if (getLexer().isNot(AsmToken::EndOfStatement))
    return TokError("unexpected token in directive");

  SectionKind Kind = computeSectionKind(Flags);
  if (Kind.isText()) {
    const Triple &T = getContext().getObjectFileInfo()->getTargetTriple();
    if (T.getArch() == Triple::arm || T.getArch() == Triple::thumb)
      Flags |= COFF::IMAGE_SCN_MEM_16BIT;
  }
  ParseSectionSwitch(SectionName, Flags, Kind, COMDATSymName, Type);
  return false;
}

bool VPEAsmParser::ParseDirectiveDef(StringRef, SMLoc) {
  StringRef SymbolName;

  if (getParser().parseIdentifier(SymbolName))
    return TokError("expected identifier in directive");

  MCSymbol *Sym = getContext().getOrCreateSymbol(SymbolName);

  getStreamer().BeginCOFFSymbolDef(Sym);

  Lex();
  return false;
}

bool VPEAsmParser::ParseDirectiveSecRel32(StringRef, SMLoc) {
  StringRef SymbolID;
  if (getParser().parseIdentifier(SymbolID))
    return TokError("expected identifier in directive");

  int64_t Offset = 0;
  SMLoc OffsetLoc;
  if (getLexer().is(AsmToken::Plus)) {
    OffsetLoc = getLexer().getLoc();
    if (getParser().parseAbsoluteExpression(Offset))
      return true;
  }

  if (getLexer().isNot(AsmToken::EndOfStatement))
    return TokError("unexpected token in directive");

  if (Offset < 0 || Offset > std::numeric_limits<uint32_t>::max())
    return Error(
        OffsetLoc,
        "invalid '.secrel32' directive offset, can't be less "
        "than zero or greater than std::numeric_limits<uint32_t>::max()");

  MCSymbol *Symbol = getContext().getOrCreateSymbol(SymbolID);

  Lex();
  getStreamer().EmitCOFFSecRel32(Symbol, Offset);
  return false;
}

bool VPEAsmParser::ParseDirectiveSecIdx(StringRef, SMLoc) {
  StringRef SymbolID;
  if (getParser().parseIdentifier(SymbolID))
    return TokError("expected identifier in directive");

  if (getLexer().isNot(AsmToken::EndOfStatement))
    return TokError("unexpected token in directive");

  MCSymbol *Symbol = getContext().getOrCreateSymbol(SymbolID);

  Lex();
  getStreamer().EmitCOFFSectionIndex(Symbol);
  return false;
}

bool VPEAsmParser::ParseDirectiveSymIdx(StringRef, SMLoc) {
  StringRef SymbolID;
  if (getParser().parseIdentifier(SymbolID))
    return TokError("expected identifier in directive");

  if (getLexer().isNot(AsmToken::EndOfStatement))
    return TokError("unexpected token in directive");

  MCSymbol *Symbol = getContext().getOrCreateSymbol(SymbolID);

  Lex();
  getStreamer().EmitCOFFSymbolIndex(Symbol);
  return false;
}

/// ::= [ identifier ]
bool VPEAsmParser::parseCOMDATType(COFF::COMDATType &Type) {
  StringRef TypeId = getTok().getIdentifier();

  Type = StringSwitch<COFF::COMDATType>(TypeId)
    .Case("one_only", COFF::IMAGE_COMDAT_SELECT_NODUPLICATES)
    .Case("discard", COFF::IMAGE_COMDAT_SELECT_ANY)
    .Case("same_size", COFF::IMAGE_COMDAT_SELECT_SAME_SIZE)
    .Case("same_contents", COFF::IMAGE_COMDAT_SELECT_EXACT_MATCH)
    .Case("associative", COFF::IMAGE_COMDAT_SELECT_ASSOCIATIVE)
    .Case("largest", COFF::IMAGE_COMDAT_SELECT_LARGEST)
    .Case("newest", COFF::IMAGE_COMDAT_SELECT_NEWEST)
    .Default((COFF::COMDATType)0);

  if (Type == 0)
    return TokError(Twine("unrecognized COMDAT type '" + TypeId + "'"));

  Lex();

  return false;
}

/// ParseDirectiveLinkOnce
///  ::= .linkonce [ identifier ]
bool VPEAsmParser::ParseDirectiveLinkOnce(StringRef, SMLoc Loc) {
  COFF::COMDATType Type = COFF::IMAGE_COMDAT_SELECT_ANY;
  if (getLexer().is(AsmToken::Identifier))
    if (parseCOMDATType(Type))
      return true;

  const MCSectionCOFF *Current =
      static_cast<const MCSectionCOFF *>(getStreamer().getCurrentSectionOnly());

  if (Type == COFF::IMAGE_COMDAT_SELECT_ASSOCIATIVE)
    return Error(Loc, "cannot make section associative with .linkonce");

  if (Current->getCharacteristics() & COFF::IMAGE_SCN_LNK_COMDAT)
    return Error(Loc, Twine("section '") + Current->getSectionName() +
                                                       "' is already linkonce");

  Current->setSelection(Type);

  if (getLexer().isNot(AsmToken::EndOfStatement))
    return TokError("unexpected token in directive");

  return false;
}

namespace llvm {

MCAsmParserExtension *createVPEAsmParser() {
  return new VPEAsmParser;
}

} // end namespace llvm
