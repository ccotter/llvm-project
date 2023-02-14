//===--- AvoidUnknownCallsWithMutexHeldCheck.cpp - clang-tidy -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AvoidUnknownCallsWithMutexHeldCheck.h"
#include "../utils/ExprSequence.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Analysis/CFG.h"

using namespace clang::ast_matchers;

namespace clang::tidy::cppcoreguidelines {

void AvoidUnknownCallsWithMutexHeldCheck::registerMatchers(
    MatchFinder *Finder) {
  auto Lock = declStmt(
      has(varDecl(hasInitializer(
                      cxxConstructExpr(
                          hasType(recordDecl(hasAnyName(
                              "::std::unique_lock", "::std::lock_guard",
                              "::std::scoped_lock", "::std::shared_lock"))),
                          unless(argumentCountIs(0)))
                          .bind("lock")))
              .bind("lock-decl")));
  auto UnlockStmt = cxxMemberCallExpr(
      on(declRefExpr(to(decl(equalsBoundNode("lock-decl"))))),
      callee(cxxMethodDecl(hasAnyName("unlock", "mutex", "swap"))));
  Finder->addMatcher(
      cxxMemberCallExpr(
          callee(cxxMethodDecl(isVirtual())),
          hasAncestor(compoundStmt(has(Lock), unless(hasDescendant(UnlockStmt)))
                          .bind("block")))
          .bind("vcall"),
      this);
}

void AvoidUnknownCallsWithMutexHeldCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *Block = Result.Nodes.getNodeAs<CompoundStmt>("block");
  const auto *VCall = Result.Nodes.getNodeAs<Expr>("vcall");
  const auto *Lock = Result.Nodes.getNodeAs<Expr>("lock");

  if (!Block || !VCall || !Lock)
    return;

  ASTContext &Context = *Result.Context;
  CFG::BuildOptions Options;
  Options.AddImplicitDtors = true;
  Options.AddTemporaryDtors = true;

  std::unique_ptr<CFG> TheCFG = CFG::buildCFG(
      nullptr, const_cast<clang::CompoundStmt *>(Block), &Context, Options);
  if (!TheCFG)
    return;

  utils::ExprSequence Sequence(TheCFG.get(), Block, &Context);

  const Stmt *LastBlockStmt = Block->body_back();

  if (Sequence.inSequence(Lock, VCall) &&
      (VCall == LastBlockStmt || Sequence.inSequence(VCall, LastBlockStmt))) {
    // diag(MatchedDecl->getLocation(), "function %0 is insufficiently awesome")
    //     << MatchedDecl;
    diag(VCall->getBeginLoc(), "virtual call while mutex held");
  }
}

} // namespace clang::tidy::cppcoreguidelines
