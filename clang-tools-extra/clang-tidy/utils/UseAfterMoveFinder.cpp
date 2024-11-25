//===--- UseAfterMoveFinder.cpp - clang-tidy ------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UseAfterMoveFinder.h"

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Analysis/Analyses/CFGReachabilityAnalysis.h"
#include "clang/Analysis/CFG.h"

#include "../utils/ExprSequence.h"
#include "../utils/Matchers.h"

namespace clang::tidy::utils {

using namespace ast_matchers;

UseAfterMoveFinder::UseAfterMoveFinder(ASTContext *TheContext)
    : Context(TheContext) {}

std::optional<UseAfterMove>
UseAfterMoveFinder::find(Stmt *CodeBlock, const Expr *MovingCall,
                         const DeclRefExpr *MovedVariable) {
  // Generate the CFG manually instead of through an AnalysisDeclContext because
  // it seems the latter can't be used to generate a CFG for the body of a
  // lambda.
  //
  // We include implicit and temporary destructors in the CFG so that
  // destructors marked [[noreturn]] are handled correctly in the control flow
  // analysis. (These are used in some styles of assertion macros.)
  CFG::BuildOptions Options;
  Options.AddImplicitDtors = true;
  Options.AddTemporaryDtors = true;
  std::unique_ptr<CFG> TheCFG =
      CFG::buildCFG(nullptr, CodeBlock, Context, Options);
  if (!TheCFG)
    return std::nullopt;

  Sequence = std::make_unique<ExprSequence>(TheCFG.get(), CodeBlock, Context);
  BlockMap = std::make_unique<StmtToBlockMap>(TheCFG.get(), Context);
  Visited.clear();

  const CFGBlock *MoveBlock = BlockMap->blockContainingStmt(MovingCall);
  if (!MoveBlock) {
    // This can happen if MovingCall is in a constructor initializer, which is
    // not included in the CFG because the CFG is built only from the function
    // body.
    MoveBlock = &TheCFG->getEntry();
  }

  auto TheUseAfterMove =
      findInternal(MoveBlock, MovingCall, MovedVariable->getDecl());

  if (TheUseAfterMove) {
    if (const CFGBlock *UseBlock =
            BlockMap->blockContainingStmt(TheUseAfterMove->DeclRef)) {
      // Does the use happen in a later loop iteration than the move?
      // - If they are in the same CFG block, we know the use happened in a
      //   later iteration if we visited that block a second time.
      // - Otherwise, we know the use happened in a later iteration if the
      //   move is reachable from the use.
      CFGReverseBlockReachabilityAnalysis CFA(*TheCFG);
      TheUseAfterMove->UseHappensInLaterLoopIteration =
          UseBlock == MoveBlock ? Visited.contains(UseBlock)
                                : CFA.isReachable(UseBlock, MoveBlock);
    }
  }
  return TheUseAfterMove;
}

std::optional<UseAfterMove>
UseAfterMoveFinder::findInternal(const CFGBlock *Block, const Expr *MovingCall,
                                 const ValueDecl *MovedVariable) {
  if (Visited.count(Block))
    return std::nullopt;

  // Mark the block as visited (except if this is the block containing the
  // std::move() and it's being visited the first time).
  if (!MovingCall)
    Visited.insert(Block);

  // Get all uses and reinits in the block.
  llvm::SmallVector<const DeclRefExpr *, 1> Uses;
  llvm::SmallPtrSet<const Stmt *, 1> Reinits;
  getUsesAndReinits(Block, MovedVariable, &Uses, &Reinits);

  // Ignore all reinitializations where the move potentially comes after the
  // reinit.
  // If `Reinit` is identical to `MovingCall`, we're looking at a move-to-self
  // (e.g. `a = std::move(a)`). Count these as reinitializations.
  llvm::SmallVector<const Stmt *, 1> ReinitsToDelete;
  for (const Stmt *Reinit : Reinits) {
    if (MovingCall && Reinit != MovingCall &&
        Sequence->potentiallyAfter(MovingCall, Reinit))
      ReinitsToDelete.push_back(Reinit);
  }
  for (const Stmt *Reinit : ReinitsToDelete) {
    Reinits.erase(Reinit);
  }

  // Find all uses that potentially come after the move.
  for (const DeclRefExpr *Use : Uses) {
    if (!MovingCall || Sequence->potentiallyAfter(Use, MovingCall)) {
      // Does the use have a saving reinit? A reinit is saving if it definitely
      // comes before the use, i.e. if there's no potential that the reinit is
      // after the use.
      bool HaveSavingReinit = false;
      for (const Stmt *Reinit : Reinits) {
        if (!Sequence->potentiallyAfter(Reinit, Use))
          HaveSavingReinit = true;
      }

      if (!HaveSavingReinit) {
        UseAfterMove TheUseAfterMove;
        TheUseAfterMove.DeclRef = Use;

        // Is this a use-after-move that depends on order of evaluation?
        // This is the case if the move potentially comes after the use (and we
        // already know that use potentially comes after the move, which taken
        // together tells us that the ordering is unclear).
        TheUseAfterMove.EvaluationOrderUndefined =
            MovingCall != nullptr &&
            Sequence->potentiallyAfter(MovingCall, Use);

        return TheUseAfterMove;
      }
    }
  }

  // If the object wasn't reinitialized, call ourselves recursively on all
  // successors.
  if (Reinits.empty()) {
    for (const auto &Succ : Block->succs()) {
      if (Succ) {
        if (auto Found = findInternal(Succ, nullptr, MovedVariable)) {
          return Found;
        }
      }
    }
  }

  return std::nullopt;
}

void UseAfterMoveFinder::getUsesAndReinits(
    const CFGBlock *Block, const ValueDecl *MovedVariable,
    llvm::SmallVectorImpl<const DeclRefExpr *> *Uses,
    llvm::SmallPtrSetImpl<const Stmt *> *Reinits) {
  llvm::SmallPtrSet<const DeclRefExpr *, 1> DeclRefs;
  llvm::SmallPtrSet<const DeclRefExpr *, 1> ReinitDeclRefs;

  getDeclRefs(Block, MovedVariable, &DeclRefs);
  getReinits(Block, MovedVariable, Reinits, &ReinitDeclRefs);

  // All references to the variable that aren't reinitializations are uses.
  Uses->clear();
  for (const DeclRefExpr *DeclRef : DeclRefs) {
    if (!ReinitDeclRefs.count(DeclRef))
      Uses->push_back(DeclRef);
  }

  // Sort the uses by their occurrence in the source code.
  llvm::sort(*Uses, [](const DeclRefExpr *D1, const DeclRefExpr *D2) {
    return D1->getExprLoc() < D2->getExprLoc();
  });
}

bool isStandardSmartPointer(const ValueDecl *VD) {
  const Type *TheType = VD->getType().getNonReferenceType().getTypePtrOrNull();
  if (!TheType)
    return false;

  const CXXRecordDecl *RecordDecl = TheType->getAsCXXRecordDecl();
  if (!RecordDecl)
    return false;

  const IdentifierInfo *ID = RecordDecl->getIdentifier();
  if (!ID)
    return false;

  StringRef Name = ID->getName();
  if (Name != "unique_ptr" && Name != "shared_ptr" && Name != "weak_ptr")
    return false;

  return RecordDecl->getDeclContext()->isStdNamespace();
}

void UseAfterMoveFinder::getDeclRefs(
    const CFGBlock *Block, const Decl *MovedVariable,
    llvm::SmallPtrSetImpl<const DeclRefExpr *> *DeclRefs) {
  DeclRefs->clear();
  for (const auto &Elem : *Block) {
    std::optional<CFGStmt> S = Elem.getAs<CFGStmt>();
    if (!S)
      continue;

    auto AddDeclRefs = [this, Block,
                        DeclRefs](const ArrayRef<BoundNodes> Matches) {
      for (const auto &Match : Matches) {
        const auto *DeclRef = Match.getNodeAs<DeclRefExpr>("declref");
        const auto *Operator = Match.getNodeAs<CXXOperatorCallExpr>("operator");
        if (DeclRef && BlockMap->blockContainingStmt(DeclRef) == Block) {
          // Ignore uses of a standard smart pointer that don't dereference the
          // pointer.
          if (Operator || !isStandardSmartPointer(DeclRef->getDecl())) {
            DeclRefs->insert(DeclRef);
          }
        }
      }
    };

    auto DeclRefMatcher = declRefExpr(hasDeclaration(equalsNode(MovedVariable)),
                                      unless(matchers::inDecltypeOrTemplateArg()))
                              .bind("declref");

    AddDeclRefs(match(traverse(TK_AsIs, findAll(DeclRefMatcher)), *S->getStmt(),
                      *Context));
    AddDeclRefs(match(findAll(cxxOperatorCallExpr(
                                  hasAnyOverloadedOperatorName("*", "->", "[]"),
                                  hasArgument(0, DeclRefMatcher))
                                  .bind("operator")),
                      *S->getStmt(), *Context));
  }
}

void UseAfterMoveFinder::getReinits(
    const CFGBlock *Block, const ValueDecl *MovedVariable,
    llvm::SmallPtrSetImpl<const Stmt *> *Stmts,
    llvm::SmallPtrSetImpl<const DeclRefExpr *> *DeclRefs) {
  auto DeclRefMatcher =
      declRefExpr(hasDeclaration(equalsNode(MovedVariable))).bind("declref");

  auto StandardContainerTypeMatcher = hasType(hasUnqualifiedDesugaredType(
      recordType(hasDeclaration(cxxRecordDecl(hasAnyName(
          "::std::basic_string", "::std::vector", "::std::deque",
          "::std::forward_list", "::std::list", "::std::set", "::std::map",
          "::std::multiset", "::std::multimap", "::std::unordered_set",
          "::std::unordered_map", "::std::unordered_multiset",
          "::std::unordered_multimap"))))));

  auto StandardSmartPointerTypeMatcher = hasType(hasUnqualifiedDesugaredType(
      recordType(hasDeclaration(cxxRecordDecl(hasAnyName(
          "::std::unique_ptr", "::std::shared_ptr", "::std::weak_ptr"))))));

  // Matches different types of reinitialization.
  auto ReinitMatcher =
      stmt(anyOf(
               // Assignment. In addition to the overloaded assignment operator,
               // test for built-in assignment as well, since template functions
               // may be instantiated to use std::move() on built-in types.
               binaryOperation(hasOperatorName("="), hasLHS(DeclRefMatcher)),
               // Declaration. We treat this as a type of reinitialization too,
               // so we don't need to treat it separately.
               declStmt(hasDescendant(equalsNode(MovedVariable))),
               // clear() and assign() on standard containers.
               cxxMemberCallExpr(
                   on(expr(DeclRefMatcher, StandardContainerTypeMatcher)),
                   // To keep the matcher simple, we check for assign() calls
                   // on all standard containers, even though only vector,
                   // deque, forward_list and list have assign(). If assign()
                   // is called on any of the other containers, this will be
                   // flagged by a compile error anyway.
                   callee(cxxMethodDecl(hasAnyName("clear", "assign")))),
               // reset() on standard smart pointers.
               cxxMemberCallExpr(
                   on(expr(DeclRefMatcher, StandardSmartPointerTypeMatcher)),
                   callee(cxxMethodDecl(hasName("reset")))),
               // Methods that have the [[clang::reinitializes]] attribute.
               cxxMemberCallExpr(
                   on(DeclRefMatcher),
                   callee(cxxMethodDecl(hasAttr(clang::attr::Reinitializes)))),
               // Passing variable to a function as a non-const pointer.
               callExpr(forEachArgumentWithParam(
                   unaryOperator(hasOperatorName("&"),
                                 hasUnaryOperand(DeclRefMatcher)),
                   unless(parmVarDecl(hasType(pointsTo(isConstQualified())))))),
               // Passing variable to a function as a non-const lvalue reference
               // (unless that function is std::move()).
               callExpr(forEachArgumentWithParam(
                            traverse(TK_AsIs, DeclRefMatcher),
                            unless(parmVarDecl(hasType(
                                references(qualType(isConstQualified())))))),
                        unless(callee(functionDecl(
                            hasAnyName("::std::move", "::std::forward")))))))
          .bind("reinit");

  Stmts->clear();
  DeclRefs->clear();
  for (const auto &Elem : *Block) {
    std::optional<CFGStmt> S = Elem.getAs<CFGStmt>();
    if (!S)
      continue;

    SmallVector<BoundNodes, 1> Matches =
        match(findAll(ReinitMatcher), *S->getStmt(), *Context);

    for (const auto &Match : Matches) {
      const auto *TheStmt = Match.getNodeAs<Stmt>("reinit");
      const auto *TheDeclRef = Match.getNodeAs<DeclRefExpr>("declref");
      if (TheStmt && BlockMap->blockContainingStmt(TheStmt) == Block) {
        Stmts->insert(TheStmt);

        // We count DeclStmts as reinitializations, but they don't have a
        // DeclRefExpr associated with them -- so we need to check 'TheDeclRef'
        // before adding it to the set.
        if (TheDeclRef)
          DeclRefs->insert(TheDeclRef);
      }
    }
  }
}
} // namespace clang::tidy::utils
