//===--- RemoveTemplateFromCtorNamesCheck.cpp - clang-tidy --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RemoveTemplateFromCtorNamesCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

std::optional<Token> findLBracket(const CXXConstructorDecl* Ctor, const SourceManager& SM, const LangOptions& LangOpts) {
  std::optional<Token> Next = Lexer::findNextToken(Ctor->getNameInfo().getBeginLoc(), SM, LangOpts);
  if (Next && Next->is(tok::less))
    return Next;
  return std::nullopt;
}

std::optional<Token> findNextBracket(SourceLocation Loc, const SourceManager& SM, const LangOptions& LangOpts) {
  while (Loc.isValid()) {
    std::optional<Token> Next = Lexer::findNextToken(Loc, SM, LangOpts);
    if (!Next)
      return std::nullopt;

    if (Next->is(tok::less) || Next->is(tok::greater))
      return Next;

    Loc = Next->getLocation();
  }

  return std::nullopt;
}

std::optional<Token> findRBracket(const CXXConstructorDecl* Ctor, const SourceManager& SM, const LangOptions& LangOpts) {
  std::optional<Token> LBracket = findLBracket(Ctor, SM, LangOpts);
  if (!LBracket)
    return std::nullopt;

  SourceLocation Loc = LBracket->getLocation();
  int bracket_level = 1;
  while (Loc.isValid()) {
    std::optional<Token> Next = findNextBracket(Loc, SM, LangOpts);
    if (!Next)
      return std::nullopt;

    if (Next->is(tok::less))
      ++bracket_level;
    else {
      --bracket_level;
      if (bracket_level == 0) {
        return Next;
      }
    }

    Loc = Next->getLocation();
  }

  return std::nullopt;
}

AST_MATCHER(CXXConstructorDecl, withTemplateAngleBrackets) {
  const ASTContext &Ctx = Finder->getASTContext();
  std::optional<Token> Next = findLBracket(&Node, Ctx.getSourceManager(), Ctx.getLangOpts());
  return Next.has_value();
}

void RemoveTemplateFromCtorNamesCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(cxxConstructorDecl(withTemplateAngleBrackets()).bind("ctor"), this);
}

void RemoveTemplateFromCtorNamesCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Ctor = Result.Nodes.getNodeAs<CXXConstructorDecl>("ctor");
  const SourceManager& SM = *Result.SourceManager;
  const LangOptions& LangOpts = Result.Context->getLangOpts();

  std::optional<Token> LBracket, RBracket;

  if (Ctor) {
    LBracket = findLBracket(Ctor, SM, LangOpts);
    RBracket = findRBracket(Ctor, SM, LangOpts);
  }

  if (LBracket && RBracket) {
    diag(Ctor->getLocation(), "type %0 should not specify template parameters")
        << Ctor
        << FixItHint::CreateRemoval(SourceRange(LBracket->getLocation(), RBracket->getLocation()));
  }
}

} // namespace clang::tidy::readability
