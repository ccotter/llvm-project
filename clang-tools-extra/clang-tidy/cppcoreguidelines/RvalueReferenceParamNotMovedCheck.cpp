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

  internal::Matcher<QualType> ReferenceTypeOrAnything =
      anyOf(qualType(references(templateTypeParmType(hasDeclaration(
                         templateTypeParmDecl().bind("template-type")))),
                     unless(references(qualType(isConstQualified())))),
            anything());

  Finder->addMatcher(
      parmVarDecl(allOf(
          parmVarDecl(hasType(type(rValueReferenceType()))).bind("param"),
          parmVarDecl(
              equalsBoundNode("param"),
              unless(
                  hasType(qualType(references(substTemplateTypeParmType())))),
              hasType(ReferenceTypeOrAnything),
              anyOf(hasAncestor(compoundStmt(hasParent(
                        lambdaExpr(has(cxxRecordDecl(has(cxxMethodDecl(
                                       ToParam, hasName("operator()"))))))
                            .bind("containing-lambda")))),
                    hasAncestor(cxxConstructorDecl(isDefinition(), ToParam,
                                                   unless(isMoveConstructor()),
                                                   isDefinition(), ToParam)
                                    .bind("containing-ctor")),
                    hasAncestor(
                        functionDecl(
                            isDefinition(), ToParam,
                            unless(cxxConstructorDecl(isMoveConstructor())),
                            unless(cxxMethodDecl(isMoveAssignmentOperator())))
                            .bind("containing-func")))))),
      this);
}

static bool isValueCapturedByAnyLambda(ASTContext &Context,
                                       const FunctionDecl *Function,
                                       const DeclRefExpr *MoveTarget,
                                       const ParmVarDecl *Param) {
  SmallVector<BoundNodes, 3> Matches =
      match(lambdaExpr(hasAncestor(equalsNode(Function)),
                       hasDescendant(declRefExpr(equalsNode(MoveTarget))))
                .bind("lambda"),
            Context);
  for (const BoundNodes &Match : Matches) {
    const auto *Lambda = Match.getNodeAs<LambdaExpr>("lambda");
    bool ParamIsValueCaptured =
        std::find_if(Lambda->capture_begin(), Lambda->capture_end(),
                     [&](const LambdaCapture &Capture) {
                       return Capture.capturesVariable() &&
                              Capture.getCapturedVar() == Param &&
                              Capture.getCaptureKind() == LCK_ByCopy;
                     }) != Lambda->capture_end();
    if (ParamIsValueCaptured) {
      return true;
    }
  }
  return false;
}

void RvalueReferenceParamNotMovedCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *Param = Result.Nodes.getNodeAs<ParmVarDecl>("param");
  const auto *ContainingLambda =
      Result.Nodes.getNodeAs<LambdaExpr>("containing-lambda");
  const auto *ContainingCtor =
      Result.Nodes.getNodeAs<FunctionDecl>("containing-ctor");
  const auto *ContainingFunc =
      Result.Nodes.getNodeAs<FunctionDecl>("containing-func");
  const auto *TemplateType =
      Result.Nodes.getNodeAs<TemplateTypeParmDecl>("template-type");

  if (!Param) {
    return;
  }

  const auto *Function = dyn_cast<FunctionDecl>(Param->getDeclContext());
  if (!Function) {
    return;
  }

  if (TemplateType) {
    if (const FunctionTemplateDecl *FuncTemplate =
            Function->getDescribedFunctionTemplate()) {
      const TemplateParameterList *Params =
          FuncTemplate->getTemplateParameters();
      if (llvm::is_contained(*Params, TemplateType)) {
        // Ignore forwarding reference
        return;
      }
    }
  }

  StatementMatcher RefToParam = declRefExpr(to(equalsNode(Param))).bind("ref");
  StatementMatcher MoveCallMatcher =
      callExpr(anyOf(callee(functionDecl(hasName("::std::move"))),
                     callee(unresolvedLookupExpr(hasAnyDeclaration(namedDecl(
                         hasUnderlyingDecl(hasName("::std::move"))))))),
               argumentCountIs(1),
               hasArgument(0, anyOf(RefToParam, hasDescendant(RefToParam))));

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
  for (const BoundNodes &Match : Matches) {
    // The DeclRefExprs of non-initializer value captured variables refer to
    // the original variable declaration in the AST. In such cases, we exclude
    // those DeclRefExprs since they are not actually moving the original
    // variable.
    if (isValueCapturedByAnyLambda(*Result.Context, Function,
                                   Match.getNodeAs<DeclRefExpr>("ref"),
                                   Param)) {
      --MoveExprsCount;
    }
  }

  if (MoveExprsCount == 0) {
    diag(Param->getLocation(), "rvalue reference parameter is never moved from "
                               "inside the function body");
  }
}

} // namespace clang::tidy::cppcoreguidelines
