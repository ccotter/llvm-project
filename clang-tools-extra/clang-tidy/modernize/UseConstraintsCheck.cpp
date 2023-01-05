//===--- UseConstraintsCheck.cpp - clang-tidy -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UseConstraintsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

#include "../utils/LexerUtils.h"

#include <optional>
#include <tuple>

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace modernize {

namespace {
AST_MATCHER(FunctionDecl, hasOtherDeclarations) {
  auto It = Node.redecls_begin();
  auto EndIt = Node.redecls_end();

  if (It == EndIt)
    return false;

  ++It;
  return It != EndIt;
}
} // namespace

void UseConstraintsCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(
      functionTemplateDecl(
          has(functionDecl(unless(hasOtherDeclarations()), isDefinition(),
                           hasReturnTypeLoc(typeLoc().bind("return")))
                  .bind("function"))).bind("functionTemplate"),
      this);
}

static std::optional<TemplateSpecializationTypeLoc>
matchEnableIfSpecialization(TypeLoc TheType) {
  llvm::errs() << "matchEnableIfSpecialization name=(" << TheType.getType().getAsString()
               << ")\n";
  TheType.getTypePtr()->dump();
  if (const auto Dep = TheType.getAs<DependentNameTypeLoc>()) {
    return matchEnableIfSpecialization(Dep.getQualifierLoc().getTypeLoc());
  } else if (const auto Elaborated = TheType.getAs<ElaboratedTypeLoc>()) {
    return matchEnableIfSpecialization(Elaborated.getNamedTypeLoc());
  } else if (const auto Typedef = TheType.getAs<TypedefTypeLoc>()) {
    Typedef.getTypePtr()->getDecl()->dump();
  } else if (const auto Specialization =
                 TheType.getAs<TemplateSpecializationTypeLoc>()) {
    std::string Name = TheType.getType().getAsString();
    if (Name.find("enable_if<") == std::string::npos &&
        Name.find("enable_if_t<") == std::string::npos) {
      return std::nullopt;
    }
    if (Specialization.getNumArgs() != 2) {
      return std::nullopt;
    }
    return std::make_optional(Specialization);
  }
  return std::nullopt;
}

static std::optional<TemplateSpecializationTypeLoc>
matchTrailingTemplateArg(const FunctionTemplateDecl* FunctionTemplate) {
  llvm::errs() << "matchTrailingTemplateArg\n";
  FunctionTemplate->dump();
  const TemplateParameterList* TemplateParams = FunctionTemplate->getTemplateParameters();
  if (TemplateParams->size() == 0) {
    return std::nullopt;
  }
  const NamedDecl* LastParam = TemplateParams->getParam(TemplateParams->size()-1);
  if (const auto* LastTemplateParam = llvm::dyn_cast<NonTypeTemplateParmDecl>(LastParam)) {
    llvm::errs() << "GOT NonTypeTemplateParmDecl\n";
    LastTemplateParam->dump();
    return matchEnableIfSpecialization(LastTemplateParam->getTypeSourceInfo()->getTypeLoc());
    //matchEnableIfSpecialization(LastTemplateParam->getType());
  } else {
    llvm::errs() << "Something else\n";
    LastParam->dump();
  }
  return std::nullopt;
}

static SourceRange
getConditionRange(ASTContext &Context,
                  const TemplateSpecializationTypeLoc &EnableIf) {
  // TemplateArgumentLoc's SourceRange End is the location of the last token
  // (per UnqualifiedId docs). E.g., in `enable_if<AAA && BBB>`, the End
  // location will be the first 'B' in 'BBB'.
  TemplateArgumentLoc NextArg = EnableIf.getArgLoc(1);
  const LangOptions &LangOpts = Context.getLangOpts();
  const SourceManager &SM = Context.getSourceManager();
  return SourceRange(
      EnableIf.getLAngleLoc().getLocWithOffset(1),
      utils::lexer::findPreviousTokenKind(NextArg.getSourceRange().getBegin(),
                                          SM, LangOpts, tok::comma));
}

static SourceRange getTypeRange(ASTContext &Context,
                                const TemplateSpecializationTypeLoc &EnableIf) {
  TemplateArgumentLoc Arg = EnableIf.getArgLoc(1);
  const LangOptions &LangOpts = Context.getLangOpts();
  const SourceManager &SM = Context.getSourceManager();
  return SourceRange(
      utils::lexer::findPreviousTokenKind(Arg.getSourceRange().getBegin(), SM,
                                          LangOpts, tok::comma)
          .getLocWithOffset(1),
      EnableIf.getRAngleLoc());
}

void UseConstraintsCheck::handleReturnType(const FunctionDecl* Function, const TypeLoc& ReturnType, const TemplateSpecializationTypeLoc& EnableIf, ASTContext& Context) {
  SourceManager &SM = Context.getSourceManager();
  const LangOptions &LangOpts = Context.getLangOpts();

  TemplateArgumentLoc EnableCondition = EnableIf.getArgLoc(0);
  TemplateArgumentLoc EnableType = EnableIf.getArgLoc(1);
  llvm::errs() << "KIND=" << EnableType.getArgument().getKind() << "\n";
  EnableCondition.getSourceExpression()->dump();

  SourceRange ConditionRange = getConditionRange(Context, EnableIf);
  SourceRange TypeRange = getTypeRange(Context, EnableIf);
  llvm::errs() << "cond sr: " << ConditionRange.printToString(SM) << "\n";
  llvm::errs() << "type sr : " << TypeRange.printToString(SM) << "\n";

  llvm::errs() << "typeloc range="
                << ReturnType.getSourceRange().printToString(SM) << "\n";

  bool Invalid = false;
  llvm::StringRef ConditionText =
      Lexer::getSourceText(CharSourceRange::getCharRange(ConditionRange), SM,
                            LangOpts, &Invalid)
          .trim();
  assert(!Invalid);
  llvm::StringRef TypeText =
      Lexer::getSourceText(CharSourceRange::getCharRange(TypeRange), SM,
                            LangOpts, &Invalid)
          .trim();
  assert(!Invalid);

  llvm::errs() << "REPLACE OBJ=/" << TypeText << "/\n";

  SmallVector<const Expr *, 3> ExistingConstraints;
  Function->getAssociatedConstraints(ExistingConstraints);
  if (ExistingConstraints.size() > 0) {
    // We don't yet support adding to existing constraints.
    diag(ReturnType.getBeginLoc(),
          "use C++20 requires constraints instead of enable_if");
    return;
  }

  const Stmt *Body = Function->getBody();
  if (!Body)
    return;
  std::vector<FixItHint> FixIts;
  FixIts.push_back(FixItHint::CreateReplacement(
      CharSourceRange::getTokenRange(ReturnType.getSourceRange()),
      TypeText));
  FixIts.push_back(FixItHint::CreateInsertion(
      Body->getBeginLoc(), "requires (" + ConditionText.str() + ") "));
  diag(ReturnType.getBeginLoc(),
        "use C++20 requires constraints instead of enable_if")
      << FixIts;
}

void UseConstraintsCheck::handleTrailingTemplateType(const FunctionDecl* Function, const TemplateSpecializationTypeLoc& EnableIf, ASTContext& Context) {
  SourceManager &SM = Context.getSourceManager();
  const LangOptions &LangOpts = Context.getLangOpts();

  TemplateArgumentLoc EnableCondition = EnableIf.getArgLoc(0);
  TemplateArgumentLoc EnableType = EnableIf.getArgLoc(1);
  llvm::errs() << "KIND=" << EnableType.getArgument().getKind() << "\n";
  EnableCondition.getSourceExpression()->dump();

  SourceRange ConditionRange = getConditionRange(Context, EnableIf);
  SourceRange TypeRange = getTypeRange(Context, EnableIf);
  llvm::errs() << "cond sr: " << ConditionRange.printToString(SM) << "\n";
  llvm::errs() << "type sr : " << TypeRange.printToString(SM) << "\n";

  return;
}

void UseConstraintsCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *FunctionTemplate = Result.Nodes.getNodeAs<FunctionTemplateDecl>("functionTemplate");
  const auto *Function = Result.Nodes.getNodeAs<FunctionDecl>("function");
  const auto *ReturnType = Result.Nodes.getNodeAs<TypeLoc>("return");
  assert(FunctionTemplate);
  assert(Function);
  assert(ReturnType);

  llvm::errs() << "\n\nType functionDecl name=" << Function->getName() << " at "
               << Function->getSourceRange().printToString(Result.Context->getSourceManager()) << "\n";

  // Check for
  //   template <...>
  //   enable_if<Condition, ReturnType> function();
  //
  //   template <..., enable_if<Condition, Type> = Type{}>
  //   function();

  std::optional<TemplateSpecializationTypeLoc> EnableIf =
      matchEnableIfSpecialization(*ReturnType);
  if (EnableIf.has_value()) {
    handleReturnType(Function, *ReturnType, *EnableIf, *Result.Context);
    return;
  }
  EnableIf = matchTrailingTemplateArg(FunctionTemplate);
  if (EnableIf.has_value()) {
    llvm::errs() << "trailing?=" << EnableIf.has_value() << "\n";
    handleTrailingTemplateType(Function, *EnableIf, *Result.Context);
    return;
  }
}

} // namespace modernize
} // namespace tidy
} // namespace clang
