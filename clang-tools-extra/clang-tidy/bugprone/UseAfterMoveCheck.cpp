//===--- UseAfterMoveCheck.cpp - clang-tidy -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UseAfterMoveCheck.h"

#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include "../utils/Matchers.h"
#include "../utils/UseAfterMoveFinder.h"
#include <optional>

using namespace clang::ast_matchers;
using namespace clang::tidy::utils;

namespace clang::tidy::bugprone {

using matchers::hasUnevaluatedContext;

enum class MoveType {
  Move,    // std::move
  Forward, // std::forward
};

static MoveType determineMoveType(const FunctionDecl *FuncDecl) {
  if (FuncDecl->getName() == "move")
    return MoveType::Move;
  if (FuncDecl->getName() == "forward")
    return MoveType::Forward;

  llvm_unreachable("Invalid move type");
}

static void emitDiagnostic(const Expr *MovingCall, const DeclRefExpr *MoveArg,
                           const utils::UseAfterMove &Use, ClangTidyCheck *Check,
                           ASTContext *Context, MoveType Type) {
  const SourceLocation UseLoc = Use.DeclRef->getExprLoc();
  const SourceLocation MoveLoc = MovingCall->getExprLoc();

  const bool IsMove = (Type == MoveType::Move);

  Check->diag(UseLoc, "'%0' used after it was %select{forwarded|moved}1")
      << MoveArg->getDecl()->getName() << IsMove;
  Check->diag(MoveLoc, "%select{forward|move}0 occurred here",
              DiagnosticIDs::Note)
      << IsMove;
  if (Use.EvaluationOrderUndefined) {
    Check->diag(
        UseLoc,
        "the use and %select{forward|move}0 are unsequenced, i.e. "
        "there is no guarantee about the order in which they are evaluated",
        DiagnosticIDs::Note)
        << IsMove;
  } else if (Use.UseHappensInLaterLoopIteration) {
    Check->diag(UseLoc,
                "the use happens in a later loop iteration than the "
                "%select{forward|move}0",
                DiagnosticIDs::Note)
        << IsMove;
  }
}

void UseAfterMoveCheck::registerMatchers(MatchFinder *Finder) {
  // try_emplace is a common maybe-moving function that returns a
  // bool to tell callers whether it moved. Ignore std::move inside
  // try_emplace to avoid false positives as we don't track uses of
  // the bool.
  auto TryEmplaceMatcher =
      cxxMemberCallExpr(callee(cxxMethodDecl(hasName("try_emplace"))));
  auto CallMoveMatcher =
      callExpr(argumentCountIs(1),
               callee(functionDecl(hasAnyName("::std::move", "::std::forward"))
                          .bind("move-decl")),
               hasArgument(0, declRefExpr().bind("arg")),
               unless(matchers::inDecltypeOrTemplateArg()),
               unless(hasParent(TryEmplaceMatcher)), expr().bind("call-move"),
               anyOf(hasAncestor(compoundStmt(
                         hasParent(lambdaExpr().bind("containing-lambda")))),
                     hasAncestor(functionDecl(anyOf(
                         cxxConstructorDecl(
                             hasAnyConstructorInitializer(withInitializer(
                                 expr(anyOf(equalsBoundNode("call-move"),
                                            hasDescendant(expr(
                                                equalsBoundNode("call-move")))))
                                     .bind("containing-ctor-init"))))
                             .bind("containing-ctor"),
                         functionDecl().bind("containing-func"))))));

  Finder->addMatcher(
      traverse(
          TK_AsIs,
          // To find the Stmt that we assume performs the actual move, we look
          // for the direct ancestor of the std::move() that isn't one of the
          // node types ignored by ignoringParenImpCasts().
          stmt(
              forEach(expr(ignoringParenImpCasts(CallMoveMatcher))),
              // Don't allow an InitListExpr to be the moving call. An
              // InitListExpr has both a syntactic and a semantic form, and the
              // parent-child relationships are different between the two. This
              // could cause an InitListExpr to be analyzed as the moving call
              // in addition to the Expr that we actually want, resulting in two
              // diagnostics with different code locations for the same move.
              unless(initListExpr()),
              unless(expr(ignoringParenImpCasts(equalsBoundNode("call-move")))))
              .bind("moving-call")),
      this);
}

void UseAfterMoveCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *ContainingCtor =
      Result.Nodes.getNodeAs<CXXConstructorDecl>("containing-ctor");
  const auto *ContainingCtorInit =
      Result.Nodes.getNodeAs<Expr>("containing-ctor-init");
  const auto *ContainingLambda =
      Result.Nodes.getNodeAs<LambdaExpr>("containing-lambda");
  const auto *ContainingFunc =
      Result.Nodes.getNodeAs<FunctionDecl>("containing-func");
  const auto *CallMove = Result.Nodes.getNodeAs<CallExpr>("call-move");
  const auto *MovingCall = Result.Nodes.getNodeAs<Expr>("moving-call");
  const auto *Arg = Result.Nodes.getNodeAs<DeclRefExpr>("arg");
  const auto *MoveDecl = Result.Nodes.getNodeAs<FunctionDecl>("move-decl");

  if (!MovingCall || !MovingCall->getExprLoc().isValid())
    MovingCall = CallMove;

  // Ignore the std::move if the variable that was passed to it isn't a local
  // variable.
  if (!Arg->getDecl()->getDeclContext()->isFunctionOrMethod())
    return;

  // Collect all code blocks that could use the arg after move.
  llvm::SmallVector<Stmt *> CodeBlocks{};
  if (ContainingCtor) {
    CodeBlocks.push_back(ContainingCtor->getBody());
    if (ContainingCtorInit) {
      // Collect the constructor initializer expressions.
      bool BeforeMove{true};
      for (CXXCtorInitializer *Init : ContainingCtor->inits()) {
        if (BeforeMove && Init->getInit()->IgnoreImplicit() ==
                              ContainingCtorInit->IgnoreImplicit())
          BeforeMove = false;
        if (!BeforeMove)
          CodeBlocks.push_back(Init->getInit());
      }
    }
  } else if (ContainingLambda) {
    CodeBlocks.push_back(ContainingLambda->getBody());
  } else if (ContainingFunc) {
    CodeBlocks.push_back(ContainingFunc->getBody());
  }

  for (Stmt *CodeBlock : CodeBlocks) {
    utils::UseAfterMoveFinder Finder(Result.Context);
    if (auto Use = Finder.find(CodeBlock, MovingCall, Arg))
      emitDiagnostic(MovingCall, Arg, *Use, this, Result.Context,
                     determineMoveType(MoveDecl));
  }
}

} // namespace clang::tidy::bugprone
