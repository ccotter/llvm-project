//===--- UnnecessaryCopyOnLastUseCheck.cpp - clang-tidy -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UnnecessaryCopyOnLastUseCheck.h"
#include "../utils/Matchers.h"
#include "../utils/OptionsUtils.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {

static constexpr auto BlockedTypesOption = "BlockedTypes";
static constexpr auto BlockedFunctionsOption = "BlockedFunctions";

namespace {
struct FindDeclRefBlockReturn {
  const CFGBlock *DeclRefBlock = nullptr;
  CFGBlock::const_iterator StartElement{};
};

enum class Usage {
  Error = -1,
  Usage = 0,
  DefiniteLastUse,
  LikelyDefiniteLastUse,
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

static Usage definiteLastUse(ASTContext *Context, CFG *const TheCFG,
                             const DeclRefExpr *DeclRef) {
  if (TheCFG == nullptr) {
    return Usage::Error;
  }

  // Find the CFGBlock containing the DeclRefExpr
  auto StartBlockElement = findDeclRefBlock(TheCFG, DeclRef);
  if (StartBlockElement.DeclRefBlock == nullptr) {
    return Usage::Error; // Todo(unexpected): DeclRefExpr not found in CFG
  }

  // Find next uses of the DeclRefExpr

  auto TraverseCFGForUsage = [&]() -> Usage {
    llvm::SmallPtrSet<CFGBlock const *, 8> VisitedBlocks;
    llvm::SmallVector<CFGBlock const *, 8> Worklist;

    auto HandleInternal = [&](FindDeclRefBlockReturn const &BlockElement) {
      CFGElement const *NextUsageE =
          nextUsageInCurrentBlock(BlockElement, DeclRef);
      if (NextUsageE) {
        if (bool const IsLastUsage =
                isLHSOfAssignment(llvm::cast<DeclRefExpr>(
                                      NextUsageE->castAs<CFGStmt>().getStmt()),
                                  *Context);
            !IsLastUsage) {
          return Usage::Usage;
        }
        return Usage::DefiniteLastUse;
      }
      assert(BlockElement.DeclRefBlock);
      // No successing DeclRefExpr found, appending successors
      for (CFGBlock const *Succ : BlockElement.DeclRefBlock->succs()) {
        if (Succ) { // Succ can be nullptr, if a block is unreachable
          Worklist.push_back(Succ);
        }
      }
      return Usage::DefiniteLastUse; // No usage found, assume last use
    };

    if (auto FoundUsage = HandleInternal(StartBlockElement);
        FoundUsage == Usage::Usage) { // Usage found
      return FoundUsage;
    }
    while (!Worklist.empty()) {
      CFGBlock const *Block = Worklist.pop_back_val();
      if (!VisitedBlocks.insert(Block).second) {
        continue;
      }
      if (auto FoundUsage = HandleInternal({Block, Block->Elements.begin()});
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
                         ))))))
          .bind("param");

  const auto IsMoveAssignable = cxxOperatorCallExpr(
      hasDeclaration(cxxMethodDecl(
          isCopyAssignmentOperator(),
          ofClass(hasMethod(cxxMethodDecl(isMoveAssignmentOperator(),
                                          unless(isDeleted())))))),
      hasRHS(ignoringParenImpCasts(ValueParameter)));

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

  Finder->addMatcher(stmt(anyOf(IsMoveAssignable, expr(IsMoveConstructible))),
                     this);
}

void UnnecessaryCopyOnLastUseCheck::check(
    const MatchFinder::MatchResult &Result) {
  const auto *Param = Result.Nodes.getNodeAs<DeclRefExpr>("param");
  const ValueDecl *const DeclOfParam = Param->getDecl();
  const DeclContext *const FunctionOfDeclContext =
      DeclOfParam->getParentFunctionOrMethod();

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

  Usage DefiniteLastUse = definiteLastUse(
      Result.Context, getOrCreateCFG(FunctionOfDecl, Result.Context), Param);

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
