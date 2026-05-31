#ifndef MALLOC_CHECKER_H
#define MALLOC_CHECKER_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Lex/Lexer.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace malloc_checker {

struct WarningRecord {
  unsigned Line = 0;
  unsigned Col = 0;
  std::string Function;
  std::string Allocator;
  std::string Message;
};

struct TrackedAllocation {
  const clang::VarDecl *Var = nullptr;
  const clang::CallExpr *AllocCall = nullptr;
  std::string AllocName;
  bool Escaped = false;
  bool Checked = false;
  bool Suppressed = false;
};

class MallocCheckerConsumer : public clang::ASTConsumer {
public:
  MallocCheckerConsumer(clang::CompilerInstance &CI,
                        const std::set<std::string> &Allocators, bool Stats,
                        const std::string &ReportPath);

  void HandleTranslationUnit(clang::ASTContext &Ctx) override;

private:
  clang::CompilerInstance &CI_;
  std::set<std::string> Allocators_;
  bool Stats_;
  std::string ReportPath_;
  unsigned TotalAllocCalls_ = 0;
  unsigned TotalWarnings_ = 0;
  std::vector<WarningRecord> Warnings_;
};

class MallocCheckerAction : public clang::PluginASTAction {
public:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &CI,
                    llvm::StringRef) override;

  bool ParseArgs(const clang::CompilerInstance &CI,
                 const std::vector<std::string> &Args) override;

private:
  std::set<std::string> Allocators_{
      "malloc", "calloc", "realloc", "aligned_alloc", "_aligned_malloc"};
  bool Stats_ = false;
  std::string ReportPath_;
};

} // namespace malloc_checker

#endif // MALLOC_CHECKER_H
