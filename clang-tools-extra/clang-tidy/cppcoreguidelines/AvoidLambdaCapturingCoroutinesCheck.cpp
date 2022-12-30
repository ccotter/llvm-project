//===--- AvoidLambdaCapturingCoroutinesCheck.cpp - clang-tidy -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AvoidLambdaCapturingCoroutinesCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace cppcoreguidelines {

void AvoidLambdaCapturingCoroutinesCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(lambdaExpr().bind("lambda"), this);
}

bool hasAnyExplicitCaptures(const LambdaExpr* Lambda) {
  return Lambda->capture_begin() != Lambda->capture_end();
}

void AvoidLambdaCapturingCoroutinesCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Lambda = Result.Nodes.getNodeAs<LambdaExpr>("lambda");

  if (llvm::dyn_cast<CoroutineBodyStmt>(Lambda->getBody()) == nullptr) {
    return;
  }

  if (hasAnyExplicitCaptures(Lambda)) {
    diag(Lambda->getBeginLoc(), "Coroutine lambdas should not capture");
  }
}

} // namespace cppcoreguidelines
} // namespace tidy
} // namespace clang
