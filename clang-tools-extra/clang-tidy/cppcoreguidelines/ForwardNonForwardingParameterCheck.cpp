//===--- ForwardNonForwardingParameterCheck.cpp - clang-tidy --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ForwardNonForwardingParameterCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang::tidy::cppcoreguidelines {

namespace {
  
AST_MATCHER_P(QualType, possiblyPackExpansionOf,
              ast_matchers::internal::Matcher<QualType>, InnerMatcher) {
  return InnerMatcher.matches(Node.getNonPackExpansionType(), Finder, Builder);
}

AST_MATCHER(ParmVarDecl, isForwardingRefTypeOfFunction) {
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

AST_MATCHER(ParmVarDecl, isInInstantiation) {
  const auto *Function = dyn_cast<FunctionDecl>(Node.getDeclContext());
  if (!Function)
    return false;
  return Function->isTemplateInstantiation();
}

} // namespace

void ForwardNonForwardingParameterCheck::registerMatchers(MatchFinder *Finder) {
  auto VarMatcher = varDecl(
      unless(parmVarDecl(isForwardingRefTypeOfFunction())),
      unless(parmVarDecl(isInInstantiation()))).bind("var");

  Finder->addMatcher(
      callExpr(anyOf(callee(functionDecl(hasName("::std::forward"))),
                     callee(unresolvedLookupExpr(hasAnyDeclaration(namedDecl(
                         hasUnderlyingDecl(hasName("::std::forward"))))))),
               argumentCountIs(1),
               hasArgument(0, declRefExpr(to(VarMatcher)).bind("ref"))).bind("call"),
      this);
}

void ForwardNonForwardingParameterCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
  const auto *Ref = Result.Nodes.getNodeAs<DeclRefExpr>("ref");
  const auto *Var = Result.Nodes.getNodeAs<VarDecl>("var");

  if (!Call || !Var)
    return;


  diag(Ref->getLocation(),
       "calling std::forward on non-forwarding reference %0") << Var;
}

} // namespace clang::tidy::cppcoreguidelines
