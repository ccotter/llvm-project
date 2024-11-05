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

static constexpr const char *BlockedTypesOption = "BlockedTypes";
static constexpr const char *BlockedFunctionsOption = "BlockedFunctions";

namespace {
struct FindDeclRefBlockReturn {
  const CFGBlock *DeclRefBlock = nullptr;
  CFGBlock::const_iterator StartElement{};
};

enum class Usage {
  Usage = 0,
  DefiniteLastUse,
};

} // namespace

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
  UnnecessaryCopyOnLastUseFinder(ASTContext *TheContext)
      : Context(TheContext) {}

  std::optional<Usage> find(Stmt *CodeBlock, const Expr *CopyCall,
                            const ValueDecl *CopiedVariable);

private:
  std::optional<Usage> findInternal(const CFGBlock *Block, const Expr *CopyCall,
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

std::optional<Usage>
UnnecessaryCopyOnLastUseFinder::find(Stmt *CodeBlock, const Expr *CopyCall,
                                     const ValueDecl *CopiedVariable) {
  CFG::BuildOptions Options;
  Options.AddImplicitDtors = true;
  Options.AddTemporaryDtors = true;
  std::unique_ptr<CFG> TheCFG =
      CFG::buildCFG(nullptr, CodeBlock, Context, Options);
  if (!TheCFG)
    return std::nullopt;

  Sequence = std::make_unique<tidy::utils::ExprSequence>(TheCFG.get(),
                                                         CodeBlock, Context);
  BlockMap =
      std::make_unique<tidy::utils::StmtToBlockMap>(TheCFG.get(), Context);
  Visited.clear();

  const CFGBlock *MoveBlock = BlockMap->blockContainingStmt(CopyCall);
  if (!MoveBlock)
    // This can happen if MovingCall is in a constructor initializer, which is
    // not included in the CFG because the CFG is built only from the function
    // body.
    MoveBlock = &TheCFG->getEntry();

  return findInternal(MoveBlock, CopyCall, CopiedVariable);
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
          if (Operator || !isStandardSmartPointer(DeclRef->getDecl()))
            DeclRefs->insert(DeclRef);
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

std::optional<Usage>
UnnecessaryCopyOnLastUseFinder::findInternal(const CFGBlock *Block,
                                             const Expr *CopyCall,
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

      if (!HaveSavingReinit)
        return Usage::Usage;
    }
  }

  // If the object wasn't reinitialized, call ourselves recursively on all
  // successors.
  if (Reinits.empty()) {
    for (const auto &Succ : Block->succs()) {
      if (Succ) {
        std::optional<Usage> Result =
            findInternal(Succ, nullptr, CopiedVariable);
        if (Result.has_value() && Result.value() == Usage::Usage)
          return Usage::Usage;
      }
    }
  }

  return Usage::DefiniteLastUse;
}

} // namespace

static std::optional<Usage> definiteLastUse(ASTContext *Context,
                                            Stmt *CodeBlock,
                                            const Expr *CopyCall,
                                            const ValueDecl *CopiedVariable) {
  UnnecessaryCopyOnLastUseFinder finder(Context);
  return finder.find(CodeBlock, CopyCall, CopiedVariable);
}

} // namespace clang

namespace clang::tidy::performance {

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

void UnnecessaryCopyOnLastUseCheck::registerMatchers(MatchFinder *Finder) {
  const auto ValueParameter = declRefExpr(
      declRefExpr().bind("param"),
      to(valueDecl(
             unless(varDecl(unless(hasAutomaticStorageDuration()))),
             hasType(qualType(
                 hasCanonicalType(qualType(
                     matchers::isExpensiveToCopy(),
                     unless(anyOf(isConstQualified(), lValueReferenceType(),
                                  pointerType())))),
                 unless(hasDeclaration(
                     namedDecl(matchers::matchesAnyListedName(BlockedTypes))) //
                        ))))
             .bind("paramDecl")),
      unless(hasAncestor(lambdaExpr(hasAnyCapture(lambdaCapture(
          capturesVar(valueDecl(equalsBoundNode("paramDecl")))))))),
      // Ignore DeclRefExprs in ctor initializers for now. Can be implemented
      // later.
      unless(hasAncestor(functionDecl(
          cxxConstructorDecl(hasAnyConstructorInitializer(withInitializer(
              expr(anyOf(equalsBoundNode("param"),
                         hasDescendant(expr(equalsBoundNode("param"))))))))))));

  const auto IsMoveAssignable =
      cxxOperatorCallExpr(
          hasDeclaration(cxxMethodDecl(
              isCopyAssignmentOperator(),
              ofClass(hasMethod(cxxMethodDecl(isMoveAssignmentOperator(),
                                              unless(isDeleted())))))),
          hasRHS(ignoringParenImpCasts(ValueParameter)))
          .bind("");

  const auto IsMoveConstructible =
      ignoringElidableConstructorCall(ignoringParenImpCasts(
          cxxConstructExpr(
              unless(hasParent(callExpr(hasDeclaration(namedDecl(
                  matchers::matchesAnyListedName(BlockedFunctions)))))),
              hasDeclaration(cxxConstructorDecl(
                  isCopyConstructor(),
                  ofClass(hasMethod(cxxConstructorDecl(isMoveConstructor(),
                                                       unless(isDeleted())))))),
              hasArgument(0, ValueParameter))
              .bind("constructExpr")));

  Finder->addMatcher(stmt(anyOf(IsMoveAssignable, expr(IsMoveConstructible)),
                          hasAncestor(functionDecl().bind("containing-func")))
                         .bind("copyExpr"),

                     this);
}

void UnnecessaryCopyOnLastUseCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *Param = Result.Nodes.getNodeAs<DeclRefExpr>("param");
  const ValueDecl *const DeclOfParam = Param->getDecl();
  const DeclContext *const FunctionOfDeclContext =
      DeclOfParam->getParentFunctionOrMethod();
  const auto *CopyExpr = Result.Nodes.getNodeAs<Expr>("copyExpr");

  if (!FunctionOfDeclContext)
    return;

  const auto *const FunctionOfDecl =
      llvm::cast<FunctionDecl>(FunctionOfDeclContext);

  const auto *const VarDeclVal = llvm::dyn_cast<VarDecl>(DeclOfParam);
  if (!VarDeclVal)
    return;

  const auto *ContainingFunc =
      Result.Nodes.getNodeAs<FunctionDecl>("containing-func");

  std::optional<Usage> DefiniteLastUse = definiteLastUse(
      Result.Context, ContainingFunc->getBody(), CopyExpr, Param->getDecl());

  if (!DefiniteLastUse.has_value() || DefiniteLastUse == Usage::Usage)
    return;

  // Template code cant be fixed currently
  if (!FunctionOfDecl->isTemplateInstantiation()) {
    clang::SourceManager &SM = *Result.SourceManager;
    auto Diag =
        diag(Param->getExprLoc(),
             "parameter '%0' is copied on last use, consider moving it instead")
        << Param->getDecl()->getNameAsString();

    if (auto *CExpr =
            Result.Nodes.getNodeAs<CXXConstructExpr>("constructExpr")) {
      if (isInLambdaCapture(Param, *Result.Context) &&
          !getLangOpts().CPlusPlus14)
        return;
      if (CExpr && CExpr->getExprLoc().isMacroID())
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

} // namespace clang::tidy::performance
