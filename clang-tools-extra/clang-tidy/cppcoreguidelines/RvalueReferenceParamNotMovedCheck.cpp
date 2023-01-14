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

#include <queue>

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace cppcoreguidelines {

void RvalueReferenceParamNotMovedCheck::registerMatchers(MatchFinder *Finder) {
  auto ToParam = hasAnyParameter(parmVarDecl(equalsBoundNode("param")));
  Finder->addMatcher(
      parmVarDecl(allOf(
          parmVarDecl(hasType(type(rValueReferenceType()))).bind("param"),
          parmVarDecl(
              equalsBoundNode("param"),
              unless(hasType(
                  qualType(references(templateTypeParmType(
                               hasDeclaration(templateTypeParmDecl()))),
                           unless(references(qualType(isConstQualified())))))),
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
                                       DynTypedNode ContainingNode,
                                       const Expr *StartingExpr,
                                       const Decl *Param) {
  std::queue<DynTypedNode> ToVisit;
  ToVisit.push(DynTypedNode::create(*StartingExpr));
  while (!ToVisit.empty()) {
    DynTypedNode At = ToVisit.front();
    ToVisit.pop();

    if (At == ContainingNode) {
      return false;
    }
    if (const auto *Lambda = At.get<LambdaExpr>()) {
      bool ParamIsValueCaptured =
          std::find_if(Lambda->capture_begin(), Lambda->capture_end(),
                       [&](const LambdaCapture &Capture) {
                         return Capture.getCapturedVar() == Param &&
                                Capture.capturesVariable() &&
                                Capture.getCaptureKind() == LCK_ByCopy;
                       }) != Lambda->capture_end();
      if (ParamIsValueCaptured) {
        return true;
      }
    }

    DynTypedNodeList Parents = Context.getParents(At);
    std::for_each(Parents.begin(), Parents.end(),
                  [&](const DynTypedNode &Node) { ToVisit.push(Node); });
    if (Parents.empty()) {
      return false;
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

  StatementMatcher MoveCallMatcher =
      callExpr(anyOf(callee(functionDecl(hasName("::std::move"))),
                     callee(unresolvedLookupExpr(hasAnyDeclaration(namedDecl(
                         hasUnderlyingDecl(hasName("::std::move"))))))),
               argumentCountIs(1),
               hasArgument(0, declRefExpr(to(equalsNode(Param))).bind("ref")));

  DynTypedNode ContainingNode;

  SmallVector<BoundNodes, 1> Matches;
  if (ContainingLambda) {
    if (!ContainingLambda->getBody())
      return;
    ContainingNode = DynTypedNode::create(*ContainingLambda);
    Matches = match(findAll(MoveCallMatcher), *ContainingLambda->getBody(),
                    *Result.Context);
  } else if (ContainingCtor) {
    ContainingNode = DynTypedNode::create(*ContainingCtor);
    Matches = match(findAll(cxxConstructorDecl(hasDescendant(MoveCallMatcher))),
                    *ContainingCtor, *Result.Context);
  } else if (ContainingFunc) {
    if (!ContainingFunc->getBody())
      return;
    ContainingNode = DynTypedNode::create(*ContainingFunc);
    Matches = match(findAll(MoveCallMatcher), *ContainingFunc->getBody(),
                    *Result.Context);
  } else {
    return;
  }

  int MoveExprs = Matches.size();
  for (const BoundNodes &Match : Matches) {
    // The DeclRefExprs of non-initializer value captured variables refer to
    // the original variable declaration in the AST. In such cases, we exclude
    // those DeclRefExprs since they are not actually moving the original
    // variable.
    if (isValueCapturedByAnyLambda(*Result.Context, ContainingNode,
                                   Match.getNodeAs<DeclRefExpr>("ref"),
                                   Param)) {
      --MoveExprs;
    }
  }

  if (MoveExprs == 0) {
    diag(Param->getBeginLoc(), "rvalue reference parameter is never moved from "
                               "inside the function body");
  }
}

} // namespace cppcoreguidelines
} // namespace tidy
} // namespace clang
