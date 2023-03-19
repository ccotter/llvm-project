//===--- AvoidReferenceCoroutineParametersCheck.cpp - clang-tidy ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AvoidReferenceCoroutineParametersCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang::tidy::cppcoreguidelines {

void AvoidReferenceCoroutineParametersCheck::registerMatchers(
    MatchFinder *Finder) {

  auto CoroReferenceParam = parmVarDecl(hasType(type(referenceType())),
      hasAncestor(functionDecl(hasBody(coroutineBodyStmt()))));
  auto CoroDefWithRefParams = functionDecl(hasAnyParameter(parmVarDecl(hasType(type(referenceType())))), hasBody(coroutineBodyStmt()));

  Finder->addMatcher(CoroReferenceParam.bind("param-in-decl"), this);
  Finder->addMatcher(callExpr(
        callee(CoroDefWithRefParams),
        optionally(hasParent(coawaitExpr().bind("await"))))
      .bind("coro-call"), this);
}

void AvoidReferenceCoroutineParametersCheck::check(
    const MatchFinder::MatchResult &Result) {

  const auto *Param = Result.Nodes.getNodeAs<ParmVarDecl>("param-in-decl");
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("coro-call");

  llvm::errs() << "Match\n";
  if (Param) Param->dump(); else llvm::errs() << "nullptr\n";
  if (Call) Call->dump(); else llvm::errs() << "nullptr\n";

  if (!IgnoreCoawaitExprs && Param)
    diag(Param->getBeginLoc(), "coroutine parameters should not be references");

  if (IgnoreCoawaitExprs && Call) {
    const auto* Await = Result.Nodes.getNodeAs<CoawaitExpr>("await");
    if (!Await)
      diag(Call->getBeginLoc(), "call to coroutine that accepts reference parameters not immediately co_await-ed");
  }
}

AvoidReferenceCoroutineParametersCheck::AvoidReferenceCoroutineParametersCheck(
    StringRef Name, ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      IgnoreCoawaitExprs(Options.get("IgnoreCoawaitExprs", false)) {}

void AvoidReferenceCoroutineParametersCheck::storeOptions(
    ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "IgnoreCoawaitExprs", IgnoreCoawaitExprs);
}

} // namespace clang::tidy::cppcoreguidelines
