//===--- ExceptionAnalyzer.h - clang-tidy -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_UTILS_USE_AFTER_MOVE_FINDER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_UTILS_USE_AFTER_MOVE_FINDER_H

#include "clang/AST/ASTContext.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"

#include "../utils/ExprSequence.h"

#include <optional>

namespace clang { class CFGBlock; }
namespace clang::tidy::utils {

/// Contains information about a use-after-move.
struct UseAfterMove {
  // The DeclRefExpr that constituted the use of the object.
  const DeclRefExpr *DeclRef;

  // Is the order in which the move and the use are evaluated undefined?
  bool EvaluationOrderUndefined = false;

  // Does the use happen in a later loop iteration than the move?
  //
  // We default to false and change it to true if required in find().
  bool UseHappensInLaterLoopIteration = false;
};

/// Finds uses of a variable after a move (and maintains state required by the
/// various internal helper functions).
class UseAfterMoveFinder {
public:
  UseAfterMoveFinder(ASTContext *TheContext);

  // Within the given code block, finds the first use of 'MovedVariable' that
  // occurs after 'MovingCall' (the expression that performs the move). If a
  // use-after-move is found, writes information about it to 'TheUseAfterMove'.
  // Returns whether a use-after-move was found.
  std::optional<UseAfterMove> find(Stmt *CodeBlock, const Expr *MovingCall,
                                   const DeclRefExpr *MovedVariable);

private:
  std::optional<UseAfterMove> findInternal(const CFGBlock *Block,
                                           const Expr *MovingCall,
                                           const ValueDecl *MovedVariable);
  void getUsesAndReinits(const CFGBlock *Block, const ValueDecl *MovedVariable,
                         llvm::SmallVectorImpl<const DeclRefExpr *> *Uses,
                         llvm::SmallPtrSetImpl<const Stmt *> *Reinits);
  void getDeclRefs(const CFGBlock *Block, const Decl *MovedVariable,
                   llvm::SmallPtrSetImpl<const DeclRefExpr *> *DeclRefs);
  void getReinits(const CFGBlock *Block, const ValueDecl *MovedVariable,
                  llvm::SmallPtrSetImpl<const Stmt *> *Stmts,
                  llvm::SmallPtrSetImpl<const DeclRefExpr *> *DeclRefs);

  ASTContext *Context;
  std::unique_ptr<ExprSequence> Sequence;
  std::unique_ptr<StmtToBlockMap> BlockMap;
  llvm::SmallPtrSet<const CFGBlock *, 8> Visited;
};


} // namespace clang::tidy::utils

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_UTILS_USE_AFTER_MOVE_FINDER_H
