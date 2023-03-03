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
#include "clang/Lex/Lexer.h"

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
  auto RvalueRefDecl =
      varDecl(hasType(type(rValueReferenceType())),
              unless(hasType(references(qualType(isConstQualified())))));
  auto VarMatcher =
      varDecl(unless(parmVarDecl(isForwardingRefTypeOfFunction())),
              unless(parmVarDecl(isInInstantiation())),
              optionally(RvalueRefDecl.bind("rvalue-var")))
          .bind("var");

  Finder->addMatcher(
      callExpr(
          anyOf(callee(functionDecl(hasName("::std::forward"))),
                callee(unresolvedLookupExpr(hasAnyDeclaration(
                    namedDecl(hasUnderlyingDecl(hasName("::std::forward"))))))),
          argumentCountIs(1),
          hasArgument(0, expr(anyOf(declRefExpr(to(VarMatcher)),
                                    expr(unless(declRefExpr(to(parmVarDecl(
                                        anyOf(isForwardingRefTypeOfFunction(),
                                              isInInstantiation()))))))))
                             .bind("fwd-arg")))
          .bind("call"),
      this);
}

void ForwardNonForwardingParameterCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
  const auto *FwdArg = Result.Nodes.getNodeAs<Expr>("fwd-arg");
  const auto *Var = Result.Nodes.getNodeAs<VarDecl>("var");
  const auto *RvalueRefVar = Result.Nodes.getNodeAs<VarDecl>("rvalue-var");

  if (!Call || !FwdArg)
    return;

  if (RvalueRefVar) {
    ASTContext &Context = *Result.Context;

    std::string MoveText = "std::move(";
    MoveText +=
        Lexer::getSourceText(CharSourceRange::getTokenRange(
                                 FwdArg->getBeginLoc(), FwdArg->getEndLoc()),
                             Context.getSourceManager(), Context.getLangOpts());
    MoveText += ")";

    diag(FwdArg->getBeginLoc(),
         "calling std::forward on rvalue reference %0; use std::move instead")
        << RvalueRefVar
        << FixItHint::CreateReplacement(
               CharSourceRange::getTokenRange(Call->getBeginLoc(),
                                              Call->getEndLoc()),
               MoveText);
  } else {
    if (Var) {
      diag(FwdArg->getBeginLoc(),
           "calling std::forward on non-forwarding reference %0")
          << Var;
    } else {
      diag(FwdArg->getBeginLoc(),
           "calling std::forward on non-forwarding reference");
    }
  }
}

} // namespace clang::tidy::cppcoreguidelines
