/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Version.h"
#include "clang/Format/Format.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/TokenConcatenation.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stack>
#include <string>
#include <tuple>
#include <unordered_set>

#include <stdio.h>
#include <stdlib.h>

#include "BindingOperations.h"
#include "FileOperations.h"
#include "StringOperations.h"
#include "from-clangd/HeuristicResolver.h"

#if CLANG_VERSION_MAJOR < 8
// Starting with Clang 8.0 some basic functions have been renamed
#define getBeginLoc getLocStart
#define getEndLoc getLocEnd
#endif
// We want std::make_unique, but that's only available in c++14.  In versions
// prior to that, we need to fall back to llvm's make_unique.  It's also the
// case that we expect clang 10 to build with c++14 and clang 9 and earlier to
// build with c++11, at least as suggested by the llvm-config --cxxflags on
// non-windows platforms.  mozilla-central seems to build with -std=c++17 on
// windows so we need to make this decision based on __cplusplus instead of
// the CLANG_VERSION_MAJOR.
#if __cplusplus < 201402L
using llvm::make_unique;
#else
using std::make_unique;
#endif

using namespace clang;

const std::string GENERATED("__GENERATED__" PATHSEP_STRING);

// Absolute path to directory containing source code.
std::string Srcdir;

// Absolute path to objdir (including generated code).
std::string Objdir;

// Absolute path where analysis JSON output will be stored.
std::string Outdir;

enum class FileType {
  // The file was either in the source tree nor objdir. It might be a system
  // include, for example.
  Unknown,
  // A file from the source tree.
  Source,
  // A file from the objdir.
  Generated,
};

// Takes an absolute path to a file, and returns the type of file it is. If
// it's a Source or Generated file, the provided inout path argument is modified
// in-place so that it is relative to the source dir or objdir, respectively.
FileType relativizePath(std::string &path) {
  if (path.compare(0, Objdir.length(), Objdir) == 0) {
    path.replace(0, Objdir.length(), GENERATED);
    return FileType::Generated;
  }
  // Empty filenames can get turned into Srcdir when they are resolved as
  // absolute paths, so we should exclude files that are exactly equal to
  // Srcdir or anything outside Srcdir.
  if (path.length() > Srcdir.length() &&
      path.compare(0, Srcdir.length(), Srcdir) == 0) {
    // Remove the trailing `/' as well.
    path.erase(0, Srcdir.length() + 1);
    return FileType::Source;
  }
  return FileType::Unknown;
}

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/time.h>

static double time() {
  struct timeval Tv;
  gettimeofday(&Tv, nullptr);
  return double(Tv.tv_sec) + double(Tv.tv_usec) / 1000000.;
}
#endif

// Return true if |input| is a valid C++ identifier. We don't want to generate
// analysis information for operators, string literals, etc. by accident since
// it trips up consumers of the data.
static bool isValidIdentifier(std::string Input) {
  for (char C : Input) {
    if (!(isalpha(C) || isdigit(C) || C == '_')) {
      return false;
    }
  }
  return true;
}

template <size_t N>
static bool stringStartsWith(const std::string &Input,
                             const char (&Prefix)[N]) {
  return Input.length() > N - 1 && memcmp(Input.c_str(), Prefix, N - 1) == 0;
}

static bool isASCII(const std::string &Input) {
  for (char C : Input) {
    if (C & 0x80) {
      return false;
    }
  }
  return true;
}

struct RAIITracer {
  RAIITracer(const char *log) : mLog(log) { printf("<%s>\n", mLog); }

  ~RAIITracer() { printf("</%s>\n", mLog); }

  const char *mLog;
};

#define TRACEFUNC RAIITracer tracer(__FUNCTION__);

// Sets variable to value on creation then resets variable to its original
// value on destruction
template <typename T> class ValueRollback {
public:
  template <typename U = T>
  ValueRollback(T &variable, U &&value)
      : mVariable{&variable},
        mSavedValue{std::exchange(variable, std::forward<U>(value))} {}

  ValueRollback(ValueRollback &&other) noexcept
      : mVariable{std::exchange(other.mVariable, nullptr)},
        mSavedValue{std::move(other.mSavedValue)} {}

  ValueRollback(const ValueRollback &) = delete;
  ValueRollback &operator=(ValueRollback &&) = delete;
  ValueRollback &operator=(const ValueRollback &) = delete;

  ~ValueRollback() {
    if (mVariable)
      *mVariable = std::move(mSavedValue);
  }

private:
  T *mVariable;
  T mSavedValue;
};

class IndexConsumer;

bool isPure(FunctionDecl *D) {
#if CLANG_VERSION_MAJOR >= 18
  return D->isPureVirtual();
#else
  return D->isPure();
#endif
}

// For each C++ file seen by the analysis (.cpp or .h), we track a
// FileInfo. This object tracks whether the file is "interesting" (i.e., whether
// it's in the source dir or the objdir). We also store the analysis output
// here.
struct FileInfo {
  FileInfo(std::string &Rname) : Realname(Rname) {
    switch (relativizePath(Realname)) {
    case FileType::Generated:
      Interesting = true;
      Generated = true;
      break;
    case FileType::Source:
      Interesting = true;
      Generated = false;
      break;
    case FileType::Unknown:
      Interesting = false;
      Generated = false;
      break;
    }
  }
  std::string Realname;
  std::vector<std::string> Output;
  bool Interesting;
  bool Generated;
};

struct MacroExpansionState {
  Token MacroNameToken;
  const MacroInfo *MacroInfo = nullptr;
  // other macro symbols this expansion depends on
  std::vector<std::string> Dependencies;
  std::string Expansion;
  std::map<SourceLocation, unsigned> TokenLocations;
  SourceRange Range;
  Token PrevPrevTok;
  Token PrevTok;
};

struct ExpandedMacro {
  std::string Symbol;
  std::string Key; // "{Symbol}(,{Dependencies})..."
  std::string Expansion;
  std::map<SourceLocation, unsigned> TokenLocations;
};

class IndexConsumer;

class PreprocessorHook : public PPCallbacks {
  IndexConsumer *Indexer;

public:
  PreprocessorHook(IndexConsumer *C) : Indexer(C) {}

  virtual void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                           SrcMgr::CharacteristicKind FileType,
                           FileID PrevFID) override;

  virtual void InclusionDirective(SourceLocation HashLoc,
                                  const Token &IncludeTok, StringRef FileName,
                                  bool IsAngled, CharSourceRange FileNameRange,
#if CLANG_VERSION_MAJOR >= 16
                                  OptionalFileEntryRef File,
#elif CLANG_VERSION_MAJOR >= 15
                                  Optional<FileEntryRef> File,
#else
                                  const FileEntry *File,
#endif
                                  StringRef SearchPath, StringRef RelativePath,
#if CLANG_VERSION_MAJOR >= 19
                                  const Module *SuggestedModule,
                                  bool ModuleImported,
#else
                                  const Module *Imported,
#endif
                                  SrcMgr::CharacteristicKind FileType) override;

  virtual void MacroDefined(const Token &Tok,
                            const MacroDirective *Md) override;

  virtual void MacroExpands(const Token &Tok, const MacroDefinition &Md,
                            SourceRange Range, const MacroArgs *Ma) override;
  virtual void MacroUndefined(const Token &Tok, const MacroDefinition &Md,
                              const MacroDirective *Undef) override;
  virtual void Defined(const Token &Tok, const MacroDefinition &Md,
                       SourceRange Range) override;
  virtual void Ifdef(SourceLocation Loc, const Token &Tok,
                     const MacroDefinition &Md) override;
  virtual void Ifndef(SourceLocation Loc, const Token &Tok,
                      const MacroDefinition &Md) override;
};

class IndexConsumer : public ASTConsumer,
                      public RecursiveASTVisitor<IndexConsumer>,
                      public DiagnosticConsumer {
private:
  CompilerInstance &CI;
  SourceManager &SM;
  LangOptions &LO;
  std::map<FileID, std::unique_ptr<FileInfo>> FileMap;
  MangleContext *CurMangleContext;
  ASTContext *AstContext;
  std::unique_ptr<clangd::HeuristicResolver> Resolver;

  // Used during a macro expansion to build the expanded string
  TokenConcatenation ConcatInfo;
  std::optional<MacroExpansionState> MacroExpansionState;
  // Keeps track of the positions of tokens inside each expanded macro
  std::map<SourceLocation, ExpandedMacro> MacroMaps;

  typedef RecursiveASTVisitor<IndexConsumer> Super;

  // Tracks the set of declarations that the current expression/statement is
  // nested inside of.
  struct AutoSetContext {
    AutoSetContext(IndexConsumer *Self, NamedDecl *Context,
                   bool VisitImplicit = false)
        : Self(Self), Prev(Self->CurDeclContext), Decl(Context) {
      this->VisitImplicit =
          VisitImplicit || (Prev ? Prev->VisitImplicit : false);
      Self->CurDeclContext = this;
    }

    ~AutoSetContext() { Self->CurDeclContext = Prev; }

    IndexConsumer *Self;
    AutoSetContext *Prev;
    NamedDecl *Decl;
    bool VisitImplicit;
  };
  AutoSetContext *CurDeclContext;

  FileInfo *getFileInfo(SourceLocation Loc) {
    FileID Id = SM.getFileID(Loc);

    std::map<FileID, std::unique_ptr<FileInfo>>::iterator It;
    It = FileMap.find(Id);
    if (It == FileMap.end()) {
      // We haven't seen this file before. We need to make the FileInfo
      // structure information ourselves
      std::string Filename = std::string(SM.getFilename(Loc));
      std::string Absolute;
      // If Loc is a macro id rather than a file id, it Filename might be
      // empty. Also for some types of file locations that are clang-internal
      // like "<scratch>" it can return an empty Filename. In these cases we
      // want to leave Absolute as empty.
      if (!Filename.empty()) {
        Absolute = getAbsolutePath(Filename);
        if (Absolute.empty()) {
          Absolute = Filename;
        }
      }
      std::unique_ptr<FileInfo> Info = make_unique<FileInfo>(Absolute);
      It = FileMap.insert(std::make_pair(Id, std::move(Info))).first;
    }
    return It->second.get();
  }

  // Helpers for processing declarations
  // Should we ignore this location?
  bool isInterestingLocation(SourceLocation Loc) {
    if (SM.isMacroBodyExpansion(Loc)) {
      Loc = SM.getFileLoc(Loc);
    }

    normalizeLocation(&Loc);
    if (Loc.isInvalid()) {
      return false;
    }

    return getFileInfo(Loc)->Interesting;
  }

  // Convert location to "line:column" or "line:column-column" given length.
  // In resulting string rep, line is 1-based and zero-padded to 5 digits, while
  // column is 0-based and unpadded.
  std::string locationToString(SourceLocation Loc, size_t Length = 0) {
    std::pair<FileID, unsigned> Pair = SM.getDecomposedLoc(Loc);

    bool IsInvalid;
    unsigned Line = SM.getLineNumber(Pair.first, Pair.second, &IsInvalid);
    if (IsInvalid) {
      return "";
    }
    unsigned Column = SM.getColumnNumber(Pair.first, Pair.second, &IsInvalid);
    if (IsInvalid) {
      return "";
    }

    if (Length) {
      return stringFormat("%05d:%d-%d", Line, Column - 1, Column - 1 + Length);
    } else {
      return stringFormat("%05d:%d", Line, Column - 1);
    }
  }

  // Convert SourceRange to "line-line" or "line".
  // In the resulting string rep, line is 1-based.
  std::string lineRangeToString(SourceRange Range, bool omitEnd = false) {
    std::pair<FileID, unsigned> Begin = SM.getDecomposedLoc(Range.getBegin());
    std::pair<FileID, unsigned> End = SM.getDecomposedLoc(Range.getEnd());

    bool IsInvalid;
    unsigned Line1 = SM.getLineNumber(Begin.first, Begin.second, &IsInvalid);
    if (IsInvalid) {
      return "";
    }
    unsigned Line2 = SM.getLineNumber(End.first, End.second, &IsInvalid);
    if (IsInvalid) {
      return "";
    }

    if (omitEnd && Line1 == Line2) {
      return stringFormat("%d", Line1);
    }

    return stringFormat("%d-%d", Line1, Line2);
  }

  // Convert SourceRange to "PATH#line-line" or "PATH#line".
  // If Range's file is same as fromFileID, PATH is omitted.
  std::string pathAndLineRangeToString(FileID fromFileID, SourceRange Range) {
    FileInfo *toFile = getFileInfo(Range.getBegin());
    FileInfo *fromFile = FileMap.find(fromFileID)->second.get();

    auto lineRange = lineRangeToString(Range, true);

    if (lineRange.empty()) {
      return "";
    }

    if (toFile == fromFile) {
      return "#" + lineRange;
    }

    if (toFile->Realname.empty()) {
      return "#" + lineRange;
    }

    std::string result = toFile->Realname;
    result += "#";
    result += lineRange;
    return result;
  }

  // Convert SourceRange to "line:column-line:column".
  // In the resulting string rep, line is 1-based, column is 0-based.
  std::string fullRangeToString(SourceRange Range) {
    std::pair<FileID, unsigned> Begin = SM.getDecomposedLoc(Range.getBegin());
    std::pair<FileID, unsigned> End = SM.getDecomposedLoc(Range.getEnd());

    bool IsInvalid;
    unsigned Line1 = SM.getLineNumber(Begin.first, Begin.second, &IsInvalid);
    if (IsInvalid) {
      return "";
    }
    unsigned Column1 =
        SM.getColumnNumber(Begin.first, Begin.second, &IsInvalid);
    if (IsInvalid) {
      return "";
    }
    unsigned Line2 = SM.getLineNumber(End.first, End.second, &IsInvalid);
    if (IsInvalid) {
      return "";
    }
    unsigned Column2 = SM.getColumnNumber(End.first, End.second, &IsInvalid);
    if (IsInvalid) {
      return "";
    }

    return stringFormat("%d:%d-%d:%d", Line1, Column1 - 1, Line2, Column2 - 1);
  }

  // Returns the qualified name of `d` without considering template parameters.
  std::string getQualifiedName(const NamedDecl *D) {
    const DeclContext *Ctx = D->getDeclContext();
    if (Ctx->isFunctionOrMethod()) {
      return D->getQualifiedNameAsString();
    }

    std::vector<const DeclContext *> Contexts;

    // Collect contexts.
    while (Ctx && isa<NamedDecl>(Ctx)) {
      Contexts.push_back(Ctx);
      Ctx = Ctx->getParent();
    }

    std::string Result;

    std::reverse(Contexts.begin(), Contexts.end());

    for (const DeclContext *DC : Contexts) {
      if (const auto *Spec = dyn_cast<ClassTemplateSpecializationDecl>(DC)) {
        Result += Spec->getNameAsString();

        if (Spec->getSpecializationKind() == TSK_ExplicitSpecialization) {
          std::string Backing;
          llvm::raw_string_ostream Stream(Backing);
          const TemplateArgumentList &TemplateArgs = Spec->getTemplateArgs();
          printTemplateArgumentList(Stream, TemplateArgs.asArray(),
                                    PrintingPolicy(CI.getLangOpts()));
          Result += Stream.str();
        }
      } else if (const auto *Nd = dyn_cast<NamespaceDecl>(DC)) {
        if (Nd->isAnonymousNamespace() || Nd->isInline()) {
          continue;
        }
        Result += Nd->getNameAsString();
      } else if (const auto *Rd = dyn_cast<RecordDecl>(DC)) {
        if (!Rd->getIdentifier()) {
          Result += "(anonymous)";
        } else {
          Result += Rd->getNameAsString();
        }
      } else if (const auto *Fd = dyn_cast<FunctionDecl>(DC)) {
        Result += Fd->getNameAsString();
      } else if (const auto *Ed = dyn_cast<EnumDecl>(DC)) {
        // C++ [dcl.enum]p10: Each enum-name and each unscoped
        // enumerator is declared in the scope that immediately contains
        // the enum-specifier. Each scoped enumerator is declared in the
        // scope of the enumeration.
        if (Ed->isScoped() || Ed->getIdentifier())
          Result += Ed->getNameAsString();
        else
          continue;
      } else {
        Result += cast<NamedDecl>(DC)->getNameAsString();
      }
      Result += "::";
    }

    if (D->getDeclName())
      Result += D->getNameAsString();
    else
      Result += "(anonymous)";

    return Result;
  }

  std::string mangleLocation(SourceLocation Loc,
                             std::string Backup = std::string()) {
    FileInfo *F = getFileInfo(Loc);
    std::string Filename = F->Realname;
    if (Filename.length() == 0 && Backup.length() != 0) {
      return Backup;
    }
    if (F->Generated) {
      // Since generated files may be different on different platforms,
      // we need to include a platform-specific thing in the hash. Otherwise
      // we can end up with hash collisions where different symbols from
      // different platforms map to the same thing.
      char *Platform = getenv("MOZSEARCH_PLATFORM");
      Filename =
          std::string(Platform ? Platform : "") + std::string("@") + Filename;
    }
    return hash(Filename + std::string("@") + locationToString(Loc));
  }

  bool isAcceptableSymbolChar(char c) {
    return isalpha(c) || isdigit(c) || c == '_' || c == '/';
  }

  std::string mangleFile(std::string Filename, FileType Type) {
    // "Mangle" the file path, such that:
    // 1. The majority of paths will still be mostly human-readable.
    // 2. The sanitization algorithm doesn't produce collisions where two
    //    different unsanitized paths can result in the same sanitized paths.
    // 3. The produced symbol doesn't cause problems with downstream consumers.
    // In order to accomplish this, we keep alphanumeric chars, underscores,
    // and slashes, and replace everything else with an "@xx" hex encoding.
    // The majority of path characters are letters and slashes which don't get
    // encoded, so that satisfies (1). Since "@" characters in the unsanitized
    // path get encoded, there should be no "@" characters in the sanitized path
    // that got preserved from the unsanitized input, so that should satisfy
    // (2). And (3) was done by trial-and-error. Note in particular the dot (.)
    // character needs to be encoded, or the symbol-search feature of mozsearch
    // doesn't work correctly, as all dot characters in the symbol query get
    // replaced by #.
    for (size_t i = 0; i < Filename.length(); i++) {
      char c = Filename[i];
      if (isAcceptableSymbolChar(c)) {
        continue;
      }
      char hex[4];
      sprintf(hex, "@%02X", ((int)c) & 0xFF);
      Filename.replace(i, 1, hex);
      i += 2;
    }

    if (Type == FileType::Generated) {
      // Since generated files may be different on different platforms,
      // we need to include a platform-specific thing in the hash. Otherwise
      // we can end up with hash collisions where different symbols from
      // different platforms map to the same thing.
      char *Platform = getenv("MOZSEARCH_PLATFORM");
      Filename =
          std::string(Platform ? Platform : "") + std::string("@") + Filename;
    }
    return Filename;
  }

  std::string mangleURL(std::string Url) {
    return mangleFile(Url, FileType::Source);
  }

  std::string mangleQualifiedName(std::string Name) {
    std::replace(Name.begin(), Name.end(), ' ', '_');
    return Name;
  }

  std::string getMangledName(clang::MangleContext *Ctx,
                             const clang::NamedDecl *Decl) {
    // Main functions will tend to collide because they inherently have similar
    // signatures, so let's provide a custom location-based signature.
    if (isa<FunctionDecl>(Decl) && cast<FunctionDecl>(Decl)->isMain()) {
      return std::string("MF_") + mangleLocation(Decl->getLocation());
    }

    if (isa<FunctionDecl>(Decl) && cast<FunctionDecl>(Decl)->isExternC()) {
      return cast<FunctionDecl>(Decl)->getNameAsString();
    }

    if (isa<FunctionDecl>(Decl) || isa<VarDecl>(Decl)) {
      const DeclContext *DC = Decl->getDeclContext();
      if (isa<TranslationUnitDecl>(DC) || isa<NamespaceDecl>(DC) ||
          isa<LinkageSpecDecl>(DC) ||
          // isa<ExternCContextDecl>(DC) ||
          isa<TagDecl>(DC)) {
        llvm::SmallVector<char, 512> Output;
        llvm::raw_svector_ostream Out(Output);
#if CLANG_VERSION_MAJOR >= 11
        // This code changed upstream in version 11:
        // https://github.com/llvm/llvm-project/commit/29e1a16be8216066d1ed733a763a749aed13ff47
        GlobalDecl GD;
        if (const CXXConstructorDecl *D = dyn_cast<CXXConstructorDecl>(Decl)) {
          GD = GlobalDecl(D, Ctor_Complete);
        } else if (const CXXDestructorDecl *D =
                       dyn_cast<CXXDestructorDecl>(Decl)) {
          GD = GlobalDecl(D, Dtor_Complete);
        } else {
          GD = GlobalDecl(Decl);
        }
        Ctx->mangleName(GD, Out);
#else
        if (const CXXConstructorDecl *D = dyn_cast<CXXConstructorDecl>(Decl)) {
          Ctx->mangleCXXCtor(D, CXXCtorType::Ctor_Complete, Out);
        } else if (const CXXDestructorDecl *D =
                       dyn_cast<CXXDestructorDecl>(Decl)) {
          Ctx->mangleCXXDtor(D, CXXDtorType::Dtor_Complete, Out);
        } else {
          Ctx->mangleName(Decl, Out);
        }
#endif
        return Out.str().str();
      } else {
        return std::string("V_") + mangleLocation(Decl->getLocation()) +
               std::string("_") + hash(std::string(Decl->getName()));
      }
    } else if (isa<TagDecl>(Decl) || isa<ObjCInterfaceDecl>(Decl)) {
      if (!Decl->getIdentifier()) {
        // Anonymous.
        return std::string("T_") + mangleLocation(Decl->getLocation());
      }

      return std::string("T_") + mangleQualifiedName(getQualifiedName(Decl));
    } else if (isa<TypedefNameDecl>(Decl)) {
      if (!Decl->getIdentifier()) {
        // Anonymous.
        return std::string("TA_") + mangleLocation(Decl->getLocation());
      }

      return std::string("TA_") + mangleQualifiedName(getQualifiedName(Decl));
    } else if (isa<NamespaceDecl>(Decl) || isa<NamespaceAliasDecl>(Decl)) {
      if (!Decl->getIdentifier()) {
        // Anonymous.
        return std::string("NS_") + mangleLocation(Decl->getLocation());
      }

      return std::string("NS_") + mangleQualifiedName(getQualifiedName(Decl));
    } else if (const ObjCIvarDecl *D2 = dyn_cast<ObjCIvarDecl>(Decl)) {
      const ObjCInterfaceDecl *Iface = D2->getContainingInterface();
      return std::string("F_<") + getMangledName(Ctx, Iface) + ">_" +
             D2->getNameAsString();
    } else if (const FieldDecl *D2 = dyn_cast<FieldDecl>(Decl)) {
      const RecordDecl *Record = D2->getParent();
      return std::string("F_<") + getMangledName(Ctx, Record) + ">_" +
             D2->getNameAsString();
    } else if (const EnumConstantDecl *D2 = dyn_cast<EnumConstantDecl>(Decl)) {
      const DeclContext *DC = Decl->getDeclContext();
      if (const NamedDecl *Named = dyn_cast<NamedDecl>(DC)) {
        return std::string("E_<") + getMangledName(Ctx, Named) + ">_" +
               D2->getNameAsString();
      }
    }

    assert(false);
    return std::string("");
  }

  void debugLocation(SourceLocation Loc) {
    std::string S = locationToString(Loc);
    StringRef Filename = SM.getFilename(Loc);
    printf("--> %s %s\n", std::string(Filename).c_str(), S.c_str());
  }

  void debugRange(SourceRange Range) {
    printf("Range\n");
    debugLocation(Range.getBegin());
    debugLocation(Range.getEnd());
  }

public:
  IndexConsumer(CompilerInstance &CI)
      : CI(CI), SM(CI.getSourceManager()), LO(CI.getLangOpts()),
        CurMangleContext(nullptr), AstContext(nullptr),
        ConcatInfo(CI.getPreprocessor()), CurDeclContext(nullptr),
        TemplateStack(nullptr) {
    CI.getPreprocessor().addPPCallbacks(make_unique<PreprocessorHook>(this));
    CI.getPreprocessor().setTokenWatcher(
        [this](const auto &token) { onTokenLexed(token); });
  }

  virtual DiagnosticConsumer *clone(DiagnosticsEngine &Diags) const {
    return new IndexConsumer(CI);
  }

#if !defined(_WIN32) && !defined(_WIN64)
  struct AutoTime {
    AutoTime(double *Counter) : Counter(Counter), Start(time()) {}
    ~AutoTime() {
      if (Start) {
        *Counter += time() - Start;
      }
    }
    void stop() {
      *Counter += time() - Start;
      Start = 0;
    }
    double *Counter;
    double Start;
  };
#endif

  // All we need is to follow the final declaration.
  virtual void HandleTranslationUnit(ASTContext &Ctx) {
    CurMangleContext =
        clang::ItaniumMangleContext::create(Ctx, CI.getDiagnostics());

    AstContext = &Ctx;
    Resolver = std::make_unique<clangd::HeuristicResolver>(Ctx);
    TraverseDecl(Ctx.getTranslationUnitDecl());

    // Emit the JSON data for all files now.
    std::map<FileID, std::unique_ptr<FileInfo>>::iterator It;
    for (It = FileMap.begin(); It != FileMap.end(); It++) {
      if (!It->second->Interesting) {
        continue;
      }

      FileInfo &Info = *It->second;

      std::string Filename = Outdir + Info.Realname;
      std::string SrcFilename =
          Info.Generated ? Objdir + Info.Realname.substr(GENERATED.length())
                         : Srcdir + PATHSEP_STRING + Info.Realname;

      ensurePath(Filename);

      // We lock the output file in case some other clang process is trying to
      // write to it at the same time.
      AutoLockFile Lock(SrcFilename, Filename);

      if (!Lock.success()) {
        fprintf(stderr, "Unable to lock file %s\n", Filename.c_str());
        exit(1);
      }

      // Merge our results with the existing lines from the output file.
      // This ensures that header files that are included multiple times
      // in different ways are analyzed completely.
      std::ifstream Fin(Filename.c_str(), std::ios::in | std::ios::binary);
      FILE *OutFp = Lock.openTmp();
      if (!OutFp) {
        fprintf(stderr, "Unable to open tmp out file for %s\n",
                Filename.c_str());
        exit(1);
      }

      // Sort our new results and get an iterator to them
      std::sort(Info.Output.begin(), Info.Output.end());
      std::vector<std::string>::const_iterator NewLinesIter =
          Info.Output.begin();
      std::string LastNewWritten;

      // Loop over the existing (sorted) lines in the analysis output file.
      // (The good() check also handles the case where Fin did not exist when we
      // went to open it.)
      while (Fin.good()) {
        std::string OldLine;
        std::getline(Fin, OldLine);
        // Skip blank lines.
        if (OldLine.length() == 0) {
          continue;
        }
        // We need to put the newlines back that getline() eats.
        OldLine.push_back('\n');

        // Write any results from Info.Output that are lexicographically
        // smaller than OldLine (read from the existing file), but make sure
        // to skip duplicates. Keep advancing NewLinesIter until we reach an
        // entry that is lexicographically greater than OldLine.
        for (; NewLinesIter != Info.Output.end(); NewLinesIter++) {
          if (*NewLinesIter > OldLine) {
            break;
          }
          if (*NewLinesIter == OldLine) {
            continue;
          }
          if (*NewLinesIter == LastNewWritten) {
            // dedupe the new entries being written
            continue;
          }
          if (fwrite(NewLinesIter->c_str(), NewLinesIter->length(), 1, OutFp) !=
              1) {
            fprintf(stderr,
                    "Unable to write %zu bytes[1] to tmp output file for %s\n",
                    NewLinesIter->length(), Filename.c_str());
            exit(1);
          }
          LastNewWritten = *NewLinesIter;
        }

        // Write the entry read from the existing file.
        if (fwrite(OldLine.c_str(), OldLine.length(), 1, OutFp) != 1) {
          fprintf(stderr,
                  "Unable to write %zu bytes[2] to tmp output file for %s\n",
                  OldLine.length(), Filename.c_str());
          exit(1);
        }
      }

      // We finished reading from Fin
      Fin.close();

      // Finish iterating our new results, discarding duplicates
      for (; NewLinesIter != Info.Output.end(); NewLinesIter++) {
        if (*NewLinesIter == LastNewWritten) {
          continue;
        }
        if (fwrite(NewLinesIter->c_str(), NewLinesIter->length(), 1, OutFp) !=
            1) {
          fprintf(stderr,
                  "Unable to write %zu bytes[3] to tmp output file for %s\n",
                  NewLinesIter->length(), Filename.c_str());
          exit(1);
        }
        LastNewWritten = *NewLinesIter;
      }

      // Done writing all the things, close it and replace the old output file
      // with the new one.
      fclose(OutFp);
      if (!Lock.moveTmp()) {
        fprintf(stderr,
                "Unable to move tmp output file into place for %s (err %d)\n",
                Filename.c_str(), errno);
        exit(1);
      }
    }
  }

  // Unfortunately, we have to override all these methods in order to track the
  // context we're inside.

  bool TraverseEnumDecl(EnumDecl *D) {
    AutoSetContext Asc(this, D);
    return Super::TraverseEnumDecl(D);
  }
  bool TraverseRecordDecl(RecordDecl *D) {
    AutoSetContext Asc(this, D);
    return Super::TraverseRecordDecl(D);
  }
  bool TraverseCXXRecordDecl(CXXRecordDecl *D) {
    AutoSetContext Asc(this, D);
    return Super::TraverseCXXRecordDecl(D);
  }
  bool TraverseFunctionDecl(FunctionDecl *D) {
    AutoSetContext Asc(this, D);
    const FunctionDecl *Def;
    // (See the larger AutoTemplateContext comment for more information.) If a
    // method on a templated class is declared out-of-line, we need to analyze
    // the definition inside the scope of the template or else we won't properly
    // handle member access on the templated type.
    if (TemplateStack && D->isDefined(Def) && Def && D != Def) {
      const auto _ = ValueRollback(CurDeclContext, nullptr);
      TraverseFunctionDecl(const_cast<FunctionDecl *>(Def));
    }
    return Super::TraverseFunctionDecl(D);
  }
  bool TraverseCXXMethodDecl(CXXMethodDecl *D) {
    AutoSetContext Asc(this, D);
    const FunctionDecl *Def;
    // See TraverseFunctionDecl.
    if (TemplateStack && D->isDefined(Def) && Def && D != Def) {
      const auto _ = ValueRollback(CurDeclContext, nullptr);
      TraverseFunctionDecl(const_cast<FunctionDecl *>(Def));
    }
    return Super::TraverseCXXMethodDecl(D);
  }
  bool TraverseCXXConstructorDecl(CXXConstructorDecl *D) {
    AutoSetContext Asc(this, D, /*VisitImplicit=*/true);
    const FunctionDecl *Def;
    // See TraverseFunctionDecl.
    if (TemplateStack && D->isDefined(Def) && Def && D != Def) {
      const auto _ = ValueRollback(CurDeclContext, nullptr);
      TraverseFunctionDecl(const_cast<FunctionDecl *>(Def));
    }
    return Super::TraverseCXXConstructorDecl(D);
  }
  bool TraverseCXXConversionDecl(CXXConversionDecl *D) {
    AutoSetContext Asc(this, D);
    const FunctionDecl *Def;
    // See TraverseFunctionDecl.
    if (TemplateStack && D->isDefined(Def) && Def && D != Def) {
      const auto _ = ValueRollback(CurDeclContext, nullptr);
      TraverseFunctionDecl(const_cast<FunctionDecl *>(Def));
    }
    return Super::TraverseCXXConversionDecl(D);
  }
  bool TraverseCXXDestructorDecl(CXXDestructorDecl *D) {
    AutoSetContext Asc(this, D);
    const FunctionDecl *Def;
    // See TraverseFunctionDecl.
    if (TemplateStack && D->isDefined(Def) && Def && D != Def) {
      const auto _ = ValueRollback(CurDeclContext, nullptr);
      TraverseFunctionDecl(const_cast<FunctionDecl *>(Def));
    }
    return Super::TraverseCXXDestructorDecl(D);
  }

  bool TraverseLambdaExpr(LambdaExpr *E) {
    AutoSetContext Asc(this, nullptr, true);
    return Super::TraverseLambdaExpr(E);
  }

  // Used to keep track of the context in which a token appears.
  struct Context {
    // Ultimately this becomes the "context" JSON property.
    std::string Name;

    // Ultimately this becomes the "contextsym" JSON property.
    std::string Symbol;

    Context() {}
    Context(std::string Name, std::string Symbol)
        : Name(Name), Symbol(Symbol) {}
  };

  Context translateContext(NamedDecl *D) {
    const FunctionDecl *F = dyn_cast<FunctionDecl>(D);
    if (F && F->isTemplateInstantiation()) {
      D = F->getTemplateInstantiationPattern();
    }

    return Context(D->getQualifiedNameAsString(),
                   getMangledName(CurMangleContext, D));
  }

  Context getContext(SourceLocation Loc) {
    if (SM.isMacroBodyExpansion(Loc)) {
      // If we're inside a macro definition, we don't return any context. It
      // will probably not be what the user expects if we do.
      return Context();
    }

    AutoSetContext *Ctxt = CurDeclContext;
    while (Ctxt) {
      if (Ctxt->Decl) {
        return translateContext(Ctxt->Decl);
      }
      Ctxt = Ctxt->Prev;
    }
    return Context();
  }

  // Similar to GetContext(SourceLocation), but it skips the declaration passed
  // in. This is useful if we want the context of a declaration that's already
  // on the stack.
  Context getContext(Decl *D) {
    if (SM.isMacroBodyExpansion(D->getLocation())) {
      // If we're inside a macro definition, we don't return any context. It
      // will probably not be what the user expects if we do.
      return Context();
    }

    AutoSetContext *Ctxt = CurDeclContext;
    while (Ctxt) {
      if (Ctxt->Decl && Ctxt->Decl != D) {
        return translateContext(Ctxt->Decl);
      }
      Ctxt = Ctxt->Prev;
    }
    return Context();
  }

  // Searches for the closest CurDeclContext parent that is a function template
  // instantiation
  const FunctionDecl *getCurrentFunctionTemplateInstantiation() {
    const auto *Ctxt = CurDeclContext;
    while (Ctxt) {
      if (Ctxt->Decl && isa<FunctionDecl>(Ctxt->Decl)) {
        const auto *F = Ctxt->Decl->getAsFunction();
        if (F->isTemplateInstantiation())
          return F;
      }
      Ctxt = Ctxt->Prev;
    }
    return nullptr;
  }

  // Analyzing template code is tricky. Suppose we have this code:
  //
  //   template<class T>
  //   bool Foo(T* ptr) { return T::StaticMethod(ptr); }
  //
  // If we analyze the body of Foo without knowing the type T, then we will not
  // be able to generate any information for StaticMethod. However, analyzing
  // Foo for every possible instantiation is inefficient and it also generates
  // too much data in some cases. For example, the following code would generate
  // one definition of Baz for every instantiation, which is undesirable:
  //
  //   template<class T>
  //   class Bar { struct Baz { ... }; };
  //
  // To solve this problem, we analyze templates only once. We do so in a
  // GatherDependent mode where we look for "dependent scoped member
  // expressions" (i.e., things like StaticMethod). We keep track of the
  // locations of these expressions. If we find one or more of them, we analyze
  // the template for each instantiation, in an AnalyzeDependent mode. This mode
  // ignores all source locations except for the ones where we found dependent
  // scoped member expressions before. For these locations, we generate a
  // separate JSON result for each instantiation.
  //
  // We inherit our parent's mode if it is exists.  This is because if our
  // parent is in analyze mode, it means we've already lived a full life in
  // gather mode and we must not restart in gather mode or we'll cause the
  // indexer to visit EVERY identifier, which is way too much data.
  struct AutoTemplateContext {
    AutoTemplateContext(IndexConsumer *Self)
        : Self(Self), CurMode(Self->TemplateStack ? Self->TemplateStack->CurMode
                                                  : Mode::GatherDependent),
          Parent(Self->TemplateStack) {
      Self->TemplateStack = this;
    }

    ~AutoTemplateContext() { Self->TemplateStack = Parent; }

    // We traverse templates in two modes:
    enum class Mode {
      // Gather mode does not traverse into specializations. It looks for
      // locations where it would help to have more info from template
      // specializations.
      GatherDependent,

      // Analyze mode traverses into template specializations and records
      // information about token locations saved in gather mode.
      AnalyzeDependent,
    };

    // We found a dependent scoped member expression! Keep track of it for
    // later.
    void visitDependent(SourceLocation Loc) {
      if (CurMode == Mode::AnalyzeDependent) {
        return;
      }

      DependentLocations.insert(Loc.getRawEncoding());
      if (Parent) {
        Parent->visitDependent(Loc);
      }
    }

    bool inGatherMode() { return CurMode == Mode::GatherDependent; }

    // Do we need to perform the extra AnalyzeDependent passes (one per
    // instantiation)?
    bool needsAnalysis() const {
      if (!DependentLocations.empty()) {
        return true;
      }
      if (Parent) {
        return Parent->needsAnalysis();
      }
      return false;
    }

    void switchMode() { CurMode = Mode::AnalyzeDependent; }

    // Do we want to analyze each template instantiation separately?
    bool shouldVisitTemplateInstantiations() const {
      if (CurMode == Mode::AnalyzeDependent) {
        return true;
      }
      if (Parent) {
        return Parent->shouldVisitTemplateInstantiations();
      }
      return false;
    }

    // For a given expression/statement, should we emit JSON data for it?
    bool shouldVisit(SourceLocation Loc) {
      if (CurMode == Mode::GatherDependent) {
        return true;
      }
      if (DependentLocations.find(Loc.getRawEncoding()) !=
          DependentLocations.end()) {
        return true;
      }
      if (Parent) {
        return Parent->shouldVisit(Loc);
      }
      return false;
    }

  private:
    IndexConsumer *Self;
    Mode CurMode;
    std::unordered_set<unsigned> DependentLocations;
    AutoTemplateContext *Parent;
  };

  AutoTemplateContext *TemplateStack;

  std::unordered_multimap<const FunctionDecl *, const Stmt *>
      ForwardingTemplates;
  std::unordered_set<unsigned> ForwardedTemplateLocations;

  bool shouldVisitTemplateInstantiations() const {
    if (TemplateStack) {
      return TemplateStack->shouldVisitTemplateInstantiations();
    }
    return false;
  }

  bool shouldVisitImplicitCode() const {
    return CurDeclContext && CurDeclContext->VisitImplicit;
  }

  // We don't want to traverse all specializations everytime we find a forward
  // declaration, so only traverse specializations related to an actual
  // definition.
  //
  // ```
  // // This is the canonical declaration for Maybe but isn't really useful.
  // template <typename T>
  // struct Maybe;
  //
  // // This is another ClassTemplateDecl, but not the canonical one, where we
  // // actually have the definition. This is the one we want to traverse.
  // template <typename T>
  // struct Maybe {
  //   // This is both the canonical declaration and the definition for
  //   // inline_method and we want to traverse it.
  //   template <typename... Args>
  //   T *inline_method(Args&&... args) {
  //     // definition
  //   }
  //
  //   // This is the canonical declaration, TraverseFunctionTemplateDecl
  //   // traverses its out of line definition too.
  //   template <typename... Args>
  //   T *out_of_line_method(Args&&... args);
  // }
  //
  // // This is the definition for Maybe<T>::out_of_line_method<Args...>
  // // It is traversed when calling TraverseFunctionTemplateDecl on the
  // // canonical declaration.
  // template <typename T>
  // template <typename... Args>
  // T *maybe(Args&&... args) {
  //   // definition
  // }
  // ```
  //
  // So:
  // - for class templates we check isThisDeclarationADefinition
  // - for function templates we check isCanonicalDecl
  bool TraverseClassTemplateDecl(ClassTemplateDecl *D) {
    AutoTemplateContext Atc(this);
    Super::TraverseClassTemplateDecl(D);

    // Gather dependent locations from partial specializations too
    SmallVector<ClassTemplatePartialSpecializationDecl *> PS;
    D->getPartialSpecializations(PS);
    for (auto *Spec : PS) {
      for (auto *Rd : Spec->redecls()) {
        TraverseDecl(Rd);
      }
    }

    if (!Atc.needsAnalysis()) {
      return true;
    }

    Atc.switchMode();

    if (!D->isThisDeclarationADefinition())
      return true;

    for (auto *Spec : D->specializations()) {
      for (auto *Rd : Spec->redecls()) {
        // We don't want to visit injected-class-names in this traversal.
        if (cast<CXXRecordDecl>(Rd)->isInjectedClassName())
          continue;

        TraverseDecl(Rd);
      }
    }

    return true;
  }

  // See also comment above TraverseClassTemplateDecl
  bool TraverseFunctionTemplateDecl(FunctionTemplateDecl *D) {
    AutoTemplateContext Atc(this);
    if (Atc.inGatherMode()) {
      Super::TraverseFunctionTemplateDecl(D);
    }

    if (!Atc.needsAnalysis()) {
      return true;
    }

    Atc.switchMode();

    if (!D->isCanonicalDecl())
      return true;

    for (auto *Spec : D->specializations()) {
      for (auto *Rd : Spec->redecls()) {
        TraverseDecl(Rd);
      }
    }

    return true;
  }

  bool shouldVisit(SourceLocation Loc) {
    if (TemplateStack) {
      return TemplateStack->shouldVisit(Loc);
    }
    return true;
  }

  enum {
    // Flag to omit the identifier from being cross-referenced across files.
    // This is usually desired for local variables.
    NoCrossref = 1 << 0,
    // Flag to indicate the token with analysis data is not an identifier.
    // Indicates
    // we want to skip the check that tries to ensure a sane identifier token.
    NotIdentifierToken = 1 << 1,
    // This indicates that the end of the provided SourceRange is valid and
    // should be respected. If this flag is not set, the visitIdentifier
    // function should use only the start of the SourceRange and auto-detect
    // the end based on whatever token is found at the start.
    LocRangeEndValid = 1 << 2,
    // Indicates this record was generated through heuristic template
    // resolution.
    Heuristic = 1 << 3,
  };

  void emitStructuredRecordInfo(llvm::json::OStream &J, SourceLocation Loc,
                                const RecordDecl *decl) {
    J.attribute("kind",
                TypeWithKeyword::getTagTypeKindName(decl->getTagKind()));

    const ASTContext &C = *AstContext;
    const ASTRecordLayout &Layout = C.getASTRecordLayout(decl);

    J.attribute("sizeBytes", Layout.getSize().getQuantity());
    J.attribute("alignmentBytes", Layout.getAlignment().getQuantity());

    emitBindingAttributes(J, *decl);

    auto cxxDecl = dyn_cast<CXXRecordDecl>(decl);

    if (cxxDecl) {
      if (Layout.hasOwnVFPtr()) {
        // Encode the size of virtual function table pointer
        // instead of just true/false, for 2 reasons:
        //  * having the size here is easier for the consumer
        //  * the size string 4/8 is shorter than true/false in the analysis
        //    file
        const QualType ptrType = C.getUIntPtrType();
        J.attribute("ownVFPtrBytes",
                    C.getTypeSizeInChars(ptrType).getQuantity());
      }

      J.attributeBegin("supers");
      J.arrayBegin();
      for (const CXXBaseSpecifier &Base : cxxDecl->bases()) {
        const CXXRecordDecl *BaseDecl = Base.getType()->getAsCXXRecordDecl();

        J.objectBegin();

        J.attribute("sym", getMangledName(CurMangleContext, BaseDecl));

        if (Base.isVirtual()) {
          CharUnits superOffsetBytes = Layout.getVBaseClassOffset(BaseDecl);
          J.attribute("offsetBytes", superOffsetBytes.getQuantity());
        } else {
          CharUnits superOffsetBytes = Layout.getBaseClassOffset(BaseDecl);
          J.attribute("offsetBytes", superOffsetBytes.getQuantity());
        }

        J.attributeBegin("props");
        J.arrayBegin();
        if (Base.isVirtual()) {
          J.value("virtual");
        }
        J.arrayEnd();
        J.attributeEnd();

        J.objectEnd();
      }
      J.arrayEnd();
      J.attributeEnd();

      J.attributeBegin("methods");
      J.arrayBegin();
      for (const CXXMethodDecl *MethodDecl : cxxDecl->methods()) {
        J.objectBegin();

        J.attribute("pretty", getQualifiedName(MethodDecl));
        J.attribute("sym", getMangledName(CurMangleContext, MethodDecl));

        // TODO: Better figure out what to do for non-isUserProvided methods
        // which means there's potentially semantic data that doesn't correspond
        // to a source location in the source.  Should we be emitting
        // structured info for those when we're processing the class here?

        J.attributeBegin("props");
        J.arrayBegin();
        if (MethodDecl->isStatic()) {
          J.value("static");
        }
        if (MethodDecl->isInstance()) {
          J.value("instance");
        }
        if (MethodDecl->isVirtual()) {
          J.value("virtual");
        }
        if (MethodDecl->isUserProvided()) {
          J.value("user");
        }
        if (MethodDecl->isDefaulted()) {
          J.value("defaulted");
        }
        if (MethodDecl->isDeleted()) {
          J.value("deleted");
        }
        if (MethodDecl->isConstexpr()) {
          J.value("constexpr");
        }
        J.arrayEnd();
        J.attributeEnd();

        J.objectEnd();
      }
      J.arrayEnd();
      J.attributeEnd();
    }

    FileID structFileID = SM.getFileID(Loc);

    J.attributeBegin("fields");
    J.arrayBegin();
    uint64_t iField = 0;
    for (RecordDecl::field_iterator It = decl->field_begin(),
                                    End = decl->field_end();
         It != End; ++It, ++iField) {
      const FieldDecl &Field = **It;
      auto sourceRange =
          SM.getExpansionRange(Field.getSourceRange()).getAsRange();
      uint64_t localOffsetBits = Layout.getFieldOffset(iField);
      CharUnits localOffsetBytes = C.toCharUnitsFromBits(localOffsetBits);

      J.objectBegin();
      J.attribute("lineRange",
                  pathAndLineRangeToString(structFileID, sourceRange));
      J.attribute("pretty", getQualifiedName(&Field));
      J.attribute("sym", getMangledName(CurMangleContext, &Field));

      QualType FieldType = Field.getType();
      QualType CanonicalFieldType = FieldType.getCanonicalType();
      LangOptions langOptions;
      PrintingPolicy Policy(langOptions);
      Policy.PrintCanonicalTypes = true;
      J.attribute("type", CanonicalFieldType.getAsString(Policy));

      const TagDecl *tagDecl = CanonicalFieldType->getAsTagDecl();
      if (!tagDecl) {
        // Try again piercing any pointers/references involved.  Note that our
        // typesym semantics are dubious-ish and right now crossref just does
        // some parsing of "type" itself until we improve this rep.
        CanonicalFieldType = CanonicalFieldType->getPointeeType();
        if (!CanonicalFieldType.isNull()) {
          tagDecl = CanonicalFieldType->getAsTagDecl();
        }
      }
      if (tagDecl) {
        J.attribute("typesym", getMangledName(CurMangleContext, tagDecl));
      }
      J.attribute("offsetBytes", localOffsetBytes.getQuantity());
      if (Field.isBitField()) {
        J.attributeBegin("bitPositions");
        J.objectBegin();

        J.attribute("begin",
                    unsigned(localOffsetBits - C.toBits(localOffsetBytes)));
#if CLANG_VERSION_MAJOR < 20
        J.attribute("width", Field.getBitWidthValue(C));
#else
        J.attribute("width", Field.getBitWidthValue());
#endif

        J.objectEnd();
        J.attributeEnd();
      } else {
        // Try and get the field as a record itself so we can know its size, but
        // we don't actually want to recurse into it.
        if (auto FieldRec = Field.getType()->getAs<RecordType>()) {
          auto const &FieldLayout = C.getASTRecordLayout(FieldRec->getDecl());
          J.attribute("sizeBytes", FieldLayout.getSize().getQuantity());
        } else {
          // We were unable to get it as a record, which suggests it's a normal
          // type, in which case let's just ask for the type size.  (Maybe this
          // would also work for the above case too?)
          uint64_t typeSizeBits = C.getTypeSize(Field.getType());
          CharUnits typeSizeBytes = C.toCharUnitsFromBits(typeSizeBits);
          J.attribute("sizeBytes", typeSizeBytes.getQuantity());
        }
      }
      J.objectEnd();
    }
    J.arrayEnd();
    J.attributeEnd();
  }

  void emitStructuredEnumInfo(llvm::json::OStream &J, const EnumDecl *ED) {
    J.attribute("kind", "enum");
  }

  void emitStructuredEnumConstantInfo(llvm::json::OStream &J,
                                      const EnumConstantDecl *ECD) {
    J.attribute("kind", "enumConstant");
  }

  void emitStructuredFunctionInfo(llvm::json::OStream &J,
                                  const FunctionDecl *decl) {
    emitBindingAttributes(J, *decl);

    J.attributeBegin("args");
    J.arrayBegin();

    for (auto param : decl->parameters()) {
      J.objectBegin();

      J.attribute("name", param->getName());
      QualType ArgType = param->getOriginalType();
      J.attribute("type", ArgType.getAsString());

      QualType CanonicalArgType = ArgType.getCanonicalType();
      const TagDecl *canonDecl = CanonicalArgType->getAsTagDecl();
      if (!canonDecl) {
        // Try again piercing any pointers/references involved.  Note that our
        // typesym semantics are dubious-ish and right now crossref just does
        // some parsing of "type" itself until we improve this rep.
        CanonicalArgType = CanonicalArgType->getPointeeType();
        if (!CanonicalArgType.isNull()) {
          canonDecl = CanonicalArgType->getAsTagDecl();
        }
      }
      if (canonDecl) {
        J.attribute("typesym", getMangledName(CurMangleContext, canonDecl));
      }

      J.objectEnd();
    }

    J.arrayEnd();
    J.attributeEnd();

    auto cxxDecl = dyn_cast<CXXMethodDecl>(decl);

    if (cxxDecl) {
      J.attribute("kind", "method");
      if (auto parentDecl = cxxDecl->getParent()) {
        J.attribute("parentsym", getMangledName(CurMangleContext, parentDecl));
      }

      J.attributeBegin("overrides");
      J.arrayBegin();
      for (const CXXMethodDecl *MethodDecl : cxxDecl->overridden_methods()) {
        J.objectBegin();

        // TODO: Make sure we're doing template traversals appropriately...
        // findOverriddenMethods (now removed) liked to do:
        //   if (Decl->isTemplateInstantiation()) {
        //     Decl =
        //     dyn_cast<CXXMethodDecl>(Decl->getTemplateInstantiationPattern());
        //   }
        // I think our pre-emptive dereferencing/avoidance of templates may
        // protect us from this, but it needs more investigation.

        J.attribute("sym", getMangledName(CurMangleContext, MethodDecl));

        J.objectEnd();
      }
      J.arrayEnd();
      J.attributeEnd();

    } else {
      J.attribute("kind", "function");
    }

    // ## Props
    J.attributeBegin("props");
    J.arrayBegin();
    // some of these are only possible on a CXXMethodDecl, but we want them all
    // in the same array, so condition these first ones.
    if (cxxDecl) {
      if (cxxDecl->isStatic()) {
        J.value("static");
      }
      if (cxxDecl->isInstance()) {
        J.value("instance");
      }
      if (cxxDecl->isVirtual()) {
        J.value("virtual");
      }
      if (cxxDecl->isUserProvided()) {
        J.value("user");
      }
    }
    if (decl->isDefaulted()) {
      J.value("defaulted");
    }
    if (decl->isDeleted()) {
      J.value("deleted");
    }
    if (decl->isConstexpr()) {
      J.value("constexpr");
    }
    J.arrayEnd();
    J.attributeEnd();
  }

  /**
   * Emit structured info for a field.  Right now the intent is for this to just
   * be a pointer to its parent's structured info with this method entirely
   * avoiding getting the ASTRecordLayout.
   *
   * TODO: Give more thought on where to locate the canonical info on fields and
   * how to normalize their exposure over the web.  We could relink the info
   * both at cross-reference time and web-server lookup time.  This is also
   * called out in `analysis.md`.
   */
  void emitStructuredFieldInfo(llvm::json::OStream &J, const FieldDecl *decl) {
    J.attribute("kind", "field");

    // XXX the call to decl::getParent will assert below for ObjCIvarDecl
    // instances because their DecContext is not a RecordDecl.  So just bail
    // for now.
    // TODO: better support ObjC.
    if (!dyn_cast<ObjCIvarDecl>(decl)) {
      if (auto parentDecl = decl->getParent()) {
        J.attribute("parentsym", getMangledName(CurMangleContext, parentDecl));
      }
    }
  }

  /**
   * Emit structured info for a variable if it is a static class member.
   */
  void emitStructuredVarInfo(llvm::json::OStream &J, const VarDecl *decl) {
    const auto *parentDecl =
        dyn_cast_or_null<RecordDecl>(decl->getDeclContext());

    if (parentDecl) {
      J.attribute("kind", "field");
    } else if (llvm::isa<ParmVarDecl>(decl)) {
      J.attribute("kind", "parameter");
    } else if (decl->isLocalVarDecl()) {
      J.attribute("kind", "localVar");
    } else {
      // namespace scope variable
      J.attribute("kind", "variable");
    }

    if (parentDecl) {
      J.attribute("parentsym", getMangledName(CurMangleContext, parentDecl));
    }

    emitBindingAttributes(J, *decl);
  }

  void emitStructuredInfo(SourceLocation Loc, const NamedDecl *decl) {
    std::string json_str;
    llvm::raw_string_ostream ros(json_str);
    llvm::json::OStream J(ros);
    // Start the top-level object.
    J.objectBegin();

    unsigned StartOffset = SM.getFileOffset(Loc);
    unsigned EndOffset =
        StartOffset + Lexer::MeasureTokenLength(Loc, SM, CI.getLangOpts());
    J.attribute("loc", locationToString(Loc, EndOffset - StartOffset));
    J.attribute("structured", 1);
    J.attribute("pretty", getQualifiedName(decl));
    J.attribute("sym", getMangledName(CurMangleContext, decl));

    if (const RecordDecl *RD = dyn_cast<RecordDecl>(decl)) {
      emitStructuredRecordInfo(J, Loc, RD);
    } else if (const EnumDecl *ED = dyn_cast<EnumDecl>(decl)) {
      emitStructuredEnumInfo(J, ED);
    } else if (const EnumConstantDecl *ECD = dyn_cast<EnumConstantDecl>(decl)) {
      emitStructuredEnumConstantInfo(J, ECD);
    } else if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(decl)) {
      emitStructuredFunctionInfo(J, FD);
    } else if (const FieldDecl *FD = dyn_cast<FieldDecl>(decl)) {
      emitStructuredFieldInfo(J, FD);
    } else if (const VarDecl *VD = dyn_cast<VarDecl>(decl)) {
      emitStructuredVarInfo(J, VD);
    }

    // End the top-level object.
    J.objectEnd();

    FileInfo *F = getFileInfo(Loc);
    // we want a newline.
    ros << '\n';
    F->Output.push_back(std::move(ros.str()));
  }

  // XXX Type annotating.
  // QualType is the type class.  It has helpers like TagDecl via getAsTagDecl.
  // ValueDecl exposes a getType() method.
  //
  // Arguably it makes sense to only expose types that Searchfox has definitions
  // for as first-class.  Probably the way to go is like context/contextsym.
  // We expose a "type" which is just a human-readable string which has no
  // semantic purposes and is just a display string, plus then a "typesym" which
  // we expose if we were able to map the type.
  //
  // Other meta-info: field offsets.  Ancestor types.

  // This is the only function that emits analysis JSON data. It should be
  // called for each identifier that corresponds to a symbol.
  void visitIdentifier(const char *Kind, const char *SyntaxKind,
                       llvm::StringRef QualName, SourceRange LocRange,
                       std::string Symbol, QualType MaybeType = QualType(),
                       Context TokenContext = Context(), int Flags = 0,
                       SourceRange PeekRange = SourceRange(),
                       SourceRange NestingRange = SourceRange(),
                       std::vector<SourceRange> *ArgRanges = nullptr) {
    SourceLocation Loc = LocRange.getBegin();

    // Also visit the spelling site.
    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);
    if (SpellingLoc != Loc) {
      visitIdentifier(Kind, SyntaxKind, QualName, SpellingLoc, Symbol,
                      MaybeType, TokenContext, Flags, PeekRange, NestingRange,
                      ArgRanges);
    }

    SourceLocation ExpansionLoc = SM.getExpansionLoc(Loc);
    normalizeLocation(&ExpansionLoc);

    if (!shouldVisit(ExpansionLoc)) {
      return;
    }

    if (ExpansionLoc != Loc)
      Flags = Flags & ~LocRangeEndValid;

    // Find the file positions corresponding to the token.
    unsigned StartOffset = SM.getFileOffset(ExpansionLoc);
    unsigned EndOffset =
        (Flags & LocRangeEndValid)
            ? SM.getFileOffset(LocRange.getEnd())
            : StartOffset +
                  Lexer::MeasureTokenLength(ExpansionLoc, SM, CI.getLangOpts());

    std::string LocStr =
        locationToString(ExpansionLoc, EndOffset - StartOffset);
    std::string RangeStr =
        locationToString(ExpansionLoc, EndOffset - StartOffset);
    std::string PeekRangeStr;

    if (!(Flags & NotIdentifierToken)) {
      // Get the token's characters so we can make sure it's a valid token.
      const char *StartChars = SM.getCharacterData(ExpansionLoc);
      std::string Text(StartChars, EndOffset - StartOffset);
      if (!isValidIdentifier(Text)) {
        return;
      }
    }

    FileInfo *F = getFileInfo(ExpansionLoc);

    if (!(Flags & NoCrossref)) {
      std::string json_str;
      llvm::raw_string_ostream ros(json_str);
      llvm::json::OStream J(ros);
      // Start the top-level object.
      J.objectBegin();

      J.attribute("loc", LocStr);
      J.attribute("target", 1);
      J.attribute("kind", Kind);
      J.attribute("pretty", QualName.data());
      J.attribute("sym", Symbol);
      if (!TokenContext.Name.empty()) {
        J.attribute("context", TokenContext.Name);
      }
      if (!TokenContext.Symbol.empty()) {
        J.attribute("contextsym", TokenContext.Symbol);
      }
      if (PeekRange.isValid()) {
        PeekRangeStr = lineRangeToString(PeekRange);
        if (!PeekRangeStr.empty()) {
          J.attribute("peekRange", PeekRangeStr);
        }
      }

      if (ArgRanges) {
        J.attributeBegin("argRanges");
        J.arrayBegin();

        for (auto range : *ArgRanges) {
          std::string ArgRangeStr = fullRangeToString(range);
          if (!ArgRangeStr.empty()) {
            J.value(ArgRangeStr);
          }
        }

        J.arrayEnd();
        J.attributeEnd();
      }

      // End the top-level object.
      J.objectEnd();
      // we want a newline.
      ros << '\n';
      F->Output.push_back(std::move(ros.str()));
    }

    // Generate a single "source":1 for all the symbols. If we search from here,
    // we want to union the results for every symbol in `symbols`.
    std::string json_str;
    llvm::raw_string_ostream ros(json_str);
    llvm::json::OStream J(ros);
    // Start the top-level object.
    J.objectBegin();

    J.attribute("loc", RangeStr);
    J.attribute("source", 1);

    if (NestingRange.isValid()) {
      std::string NestingRangeStr = fullRangeToString(NestingRange);
      if (!NestingRangeStr.empty()) {
        J.attribute("nestingRange", NestingRangeStr);
      }
    }

    std::string Syntax;
    if (Flags & NoCrossref) {
      J.attribute("syntax", "");
    } else {
      Syntax = Kind;
      Syntax.push_back(',');
      Syntax.append(SyntaxKind);
      J.attribute("syntax", Syntax);
    }

    if (!MaybeType.isNull()) {
      J.attribute("type", MaybeType.getAsString());
      QualType canonical = MaybeType.getCanonicalType();
      const TagDecl *decl = canonical->getAsTagDecl();
      if (!decl) {
        // Try again piercing any pointers/references involved.  Note that our
        // typesym semantics are dubious-ish and right now crossref just does
        // some parsing of "type" itself until we improve this rep.
        canonical = canonical->getPointeeType();
        if (!canonical.isNull()) {
          decl = canonical->getAsTagDecl();
        }
      }
      if (decl) {
        std::string Mangled = getMangledName(CurMangleContext, decl);
        J.attribute("typesym", Mangled);
      }
    }

    std::string Pretty(SyntaxKind);
    Pretty.push_back(' ');
    Pretty.append(QualName.data());
    J.attribute("pretty", Pretty);

    J.attribute("sym", Symbol);

    if (Flags & NoCrossref) {
      J.attribute("no_crossref", 1);
    }

    if (Flags & Heuristic) {
      J.attributeBegin("confidence");
      J.arrayBegin();
      J.value("cppTemplateHeuristic");
      J.arrayEnd();
      J.attributeEnd();
    }

    if (ArgRanges) {
      J.attributeBegin("argRanges");
      J.arrayBegin();

      for (auto range : *ArgRanges) {
        std::string ArgRangeStr = fullRangeToString(range);
        if (!ArgRangeStr.empty()) {
          J.value(ArgRangeStr);
        }
      }

      J.arrayEnd();
      J.attributeEnd();
    }

    const auto macro = MacroMaps.find(ExpansionLoc);
    if (macro != MacroMaps.end()) {
      const auto &macroInfo = macro->second;
      if (macroInfo.Symbol == Symbol) {
        J.attributeBegin("expandsTo");
        J.objectBegin();
        J.attributeBegin(macroInfo.Key);
        J.objectBegin();
        J.attribute("", macroInfo.Expansion); // "" is the platform key,
                                              // populated by the merge step
        J.objectEnd();
        J.attributeEnd();
        J.objectEnd();
        J.attributeEnd();
      } else {
        const auto it = macroInfo.TokenLocations.find(Loc);
        if (it != macroInfo.TokenLocations.end()) {
          J.attributeBegin("inExpansionAt");
          J.objectBegin();
          J.attributeBegin(macroInfo.Key);
          J.objectBegin();
          J.attributeBegin(
              ""); // "" is the platform key, populated by the merge step
          J.arrayBegin();
          J.value(it->second);
          J.arrayEnd();
          J.attributeEnd();
          J.objectEnd();
          J.attributeEnd();
          J.objectEnd();
          J.attributeEnd();
        }
      }
    }

    // End the top-level object.
    J.objectEnd();

    // we want a newline.
    ros << '\n';
    F->Output.push_back(std::move(ros.str()));
  }

  void normalizeLocation(SourceLocation *Loc) {
    *Loc = SM.getSpellingLoc(*Loc);
  }

  // For cases where the left-brace is not directly accessible from the AST,
  // helper to use the lexer to find the brace.  Make sure you're picking the
  // start location appropriately!
  SourceLocation findLeftBraceFromLoc(SourceLocation Loc) {
    return Lexer::findLocationAfterToken(Loc, tok::l_brace, SM, LO, false);
  }

  // If the provided statement is compound, return its range.
  SourceRange getCompoundStmtRange(Stmt *D) {
    if (!D) {
      return SourceRange();
    }

    CompoundStmt *D2 = dyn_cast<CompoundStmt>(D);
    if (D2) {
      return D2->getSourceRange();
    }

    return SourceRange();
  }

  SourceRange getFunctionPeekRange(FunctionDecl *D) {
    // We always start at the start of the function decl, which may include the
    // return type on a separate line.
    SourceLocation Start = D->getBeginLoc();

    // By default, we end at the line containing the function's name.
    SourceLocation End = D->getLocation();

    std::pair<FileID, unsigned> FuncLoc = SM.getDecomposedLoc(End);

    // But if there are parameters, we want to include those as well.
    for (ParmVarDecl *Param : D->parameters()) {
      std::pair<FileID, unsigned> ParamLoc =
          SM.getDecomposedLoc(Param->getLocation());

      // It's possible there are macros involved or something. We don't include
      // the parameters in that case.
      if (ParamLoc.first == FuncLoc.first) {
        // Assume parameters are in order, so we always take the last one.
        End = Param->getEndLoc();
      }
    }

    return SourceRange(Start, End);
  }

  SourceRange getTagPeekRange(TagDecl *D) {
    SourceLocation Start = D->getBeginLoc();

    // By default, we end at the line containing the name.
    SourceLocation End = D->getLocation();

    std::pair<FileID, unsigned> FuncLoc = SM.getDecomposedLoc(End);

    if (CXXRecordDecl *D2 = dyn_cast<CXXRecordDecl>(D)) {
      // But if there are parameters, we want to include those as well.
      for (CXXBaseSpecifier &Base : D2->bases()) {
        std::pair<FileID, unsigned> Loc = SM.getDecomposedLoc(Base.getEndLoc());

        // It's possible there are macros involved or something. We don't
        // include the parameters in that case.
        if (Loc.first == FuncLoc.first) {
          // Assume parameters are in order, so we always take the last one.
          End = Base.getEndLoc();
        }
      }
    }

    return SourceRange(Start, End);
  }

  SourceRange getCommentRange(NamedDecl *D) {
    const RawComment *RC = AstContext->getRawCommentForDeclNoCache(D);
    if (!RC) {
      return SourceRange();
    }

    return RC->getSourceRange();
  }

  // Sanity checks that all ranges are in the same file, returning the first if
  // they're in different files.  Unions the ranges based on which is first.
  SourceRange combineRanges(SourceRange Range1, SourceRange Range2) {
    if (Range1.isInvalid()) {
      return Range2;
    }
    if (Range2.isInvalid()) {
      return Range1;
    }

    std::pair<FileID, unsigned> Begin1 = SM.getDecomposedLoc(Range1.getBegin());
    std::pair<FileID, unsigned> End1 = SM.getDecomposedLoc(Range1.getEnd());
    std::pair<FileID, unsigned> Begin2 = SM.getDecomposedLoc(Range2.getBegin());
    std::pair<FileID, unsigned> End2 = SM.getDecomposedLoc(Range2.getEnd());

    if (End1.first != Begin2.first) {
      // Something weird is probably happening with the preprocessor. Just
      // return the first range.
      return Range1;
    }

    // See which range comes first.
    if (Begin1.second <= End2.second) {
      return SourceRange(Range1.getBegin(), Range2.getEnd());
    } else {
      return SourceRange(Range2.getBegin(), Range1.getEnd());
    }
  }

  // Given a location and a range, returns the range if:
  // - The location and the range live in the same file.
  // - The range is well ordered (end is not before begin).
  // Returns an empty range otherwise.
  SourceRange validateRange(SourceLocation Loc, SourceRange Range) {
    std::pair<FileID, unsigned> Decomposed = SM.getDecomposedLoc(Loc);
    std::pair<FileID, unsigned> Begin = SM.getDecomposedLoc(Range.getBegin());
    std::pair<FileID, unsigned> End = SM.getDecomposedLoc(Range.getEnd());

    if (Begin.first != Decomposed.first || End.first != Decomposed.first) {
      return SourceRange();
    }

    if (Begin.second >= End.second) {
      return SourceRange();
    }

    return Range;
  }

  bool VisitNamedDecl(NamedDecl *D) {
    SourceLocation Loc = D->getLocation();
    if (!isInterestingLocation(Loc)) {
      return true;
    }

    SourceLocation ExpansionLoc = Loc;
    if (SM.isMacroBodyExpansion(Loc)) {
      ExpansionLoc = SM.getFileLoc(Loc);
    }
    normalizeLocation(&ExpansionLoc);

    if (isa<ParmVarDecl>(D) && !D->getDeclName().getAsIdentifierInfo()) {
      // Unnamed parameter in function proto.
      return true;
    }

    int Flags = 0;
    const char *Kind = "def";
    const char *PrettyKind = "?";
    bool wasTemplate = false;
    SourceRange PeekRange(D->getBeginLoc(), D->getEndLoc());
    // The nesting range identifies the left brace and right brace, which
    // heavily depends on the AST node type.
    SourceRange NestingRange;
    QualType qtype = QualType();
    if (FunctionDecl *D2 = dyn_cast<FunctionDecl>(D)) {
      if (D2->isTemplateInstantiation()) {
        wasTemplate = true;
        D = D2->getTemplateInstantiationPattern();
      }
      // We treat pure virtual declarations as definitions.
      Kind =
          (D2->isThisDeclarationADefinition() || isPure(D2)) ? "def" : "decl";
      PrettyKind = "function";
      PeekRange = getFunctionPeekRange(D2);

      // Only emit the nesting range if:
      // - This is a definition AND
      // - This isn't a template instantiation.  Function templates'
      //   instantiations can end up as a definition with a Loc at their point
      //   of declaration but with the CompoundStmt of the template's
      //   point of definition.  This really messes up the nesting range logic.
      //   At the time of writing this, the test repo's `big_header.h`'s
      //   `WhatsYourVector_impl::forwardDeclaredTemplateThingInlinedBelow` as
      //   instantiated by `big_cpp.cpp` triggers this phenomenon.
      //
      // Note: As covered elsewhere, template processing is tricky and it's
      // conceivable that we may change traversal patterns in the future,
      // mooting this guard.
      if (D2->isThisDeclarationADefinition() &&
          !D2->isTemplateInstantiation()) {
        // The CompoundStmt range is the brace range.
        NestingRange = getCompoundStmtRange(D2->getBody());
      }
    } else if (TagDecl *D2 = dyn_cast<TagDecl>(D)) {
      Kind = D2->isThisDeclarationADefinition() ? "def" : "forward";
      PrettyKind = "type";

      if (D2->isThisDeclarationADefinition() && D2->getDefinition() == D2) {
        PeekRange = getTagPeekRange(D2);
        NestingRange = D2->getBraceRange();
      } else {
        PeekRange = SourceRange();
      }
    } else if (TypedefNameDecl *D2 = dyn_cast<TypedefNameDecl>(D)) {
      Kind = "alias";
      PrettyKind = "type";
      PeekRange = SourceRange(ExpansionLoc, ExpansionLoc);
      qtype = D2->getUnderlyingType();
    } else if (VarDecl *D2 = dyn_cast<VarDecl>(D)) {
      if (D2->isLocalVarDeclOrParm()) {
        Flags = NoCrossref;
      }

      Kind = D2->isThisDeclarationADefinition() == VarDecl::DeclarationOnly
                 ? "decl"
                 : "def";
      PrettyKind = "variable";
    } else if (isa<NamespaceDecl>(D) || isa<NamespaceAliasDecl>(D)) {
      Kind = "def";
      PrettyKind = "namespace";
      PeekRange = SourceRange(ExpansionLoc, ExpansionLoc);
      NamespaceDecl *D2 = dyn_cast<NamespaceDecl>(D);
      if (D2) {
        // There's no exposure of the left brace so we have to find it.
        NestingRange = SourceRange(
            findLeftBraceFromLoc(D2->isAnonymousNamespace() ? D2->getBeginLoc()
                                                            : ExpansionLoc),
            D2->getRBraceLoc());
      }
    } else if (isa<FieldDecl>(D)) {
      Kind = "def";
      PrettyKind = "field";
    } else if (isa<EnumConstantDecl>(D)) {
      Kind = "def";
      PrettyKind = "enum constant";
    } else {
      return true;
    }

    if (ValueDecl *D2 = dyn_cast<ValueDecl>(D)) {
      qtype = D2->getType();
    }

    SourceRange CommentRange = getCommentRange(D);
    PeekRange = combineRanges(PeekRange, CommentRange);
    PeekRange = validateRange(Loc, PeekRange);
    NestingRange = validateRange(Loc, NestingRange);

    std::string Symbol = getMangledName(CurMangleContext, D);

    // In the case of destructors, Loc might point to the ~ character. In that
    // case we want to skip to the name of the class. However, Loc might also
    // point to other places that generate destructors, such as a lambda
    // (apparently clang 8 creates a destructor declaration for at least some
    // lambdas). In that case we'll just drop the declaration.
    if (isa<CXXDestructorDecl>(D)) {
      PrettyKind = "destructor";
      const char *P = SM.getCharacterData(Loc);
      if (*P == '~') {
        // Advance Loc to the class name
        P++;

        unsigned Skipped = 1;
        while (*P == ' ' || *P == '\t' || *P == '\r' || *P == '\n') {
          P++;
          Skipped++;
        }

        Loc = Loc.getLocWithOffset(Skipped);
      } else {
        return true;
      }
    }

    visitIdentifier(Kind, PrettyKind, getQualifiedName(D), SourceRange(Loc),
                    Symbol, qtype, getContext(D), Flags, PeekRange,
                    NestingRange);

    // In-progress structured info emission.
    if (RecordDecl *D2 = dyn_cast<RecordDecl>(D)) {
      if (D2->isThisDeclarationADefinition() &&
          // XXX getASTRecordLayout doesn't work for dependent types, so we
          // avoid calling into emitStructuredInfo for now if there's a
          // dependent type or if we're in any kind of template context.  This
          // should be re-evaluated once this is working for normal classes and
          // we can better evaluate what is useful.
          !D2->isDependentType() && !TemplateStack) {
        if (auto *D3 = dyn_cast<CXXRecordDecl>(D2)) {
          findBindingToJavaClass(*AstContext, *D3);
          findBoundAsJavaClasses(*AstContext, *D3);
        }
        emitStructuredInfo(ExpansionLoc, D2);
      }
    }
    if (EnumDecl *D2 = dyn_cast<EnumDecl>(D)) {
      if (D2->isThisDeclarationADefinition() && !D2->isDependentType() &&
          !TemplateStack) {
        emitStructuredInfo(ExpansionLoc, D2);
      }
    }
    if (EnumConstantDecl *D2 = dyn_cast<EnumConstantDecl>(D)) {
      if (!D2->isTemplated() && !TemplateStack) {
        emitStructuredInfo(ExpansionLoc, D2);
      }
    }
    if (FunctionDecl *D2 = dyn_cast<FunctionDecl>(D)) {
      if ((D2->isThisDeclarationADefinition() || isPure(D2)) &&
          // a clause at the top should have generalized and set wasTemplate so
          // it shouldn't be the case that isTemplateInstantiation() is true.
          !D2->isTemplateInstantiation() && !wasTemplate &&
          !D2->isFunctionTemplateSpecialization() && !TemplateStack) {
        if (auto *D3 = dyn_cast<CXXMethodDecl>(D2)) {
          findBindingToJavaMember(*AstContext, *D3);
        } else {
          findBindingToJavaFunction(*AstContext, *D2);
        }
        emitStructuredInfo(ExpansionLoc, D2);
      }
    }
    if (FieldDecl *D2 = dyn_cast<FieldDecl>(D)) {
      if (!D2->isTemplated() && !TemplateStack) {
        emitStructuredInfo(ExpansionLoc, D2);
      }
    }
    if (VarDecl *D2 = dyn_cast<VarDecl>(D)) {
      if (!D2->isTemplated() && !TemplateStack) {
        findBindingToJavaConstant(*AstContext, *D2);
        emitStructuredInfo(ExpansionLoc, D2);
      }
    }

    return true;
  }

  bool VisitCXXConstructExpr(const CXXConstructExpr *E) {
    // If we are in a template and find a Stmt that was registed in
    // ForwardedTemplateLocations, convert the location to an actual Stmt* in
    // ForwardingTemplates
    if (TemplateStack && !TemplateStack->inGatherMode()) {
      if (ForwardedTemplateLocations.find(E->getBeginLoc().getRawEncoding()) !=
          ForwardedTemplateLocations.end()) {
        if (const auto *currentTemplate =
                getCurrentFunctionTemplateInstantiation()) {
          ForwardingTemplates.insert({currentTemplate, E});
        }
        return true;
      }
    }

    SourceLocation Loc = E->getBeginLoc();
    if (!isInterestingLocation(Loc)) {
      return true;
    }

    return VisitCXXConstructExpr(E, Loc);
  }

  bool VisitCXXConstructExpr(const CXXConstructExpr *E, SourceLocation Loc) {
    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);

    FunctionDecl *Ctor = E->getConstructor();
    if (Ctor->isTemplateInstantiation()) {
      Ctor = Ctor->getTemplateInstantiationPattern();
    }
    std::string Mangled = getMangledName(CurMangleContext, Ctor);

    // FIXME: Need to do something different for list initialization.

    visitIdentifier("use", "constructor", getQualifiedName(Ctor), Loc, Mangled,
                    QualType(), getContext(SpellingLoc));

    return true;
  }

  CallExpr *CurrentCall = nullptr;
  bool TraverseCallExpr(CallExpr *E) {
    const auto _ = ValueRollback(CurrentCall, E);
    return Super::TraverseCallExpr(E);
  }

  bool VisitCallExpr(CallExpr *E) {
    Expr *CalleeExpr = E->getCallee()->IgnoreParenImpCasts();

    if (TemplateStack) {
      const auto CalleeLocation = [&] {
        if (const auto *Member =
                dyn_cast<CXXDependentScopeMemberExpr>(CalleeExpr)) {
          return Member->getMemberLoc();
        }
        if (const auto *DeclRef =
                dyn_cast<DependentScopeDeclRefExpr>(CalleeExpr)) {
          return DeclRef->getLocation();
        }
        if (const auto *DeclRef = dyn_cast<DeclRefExpr>(CalleeExpr)) {
          return DeclRef->getLocation();
        }

        // Does the right thing for MemberExpr and UnresolvedMemberExpr at
        // least.
        return CalleeExpr->getExprLoc();
      }();

      // If we are in a template:
      // - when in GatherDependent mode and the callee is type-dependent,
      //   register it in ForwardedTemplateLocations
      // - when in AnalyseDependent mode and the callee is in
      //   ForwardedTemplateLocations, convert the location to an actual Stmt*
      //   in ForwardingTemplates
      if (TemplateStack->inGatherMode()) {
        if (CalleeExpr->isTypeDependent()) {
          TemplateStack->visitDependent(CalleeLocation);
          ForwardedTemplateLocations.insert(CalleeLocation.getRawEncoding());
        }
      } else {
        if (ForwardedTemplateLocations.find(CalleeLocation.getRawEncoding()) !=
            ForwardedTemplateLocations.end()) {
          if (const auto *currentTemplate =
                  getCurrentFunctionTemplateInstantiation()) {
            ForwardingTemplates.insert({currentTemplate, E});
          }
        }
      }
    }

    Decl *Callee = E->getCalleeDecl();
    if (!Callee || !FunctionDecl::classof(Callee)) {
      return true;
    }

    const NamedDecl *NamedCallee = dyn_cast<NamedDecl>(Callee);

    SourceLocation Loc;

    const FunctionDecl *F = dyn_cast<FunctionDecl>(NamedCallee);
    if (F->isTemplateInstantiation()) {
      NamedCallee = F->getTemplateInstantiationPattern();
    }

    std::string Mangled = getMangledName(CurMangleContext, NamedCallee);
    int Flags = 0;

    if (CXXOperatorCallExpr::classof(E)) {
      // Just take the first token.
      CXXOperatorCallExpr *Op = dyn_cast<CXXOperatorCallExpr>(E);
      Loc = Op->getOperatorLoc();
      Flags |= NotIdentifierToken;
    } else if (MemberExpr::classof(CalleeExpr)) {
      MemberExpr *Member = dyn_cast<MemberExpr>(CalleeExpr);
      Loc = Member->getMemberLoc();
    } else if (DeclRefExpr::classof(CalleeExpr)) {
      // We handle this in VisitDeclRefExpr.
      return true;
    } else {
      return true;
    }

    if (!isInterestingLocation(Loc)) {
      return true;
    }

    if (F->isTemplateInstantiation()) {
      VisitForwardedStatements(E, Loc);
    }

    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);

    std::vector<SourceRange> argRanges;
    for (auto argExpr : E->arguments()) {
      argRanges.push_back(argExpr->getSourceRange());
    }

    visitIdentifier("use", "function", getQualifiedName(NamedCallee), Loc,
                    Mangled, E->getCallReturnType(*AstContext),
                    getContext(SpellingLoc), Flags, SourceRange(),
                    SourceRange(), &argRanges);

    return true;
  }

  bool VisitTagTypeLoc(TagTypeLoc L) {
    SourceLocation Loc = L.getBeginLoc();
    if (!isInterestingLocation(Loc)) {
      return true;
    }

    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);

    TagDecl *Decl = L.getDecl();
    std::string Mangled = getMangledName(CurMangleContext, Decl);
    visitIdentifier("use", "type", getQualifiedName(Decl), Loc, Mangled,
                    L.getType(), getContext(SpellingLoc));
    return true;
  }

  bool VisitTypedefTypeLoc(TypedefTypeLoc L) {
    SourceLocation Loc = L.getBeginLoc();
    if (!isInterestingLocation(Loc)) {
      return true;
    }

    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);

    NamedDecl *Decl = L.getTypedefNameDecl();
    std::string Mangled = getMangledName(CurMangleContext, Decl);
    visitIdentifier("use", "type", getQualifiedName(Decl), Loc, Mangled,
                    L.getType(), getContext(SpellingLoc));
    return true;
  }

  bool VisitInjectedClassNameTypeLoc(InjectedClassNameTypeLoc L) {
    SourceLocation Loc = L.getBeginLoc();
    if (!isInterestingLocation(Loc)) {
      return true;
    }

    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);

    NamedDecl *Decl = L.getDecl();
    std::string Mangled = getMangledName(CurMangleContext, Decl);
    visitIdentifier("use", "type", getQualifiedName(Decl), Loc, Mangled,
                    L.getType(), getContext(SpellingLoc));
    return true;
  }

  bool VisitTemplateSpecializationTypeLoc(TemplateSpecializationTypeLoc L) {
    SourceLocation Loc = L.getBeginLoc();
    if (!isInterestingLocation(Loc)) {
      return true;
    }

    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);

    TemplateDecl *Td = L.getTypePtr()->getTemplateName().getAsTemplateDecl();
    if (ClassTemplateDecl *D = dyn_cast<ClassTemplateDecl>(Td)) {
      NamedDecl *Decl = D->getTemplatedDecl();
      std::string Mangled = getMangledName(CurMangleContext, Decl);
      visitIdentifier("use", "type", getQualifiedName(Decl), Loc, Mangled,
                      QualType(), getContext(SpellingLoc));
    } else if (TypeAliasTemplateDecl *D = dyn_cast<TypeAliasTemplateDecl>(Td)) {
      NamedDecl *Decl = D->getTemplatedDecl();
      std::string Mangled = getMangledName(CurMangleContext, Decl);
      visitIdentifier("use", "type", getQualifiedName(Decl), Loc, Mangled,
                      QualType(), getContext(SpellingLoc));
    }

    return true;
  }

  bool VisitDependentNameTypeLoc(DependentNameTypeLoc L) {
    SourceLocation Loc = L.getNameLoc();
    if (!isInterestingLocation(Loc)) {
      return true;
    }

    for (const NamedDecl *D :
         Resolver->resolveDependentNameType(L.getTypePtr())) {
      visitHeuristicResult(Loc, D);
    }
    return true;
  }

  void VisitForwardedStatements(const Expr *E, SourceLocation Loc) {
    // If Loc itself is forwarded to its callers, do nothing
    if (ForwardedTemplateLocations.find(Loc.getRawEncoding()) !=
        ForwardedTemplateLocations.cend())
      return;

    // If this is a forwarding template (eg MakeUnique), visit the forwarded
    // statements
    auto todo = std::stack{std::vector<const Stmt *>{E}};
    auto seen = std::unordered_set<const Stmt *>{};
    while (!todo.empty()) {
      const auto forwarded = std::move(todo.top());
      todo.pop();
      if (seen.find(forwarded) != seen.end())
        continue;
      seen.insert(forwarded);

      if (const auto *C = dyn_cast<CXXConstructExpr>(forwarded))
        VisitCXXConstructExpr(C, Loc);

      const Decl *Decl = nullptr;
      if (const auto *D = dyn_cast<CallExpr>(forwarded))
        Decl = D->getCalleeDecl();
      if (const auto *D = dyn_cast<DeclRefExpr>(forwarded))
        Decl = D->getDecl();

      if (!Decl)
        continue;
      const auto *F = Decl->getAsFunction();
      if (!F)
        continue;
      if (!F->isTemplateInstantiation())
        continue;
      const auto [ForwardedBegin, ForwardedEnd] =
          ForwardingTemplates.equal_range(F);
      for (auto ForwardedIt = ForwardedBegin; ForwardedIt != ForwardedEnd;
           ++ForwardedIt)
        if (seen.find(ForwardedIt->second) == seen.end())
          todo.push(ForwardedIt->second);
    }
  }

  bool VisitDeclRefExpr(const DeclRefExpr *E) {
    SourceLocation Loc = E->getExprLoc();
    if (!isInterestingLocation(Loc)) {
      return true;
    }

    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);

    if (E->hasQualifier()) {
      Loc = E->getNameInfo().getLoc();
      SpellingLoc = SM.getSpellingLoc(Loc);
    }

    const NamedDecl *Decl = E->getDecl();
    if (const VarDecl *D2 = dyn_cast<VarDecl>(Decl)) {
      int Flags = 0;
      if (D2->isLocalVarDeclOrParm()) {
        Flags = NoCrossref;
      }
      std::string Mangled = getMangledName(CurMangleContext, Decl);
      visitIdentifier("use", "variable", getQualifiedName(Decl), Loc, Mangled,
                      D2->getType(), getContext(SpellingLoc), Flags);
    } else if (isa<FunctionDecl>(Decl)) {
      const FunctionDecl *F = dyn_cast<FunctionDecl>(Decl);
      if (F->isTemplateInstantiation()) {
        Decl = F->getTemplateInstantiationPattern();
        VisitForwardedStatements(E, Loc);
      }

      std::string Mangled = getMangledName(CurMangleContext, Decl);
      visitIdentifier("use", "function", getQualifiedName(Decl), Loc, Mangled,
                      E->getType(), getContext(SpellingLoc));
    } else if (isa<EnumConstantDecl>(Decl)) {
      std::string Mangled = getMangledName(CurMangleContext, Decl);
      visitIdentifier("use", "enum", getQualifiedName(Decl), Loc, Mangled,
                      E->getType(), getContext(SpellingLoc));
    }

    return true;
  }

  bool VisitCXXConstructorDecl(CXXConstructorDecl *D) {
    if (!isInterestingLocation(D->getLocation())) {
      return true;
    }

    for (CXXConstructorDecl::init_const_iterator It = D->init_begin();
         It != D->init_end(); ++It) {
      const CXXCtorInitializer *Ci = *It;
      if (!Ci->getMember() || !Ci->isWritten()) {
        continue;
      }

      SourceLocation Loc = Ci->getMemberLocation();
      if (!isInterestingLocation(Loc)) {
        continue;
      }

      FieldDecl *Member = Ci->getMember();
      std::string Mangled = getMangledName(CurMangleContext, Member);
      // We want the constructor to be the context of the field use and
      // `getContext(D)` would skip the current context.  An alternate approach
      // would be `getContext(Loc)` but the heuristic to omit a context if we're
      // in a macro body expansion seems incorrect for field initializations; if
      // code is using macros to initialize the fields, we still care.
      visitIdentifier("use", "field", getQualifiedName(Member), Loc, Mangled,
                      Member->getType(), translateContext(D));
    }

    return true;
  }

  bool VisitMemberExpr(MemberExpr *E) {
    SourceLocation Loc = E->getExprLoc();
    if (!isInterestingLocation(Loc)) {
      return true;
    }

    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);

    ValueDecl *Decl = E->getMemberDecl();
    if (FieldDecl *Field = dyn_cast<FieldDecl>(Decl)) {
      std::string Mangled = getMangledName(CurMangleContext, Field);
      visitIdentifier("use", "field", getQualifiedName(Field), Loc, Mangled,
                      Field->getType(), getContext(SpellingLoc));
    }
    return true;
  }

  // Helper function for producing heuristic results for usages in dependent
  // code. These are distinguished from concrete results (obtained for dependent
  // code using the AutoTemplateContext machinery) by setting the “confidence”
  // property to “cppTemplateHeuristic”. We don't expect this method to be
  // intentionally called multiple times for a given (Loc, NamedDecl) pair
  // because our callers should be mutually exclusive AST node types. However,
  // it's fine if this method is called multiple time for a given pair because
  // we explicitly de-duplicate records with an identical string representation
  // (which is a good reason to have this helper, as it ensures identical
  // representations).
  void visitHeuristicResult(SourceLocation Loc, const NamedDecl *ND) {
    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);

    if (const UsingShadowDecl *USD = dyn_cast<UsingShadowDecl>(ND)) {
      ND = USD->getTargetDecl();
    }
    if (const TemplateDecl *TD = dyn_cast<TemplateDecl>(ND)) {
      ND = TD->getTemplatedDecl();
    }
    QualType MaybeType;
    const char *SyntaxKind = nullptr;
    if (const FunctionDecl *F = dyn_cast<FunctionDecl>(ND)) {
      MaybeType = F->getType();
      SyntaxKind = "function";
    } else if (const FieldDecl *F = dyn_cast<FieldDecl>(ND)) {
      MaybeType = F->getType();
      SyntaxKind = "field";
    } else if (const EnumConstantDecl *E = dyn_cast<EnumConstantDecl>(ND)) {
      MaybeType = E->getType();
      SyntaxKind = "enum";
    } else if (const TypedefNameDecl *T = dyn_cast<TypedefNameDecl>(ND)) {
      MaybeType = T->getUnderlyingType();
      SyntaxKind = "type";
    }
    if (SyntaxKind) {
      std::string Mangled = getMangledName(CurMangleContext, ND);
      visitIdentifier("use", SyntaxKind, getQualifiedName(ND), Loc, Mangled,
                      MaybeType, getContext(SpellingLoc), Heuristic);
    }
  }

  bool arityMatchesCurrentCallExpr(const Expr *E, const NamedDecl *Candidate) {
    const auto IsCurrentCallee = CurrentCall && E == CurrentCall->getCallee();
    const auto CallNumArgs =
        IsCurrentCallee ? CurrentCall->getNumArgs() : std::optional<uint>{};

    const FunctionDecl *CandidateFunc;
    if (const auto *UsingDecl = dyn_cast<UsingShadowDecl>(Candidate)) {
      CandidateFunc = UsingDecl->getTargetDecl()->getAsFunction();
    } else {
      CandidateFunc = Candidate->getAsFunction();
    }

    // We try and filter candidates by arity, but be conservative and accept
    // them when we don't know better
    if (!CandidateFunc || !CallNumArgs) {
      return true;
    }

    const auto MinNumArgs = CandidateFunc->getMinRequiredExplicitArguments();
    const auto MaxNumArgs = [&]() -> std::optional<uint> {
      const auto IsVariadic =
          CandidateFunc->isVariadic() ||
          std::any_of(CandidateFunc->param_begin(), CandidateFunc->param_end(),
                      [](const ParmVarDecl *param) {
                        return param->isParameterPack();
                      });

      if (IsVariadic)
        return {};

      return CandidateFunc->getNumNonObjectParams();
    }();

    if (CallNumArgs < MinNumArgs || (MaxNumArgs && CallNumArgs > *MaxNumArgs)) {
      return false;
    }

    return true;
  }

  bool VisitOverloadExpr(OverloadExpr *E) {
    SourceLocation Loc = E->getExprLoc();
    normalizeLocation(&Loc);
    if (!isInterestingLocation(Loc)) {
      return true;
    }

    for (auto *Candidate : E->decls()) {
      if (arityMatchesCurrentCallExpr(E, Candidate))
        visitHeuristicResult(Loc, Candidate);
    }

    // Also record this location so that if we have instantiations, we can
    // gather more accurate results from them.
    if (TemplateStack) {
      TemplateStack->visitDependent(Loc);
    }
    return true;
  }

  bool VisitCXXDependentScopeMemberExpr(CXXDependentScopeMemberExpr *E) {
    SourceLocation Loc = E->getMemberLoc();
    normalizeLocation(&Loc);
    if (!isInterestingLocation(Loc)) {
      return true;
    }

    for (const NamedDecl *Candidate : Resolver->resolveMemberExpr(E)) {
      if (arityMatchesCurrentCallExpr(E, Candidate))
        visitHeuristicResult(Loc, Candidate);
    }

    // Also record this location so that if we have instantiations, we can
    // gather more accurate results from them.
    if (TemplateStack) {
      TemplateStack->visitDependent(Loc);
    }
    return true;
  }

  bool VisitCXXNewExpr(CXXNewExpr *N) {
    // If we are in a template and the new is type-dependent, register it in
    // ForwardedTemplateLocations to forward its uses to the surrounding
    // template call site
    if (TemplateStack && TemplateStack->inGatherMode()) {
      const auto *TypeInfo = N->getAllocatedTypeSourceInfo();
      const auto ConstructExprLoc = TypeInfo->getTypeLoc().getBeginLoc();
      if (N->isTypeDependent()) {
        TemplateStack->visitDependent(ConstructExprLoc);
        ForwardedTemplateLocations.insert(ConstructExprLoc.getRawEncoding());
      }
    }
    return true;
  }

  bool VisitDependentScopeDeclRefExpr(DependentScopeDeclRefExpr *E) {
    SourceLocation Loc = E->getLocation();
    normalizeLocation(&Loc);
    if (!isInterestingLocation(Loc)) {
      return true;
    }

    for (const NamedDecl *Candidate : Resolver->resolveDeclRefExpr(E)) {
      if (arityMatchesCurrentCallExpr(E, Candidate))
        visitHeuristicResult(Loc, Candidate);
    }

    // Also record this location so that if we have instantiations, we can
    // gather more accurate results from them.
    if (TemplateStack) {
      TemplateStack->visitDependent(Loc);

      // Also record the dependent NestedNameSpecifier locations
      for (auto NestedNameLoc = E->getQualifierLoc();
           NestedNameLoc &&
           NestedNameLoc.getNestedNameSpecifier()->isDependent();
           NestedNameLoc = NestedNameLoc.getPrefix()) {
        TemplateStack->visitDependent(NestedNameLoc.getLocalBeginLoc());
      }
    }

    return true;
  }

  bool VisitStringLiteral(StringLiteral *E) {
    if (E->getCharByteWidth() != 1) {
      return true;
    }

    StringRef sref = E->getString();
    std::string s = sref.str();

    bool isMozSrc = stringStartsWith(s, "moz-src:///");

    if (!stringStartsWith(s, "chrome://") &&
        !stringStartsWith(s, "resource://") &&
        !isMozSrc) {
      return true;
    }

    if (!isASCII(s)) {
      return true;
    }

    SourceLocation Loc = E->getStrTokenLoc(0);
    normalizeLocation(&Loc);

    std::string symbol;

    if (isMozSrc) {
      symbol = std::string("FILE_") + mangleFile(s.substr(11), FileType::Source);
    } else {
      symbol = std::string("URL_") + mangleURL(s);
    }

    visitIdentifier("use", "file", StringRef(s), Loc, symbol, QualType(),
                    Context(), NotIdentifierToken | LocRangeEndValid);

    return true;
  }

  void enterSourceFile(SourceLocation Loc) {
    normalizeLocation(&Loc);
    FileInfo *newFile = getFileInfo(Loc);
    if (!newFile->Interesting) {
      return;
    }
    FileType type = newFile->Generated ? FileType::Generated : FileType::Source;
    std::string symbol =
        std::string("FILE_") + mangleFile(newFile->Realname, type);

    // We use an explicit zero-length source range at the start of the file. If
    // we don't set the LocRangeEndValid flag, the visitIdentifier code will use
    // the entire first token, which could be e.g. a long multiline-comment.
    visitIdentifier("def", "file", newFile->Realname, SourceRange(Loc), symbol,
                    QualType(), Context(),
                    NotIdentifierToken | LocRangeEndValid);
  }

  void inclusionDirective(SourceRange FileNameRange, const FileEntry *File) {
    std::string includedFile(File->tryGetRealPathName());
    FileType type = relativizePath(includedFile);
    if (type == FileType::Unknown) {
      return;
    }
    std::string symbol = std::string("FILE_") + mangleFile(includedFile, type);

    // Support the #include MACRO use-case
    // When parsing #include MACRO:
    // - the filename is never passed to onTokenLexed
    // - inclusionDirective is called before endMacroExpansion (which is only
    // called when the following token is parsed) So add the filename here and
    // call endMacroExpansion immediately. This ensures the macro has a correct
    // expansion and it has been added to MacroMaps so the referenced filename
    // knows to populate inExpansionAt.
    if (MacroExpansionState) {
      MacroExpansionState->TokenLocations[FileNameRange.getBegin()] =
          MacroExpansionState->Expansion.length();
      MacroExpansionState->Expansion += '"';
      MacroExpansionState->Expansion += includedFile;
      MacroExpansionState->Expansion += '"';
      endMacroExpansion();
    }

    visitIdentifier("use", "file", includedFile, FileNameRange, symbol,
                    QualType(), Context(),
                    NotIdentifierToken | LocRangeEndValid);
  }

  void macroDefined(const Token &Tok, const MacroDirective *Macro) {
    if (Macro->getMacroInfo()->isBuiltinMacro()) {
      return;
    }
    SourceLocation Loc = Tok.getLocation();
    normalizeLocation(&Loc);
    if (!isInterestingLocation(Loc)) {
      return;
    }

    IdentifierInfo *Ident = Tok.getIdentifierInfo();
    if (Ident) {
      std::string Mangled = std::string("M_") +
                            mangleLocation(Loc, std::string(Ident->getName()));
      visitIdentifier("def", "macro", Ident->getName(), Loc, Mangled);
    }
  }

  void macroUsed(const Token &Tok, const MacroInfo *Macro) {
    if (!Macro) {
      return;
    }
    if (Macro->isBuiltinMacro()) {
      return;
    }
    SourceLocation Loc = Tok.getLocation();
    if (!isInterestingLocation(Loc)) {
      return;
    }

    IdentifierInfo *Ident = Tok.getIdentifierInfo();
    if (Ident) {
      std::string Mangled =
          std::string("M_") + mangleLocation(Macro->getDefinitionLoc(),
                                             std::string(Ident->getName()));
      visitIdentifier("use", "macro", Ident->getName(), Loc, Mangled);
    }
  }

  void beginMacroExpansion(const Token &Tok, const MacroInfo *Macro,
                           SourceRange Range) {
    if (!Macro)
      return;

    if (Macro->isBuiltinMacro())
      return;

    if (!Tok.getIdentifierInfo())
      return;

    auto location = Tok.getLocation();
    normalizeLocation(&location);
    if (!isInterestingLocation(location))
      return;

    if (MacroExpansionState) {
      const auto InMacroArgs = MacroExpansionState->Range.fullyContains(
          SM.getExpansionRange(Range).getAsRange());
      const auto InMacroBody =
          SM.getExpansionLoc(Tok.getLocation()) ==
          SM.getExpansionLoc(MacroExpansionState->MacroNameToken.getLocation());
      if (InMacroArgs || InMacroBody) {
        if (MacroExpansionState->MacroInfo->getDefinitionLoc() !=
            Macro->getDefinitionLoc()) {
          IdentifierInfo *DependencyIdent = Tok.getIdentifierInfo();
          std::string DependencySymbol =
              std::string("M_") +
              mangleLocation(Macro->getDefinitionLoc(),
                             std::string(DependencyIdent->getName()));

          MacroExpansionState->Dependencies.push_back(DependencySymbol);
        }

        macroUsed(Tok, Macro);
        return;
      }

      endMacroExpansion();
    }

    MacroExpansionState = ::MacroExpansionState{
        .MacroNameToken = Tok,
        .MacroInfo = Macro,
        .Expansion = {},
        .TokenLocations = {},
        .Range = Range,
        .PrevPrevTok = {},
        .PrevTok = {},
    };
  }

  void endMacroExpansion() {
    // large macros are too slow to reformat, don't reformat macros larger than
    // those arbitrary thresholds
    static constexpr auto includedFileExpansionReformatThreshold = 20'000;
    static constexpr auto mainFileExpansionReformatThreshold = 200'000;

    const auto expansionLocation =
        SM.getExpansionLoc(MacroExpansionState->MacroNameToken.getLocation());
    const auto expansionFilename = SM.getFilename(expansionLocation);
    const auto includedExtensions =
        std::array{".h", ".hpp", ".hxx", ".inc", ".def"};
    const auto isIncludedFile =
        std::any_of(includedExtensions.begin(), includedExtensions.end(),
                    [&](const auto *extension) {
                      return expansionFilename.ends_with_insensitive(extension);
                    });
    const auto expansionReformatThreshold =
        isIncludedFile ? includedFileExpansionReformatThreshold
                       : mainFileExpansionReformatThreshold;

    if (MacroExpansionState->Expansion.length() < expansionReformatThreshold) {
      // large macros are too memory-hungry to reformat with ColumnLimit != 0
      // see https://github.com/llvm/llvm-project/issues/107434
      auto style = clang::format::getMozillaStyle();
      if (MacroExpansionState->Expansion.length() >
          includedFileExpansionReformatThreshold)
        style.ColumnLimit = 0;

      const auto replacements = clang::format::reformat(
          style, MacroExpansionState->Expansion,
          {tooling::Range(0, MacroExpansionState->Expansion.length())});
      auto formatted = clang::tooling::applyAllReplacements(
          MacroExpansionState->Expansion, replacements);
      if (formatted) {
        for (auto &[k, v] : MacroExpansionState->TokenLocations) {
          v = replacements.getShiftedCodePosition(v);
        }
        MacroExpansionState->Expansion = std::move(formatted.get());
      }
    }

    IdentifierInfo *Ident =
        MacroExpansionState->MacroNameToken.getIdentifierInfo();
    std::string Symbol =
        std::string("M_") +
        mangleLocation(MacroExpansionState->MacroInfo->getDefinitionLoc(),
                       std::string(Ident->getName()));

    const auto dependenciesBegin = MacroExpansionState->Dependencies.begin();
    const auto dependenciesEnd = MacroExpansionState->Dependencies.end();
    std::sort(dependenciesBegin, dependenciesEnd);
    MacroExpansionState->Dependencies.erase(
        std::unique(dependenciesBegin, dependenciesEnd), dependenciesEnd);

    auto Key = Symbol;
    for (const auto &Dependency : MacroExpansionState->Dependencies) {
      Key.push_back(',');
      Key += Dependency;
    }

    MacroMaps.emplace(std::pair{
        MacroExpansionState->MacroNameToken.getLocation(),
        ExpandedMacro{
            std::move(Symbol),
            std::move(Key),
            std::move(MacroExpansionState->Expansion),
            std::move(MacroExpansionState->TokenLocations),
        },
    });

    MacroExpansionState.reset();

    macroUsed(MacroExpansionState->MacroNameToken,
              MacroExpansionState->MacroInfo);
  }

  void onTokenLexed(const Token &Tok) {
    if (!MacroExpansionState)
      return;

    // check if we exited the macro expansion
    SourceLocation SLoc = Tok.getLocation();
    if (!SLoc.isMacroID()) {
      endMacroExpansion();
      return;
    }

    if (ConcatInfo.AvoidConcat(MacroExpansionState->PrevPrevTok,
                               MacroExpansionState->PrevTok, Tok)) {
      MacroExpansionState->Expansion += ' ';
    }

    if (Tok.isAnnotation()) {
      const auto Range = SM.getImmediateExpansionRange(Tok.getLocation());
      const char *Start = SM.getCharacterData(Range.getBegin());
      const char *End = SM.getCharacterData(Range.getEnd()) + 1;
      MacroExpansionState->Expansion += StringRef(Start, End - Start);
    } else {
      const auto spelling = CI.getPreprocessor().getSpelling(Tok);
      if (Tok.isAnyIdentifier()) {
        MacroExpansionState->TokenLocations[SLoc] =
            MacroExpansionState->Expansion.length();
      }
      MacroExpansionState->Expansion += spelling;
    }

    MacroExpansionState->PrevPrevTok = MacroExpansionState->PrevTok;
    MacroExpansionState->PrevTok = Tok;
  }
};

void PreprocessorHook::FileChanged(SourceLocation Loc, FileChangeReason Reason,
                                   SrcMgr::CharacteristicKind FileType,
                                   FileID PrevFID = FileID()) {
  switch (Reason) {
  case PPCallbacks::RenameFile:
  case PPCallbacks::SystemHeaderPragma:
    // Don't care about these, since we want the actual on-disk filenames
    break;
  case PPCallbacks::EnterFile:
    Indexer->enterSourceFile(Loc);
    break;
  case PPCallbacks::ExitFile:
    // Don't care about exiting files
    break;
  }
}

void PreprocessorHook::InclusionDirective(
    SourceLocation HashLoc, const Token &IncludeTok, StringRef FileName,
    bool IsAngled, CharSourceRange FileNameRange,
#if CLANG_VERSION_MAJOR >= 16
    OptionalFileEntryRef File,
#elif CLANG_VERSION_MAJOR >= 15
    Optional<FileEntryRef> File,
#else
    const FileEntry *File,
#endif
    StringRef SearchPath, StringRef RelativePath,
#if CLANG_VERSION_MAJOR >= 19
    const Module *SuggestedModule, bool ModuleImported,
#else
    const Module *Imported,
#endif
    SrcMgr::CharacteristicKind FileType) {
#if CLANG_VERSION_MAJOR >= 15
  if (!File) {
    return;
  }
  Indexer->inclusionDirective(FileNameRange.getAsRange(),
                              &File->getFileEntry());
#else
  Indexer->inclusionDirective(FileNameRange.getAsRange(), File);
#endif
}

void PreprocessorHook::MacroDefined(const Token &Tok,
                                    const MacroDirective *Md) {
  Indexer->macroDefined(Tok, Md);
}

void PreprocessorHook::MacroExpands(const Token &Tok, const MacroDefinition &Md,
                                    SourceRange Range, const MacroArgs *Ma) {
  Indexer->beginMacroExpansion(Tok, Md.getMacroInfo(), Range);
}

void PreprocessorHook::MacroUndefined(const Token &Tok,
                                      const MacroDefinition &Md,
                                      const MacroDirective *Undef) {
  Indexer->macroUsed(Tok, Md.getMacroInfo());
}

void PreprocessorHook::Defined(const Token &Tok, const MacroDefinition &Md,
                               SourceRange Range) {
  Indexer->macroUsed(Tok, Md.getMacroInfo());
}

void PreprocessorHook::Ifdef(SourceLocation Loc, const Token &Tok,
                             const MacroDefinition &Md) {
  Indexer->macroUsed(Tok, Md.getMacroInfo());
}

void PreprocessorHook::Ifndef(SourceLocation Loc, const Token &Tok,
                              const MacroDefinition &Md) {
  Indexer->macroUsed(Tok, Md.getMacroInfo());
}

class IndexAction : public PluginASTAction {
protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 llvm::StringRef F) {
    return make_unique<IndexConsumer>(CI);
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string> &Args) {
    if (Args.size() != 3) {
      DiagnosticsEngine &D = CI.getDiagnostics();
      unsigned DiagID = D.getCustomDiagID(
          DiagnosticsEngine::Error,
          "Need arguments for the source, output, and object directories");
      D.Report(DiagID);
      return false;
    }

    // Load our directories
    Srcdir = getAbsolutePath(Args[0]);
    if (Srcdir.empty()) {
      DiagnosticsEngine &D = CI.getDiagnostics();
      unsigned DiagID = D.getCustomDiagID(
          DiagnosticsEngine::Error, "Source directory '%0' does not exist");
      D.Report(DiagID) << Args[0];
      return false;
    }

    ensurePath(Args[1] + PATHSEP_STRING);
    Outdir = getAbsolutePath(Args[1]);
    Outdir += PATHSEP_STRING;

    Objdir = getAbsolutePath(Args[2]);
    if (Objdir.empty()) {
      DiagnosticsEngine &D = CI.getDiagnostics();
      unsigned DiagID = D.getCustomDiagID(DiagnosticsEngine::Error,
                                          "Objdir '%0' does not exist");
      D.Report(DiagID) << Args[2];
      return false;
    }
    Objdir += PATHSEP_STRING;

    printf("MOZSEARCH: %s %s %s\n", Srcdir.c_str(), Outdir.c_str(),
           Objdir.c_str());

    return true;
  }

  void printHelp(llvm::raw_ostream &Ros) {
    Ros << "Help for mozsearch plugin goes here\n";
  }
};

static FrontendPluginRegistry::Add<IndexAction>
    Y("mozsearch-index", "create the mozsearch index database");
