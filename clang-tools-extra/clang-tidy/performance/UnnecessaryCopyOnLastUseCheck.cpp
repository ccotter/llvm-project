//===--- UnnecessaryCopyOnLastUseCheck.cpp - clang-tidy -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UnnecessaryCopyOnLastUseCheck.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

#include "../utils/Matchers.h"
#include "../utils/OptionsUtils.h"
#include "../utils/UseAfterMoveFinder.h"

using namespace clang::ast_matchers;
using namespace clang::tidy::utils;

namespace clang {

static constexpr const char *BlockedTypesOption = "BlockedTypes";
static constexpr const char *BlockedFunctionsOption = "BlockedFunctions";
using tidy::matchers::hasUnevaluatedContext;

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

  utils::UseAfterMoveFinder Finder(Result.Context);
  if (auto Use = Finder.find(ContainingFunc->getBody(), CopyExpr, Param))
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
