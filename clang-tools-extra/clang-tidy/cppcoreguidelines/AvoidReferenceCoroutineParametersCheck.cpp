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
#include "clang/Analysis/CFG.h"

#include "../utils/ExprSequence.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace cppcoreguidelines {

void AvoidReferenceCoroutineParametersCheck::registerMatchers(
    MatchFinder *Finder) {
  auto IsCoroMatcher =
      hasDescendant(expr(anyOf(coyieldExpr(), coreturnStmt(), coawaitExpr())));

  // There is no coroutineBodyStmt matcher, so below approximates this by
  // finding the CoroutineBodyStmt's body (which will only be a CompoundStmt or
  // CXXTryStmt), then matching DeclRefExprs contained in that statement.
  // CoroutineBodyStmt's other children will be other Stmt types.
  Finder->addMatcher(
      declRefExpr(
          to(parmVarDecl(hasType(type(referenceType()))).bind("param")),
          hasAncestor(
              functionDecl(
                  IsCoroMatcher,
                  hasAnyParameter(parmVarDecl(equalsBoundNode("param"))),
                  hasBody(stmt(has(stmt(anyOf(compoundStmt(), cxxTryStmt()))
                                       .bind("coroutineBody")))))
                  .bind("coroutine")),
          hasAncestor(stmt(equalsBoundNode("coroutineBody"))))
          .bind("usage"),
      this);
}

static bool accessedAfterSuspendPoint(const CoroutineBodyStmt *Coroutine,
                                      const DeclRefExpr *Usage,
                                      const ParmVarDecl *Param,
                                      ASTContext *Context) {
  Stmt *FunctionBody = Coroutine->getBody();
  CFG::BuildOptions Options;
  Options.AddImplicitDtors = true;
  Options.AddTemporaryDtors = true;
  std::unique_ptr<CFG> TheCFG =
      CFG::buildCFG(nullptr, FunctionBody, Context, Options);
  if (!TheCFG) {
    return false;
  }

  auto Sequence = std::make_unique<utils::ExprSequence>(TheCFG.get(),
                                                        FunctionBody, Context);

  for (const auto &Match :
       match(traverse(TK_AsIs, findAll(coawaitExpr().bind("coawait"))),
             *FunctionBody, *Context)) {
    const auto *Coawait = Match.getNodeAs<CoawaitExpr>("coawait");
    if (Sequence->potentiallyAfter(Usage, Coawait)) {
      return true;
    }
  }

  return false;
}

void AvoidReferenceCoroutineParametersCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *Coroutine = llvm::dyn_cast<CoroutineBodyStmt>(
      Result.Nodes.getNodeAs<FunctionDecl>("coroutine")->getBody());
  const auto *Usage = Result.Nodes.getNodeAs<DeclRefExpr>("usage");
  const auto *Param = Result.Nodes.getNodeAs<ParmVarDecl>("param");

  if (accessedAfterSuspendPoint(Coroutine, Usage, Param, Result.Context)) {
    diag(Usage->getBeginLoc(),
         "coroutine reference parameter %0 accessed after suspend point")
        << Param;
  }
}

} // namespace cppcoreguidelines
} // namespace tidy
} // namespace clang
