//===--- MismatchedIteratorCompareCheck.cpp - clang-tidy ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MismatchedIteratorCompareCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace bugprone {

void MismatchedIteratorCompareCheck::registerMatchers(MatchFinder *Finder) {
  auto makeSourceMatcher = [](std::string_view name) {
    auto IteratorCallee = callee(cxxMethodDecl(hasAnyName("find", "begin", "end")));
    auto StlContainerType = hasType(hasCanonicalType(hasDeclaration(cxxRecordDecl(hasAnyName(
                "::std::set",
                "::std::map",
                "::std::multiset",
                "::std::multimap",
                "::std::unordered_set",
                "::std::unordered_map",
                "::std::unordered_multiset",
                "::std::unordered_multimap",
                "::std::unordered_set",
                "::std::vector",
                "::std::list",
                "::std::stack",
                "::std::deque",
                "::std::queue",
                "::std::array",
                "::std::forward_list",

                "::bsl::set",
                "::bsl::map",
                "::bsl::multiset",
                "::bsl::multimap",
                "::bsl::unordered_set",
                "::bsl::unordered_map",
                "::bsl::unordered_multiset",
                "::bsl::unordered_multimap",
                "::bsl::unordered_set",
                "::bsl::vector",
                "::bsl::list",
                "::bsl::stack",
                "::bsl::deque",
                "::bsl::queue",
                "::bsl::array",
                "::bsl::forward_list"
                )))));
    auto OnExpr = anyOf(
        on(declRefExpr(hasDeclaration(varDecl(StlContainerType).bind(name)))),
        callee(memberExpr(hasObjectExpression(memberExpr(hasDeclaration(fieldDecl(StlContainerType).bind(name))))))
      );
    return anyOf(
        declRefExpr(hasDeclaration(varDecl(hasInitializer(
            anyOf(
              cxxMemberCallExpr(IteratorCallee,  on(declRefExpr(hasDeclaration(varDecl(StlContainerType).bind(name))))),
              cxxMemberCallExpr(IteratorCallee, callee(memberExpr(hasObjectExpression(memberExpr(hasDeclaration(fieldDecl(StlContainerType).bind(name)))))))
          ))))),
        cxxMemberCallExpr(IteratorCallee, on(declRefExpr(hasDeclaration(varDecl(StlContainerType).bind(name))))),
        cxxMemberCallExpr(IteratorCallee, callee(memberExpr(hasObjectExpression(memberExpr(hasDeclaration(fieldDecl(StlContainerType).bind(name)))))))
        );
  };
  Finder->addMatcher(binaryOperation(
        hasAnyOperatorName("==", "!="),
        hasLHS(makeSourceMatcher("stl-container-A")),
        hasRHS(makeSourceMatcher("stl-container-B"))).bind("op"), this);
}

void MismatchedIteratorCompareCheck::check(const MatchFinder::MatchResult &Result) {
  const auto* Op = Result.Nodes.getNodeAs<Expr>("op");
  const auto* ContainerA = Result.Nodes.getNodeAs<Decl>("stl-container-A");
  const auto* ContainerB = Result.Nodes.getNodeAs<Decl>("stl-container-B");
  if (ContainerA != ContainerB)
    diag(Op->getExprLoc(), "comparing iterators from different containers");
}

} // namespace bugprone
} // namespace tidy
} // namespace clang
