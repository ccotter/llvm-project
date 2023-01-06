//===--- UseConceptsCheck.cpp - clang-tidy --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UseConceptsCheck.h"
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

void UseConceptsCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(functionTemplateDecl(has(functionDecl(hasReturnTypeLoc(typeLoc().bind("return"))).bind("function"))), this);
}

static std::optional<TemplateSpecializationTypeLoc> checkIsEnableIf(TypeLoc TheType) {
  llvm::errs() << "checkIsEnableIf name=(" << TheType.getType().getAsString() << ")\n";
  TheType.getTypePtr()->dump();
  if (const auto Dep = TheType.getAs<DependentNameTypeLoc>()) {
    return checkIsEnableIf(Dep.getQualifierLoc().getTypeLoc());
  } else if (const auto Elaborated = TheType.getAs<ElaboratedTypeLoc>()) {
    return checkIsEnableIf(Elaborated.getNamedTypeLoc());
  } else if (const auto Typedef = TheType.getAs<TypedefTypeLoc>()) {
    llvm::errs() << "got TypedefType\n";
    Typedef.getTypePtr()->getDecl()->dump();
  } else if (const auto Specialization = TheType.getAs<TemplateSpecializationTypeLoc>()) {
    std::string Name = TheType.getType().getAsString();
    if (Name.find("enable_if<") == std::string::npos && Name.find("enable_if_t<") == std::string::npos) {
      llvm::errs() << "did not Found it\n";
      return std::nullopt;
    }
    if (Specialization.getNumArgs() != 2) {
      return std::nullopt;
    }
    return std::make_optional(Specialization);

    /*
    llvm::errs() << "type=" << Specialization->getTemplateName().getKind() << "\n";
    TemplateName TName = Specialization->getTemplateName();
    if (TName.getKind() == TemplateName::Template) {
      TName.getAsTemplateDecl()->dump();
      if (const auto* Alias = llvm::dyn_cast<TypeAliasTemplateDecl>(TName.getAsTemplateDecl())) {
        llvm::errs() << "the alias\n";
        Alias->getTemplatedDecl()->dump();
        Alias->getTemplatedDecl()->getUnderlyingType()->dump();
        checkIsEnableIf(Alias->getTemplatedDecl()->getUnderlyingType().getTypePtr());
      }
    }*/
  }
  return std::nullopt;
}

static SourceRange getConditionRange(ASTContext& Context, const TemplateSpecializationTypeLoc& EnableIf) {
  // TemplateArgumentLoc's SourceRange End is the location of the last token
  // (per UnqualifiedId docs). E.g., in `enable_if<AAA && BBB>`, the End
  // location will be the first 'B' in 'BBB'.
  TemplateArgumentLoc NextArg = EnableIf.getArgLoc(1);
  const LangOptions& LangOpts = Context.getLangOpts();
  const SourceManager& SM = Context.getSourceManager();
  return SourceRange(
      EnableIf.getLAngleLoc().getLocWithOffset(1),
      utils::lexer::findPreviousTokenKind(NextArg.getSourceRange().getBegin(), SM, LangOpts, tok::comma));
}

static SourceRange getTypeRange(ASTContext& Context, const TemplateSpecializationTypeLoc& EnableIf) {
  TemplateArgumentLoc Arg = EnableIf.getArgLoc(1);
  const LangOptions& LangOpts = Context.getLangOpts();
  const SourceManager& SM = Context.getSourceManager();
  return SourceRange(
      utils::lexer::findPreviousTokenKind(Arg.getSourceRange().getBegin(), SM, LangOpts, tok::comma).getLocWithOffset(1),
      EnableIf.getRAngleLoc());
}

void UseConceptsCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Function = Result.Nodes.getNodeAs<FunctionDecl>("function");
  const auto *ReturnType = Result.Nodes.getNodeAs<TypeLoc>("return");
  assert(Function);
  assert(ReturnType);
  llvm::errs() << "\n\nType functionDecl name=" << Function->getName() << "\n";

  Function->dump();

  ASTContext& Context = *Result.Context;
  SourceManager& SM = Context.getSourceManager();
  const LangOptions& LangOpts = Context.getLangOpts();

  std::optional<TemplateSpecializationTypeLoc> EnableIf = checkIsEnableIf(*ReturnType);
  if (EnableIf) {
    llvm::errs() << "GOT IT\n";
    TemplateArgumentLoc EnableCondition = EnableIf->getArgLoc(0);
    TemplateArgumentLoc  EnableType = EnableIf->getArgLoc(1);
    llvm::errs() << "KIND=" << EnableType.getArgument().getKind() << "\n";
    EnableCondition.getSourceExpression()->dump();

    SourceRange ConditionRange = getConditionRange(Context, *EnableIf);
    SourceRange TypeRange = getTypeRange(Context, *EnableIf);
    llvm::errs() << "cond sr: " << ConditionRange.printToString(SM) << "\n";
    llvm::errs() << "type sr : " << TypeRange.printToString(SM) << "\n";

    llvm::errs() << "typeloc range=" << ReturnType->getSourceRange().printToString(SM) << "\n";

    bool Invalid = false;
    llvm::StringRef ConditionText = Lexer::getSourceText(CharSourceRange::getCharRange(ConditionRange), SM, LangOpts, &Invalid).trim();
    assert(!Invalid);
    llvm::StringRef TypeText = Lexer::getSourceText(CharSourceRange::getCharRange(TypeRange), SM, LangOpts, &Invalid).trim();
    assert(!Invalid);

    llvm::errs() << "REPLACE OBJ=/" << TypeText << "/\n";

    SmallVector<const Expr*, 3> ExistingConstraints;
    Function->getAssociatedConstraints(ExistingConstraints);
    if (ExistingConstraints.size() > 0) {
      // We don't yet support adding to existing constraints.
      diag(ReturnType->getBeginLoc(), "use C++20 requires constraints instead of enable_if");
      return;
    }

    const Stmt* Body = Function->getBody();
    std::vector<FixItHint> FixIts;
    FixIts.push_back(FixItHint::CreateReplacement(
          CharSourceRange::getTokenRange(ReturnType->getSourceRange()), TypeText));
    FixIts.push_back(FixItHint::CreateInsertion(Body->getBeginLoc(), "requires " + ConditionText.str() + " "));
    diag(ReturnType->getBeginLoc(), "use C++20 requires constraints instead of enable_if") << FixIts;
  }
}

} // namespace modernize
} // namespace tidy
} // namespace clang
