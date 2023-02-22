//===--- RvalueReferenceParamNotMovedCheck.cpp - clang-tidy ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RvalueReferenceParamNotMovedCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang::tidy::cppcoreguidelines {

namespace {
AST_MATCHER_P(LambdaExpr, valueCapturesVar, DeclarationMatcher, VarMatcher) {
  return std::find_if(Node.capture_begin(), Node.capture_end(),
                      [&](const LambdaCapture &Capture) {
                        return Capture.capturesVariable() &&
                               VarMatcher.matches(*Capture.getCapturedVar(),
                                                  Finder, Builder) &&
                               Capture.getCaptureKind() == LCK_ByCopy;
                      }) != Node.capture_end();
}
AST_MATCHER_P2(Stmt, argumentOf, bool, StrictMode, StatementMatcher, Ref) {
  if (StrictMode) {
    return Ref.matches(Node, Finder, Builder);
  } else {
    return stmt(anyOf(Ref, hasDescendant(Ref))).matches(Node, Finder, Builder);
  }
}
} // namespace

void RvalueReferenceParamNotMovedCheck::registerMatchers(MatchFinder *Finder) {
  auto ToParam = hasAnyParameter(parmVarDecl(equalsBoundNode("param")));

  StatementMatcher MoveCallMatcher =
      callExpr(
          anyOf(callee(functionDecl(hasName("::std::move"))),
                callee(unresolvedLookupExpr(hasAnyDeclaration(
                    namedDecl(hasUnderlyingDecl(hasName("::std::move"))))))),
          argumentCountIs(1),
          hasArgument(
              0, argumentOf(
                     StrictMode,
                     declRefExpr(to(equalsBoundNode("param"))).bind("ref"))),
          unless(hasAncestor(
              lambdaExpr(hasDescendant(declRefExpr(equalsBoundNode("ref"))),
                         valueCapturesVar(equalsBoundNode("param"))))))
          .bind("move-call");

  Finder->addMatcher(
      parmVarDecl(
          parmVarDecl(hasType(type(rValueReferenceType()))).bind("param"),
          parmVarDecl(
              unless(hasType(references(qualType(anyOf(
                  isConstQualified(),
                  templateTypeParmType(hasDeclaration(templateTypeParmDecl())),
                  substTemplateTypeParmType()))))),
              anyOf(
                  hasAncestor(compoundStmt(hasParent(lambdaExpr(
                      has(cxxRecordDecl(
                          has(cxxMethodDecl(ToParam, hasName("operator()"))))),
                      optionally(hasDescendant(MoveCallMatcher)))))),
                  hasAncestor(cxxConstructorDecl(
                      ToParam, isDefinition(), unless(isMoveConstructor()),
                      optionally(hasDescendant(MoveCallMatcher)))),
                  hasAncestor(functionDecl(
                      unless(cxxConstructorDecl()), ToParam,
                      unless(cxxMethodDecl(isMoveAssignmentOperator())),
                      hasBody(optionally(hasDescendant(MoveCallMatcher)))))))),
      this);
}

void RvalueReferenceParamNotMovedCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *Param = Result.Nodes.getNodeAs<ParmVarDecl>("param");

  if (!Param)
    return;

  if (IgnoreUnnamedParams && Param->getName().empty())
    return;

  const auto *MoveCall = Result.Nodes.getNodeAs<CallExpr>("move-call");
  if (!MoveCall) {
    diag(Param->getLocation(),
         "rvalue reference parameter %0 is never moved from "
         "inside the function body")
        << Param;
  }
}

RvalueReferenceParamNotMovedCheck::RvalueReferenceParamNotMovedCheck(
    StringRef Name, ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      StrictMode(Options.getLocalOrGlobal("StrictMode", true)),
      IgnoreUnnamedParams(
          Options.getLocalOrGlobal("IgnoreUnnamedParams", false)) {}

void RvalueReferenceParamNotMovedCheck::storeOptions(
    ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "StrictMode", StrictMode);
  Options.store(Opts, "IgnoreUnnamedParams", IgnoreUnnamedParams);
}

} // namespace clang::tidy::cppcoreguidelines
