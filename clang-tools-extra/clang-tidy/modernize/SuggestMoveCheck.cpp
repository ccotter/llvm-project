//===--- SuggestMoveCheck.cpp - clang-tidy --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SuggestMoveCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace modernize {

void SuggestMoveCheck::registerMatchers(MatchFinder *Finder) {
  // FIXME: Add matchers.

    auto src = declRefExpr(
                    hasDeclaration(anyOf(
                        varDecl(hasAncestor(compoundStmt().bind("scope"))).bind("srcDecl"),
                        parmVarDecl(
                            hasAncestor(functionDecl(hasAnyBody(compoundStmt().bind("scope")))))
                            .bind("srcDecl"))))
                    .bind("src");

    auto copyCtors = cxxConstructExpr(
                          isExpansionInMainFile(),
                          hasArgument(0, src),
                          hasDeclaration(
                              decl(cxxConstructorDecl(isCopyConstructor()).bind("copyCtor"))))
                          .bind("root");
    auto copyAssigns = cxxOperatorCallExpr(
                            isExpansionInMainFile(),
                            hasArgument(1, src),
                            isAssignmentOperator(),
                            hasDeclaration(
                                cxxMethodDecl(isCopyAssignmentOperator()).bind("copyAssign")))
                            .bind("root");

    auto allRefs = declRefExpr(isExpansionInMainFile(), hasDeclaration(varDecl().bind("decl")))
                        .bind("allRefs");
    auto allStmts = stmt(isExpansionInMainFile()).bind("allStmts");
    auto bitwiseMoveable = classTemplateSpecializationDecl(
                                hasName("BloombergLP::bslmf::IsBitwiseMoveable"),
                                hasTemplateArgument(
                                    0, refersToType(qualType().bind("bitwiseMoveable"))))
                                .bind("classSpecialization");
  Finder->addMatcher(copyCtors, this);
  Finder->addMatcher(copyAssigns, this);
  Finder->addMatcher(allRefs, this);
  Finder->addMatcher(allStmts, this);
  Finder->addMatcher(bitwiseMoveable, this);
}

void SuggestMoveCheck::check(const MatchFinder::MatchResult &Result) {
  // FIXME: Add callback implementation.
  const auto *MatchedDecl = Result.Nodes.getNodeAs<FunctionDecl>("x");
  if (!MatchedDecl->getIdentifier() || MatchedDecl->getName().startswith("awesome_"))
    return;
  diag(MatchedDecl->getLocation(), "function %0 is insufficiently awesome")
      << MatchedDecl;
  diag(MatchedDecl->getLocation(), "insert 'awesome'", DiagnosticIDs::Note)
      << FixItHint::CreateInsertion(MatchedDecl->getLocation(), "awesome_");
}

} // namespace modernize
} // namespace tidy
} // namespace clang
