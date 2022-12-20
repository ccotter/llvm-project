//===--- SuggestMoveCheck.h - clang-tidy ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MODERNIZE_SUGGESTMOVECHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MODERNIZE_SUGGESTMOVECHECK_H

#include "../ClangTidyCheck.h"

#include "clang/AST/ASTContext.h"
#include "clang/Frontend/CompilerInstance.h"

#include "../utils/ExprSequence.h"

#include <unordered_map>
#include <unordered_set>

namespace clang {
namespace tidy {
namespace modernize {

/// FIXME: Write a short description.
///
/// For the user-facing documentation see:
/// http://clang.llvm.org/extra/clang-tidy/checks/modernize/suggest-move.html
class SuggestMoveCheck : public ClangTidyCheck {
public:
  SuggestMoveCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;

  struct Scope
  {
    const clang::CompoundStmt* stmt;
    std::unique_ptr<clang::CFG> cfg;
    std::unique_ptr<utils::ExprSequence> sequence;
    std::unique_ptr<utils::StmtToBlockMap> blockMap;
  };

  struct CopiedFromData
  {
    bool isConstruct = false;
    const clang::VarDecl* decl = nullptr;
  };

  std::unordered_map<const clang::DeclRefExpr*, CopiedFromData> d_variablesCopiedFrom;
  std::unordered_multimap<const clang::VarDecl*, const clang::DeclRefExpr*> d_varUsages;
  std::unordered_map<const clang::VarDecl*, Scope> d_scopes;
  std::unordered_set<const clang::VarDecl*> d_escaped;
  std::unordered_set<const clang::Type*> d_bitwiseMoveable;

  const decltype(d_variablesCopiedFrom)& variablesCopiedFrom() const
  {
      return d_variablesCopiedFrom;
  }
  const decltype(d_varUsages)& varUsages() const
  {
      return d_varUsages;
  }
  const decltype(d_scopes)& scopes() const
  {
      return d_scopes;
  }
  const decltype(d_escaped)& escaped() const
  {
      return d_escaped;
  }
  const decltype(d_bitwiseMoveable)& bitwiseMoveable() const
  {
      return d_bitwiseMoveable;
  }

  // Evaluates whether T has a const member value equal to true.
  // E.g., std::true_type. Maybe there is a better way built into
  // libTooling to directly evaluate the equivalent of
  // `if constexpr (T::value)`?
  static bool isTrueType(const clang::CXXRecordDecl* recordDecl)
  {
      for (const auto* decl : recordDecl->decls())
      {
          if (const auto* varDecl = llvm::dyn_cast<clang::VarDecl>(decl))
          {
              if (varDecl->getNameAsString() == "value")
              {
                  clang::APValue* evaluated = varDecl->evaluateValue();
                  if (evaluated->hasValue() && evaluated->isInt())
                  {
                      return evaluated->getInt().getBoolValue();
                  }
                  else
                  {
                      return false;
                  }
              }
          }
      }

      for (auto base : recordDecl->bases())
      {
          if (const auto* baseRecordDecl = base.getType()->getAsCXXRecordDecl())
          {
              if (isTrueType(baseRecordDecl))
              {
                  return true;
              }
          }
      }
      return false;
  }

  void onEndOfTranslationUnit() override;

  clang::ASTContext* d_ctx;
  clang::SourceManager* d_sm;
};

} // namespace modernize
} // namespace tidy
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MODERNIZE_SUGGESTMOVECHECK_H
