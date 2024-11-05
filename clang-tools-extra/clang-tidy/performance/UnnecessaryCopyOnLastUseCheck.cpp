//===--- UnnecessaryCopyOnLastUseCheck.cpp - clang-tidy -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UnnecessaryCopyOnLastUseCheck.h"
#include "../utils/ExprSequence.h"
#include "../utils/Matchers.h"
#include "../utils/OptionsUtils.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {

static constexpr const char* BlockedTypesOption = "BlockedTypes";
static constexpr const char* BlockedFunctionsOption = "BlockedFunctions";

namespace {
struct FindDeclRefBlockReturn {
  const CFGBlock *DeclRefBlock = nullptr;
  CFGBlock::const_iterator StartElement{};
};

enum class Usage {
  Error = -1,
  Usage = 0,
  DefiniteLastUse,
};

} // namespace

static FindDeclRefBlockReturn findDeclRefBlock(CFG const *TheCFG,
                                               const DeclRefExpr *DeclRef) {
  for (CFGBlock *Block : *TheCFG) {
    auto Iter =
        llvm::find_if(Block->Elements, [&, DeclRef](const CFGElement &Element) {
          if (Element.getKind() == CFGElement::Statement) {
            return Element.template castAs<CFGStmt>().getStmt() == DeclRef;
          }
          return false;
        });
    if (Iter != Block->Elements.end()) {
      return {Block, ++Iter};
    }
  }
  return {nullptr, {}};
}

static std::vector<const Stmt*>
findStmtsTo(const FindDeclRefBlockReturn &StartBlockElement, const DeclRefExpr* To) {
  std::vector<const Stmt*> Ss;
  auto Begi = StartBlockElement.DeclRefBlock->Elements.begin();
  auto Endi = StartBlockElement.DeclRefBlock->Elements.end();
  for (auto Iter = Begi; Iter != Endi; ++Iter) {
    const CFGElement &Element = *Iter;
    if (Element.getKind() == CFGElement::Statement) {
      if (auto *Stmt = Element.template castAs<CFGStmt>().getStmt()) {
        if (auto *DRE = dyn_cast<DeclRefExpr>(Stmt)) {
          if (DRE->getDecl() == To->getDecl())
            Ss.push_back(DRE);
        }
      }
    }
  }
  return Ss;
}

static const clang::CFGElement *
nextUsageInCurrentBlock(const FindDeclRefBlockReturn &StartBlockElement,
                        const DeclRefExpr *DeclRef) {
  // Search for uses in the current block
  auto Begi = StartBlockElement.StartElement;
  auto Endi = StartBlockElement.DeclRefBlock->Elements.end();
  auto Iter = std::find_if(Begi, Endi, [&](const CFGElement &Element) {
    if (Element.getKind() == CFGElement::Statement) {
      if (auto *Stmt = Element.template castAs<CFGStmt>().getStmt()) {
        if (auto *DRE = dyn_cast<DeclRefExpr>(Stmt)) {
          if (DRE->getDecl() == DeclRef->getDecl()) {
            return true;
          }
        }
      }
    }
    return false;
  });
  return Iter != Endi ? &*Iter : nullptr;
}

static bool isLHSOfAssignment(const DeclRefExpr *DeclRef, ASTContext &Context) {
  const TraversalKindScope RAII(Context, TK_IgnoreUnlessSpelledInSource);
  // Todo (performance): While this is faster than a match expression,
  //      it would be faster to start from the DeclRefExpr directly
  struct IsLHSOfAssignment : RecursiveASTVisitor<IsLHSOfAssignment> {
    const DeclRefExpr *Ref{};

    IsLHSOfAssignment(const DeclRefExpr *Ref, ASTContext &Context) : Ref(Ref) {}

    bool shouldWalkTypesOfTypeLocs() const { return false; }
    bool shouldVisitTemplateInstantiations() const { return true; }

    bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *BO) {
      if (BO->isAssignmentOp()) {
        if (auto *DRE =
                dyn_cast<DeclRefExpr>(BO->getArg(0)->IgnoreParenImpCasts())) {
          if (DRE && DRE == Ref) {
            return false;
          }
        }
      }
      return true;
    }
  };
  return !IsLHSOfAssignment{DeclRef, Context}.TraverseAST(Context);
}

static bool isInLambdaCapture(const DeclRefExpr *MyDeclRef,
                              ASTContext &Context) {

  // Todo (improvement): Expand this Visitor to also determine, if an explicit
  // CaptureInitExpr is used or if a DeclRefExpr is used implicitelly via a
  // cature default
  struct IsInLambdaCapture : RecursiveASTVisitor<IsInLambdaCapture> {
    const DeclRefExpr *Ref{};

    IsInLambdaCapture(const DeclRefExpr *Ref) : Ref(Ref) {}

    bool shouldWalkTypesOfTypeLocs() const { return false; }
    bool shouldVisitTemplateInstantiations() const { return true; }

    bool VisitLambdaExpr(LambdaExpr *Lambda) {
      for (Expr *Inits : Lambda->capture_inits()) {
        auto *S = cast<Stmt>(Inits);
        llvm::SmallVector<Stmt *, 6> Childs;
        while (S != nullptr) {
          if (auto *DeclRef = dyn_cast<DeclRefExpr>(S);
              DeclRef != nullptr && Ref == DeclRef) {
            return false;
          }
          Childs.append(S->child_begin(), S->child_end());
          S = Childs.empty() ? nullptr : Childs.pop_back_val();
        }
      }
      return true;
    }
  };
  return !IsInLambdaCapture{MyDeclRef}.TraverseAST(Context);
}

namespace {

class UnnecessaryCopyOnLastUseFinder {
public:
  UnnecessaryCopyOnLastUseFinder(ASTContext *TheContext) : Context(TheContext) {}

  Usage find(Stmt *CodeBlock, const Expr *CopyCall, const ValueDecl* CopiedVariable);

private:
  std::optional<Usage> findInternal(const CFGBlock *Block,
      const Expr *CopyCall,
      const ValueDecl *CopiedVariable);
  void getUsesAndReinits(const CFGBlock *Block, const ValueDecl *From,
                         llvm::SmallVectorImpl<const DeclRefExpr *> *Uses,
                         llvm::SmallPtrSetImpl<const Stmt *> *Reinits);
  void getDeclRefs(const CFGBlock *Block, const Decl *From,
                   llvm::SmallPtrSetImpl<const DeclRefExpr *> *DeclRefs);
  void getReinits(const CFGBlock *Block, const ValueDecl *From,
                  llvm::SmallPtrSetImpl<const Stmt *> *Stmts,
                  llvm::SmallPtrSetImpl<const DeclRefExpr *> *DeclRefs);

  ASTContext *Context;
  std::unique_ptr<tidy::utils::ExprSequence> Sequence;
  std::unique_ptr<tidy::utils::StmtToBlockMap> BlockMap;
  llvm::SmallPtrSet<const CFGBlock *, 8> Visited;
};

void UnnecessaryCopyOnLastUseFinder::getUsesAndReinits(
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

Usage UnnecessaryCopyOnLastUseFinder::find(Stmt* CodeBlock, const Expr* CopyCall, const ValueDecl* CopiedVariable) {
  CFG::BuildOptions Options;
  Options.AddImplicitDtors = true;
  Options.AddTemporaryDtors = true;
  std::unique_ptr<CFG> TheCFG =
      CFG::buildCFG(nullptr, CodeBlock, Context, Options);
  if (!TheCFG) {
    llvm::errs() << "Failed to build CFG\n";
    return Usage::Usage;
  }

  Sequence = std::make_unique<tidy::utils::ExprSequence>(TheCFG.get(), CodeBlock, Context);
  BlockMap = std::make_unique<tidy::utils::StmtToBlockMap>(TheCFG.get(), Context);
  Visited.clear();

  const CFGBlock *MoveBlock = BlockMap->blockContainingStmt(CopyCall);
  if (!MoveBlock) {
    // This can happen if MovingCall is in a constructor initializer, which is
    // not included in the CFG because the CFG is built only from the function
    // body.
    MoveBlock = &TheCFG->getEntry();
  }

  std::optional<Usage> Result = findInternal(MoveBlock, CopyCall, CopiedVariable);
  return Result.value_or(Usage::Usage);
}

static bool isStandardSmartPointer(const ValueDecl *VD) {
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

// Matches nodes that are
// - Part of a decltype argument or class template argument (we check this by
//   seeing if they are children of a TypeLoc), or
// - Part of a function template argument (we check this by seeing if they are
//   children of a DeclRefExpr that references a function template).
// DeclRefExprs that fulfill these conditions should not be counted as a use or
// move.
static StatementMatcher inDecltypeOrTemplateArg() {
  return anyOf(hasAncestor(typeLoc()),
               hasAncestor(declRefExpr(
                   to(functionDecl(ast_matchers::isTemplateInstantiation())))),
               hasAncestor(expr(tidy::matchers::hasUnevaluatedContext())));
}

void UnnecessaryCopyOnLastUseFinder::getDeclRefs(
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
                                      unless(inDecltypeOrTemplateArg()))
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

void UnnecessaryCopyOnLastUseFinder::getReinits(
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

std::optional<Usage> UnnecessaryCopyOnLastUseFinder::findInternal(const CFGBlock *Block, const Expr *CopyCall,
                                 const ValueDecl *CopiedVariable) {
  if (Visited.count(Block))
    return std::nullopt;

  // Mark the block as visited (except if this is the block containing the
  // std::move() and it's being visited the first time).
  if (!CopyCall)
    Visited.insert(Block);

  // Get all uses and reinits in the block.
  llvm::SmallVector<const DeclRefExpr *, 1> Uses;
  llvm::SmallPtrSet<const Stmt *, 1> Reinits;
  getUsesAndReinits(Block, CopiedVariable, &Uses, &Reinits);

  // Ignore all reinitializations where the move potentially comes after the
  // reinit.
  // If `Reinit` is identical to `CopyCall`, we're looking at a move-to-self
  // (e.g. `a = std::move(a)`). Count these as reinitializations.
  llvm::SmallVector<const Stmt *, 1> ReinitsToDelete;
  for (const Stmt *Reinit : Reinits) {
    if (CopyCall && Reinit != CopyCall &&
        Sequence->potentiallyAfter(CopyCall, Reinit))
      ReinitsToDelete.push_back(Reinit);
  }
  for (const Stmt *Reinit : ReinitsToDelete) {
    Reinits.erase(Reinit);
  }

  // Find all uses that potentially come after the move.
  for (const DeclRefExpr *Use : Uses) {
    if (!CopyCall || Sequence->potentiallyAfter(Use, CopyCall)) {
      // Does the use have a saving reinit? A reinit is saving if it definitely
      // comes before the use, i.e. if there's no potential that the reinit is
      // after the use.
      bool HaveSavingReinit = false;
      for (const Stmt *Reinit : Reinits) {
        if (!Sequence->potentiallyAfter(Reinit, Use))
          HaveSavingReinit = true;
      }

      if (!HaveSavingReinit) {
        return Usage::Usage;
      }
    }
  }

  // If the object wasn't reinitialized, call ourselves recursively on all
  // successors.
  if (Reinits.empty()) {
    for (const auto &Succ : Block->succs()) {
      if (Succ) {
        std::optional<Usage> Result = findInternal(Succ, nullptr, CopiedVariable);
        if (Result.value_or(Usage::DefiniteLastUse) != Usage::DefiniteLastUse) {
          return Usage::Usage;
        }
      }
    }
  }

  return Usage::DefiniteLastUse;
}

}

static Usage definiteLastUse(ASTContext *Context,
                             Stmt* CodeBlock,
                             const Expr *CopyCall,
                             const ValueDecl* CopiedVariable) {
  UnnecessaryCopyOnLastUseFinder finder(Context);
  return finder.find(CodeBlock, CopyCall, CopiedVariable);
}

static Usage definiteLastUse2(ASTContext *Context, CFG *const TheCFG,
                             const DeclRefExpr *DeclRef) {
  if (TheCFG == nullptr) {
    return Usage::Error;
  }

  // Find the CFGBlock containing the DeclRefExpr
  FindDeclRefBlockReturn StartBlockElement = findDeclRefBlock(TheCFG, DeclRef);
  if (StartBlockElement.DeclRefBlock == nullptr) {
    return Usage::Error;
  }

  llvm::errs() << "DUMP\n";
  DeclRef->dump();
  StartBlockElement.DeclRefBlock->dump();

  // Find next uses of the DeclRefExpr

  auto TraverseCFGForUsage = [&]() -> Usage {
    llvm::SmallPtrSet<CFGBlock const *, 8> VisitedBlocks;
    llvm::SmallVector<CFGBlock const *, 8> Worklist;

    auto HandleInternal = [&](const FindDeclRefBlockReturn &BlockElement) -> Usage {
      CFGElement const *NextUsageE =
          nextUsageInCurrentBlock(BlockElement, DeclRef);
      if (NextUsageE) {
        llvm::errs() << "NextUsageE non null\n";
          llvm::cast<DeclRefExpr>(
                                      NextUsageE->castAs<CFGStmt>().getStmt())->dump();
        if (bool const IsLastUsage =
                isLHSOfAssignment(llvm::cast<DeclRefExpr>(
                                      NextUsageE->castAs<CFGStmt>().getStmt()),
                                  *Context);
            !IsLastUsage) {
          return Usage::Usage;
        }

        llvm::errs() << __LINE__ << " return DefiniteLastUse\n";
        return Usage::DefiniteLastUse;
      }
      llvm::errs() << "NextUsageE null\n";
      assert(BlockElement.DeclRefBlock);
      // No successing DeclRefExpr found, appending successors
      for (CFGBlock const *Succ : BlockElement.DeclRefBlock->succs()) {
        if (Succ) { // Succ can be nullptr, if a block is unreachable
          Worklist.push_back(Succ);
        }
      }

      auto Sequence = std::make_unique<tidy::utils::ExprSequence>(TheCFG, DeclRef, Context);

      llvm::errs() << "DUMP SS\n";
      std::vector<const Stmt*> Ss = findStmtsTo(StartBlockElement, DeclRef);
      for (const auto*S:Ss) {
        if (S == DeclRef) continue;
        if (!Sequence->inSequence(S, DeclRef) & !Sequence->inSequence(DeclRef, S)) {
          llvm::errs() << "Neither inSequence S <=> DeclRef\n";
          S->dump();
          DeclRef->dump();
          return Usage::Usage;
        }
        if (Sequence->inSequence(S, DeclRef)) {
          llvm::errs() << "inSequence S DeclRef\n";
        }
        if (Sequence->inSequence(DeclRef, S)) {
          llvm::errs() << "inSequence DeclRef S\n";
        }
        if (Sequence->inSequence(S, DeclRef)) {
          llvm::errs() << "inSequence!!!\n";
          S->dump();
          DeclRef->dump();
          return Usage::Usage;
        }
        S->dump();
      }
      llvm::errs() << "DONE DUMP SS\n";
      llvm::errs() << __LINE__ << " return DefiniteLastUse\n";
      return Usage::DefiniteLastUse; // No usage found, assume last use
    };

    if (Usage FoundUsage = HandleInternal(StartBlockElement);
        FoundUsage == Usage::Usage) { // Usage found
      return FoundUsage;
    }
    while (!Worklist.empty()) {
      CFGBlock const *Block = Worklist.pop_back_val();
      if (!VisitedBlocks.insert(Block).second) {
        continue;
      }
      if (Usage FoundUsage = HandleInternal({Block, Block->Elements.begin()});
          FoundUsage == Usage::Usage) {
        return FoundUsage;
      }
    }
    return Usage::DefiniteLastUse;
  };

  return TraverseCFGForUsage();
}

} // namespace clang

namespace clang::tidy::performance {

#if 0
clang::SmallVector<const clang::Stmt *, 1>
static getParentStmts(const clang::Stmt *S) const {
	using namespace clang;
	auto *Context = &d_ctx;
	SmallVector<const Stmt *, 1> Result;

	TraversalKindScope RAII(*Context, TK_AsIs);
	DynTypedNodeList Parents = Context->getParents(*S);

	SmallVector<DynTypedNode, 1> NodesToProcess(Parents.begin(), Parents.end());

	while (!NodesToProcess.empty()) {
		DynTypedNode Node = NodesToProcess.back();
		NodesToProcess.pop_back();

		if (const auto *S = Node.get<Stmt>()) {
			Result.push_back(S);
		} else {
			Parents = Context->getParents(Node);
			NodesToProcess.append(Parents.begin(), Parents.end());
		}
	}

	return Result;
}

static const clang::Stmt *getOuterStmt(const clang::Stmt *stmt) const {
    // OuterStmt is a bespoke concept that defines the smallest
    // clang::Stmt in an AST that can be considered safe to apply
    // the move optimization. The simplest example where this comes
    // in handy is to detect code like `consumes_strings(a,a`) where
    // the function call accepts both parameters as `std::string`,
    // and we do not want to consider this as a candidate for move
    // since there is no sequencing in argument evaluation order.
    // The OuterStmt would be the outer most clang::CallExpr.
    // If two DeclRefExpr (`a` in this case) map to the same OuterStmt
    // as would happen here, then neither are not candidates for move.

    for (const clang::Stmt *parent : getParentStmts(stmt)) {
      if (llvm::dyn_cast<clang::CompoundStmt>(parent)) {
        return stmt;
      }
      const clang::Stmt *FullExpr = getOuterStmt(parent);
      if (FullExpr) {
        return FullExpr;
      }
    }
    return nullptr;
  }

AST_MATCHER_P(Stmt, sharesFullExpr) {
  clang::Stmt* Outer = getOuterStmt(&Node);
  if (Outer) {
  }
}
#endif

void UnnecessaryCopyOnLastUseCheck::registerMatchers(MatchFinder *Finder) {
  const auto ValueParameter =
      declRefExpr(
          to(valueDecl(
              unless(varDecl(unless(hasAutomaticStorageDuration()))),
              hasType(qualType(
                  hasCanonicalType(qualType(
                      matchers::isExpensiveToCopy(),
                      unless(anyOf(isConstQualified(), lValueReferenceType(),
                                   pointerType())))),
                  unless(hasDeclaration(namedDecl(
                      matchers::matchesAnyListedName(BlockedTypes))) //
                         )))).bind("paramDecl")))
          .bind("param");

#if 0
  const auto AnotherRef = declRefExpr(unless(equalsBoundNode("param")), to(valueDecl(equalsBoundNode("paramDecl")))).bind("other");
  const auto UniqueValueParameter = declRefExpr(ValueParameter,
      hasAncestor(stmt(hasParent(stmt(anyOf(lambdaExpr(), compoundStmt()))))));
      //hasAncestor(stmt(hasParent(stmt(anyOf(lambdaExpr(), compoundStmt()))), unless(hasDescendant(AnotherRef)))));
#endif
  const auto UniqueValueParameter = ValueParameter;

  const auto IsMoveAssignable = cxxOperatorCallExpr(
      hasDeclaration(cxxMethodDecl(
          isCopyAssignmentOperator(),
          ofClass(hasMethod(cxxMethodDecl(isMoveAssignmentOperator(),
                                          unless(isDeleted())))))),
      hasRHS(ignoringParenImpCasts(UniqueValueParameter))).bind("");

  const auto IsMoveConstructible =
      ignoringElidableConstructorCall(ignoringParenImpCasts(
          cxxConstructExpr(
              unless(hasParent(callExpr(hasDeclaration(namedDecl(
                  matchers::matchesAnyListedName(BlockedFunctions)))))),
              hasDeclaration(cxxConstructorDecl(
                  isCopyConstructor(),
                  ofClass(hasMethod(cxxConstructorDecl(isMoveConstructor(),
                                                       unless(isDeleted())))))),
              hasArgument(0, UniqueValueParameter))
              .bind("constructExpr")));

  Finder->addMatcher(stmt(anyOf(IsMoveAssignable, expr(IsMoveConstructible)),
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
                functionDecl().bind("containing-func")))))).bind("copyExpr"),

                     this);
  Finder->addMatcher(functionDecl().bind("FN"), this);
}

void UnnecessaryCopyOnLastUseCheck::check(
    const MatchFinder::MatchResult &Result) {
  if (const auto* FN = Result.Nodes.getNodeAs<FunctionDecl>("FN")) {
    FN->dump();
    return;
  }
  const auto *Param = Result.Nodes.getNodeAs<DeclRefExpr>("param");
  const ValueDecl *const DeclOfParam = Param->getDecl();
  const DeclContext *const FunctionOfDeclContext =
      DeclOfParam->getParentFunctionOrMethod();
  const auto *CopyExpr = Result.Nodes.getNodeAs<Expr>("copyExpr");

  if (!FunctionOfDeclContext) {
    // The parameter is not defined in a function, therefore it is not
    // possible to check if it is the last use via CFG analysis
    // Todo (improvement): Add a flag to show unanalyzable cases
    return;
  }

  const auto *const FunctionOfDecl =
      llvm::cast<FunctionDecl>(FunctionOfDeclContext);

  const auto *const VarDeclVal = llvm::dyn_cast<VarDecl>(DeclOfParam);
  if (!VarDeclVal) {
    return;
  }

  const auto *ContainingFunc =
      Result.Nodes.getNodeAs<FunctionDecl>("containing-func");

  Usage DefiniteLastUse = definiteLastUse(
      Result.Context, ContainingFunc->getBody(), CopyExpr, Param->getDecl());
  //Usage DefiniteLastUse = definiteLastUse(
  //    Result.Context, getOrCreateCFG(FunctionOfDecl, Result.Context), Param);

  if (DefiniteLastUse == Usage::Usage || DefiniteLastUse == Usage::Error) {
    return;
  }

  // Template code cant be fixed currently
  if (!FunctionOfDecl->isTemplateInstantiation()) {
    clang::SourceManager &SM = *Result.SourceManager;
    auto Diag =
        diag(Param->getExprLoc(),
             "parameter '%0' is copied on last use, consider moving it instead")
        << Param->getDecl()->getNameAsString();

    if (auto *CExpr = Result.Nodes.getNodeAs<CXXConstructExpr>("constructExpr");
        isInLambdaCapture(Param, *Result.Context) ||
        (CExpr && CExpr->getExprLoc().isMacroID())) {
      // Lambda captures should not be fixed.
      // They also require at least c++14
      return;
    }
    auto MVStmt = "std::move(" + Param->getDecl()->getNameAsString() + ")";
    Diag << FixItHint::CreateReplacement(Param->getSourceRange(), MVStmt)
         << Param->getDecl()->getNameAsString()
         << Inserter.createIncludeInsertion(SM.getFileID(Param->getBeginLoc()),
                                            "<utility>");
  } else { // Template code can't be fixed currently, also a std::forward may be
           // more appropriate
    auto Diag =
        diag(Param->getExprLoc(), "parameter '%0' may be copied on last use, "
                                  "consider forwarding it instead")
        << Param->getDecl()->getNameAsString();
  }
}

UnnecessaryCopyOnLastUseCheck::UnnecessaryCopyOnLastUseCheck(
    StringRef Name, ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      Inserter(Options.getLocalOrGlobal("IncludeStyle",
                                        utils::IncludeSorter::IS_LLVM),
               areDiagsSelfContained()),
      BlockedTypes(
          utils::options::parseStringList(Options.get(BlockedTypesOption, ""))),
      BlockedFunctions(utils::options::parseStringList(
          Options.get(BlockedFunctionsOption, ""))),
      CFGs() {}

static CFG::BuildOptions createBuildOptions() {
  CFG::BuildOptions Options;
  Options.setAlwaysAdd(DeclRefExpr::DeclRefExprClass);
  Options.AddImplicitDtors = true;
  Options.AddTemporaryDtors = true;
  return Options;
}

CFG *UnnecessaryCopyOnLastUseCheck::getOrCreateCFG(const FunctionDecl *FD,
                                                   ASTContext *C) {
  static auto BO = createBuildOptions();
  if (auto Iter = this->CFGs.find(FD); Iter != this->CFGs.end()) {
    return Iter->second.get();
  }

  auto EmplaceResult =
      this->CFGs.try_emplace(FD, CFG::buildCFG(nullptr, FD->getBody(), C, BO));
  return EmplaceResult.first->second.get();
}

void UnnecessaryCopyOnLastUseCheck::registerPPCallbacks(
    const SourceManager &SM, Preprocessor *PP, Preprocessor *ModuleExpanderPP) {
  Inserter.registerPreprocessor(PP);
}

} // namespace clang::tidy::performance
