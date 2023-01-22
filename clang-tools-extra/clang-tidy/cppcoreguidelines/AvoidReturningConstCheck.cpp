//===--- AvoidReturningConstCheck.cpp - clang-tidy ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AvoidReturningConstCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include <algorithm>
#include <optional>

using namespace clang::ast_matchers;

namespace clang::tidy::cppcoreguidelines {

void AvoidReturningConstCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(
      functionDecl(
          hasReturnTypeLoc(
              typeLoc(loc(qualType(isConstQualified()))).bind("return_type")))
          .bind("function"),
      this);
}

static Token getPreviousToken(SourceLocation &Location, const SourceManager &SM,
                              const LangOptions &LangOpts,
                              bool SkipComments = false) {
  Token Token;
  Token.setKind(tok::unknown);

  Location = Location.getLocWithOffset(-1);
  if (Location.isInvalid())
    return Token;

  SourceLocation StartOfFile = SM.getLocForStartOfFile(SM.getFileID(Location));
  while (Location != StartOfFile) {
    Location = Lexer::GetBeginningOfToken(Location, SM, LangOpts);
    if (!Lexer::getRawToken(Location, Token, SM, LangOpts) &&
        (!SkipComments || !Token.is(tok::comment))) {
      break;
    }
    Location = Location.getLocWithOffset(-1);
  }
  return Token;
}

// QualType does not record the location of qualifiers such as 'const'.
// We traverse the tokens to look for the 'const' qualifier, but only
// in specific known to be correct cases (more cases can always be added).
static std::optional<SourceRange>
getConstQualifiefLoc(const FunctionDecl *Function, const ASTContext &Ctx) {
  const SourceManager &SM = Ctx.getSourceManager();
  const LangOptions &LangOpts = Ctx.getLangOpts();

  SourceLocation FunctionBegin = Function->getBeginLoc();
  SourceLocation At = Function->getReturnTypeSourceRange().getBegin();
  while (SM.isBeforeInTranslationUnit(FunctionBegin, At)) {
    Token Tok = getPreviousToken(At, SM, LangOpts);

    if (Tok.is(tok::raw_identifier)) {
      IdentifierInfo &Info = Ctx.Idents.get(
          StringRef(SM.getCharacterData(Tok.getLocation()), Tok.getLength()));
      Tok.setKind(Info.getTokenID());
    }

    if (Tok.is(tok::kw_const)) {
      std::optional<Token> NextToken =
          Lexer::findNextToken(Tok.getLocation(), SM, LangOpts);
      if (!NextToken) {
        return std::nullopt;
      }
      return std::make_optional(
          SourceRange(Tok.getLocation(), NextToken->getLocation()));
    }
  }

  return std::nullopt;
}

static bool hasDefinition(const FunctionDecl *Function) {
  return std::any_of(
      Function->redecls_begin(), Function->redecls_end(),
      [](const FunctionDecl *OtherDecl) { return OtherDecl->hasBody(); });
}

void AvoidReturningConstCheck::check(const MatchFinder::MatchResult &Result) {
  const QualifiedTypeLoc *ReturnType =
      Result.Nodes.getNodeAs<QualifiedTypeLoc>("return_type");
  const FunctionDecl *Function =
      Result.Nodes.getNodeAs<FunctionDecl>("function");

  if (!ReturnType || !Function) {
    return;
  }

  if (!hasDefinition(Function)) {
    // Only emit FixIts if the function is defined in the source.
    diag(ReturnType->getBeginLoc(), "avoid returning const values");
    return;
  }

  std::optional<SourceRange> ConstRange =
      getConstQualifiefLoc(Function, *Result.Context);

  if (ConstRange) {
    diag(ConstRange->getBegin(), "avoid returning const values")
        << FixItHint::CreateRemoval(CharSourceRange::getCharRange(*ConstRange));
  } else {
    diag(ReturnType->getBeginLoc(), "avoid returning const values");
  }
}

} // namespace clang::tidy::cppcoreguidelines
