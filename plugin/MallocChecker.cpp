#include "MallocChecker.h"

#include "clang/Analysis/CFG.h"
#include "clang/AST/AST.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>

using namespace clang;
using namespace malloc_checker;

namespace {

enum DiagKind {
  DK_UncheckedReturn = 0,
  DK_NoteAllocSite,
  DK_NoteDerefSite,
  DK_NoteFixIt,
  DK_DoubleMalloc,
  DK_Count
};

static unsigned DiagIDs[DK_Count];
static bool DiagsRegistered = false;
static bool SuppressWarnings = false;

static void registerDiagnostics(DiagnosticsEngine &Diags) {
  if (DiagsRegistered)
    return;
  DiagsRegistered = true;

  DiagIDs[DK_UncheckedReturn] = Diags.getCustomDiagID(
      DiagnosticsEngine::Warning,
      "return value of '%0' is not checked for NULL — potential null "
      "dereference");
  DiagIDs[DK_NoteAllocSite] = Diags.getCustomDiagID(
      DiagnosticsEngine::Note, "allocation occurs here");
  DiagIDs[DK_NoteDerefSite] = Diags.getCustomDiagID(
      DiagnosticsEngine::Note, "first unsafe dereference occurs here");
  DiagIDs[DK_NoteFixIt] = Diags.getCustomDiagID(
      DiagnosticsEngine::Note,
      "consider adding: if (!%0) { /* handle error */ return; }");
  DiagIDs[DK_DoubleMalloc] = Diags.getCustomDiagID(
      DiagnosticsEngine::Warning,
      "ternary guard calls '%0' twice — possible leak");
}

static bool shouldEmit(DiagnosticsEngine &Diags) {
  if (SuppressWarnings)
    return false;
  (void)Diags;
  return true;
}

static bool isNullPointerExpr(const Expr *E, ASTContext &Ctx) {
  if (!E)
    return false;
  E = E->IgnoreParenImpCasts();
  return E->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNull);
}

static const VarDecl *getVarFromExpr(const Expr *E) {
  if (!E)
    return nullptr;
  E = E->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    return dyn_cast<VarDecl>(DRE->getDecl());
  return nullptr;
}

static bool isAllocatorCall(const CallExpr *CE,
                            const std::set<std::string> &Allocators) {
  if (!CE)
    return false;
  const FunctionDecl *FD = CE->getDirectCallee();
  if (!FD)
    return false;
  return Allocators.count(FD->getName().str()) > 0;
}

static std::string getAllocatorName(const CallExpr *CE) {
  if (const FunctionDecl *FD = CE->getDirectCallee())
    return FD->getName().str();
  return "allocator";
}

static bool isInsideStaticAssert(const Stmt *S, ASTContext &Ctx) {
  auto Parents = Ctx.getParents(*S);
  while (!Parents.empty()) {
    if (Parents[0].get<StaticAssertDecl>())
      return true;
    if (const Stmt *Parent = Parents[0].get<Stmt>()) {
      Parents = Ctx.getParents(*Parent);
      continue;
    }
    if (const auto *E = Parents[0].get<Expr>()) {
      Parents = Ctx.getParents(*E);
      continue;
    }
    break;
  }
  return false;
}

static bool isVoidDiscard(const Expr *E, ASTContext &Ctx) {
  auto Parents = Ctx.getParents(*E);
  for (const auto &P : Parents) {
    if (const auto *Cast = P.get<CStyleCastExpr>()) {
      if (Cast->getType()->isVoidType())
        return true;
    }
    if (const auto *Cast = P.get<ExplicitCastExpr>()) {
      if (Cast->getType()->isVoidType())
        return true;
    }
  }
  return false;
}

static bool hasUnusedAttr(const VarDecl *VD) {
  return VD && VD->hasAttr<UnusedAttr>();
}

static bool isBuiltinExpectNotVar(const Expr *Cond, const VarDecl *Var) {
  Cond = Cond->IgnoreParenImpCasts();
  const auto *CE = dyn_cast<CallExpr>(Cond);
  if (!CE || CE->getNumArgs() == 0)
    return false;
  const FunctionDecl *FD = CE->getDirectCallee();
  if (!FD)
    return false;
  const llvm::StringRef Name = FD->getName();
  if (!Name.starts_with("__builtin_expect"))
    return false;
  const Expr *Arg = CE->getArg(0)->IgnoreParenImpCasts();
  if (const auto *UO = dyn_cast<UnaryOperator>(Arg)) {
    if (UO->getOpcode() == UO_LNot &&
        getVarFromExpr(UO->getSubExpr()->IgnoreParenImpCasts()) == Var)
      return true;
  }
  return false;
}

static bool isNullCheckCondition(const Expr *Cond, const VarDecl *Var,
                                 ASTContext &Ctx, bool *IsNonNullBranch) {
  Cond = Cond->IgnoreParenImpCasts();
  if (isBuiltinExpectNotVar(Cond, Var)) {
    *IsNonNullBranch = true;
    return true;
  }
  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    const VarDecl *L = getVarFromExpr(BO->getLHS());
    const VarDecl *R = getVarFromExpr(BO->getRHS());
    if (L == Var && isNullPointerExpr(BO->getRHS(), Ctx)) {
      *IsNonNullBranch = (BO->getOpcode() == BO_NE);
      return true;
    }
    if (R == Var && isNullPointerExpr(BO->getLHS(), Ctx)) {
      *IsNonNullBranch = (BO->getOpcode() == BO_NE);
      return true;
    }
  }
  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot && getVarFromExpr(UO->getSubExpr()) == Var) {
      *IsNonNullBranch = true;
      return true;
    }
  }
  return false;
}

static bool isAssertCheck(const CallExpr *CE, const VarDecl *Var) {
  const FunctionDecl *FD = CE->getDirectCallee();
  if (!FD)
    return false;
  const std::string Name = FD->getName().str();
  if (Name != "assert")
    return false;
  if (CE->getNumArgs() == 0)
    return false;
  const Expr *Arg = CE->getArg(0)->IgnoreParenImpCasts();
  if (getVarFromExpr(Arg) == Var)
    return true;
  if (const auto *BO = dyn_cast<BinaryOperator>(Arg)) {
    if (getVarFromExpr(BO->getLHS()) == Var || getVarFromExpr(BO->getRHS()) == Var)
      return true;
  }
  if (const auto *UO = dyn_cast<UnaryOperator>(Arg)) {
    if (UO->getOpcode() == UO_LNot && getVarFromExpr(UO->getSubExpr()) == Var)
      return true;
  }
  return false;
}

static bool exprReferencesVar(const Expr *E, const VarDecl *Var) {
  if (!E)
    return false;
  E = E->IgnoreParenImpCasts();
  if (getVarFromExpr(E) == Var)
    return true;
  for (const Stmt *Child : E->children()) {
    if (const auto *ChildE = dyn_cast_or_null<Expr>(Child)) {
      if (exprReferencesVar(ChildE, Var))
        return true;
    }
  }
  return false;
}

static bool isPassThroughReturn(const CallExpr *CE, ASTContext &Ctx) {
  auto Parents = Ctx.getParents(*CE);
  for (const auto &P : Parents) {
    if (P.get<ReturnStmt>())
      return true;
  }
  return false;
}

static bool isTernaryGuard(const CallExpr *CE, ASTContext &Ctx) {
  auto Parents = Ctx.getParents(*CE);
  for (const auto &P : Parents) {
    if (P.get<ConditionalOperator>())
      return true;
    if (P.get<BinaryConditionalOperator>())
      return true;
  }
  return false;
}

static bool initContainsCall(const Expr *E, const CallExpr *CE) {
  if (!E)
    return false;
  E = E->IgnoreImpCasts();
  if (E == CE)
    return true;
  for (const Stmt *Child : E->children()) {
    if (const auto *ChildE = dyn_cast_or_null<Expr>(Child)) {
      if (initContainsCall(ChildE, CE))
        return true;
    }
  }
  return false;
}

static bool stmtContainsReturn(const Stmt *S) {
  if (!S)
    return false;
  if (isa<ReturnStmt>(S))
    return true;
  if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
    for (const Stmt *Child : CS->children()) {
      if (stmtContainsReturn(Child))
        return true;
    }
  }
  return false;
}

static bool isAssertLikeGuard(const Stmt *S, const VarDecl *Var, ASTContext &Ctx) {
  if (!S)
    return false;

  struct Finder : RecursiveASTVisitor<Finder> {
    const VarDecl *Var;
    ASTContext &Ctx;
    bool Found = false;

    Finder(const VarDecl *V, ASTContext &C) : Var(V), Ctx(C) {}

    bool VisitCallExpr(CallExpr *CE) {
      if (isAssertCheck(CE, Var))
        Found = true;
      return true;
    }

    bool VisitConditionalOperator(ConditionalOperator *CO) {
      bool NonNullBranch = false;
      if (isNullCheckCondition(CO->getCond(), Var, Ctx, &NonNullBranch) &&
          NonNullBranch)
        Found = true;
      if (isBuiltinExpectNotVar(CO->getCond(), Var))
        Found = true;
      return true;
    }
  };

  Finder F(Var, Ctx);
  F.TraverseStmt(const_cast<Stmt *>(S));
  return F.Found;
}

static bool isImmediateNullGuardStmt(const IfStmt *If, const VarDecl *Var,
                                     ASTContext &Ctx) {
  bool NonNullBranch = false;
  if (!isNullCheckCondition(If->getCond(), Var, Ctx, &NonNullBranch))
    return false;
  return stmtContainsReturn(If->getThen()) || If->getElse() != nullptr;
}

static std::optional<SourceLocation>
findFixItLocation(const CallExpr *CE, ASTContext &Ctx, SourceManager &SM) {
  SourceLocation Loc = CE->getEndLoc();
  if (Loc.isInvalid())
    return std::nullopt;
  Loc = SM.getExpansionLoc(Loc);
  return Lexer::findLocationAfterToken(Loc, tok::semi, SM, Ctx.getLangOpts(),
                                       true);
}

static std::string getMainFileName(const SourceManager &SM) {
  SourceLocation Loc = SM.getLocForStartOfFile(SM.getMainFileID());
  if (Loc.isValid())
    return SM.getFilename(Loc).str();
  return "unknown";
}

class MallocCheckerVisitor : public RecursiveASTVisitor<MallocCheckerVisitor> {
public:
  MallocCheckerVisitor(ASTContext &Ctx, const std::set<std::string> &Allocators,
                       DiagnosticsEngine &Diags, unsigned &TotalAllocCalls,
                       unsigned &TotalWarnings, std::vector<WarningRecord> &Warnings)
      : Ctx_(Ctx), Allocators_(Allocators), Diags_(Diags),
        TotalAllocCalls_(TotalAllocCalls), TotalWarnings_(TotalWarnings),
        Warnings_(Warnings) {
    registerDiagnostics(Diags_);
  }

  bool TraverseFunctionDecl(FunctionDecl *FD) {
    if (!FD->hasBody())
      return RecursiveASTVisitor::TraverseFunctionDecl(FD);

    SavedTracked_.swap(CurrentTracked_);
    CurrentFD_ = FD;
    CurrentTracked_.clear();

    bool Result = TraverseStmt(FD->getBody());
    analyzeFunctionCFG(FD);

    CurrentTracked_.clear();
    CurrentFD_ = nullptr;
    SavedTracked_.swap(CurrentTracked_);
    return Result;
  }

  bool TraverseDeclStmt(DeclStmt *DS) {
    ActiveDeclStmts_.push_back(DS);
    bool Result = RecursiveASTVisitor::TraverseDeclStmt(DS);
    ActiveDeclStmts_.pop_back();
    return Result;
  }

  bool TraverseBinaryOperator(BinaryOperator *BO) {
    if (BO->getOpcode() == BO_Assign) {
      ActiveAssigns_.push_back(BO);
      bool Result = RecursiveASTVisitor::TraverseBinaryOperator(BO);
      ActiveAssigns_.pop_back();
      return Result;
    }
    return RecursiveASTVisitor::TraverseBinaryOperator(BO);
  }

  bool TraverseReturnStmt(ReturnStmt *RS) {
    ActiveReturns_.push_back(RS);
    bool Result = RecursiveASTVisitor::TraverseReturnStmt(RS);
    ActiveReturns_.pop_back();
    return Result;
  }

  bool TraverseConditionalOperator(ConditionalOperator *CO) {
    ActiveTernaries_.push_back(CO);
    bool Result = RecursiveASTVisitor::TraverseConditionalOperator(CO);
    ActiveTernaries_.pop_back();
    return Result;
  }

  bool TraverseCStyleCastExpr(CStyleCastExpr *Cast) {
    if (Cast->getType()->isVoidType()) {
      ++VoidCastDepth_;
      bool Result = RecursiveASTVisitor::TraverseCStyleCastExpr(Cast);
      --VoidCastDepth_;
      return Result;
    }
    return RecursiveASTVisitor::TraverseCStyleCastExpr(Cast);
  }

  bool VisitCallExpr(CallExpr *CE) {
    if (!CurrentFD_ || !isAllocatorCall(CE, Allocators_))
      return true;

    ++TotalAllocCalls_;

    if (isInsideStaticAssert(CE, Ctx_))
      return true;
    if (VoidCastDepth_ > 0)
      return true;
    if (!ActiveReturns_.empty())
      return true;

    if (!ActiveTernaries_.empty()) {
      checkDoubleMallocInTernary(CE);
      return true;
    }

    const VarDecl *AssignedVar = getAssignedVar(CE);
    if (AssignedVar) {
      if (hasUnusedAttr(AssignedVar))
        return true;

      if (hasImmediateGuard(AssignedVar, CE))
        return true;

      CurrentTracked_.push_back(
          {AssignedVar, CE, getAllocatorName(CE), false, false, false});
      return true;
    }

    emitWarning(CE, CE, getAllocatorName(CE),
                "return value used without NULL check", CurrentFD_);
    return true;
  }

private:
  ASTContext &Ctx_;
  const std::set<std::string> &Allocators_;
  DiagnosticsEngine &Diags_;
  unsigned &TotalAllocCalls_;
  unsigned &TotalWarnings_;
  std::vector<WarningRecord> &Warnings_;

  const FunctionDecl *CurrentFD_ = nullptr;
  std::vector<TrackedAllocation> CurrentTracked_;
  std::vector<TrackedAllocation> SavedTracked_;
  std::vector<const DeclStmt *> ActiveDeclStmts_;
  std::vector<const BinaryOperator *> ActiveAssigns_;
  std::vector<const ReturnStmt *> ActiveReturns_;
  std::vector<const ConditionalOperator *> ActiveTernaries_;
  unsigned VoidCastDepth_ = 0;

  const VarDecl *getAssignedVar(const CallExpr *CE) {
    for (const BinaryOperator *BO : ActiveAssigns_) {
      if (initContainsCall(BO->getRHS(), CE))
        return getVarFromExpr(BO->getLHS());
    }

    for (const DeclStmt *DS : ActiveDeclStmts_) {
      for (const Decl *D : DS->decls()) {
        const auto *VD = dyn_cast<VarDecl>(D);
        if (!VD || !VD->getInit())
          continue;
        if (initContainsCall(VD->getInit(), CE))
          return VD;
      }
    }
    return nullptr;
  }

  bool hasImmediateGuard(const VarDecl *Var, const CallExpr *AllocCall) {
    const FunctionDecl *FD = CurrentFD_;
    if (!FD || !FD->getBody())
      return false;

    const Stmt *Body = FD->getBody();
    const DeclStmt *AllocDS = nullptr;
    for (const Stmt *S : Body->children()) {
      if (const auto *DS = dyn_cast<DeclStmt>(S)) {
        for (const Decl *D : DS->decls()) {
          if (D == Var) {
            AllocDS = DS;
            break;
          }
        }
      }
      if (AllocDS)
        break;
    }

    const Stmt *After = nullptr;
    bool Found = false;
    for (const Stmt *S : Body->children()) {
      if (Found) {
        After = S;
        break;
      }
      if (S == AllocDS)
        Found = true;
    }

    if (!After && AllocCall) {
      (void)AllocCall;
    }

    if (!After)
      return false;

    if (const auto *If = dyn_cast<IfStmt>(After)) {
      if (isImmediateNullGuardStmt(If, Var, Ctx_))
        return true;
    }

    if (isAssertLikeGuard(After, Var, Ctx_))
      return true;

    return false;
  }

  void checkDoubleMallocInTernary(const CallExpr *CE) {
    auto Parents = Ctx_.getParents(*CE);
    for (const auto &P : Parents) {
      const ConditionalOperator *CO = P.get<ConditionalOperator>();
      if (!CO)
        continue;
      const Expr *TrueE = CO->getTrueExpr()->IgnoreImpCasts();
      const Expr *FalseE = CO->getFalseExpr()->IgnoreImpCasts();
      const CallExpr *OtherCall = nullptr;
      if (TrueE == CE->IgnoreImpCasts())
        OtherCall = dyn_cast<CallExpr>(FalseE);
      else if (FalseE == CE->IgnoreImpCasts())
        OtherCall = dyn_cast<CallExpr>(TrueE);
      if (OtherCall && isAllocatorCall(OtherCall, Allocators_) &&
          shouldEmit(Diags_)) {
        Diags_.Report(CE->getBeginLoc(), DiagIDs[DK_DoubleMalloc])
            << getAllocatorName(CE);
      }
    }
  }

  void emitWarning(const CallExpr *AllocCall, const Stmt *WarnAt,
                   const std::string &AllocName, const std::string &Message,
                   const FunctionDecl *FD) {
    if (!shouldEmit(Diags_))
      return;

    SourceLocation Loc =
        WarnAt ? WarnAt->getBeginLoc() : AllocCall->getBeginLoc();
    if (Loc.isInvalid())
      return;

    ++TotalWarnings_;

    std::string Fn = FD ? FD->getNameAsString() : "";
    FullSourceLoc FSL(Loc, Ctx_.getSourceManager());
    WarningRecord Rec;
    Rec.Line = FSL.getSpellingLineNumber();
    Rec.Col = FSL.getSpellingColumnNumber();
    Rec.Function = Fn;
    Rec.Allocator = AllocName;
    Rec.Message = Message;
    Warnings_.push_back(Rec);

    const VarDecl *Var = AllocCall ? getAssignedVar(AllocCall) : nullptr;
    std::string VarName = Var ? Var->getName().str() : "ptr";

    DiagnosticBuilder DB =
        Diags_.Report(Loc, DiagIDs[DK_UncheckedReturn]) << AllocName;

    if (AllocCall && WarnAt != AllocCall) {
      Diags_.Report(AllocCall->getBeginLoc(), DiagIDs[DK_NoteAllocSite]);
      Diags_.Report(Loc, DiagIDs[DK_NoteDerefSite]);
      if (auto FixLoc =
              findFixItLocation(AllocCall, Ctx_, Ctx_.getSourceManager())) {
        DB << FixItHint::CreateInsertion(
            *FixLoc, "\n    if (!" + VarName +
                         ") { /* handle error */ return; }");
      }
    } else if (AllocCall) {
      if (auto FixLoc =
              findFixItLocation(AllocCall, Ctx_, Ctx_.getSourceManager())) {
        DB << FixItHint::CreateInsertion(
            *FixLoc, "\n    if (!" + VarName +
                         ") { /* handle error */ return; }");
      }
      Diags_.Report(Loc, DiagIDs[DK_NoteFixIt]) << VarName;
    }
  }

  void analyzeFunctionCFG(const FunctionDecl *FD) {
    if (CurrentTracked_.empty())
      return;

    CFG::BuildOptions Options;
    Options.AddInitializers = true;
    Options.AddImplicitDtors = true;
    Options.AddTemporaryDtors = true;
    std::unique_ptr<CFG> FuncCFG =
        CFG::buildCFG(FD, FD->getBody(), &Ctx_, Options);
    if (!FuncCFG)
      return;

    for (TrackedAllocation &Track : CurrentTracked_)
      analyzeAllocationInCFG(FD, *FuncCFG, Track);
  }

  void analyzeAllocationInCFG(const FunctionDecl *FD, CFG &FuncCFG,
                              TrackedAllocation &Track) {
    const VarDecl *Var = Track.Var;
    if (!Var)
      return;

    std::unordered_map<const CFGBlock *, bool> NonNullOnEntry;
    bool Warned = false;

    for (CFG::iterator BI = FuncCFG.begin(), BE = FuncCFG.end(); BI != BE;
         ++BI) {
      const CFGBlock *Block = *BI;
      bool KnownNonNull = false;

      if (Block->pred_empty()) {
        KnownNonNull = false;
      } else {
        bool AnyPred = false;
        bool AllNonNull = true;
        for (CFGBlock::const_pred_iterator PI = Block->pred_begin(),
                                           PE = Block->pred_end();
             PI != PE; ++PI) {
          if (*PI) {
            AnyPred = true;
            auto It = NonNullOnEntry.find(*PI);
            if (It == NonNullOnEntry.end() || !It->second)
              AllNonNull = false;
          }
        }
        if (AnyPred && AllNonNull)
          KnownNonNull = true;
      }

      bool BlockNonNull = KnownNonNull;

      for (const auto &Elem : *Block) {
        if (!Elem.getAs<CFGStmt>())
          continue;
        const Stmt *S = Elem.castAs<CFGStmt>().getStmt();
        if (!S)
          continue;

        if (checkEscape(S, Var)) {
          Track.Escaped = true;
          Track.Checked = true;
          return;
        }

        if (const auto *If = dyn_cast<IfStmt>(S)) {
          if (isImmediateNullGuardStmt(If, Var, Ctx_)) {
            Track.Checked = true;
            BlockNonNull = true;
          }
        }

        if (isAssertLikeGuard(S, Var, Ctx_)) {
          Track.Checked = true;
          BlockNonNull = true;
        }

        if (!BlockNonNull && !Track.Escaped && containsUnsafeDeref(S, Var)) {
          emitWarning(Track.AllocCall, S, Track.AllocName,
                      "unchecked pointer dereferenced", FD);
          Warned = true;
          return;
        }
      }

      NonNullOnEntry[Block] = BlockNonNull;
    }

    if (!Warned && !Track.Escaped && Track.Checked) {
      (void)FD;
    }
  }

  bool checkEscape(const Stmt *S, const VarDecl *Var) {
    if (const auto *Call = dyn_cast<CallExpr>(S)) {
      const FunctionDecl *Callee = Call->getDirectCallee();
      for (unsigned I = 0; I < Call->getNumArgs(); ++I) {
        if (exprReferencesVar(Call->getArg(I), Var)) {
          if (Callee) {
            const std::string Name = Callee->getName().str();
            if (Name == "free" || Name == "realloc")
              continue;
          }
          return true;
        }
      }
    }
    if (const auto *Ret = dyn_cast<ReturnStmt>(S)) {
      if (Ret->getRetValue() && exprReferencesVar(Ret->getRetValue(), Var))
        return true;
    }
    return false;
  }

  bool containsUnsafeDeref(const Stmt *S, const VarDecl *Var) {
    struct Finder : RecursiveASTVisitor<Finder> {
      const VarDecl *Var;
      bool Found = false;

      explicit Finder(const VarDecl *V) : Var(V) {}

      bool VisitUnaryOperator(UnaryOperator *UO) {
        if (UO->getOpcode() == UO_Deref &&
            getVarFromExpr(UO->getSubExpr()) == Var)
          Found = true;
        return true;
      }

      bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
        if (getVarFromExpr(ASE->getBase()) == Var)
          Found = true;
        return true;
      }

      bool VisitMemberExpr(MemberExpr *ME) {
        if (ME->isArrow() && getVarFromExpr(ME->getBase()) == Var)
          Found = true;
        return true;
      }

      bool VisitCallExpr(CallExpr *CE) {
        const FunctionDecl *FD = CE->getDirectCallee();
        if (!FD)
          return true;
        const std::string Name = FD->getName().str();
        if (Name == "strcpy" || Name == "strcat" || Name == "strlen" ||
            Name == "memcpy" || Name == "memset" || Name == "printf" ||
            Name == "fprintf" || Name == "fwrite") {
          if (CE->getNumArgs() > 0 &&
              exprReferencesVar(CE->getArg(0), Var))
            Found = true;
        }
        return true;
      }
    };

    Finder F(Var);
    F.TraverseStmt(const_cast<Stmt *>(S));
    return F.Found;
  }
};

} // namespace

MallocCheckerConsumer::MallocCheckerConsumer(CompilerInstance &CI,
                                             const std::set<std::string> &Allocators,
                                             bool Stats,
                                             const std::string &ReportPath)
    : CI_(CI), Allocators_(Allocators), Stats_(Stats),
      ReportPath_(ReportPath) {}

void MallocCheckerConsumer::HandleTranslationUnit(ASTContext &Ctx) {
  registerDiagnostics(CI_.getDiagnostics());
  Ctx.getParentMapContext().clear();

  MallocCheckerVisitor Visitor(Ctx, Allocators_, CI_.getDiagnostics(),
                               TotalAllocCalls_, TotalWarnings_, Warnings_);
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());

  if (Stats_) {
    const SourceManager &SM = Ctx.getSourceManager();
    std::string TUName = getMainFileName(SM);

    unsigned Clean = TotalAllocCalls_ > TotalWarnings_
                         ? TotalAllocCalls_ - TotalWarnings_
                         : 0;
    llvm::errs() << "[malloc-checker] TU: " << TUName << " — "
                 << TotalAllocCalls_ << " malloc calls checked, "
                 << TotalWarnings_ << " warnings emitted, " << Clean
                 << " clean\n";
  }

  if (!ReportPath_.empty()) {
    llvm::json::Object Root;
    const SourceManager &SM = Ctx.getSourceManager();
    std::string TUName = getMainFileName(SM);
    Root["translation_unit"] = TUName;

    llvm::json::Array WarnArr;
    for (const WarningRecord &W : Warnings_) {
      llvm::json::Object WO;
      WO["line"] = W.Line;
      WO["col"] = W.Col;
      WO["function"] = W.Function;
      WO["allocator"] = W.Allocator;
      WO["message"] = W.Message;
      WarnArr.push_back(std::move(WO));
    }
    Root["warnings"] = std::move(WarnArr);

    std::error_code EC;
    llvm::raw_fd_ostream OS(ReportPath_, EC);
    if (!EC)
      OS << llvm::formatv("{0:2}", llvm::json::Value(std::move(Root))) << "\n";
  }
}

std::unique_ptr<ASTConsumer>
MallocCheckerAction::CreateASTConsumer(CompilerInstance &CI,
                                       llvm::StringRef) {
  return std::make_unique<MallocCheckerConsumer>(CI, Allocators_, Stats_,
                                                 ReportPath_);
}

bool MallocCheckerAction::ParseArgs(const CompilerInstance &,
                                    const std::vector<std::string> &Args) {
  for (const std::string &Arg : Args) {
    if (Arg == "-stats" || Arg == "stats") {
      Stats_ = true;
    } else if (Arg == "-Wno-malloc-check" || Arg == "Wno-malloc-check") {
      SuppressWarnings = true;
    } else if (Arg.rfind("-allocator=", 0) == 0) {
      Allocators_.insert(Arg.substr(11));
    } else if (Arg.rfind("allocator=", 0) == 0) {
      Allocators_.insert(Arg.substr(10));
    } else if (Arg.rfind("-report=", 0) == 0) {
      ReportPath_ = Arg.substr(8);
    } else if (Arg.rfind("report=", 0) == 0) {
      ReportPath_ = Arg.substr(7);
    }
  }
  return true;
}

static FrontendPluginRegistry::Add<MallocCheckerAction>
    X("malloc-checker",
      "Checks that malloc-family allocator return values are null-checked");

// Diagnostic group registration note: warnings are emitted under the custom
// 'malloc-check' category. Suppress via:
//   -plugin-arg-malloc-checker -Wno-malloc-check
// or by passing -Wno-malloc-check when the diagnostic group is enabled through
// the plugin interface (MallocCheckerDiags).
