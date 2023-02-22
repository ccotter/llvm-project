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

void RvalueReferenceParamNotMovedCheck::registerMatchers(MatchFinder *Finder) {
  auto ToParam = hasAnyParameter(parmVarDecl(equalsBoundNode("param")));

  Finder->addMatcher(
      parmVarDecl(
          parmVarDecl(hasType(type(rValueReferenceType()))).bind("param"),
          parmVarDecl(
              unless(hasType(references(qualType(anyOf(
                  isConstQualified(),
                  templateTypeParmType(hasDeclaration(templateTypeParmDecl())),
                  substTemplateTypeParmType()))))),
              anyOf(hasAncestor(compoundStmt(hasParent(
                        lambdaExpr(has(cxxRecordDecl(has(cxxMethodDecl(
                                       ToParam, hasName("operator()"))))))
                            .bind("containing-lambda")))),
                    hasAncestor(
                        functionDecl(
                            isDefinition(), ToParam,
                            optionally(
                                cxxConstructorDecl().bind("containing-ctor")),
                            unless(cxxConstructorDecl(isMoveConstructor())),
                            unless(cxxMethodDecl(isMoveAssignmentOperator())))
                            .bind("containing-func"))))),
      this);
}

namespace {
AST_MATCHER_P(LambdaExpr, valueCapturesVar, const VarDecl *, Var) {
  return std::find_if(Node.capture_begin(), Node.capture_end(),
                      [&](const LambdaCapture &Capture) {
                        return Capture.capturesVariable() &&
                               Capture.getCapturedVar() == Var &&
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

void RvalueReferenceParamNotMovedCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *Param = Result.Nodes.getNodeAs<ParmVarDecl>("param");
  const auto *ContainingLambda =
      Result.Nodes.getNodeAs<LambdaExpr>("containing-lambda");
  const auto *ContainingCtor =
      Result.Nodes.getNodeAs<FunctionDecl>("containing-ctor");
  const auto *ContainingFunc =
      Result.Nodes.getNodeAs<FunctionDecl>("containing-func");

  if (!Param)
    return;

  if (IgnoreUnnamedParams && Param->getName().empty())
    return;

  const auto *Function = dyn_cast<FunctionDecl>(Param->getDeclContext());
  if (!Function)
    return;

  StatementMatcher MoveCallMatcher = callExpr(
      anyOf(callee(functionDecl(hasName("::std::move"))),
            callee(unresolvedLookupExpr(hasAnyDeclaration(
                namedDecl(hasUnderlyingDecl(hasName("::std::move"))))))),
      argumentCountIs(1),
      hasArgument(0,
                  argumentOf(StrictMode,
                             declRefExpr(to(equalsNode(Param))).bind("ref"))),
      unless(hasAncestor(
          lambdaExpr(hasDescendant(declRefExpr(equalsBoundNode("ref"))),
                     valueCapturesVar(Param)))));

  SmallVector<BoundNodes, 1> Matches;
  if (ContainingLambda) {
    if (!ContainingLambda->getBody())
      return;
    Matches = match(findAll(MoveCallMatcher), *ContainingLambda->getBody(),
                    *Result.Context);
  } else if (ContainingCtor) {
    Matches = match(findAll(cxxConstructorDecl(hasDescendant(MoveCallMatcher))),
                    *ContainingCtor, *Result.Context);
  } else if (ContainingFunc) {
    if (!ContainingFunc->getBody())
      return;
    Matches = match(findAll(MoveCallMatcher), *ContainingFunc->getBody(),
                    *Result.Context);
  } else {
    return;
  }

  int MoveExprsCount = Matches.size();
  if (MoveExprsCount == 0) {
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
