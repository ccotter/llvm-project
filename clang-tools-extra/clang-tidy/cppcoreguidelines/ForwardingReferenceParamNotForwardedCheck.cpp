//===--- ForwardingReferenceParamNotForwardedCheck.cpp - clang-tidy -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ForwardingReferenceParamNotForwardedCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang::tidy::cppcoreguidelines {

namespace {

AST_MATCHER_P(QualType, possiblyPackExpansionOf,
              ast_matchers::internal::Matcher<QualType>, InnerMatcher) {
  return InnerMatcher.matches(Node.getNonPackExpansionType(), Finder, Builder);
}

AST_MATCHER(ParmVarDecl, isTemplateTypeOfFunction) {
  ast_matchers::internal::Matcher<QualType> Inner = possiblyPackExpansionOf(
      qualType(rValueReferenceType(),
               references(templateTypeParmType(
                   hasDeclaration(templateTypeParmDecl()))),
               unless(references(qualType(isConstQualified())))));
  if (!Inner.matches(Node.getType(), Finder, Builder))
    return false;

  const auto *Function = dyn_cast<FunctionDecl>(Node.getDeclContext());
  if (!Function)
    return false;

  const FunctionTemplateDecl *FuncTemplate =
      Function->getDescribedFunctionTemplate();
  if (!FuncTemplate)
    return false;

  QualType ParamType =
      Node.getType().getNonPackExpansionType()->getPointeeType();
  const auto *TemplateType = ParamType->getAs<TemplateTypeParmType>();
  if (!TemplateType)
    return false;

  return TemplateType->getDepth() ==
         FuncTemplate->getTemplateParameters()->getDepth();
}

} // namespace

void ForwardingReferenceParamNotForwardedCheck::registerMatchers(
    MatchFinder *Finder) {
  auto ToParam = hasAnyParameter(parmVarDecl(equalsBoundNode("param")));

  StatementMatcher ForwardCallMatcher =
      callExpr(
          callee(unresolvedLookupExpr(hasAnyDeclaration(
              namedDecl(hasUnderlyingDecl(hasName("::std::forward")))))),
          argumentCountIs(1),
          hasArgument(0, declRefExpr(to(equalsBoundNode("param"))).bind("ref")))
          .bind("forward-call");

  Finder->addMatcher(
      parmVarDecl(
          parmVarDecl().bind("param"), isTemplateTypeOfFunction(),
          anyOf(hasAncestor(cxxConstructorDecl(
                    ToParam, isDefinition(),
                    optionally(hasDescendant(ForwardCallMatcher)))),
                hasAncestor(functionDecl(
                    unless(cxxConstructorDecl()), ToParam,
                    hasBody(optionally(hasDescendant(ForwardCallMatcher))))))),
      this);
}

void ForwardingReferenceParamNotForwardedCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *Param = Result.Nodes.getNodeAs<ParmVarDecl>("param");
  const auto *ForwardCall = Result.Nodes.getNodeAs<CallExpr>("forward-call");

  if (!Param)
    return;

  if (!ForwardCall) {
    diag(Param->getLocation(),
         "forwarding reference parameter %0 is never forwarded "
         "inside the function body")
        << Param;
  }
}

} // namespace clang::tidy::cppcoreguidelines
