//===--- UnnecessaryCopyOnLastUseCheck.h - clang-tidy -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_PERFORMANCE_UNNECESSARYCOPYONLASTUSECHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_PERFORMANCE_UNNECESSARYCOPYONLASTUSECHECK_H

#include "../ClangTidyCheck.h"
#include "../utils/IncludeInserter.h"
#include "clang/Analysis/CFG.h"
#include "llvm/ADT/DenseMap.h"

namespace clang::tidy::performance {

/// FIXME: Write a short description.
///
/// For the user-facing documentation see:
/// http://clang.llvm.org/extra/clang-tidy/checks/performance/unnecessary-copy-on-last-use.html
class UnnecessaryCopyOnLastUseCheck : public ClangTidyCheck {
public:
  UnnecessaryCopyOnLastUseCheck(StringRef Name, ClangTidyContext *Context);
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
  bool isLanguageVersionSupported(const LangOptions &LangOpts) const override {
    return LangOpts.CPlusPlus11;
  }
  void registerPPCallbacks(const SourceManager &SM, Preprocessor *PP,
                           Preprocessor *ModuleExpanderPP) override;

private:
  CFG *getOrCreateCFG(const FunctionDecl *FD, ASTContext *C);

  utils::IncludeInserter Inserter;
  const std::vector<StringRef> BlockedTypes;
  const std::vector<StringRef> BlockedFunctions;

  llvm::DenseMap<const FunctionDecl *, std::unique_ptr<CFG>> CFGs;
};

} // namespace clang::tidy::performance

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_PERFORMANCE_UNNECESSARYCOPYONLASTUSECHECK_H
