//===--- UseNamedCastCheck.cpp - clang-tidy -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UseNamedCastCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang::tidy::readability {

AST_MATCHER(Decl, conceptDecl) {
  return dyn_cast<ConceptDecl>(&Node) != nullptr;
}

AST_MATCHER_P(QualType, possiblyPackExpansionOf,
              ast_matchers::internal::Matcher<QualType>, InnerMatcher) {
  return InnerMatcher.matches(Node.getNonPackExpansionType(), Finder, Builder);
}

AST_MATCHER(ParmVarDecl, isTemplateTypeParameter) {
  ast_matchers::internal::Matcher<QualType> Inner = possiblyPackExpansionOf(
      qualType(rValueReferenceType(),
               references(templateTypeParmType(
                   hasDeclaration(templateTypeParmDecl()))),
               unless(references(qualType(isConstQualified())))));
  if (!Inner.matches(Node.getType(), Finder, Builder))
    return false;

  const auto *Function = dyn_cast<FunctionDecl>(Node.getDeclContext());
  if (!Function)
    return false;

  const FunctionTemplateDecl *FuncTemplate =
      Function->getDescribedFunctionTemplate();
  if (!FuncTemplate)
    return false;

  QualType ParamType =
      Node.getType().getNonPackExpansionType()->getPointeeType();
  const auto *TemplateType = ParamType->getAs<TemplateTypeParmType>();
  if (!TemplateType)
    return false;

  return TemplateType->getDepth() ==
         FuncTemplate->getTemplateParameters()->getDepth();
}

UseNamedCastCheck::UseNamedCastCheck(StringRef Name,
    ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      IncludeInserter(Options.getLocalOrGlobal("IncludeStyle",
                                               utils::IncludeSorter::IS_LLVM),
                      areDiagsSelfContained()),
      IgnoreDependentTypesForMove(
          Options.getLocalOrGlobal("IgnoreDependentTypesForMove", true)) {}

void UseNamedCastCheck::registerPPCallbacks(
    const SourceManager &SM, Preprocessor *PP, Preprocessor *ModuleExpanderPP) {
  IncludeInserter.registerPreprocessor(PP);
}

void UseNamedCastCheck::storeOptions(
    ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "IncludeStyle", IncludeInserter.getStyle());
  Options.store(Opts, "IgnoreDependentTypesForMove", IgnoreDependentTypesForMove);
}

void UseNamedCastCheck::registerMatchers(MatchFinder *Finder) {
  using ast_matchers::isTemplateInstantiation;
  auto ForwardCastExpr = declRefExpr(hasDeclaration(parmVarDecl(isTemplateTypeParameter())));
  auto MoveCastExpr = expr(unless(ForwardCastExpr), hasType(hasCanonicalType(type(equalsBoundNode("underlyingType")))));
  auto AnyLValueRefDeducedOrForwardingParam = hasAnyParameter(parmVarDecl(
        anyOf(
          allOf(isTemplateTypeParameter(), hasType(rValueReferenceType(pointee(templateTypeParmType(equalsBoundNode("templateType")))))),
          hasType(lValueReferenceType(pointee(templateTypeParmType(equalsBoundNode("templateType"))))))));

  Finder->addMatcher(explicitCastExpr(
      hasDestinationType(type(rValueReferenceType()).bind("destinationType")),
      hasSourceExpression(expr(ForwardCastExpr).bind("sourceExpr")),
      unless(hasAncestor(functionDecl(isTemplateInstantiation()))),
      optionally(hasAncestor(decl(conceptDecl()).bind("concept")))
    ).bind("forwardCast"), this);
  Finder->addMatcher(explicitCastExpr(
      hasDestinationType(type(rValueReferenceType(pointee(
              hasCanonicalType(type().bind("underlyingType")),
              optionally(templateTypeParmType().bind("templateType"))))).bind("destinationType")),
      unless(hasAncestor(functionDecl(AnyLValueRefDeducedOrForwardingParam))),
      hasSourceExpression(expr(MoveCastExpr).bind("sourceExpr")),
      unless(hasAncestor(functionDecl(isTemplateInstantiation()))),
      optionally(hasAncestor(decl(conceptDecl()).bind("concept")))
    ).bind("moveCast"), this);
}

SourceLocation forwardSkipWhitespace(SourceLocation Loc, const SourceManager& SM, const ASTContext* Context) {
  while (Loc.isValid() && isWhitespace(*SM.getCharacterData(Loc)))
    Loc = Loc.getLocWithOffset(1);
  return Loc;
}

void UseNamedCastCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *SourceExpr  = Result.Nodes.getNodeAs<Expr>("sourceExpr");
  SourceManager& SM = Result.Context->getSourceManager();
  LangOptions LangOpts = Result.Context->getLangOpts();

  // (T&&)t
  // (T&&) /*comment*/ t
  // (T&&)something()
  // (T&&)something(t)...
  // (T&&)t ...
  // (T&&)t...
  // (T&&)(t)
  // static_cast<T&&>(t)
  // static_cast<T&&>/*coment*/(t)

  const ExplicitCastExpr* Cast = nullptr;
  bool IsForward = false;
  std::vector<FixItHint> FixIts;

  if ((Cast = Result.Nodes.getNodeAs<ExplicitCastExpr>("forwardCast"))) {
    IsForward = true;
    //llvm::errs() << "forward Found " << SourceExpr->getSourceRange().printToString(SM) << "\n";
  } else if ((Cast = Result.Nodes.getNodeAs<ExplicitCastExpr>("moveCast"))) {
    IsForward = false;
    //llvm::errs() << "move Found " << SourceExpr->getSourceRange().printToString(SM) << " dep=" << Cast->getTypeAsWritten()->isDependentType() << "\n";
  } else
    return;

  if (!IsForward && IgnoreDependentTypesForMove && Cast->getTypeAsWritten()->isDependentType())
    return;

  // TODO: remove, not a real option
  if (getenv("SKIP_FORWARD") && IsForward)
    return;

  auto GetForwardTypeRange = [](const ExplicitCastExpr* Cast) -> std::optional<clang::CharSourceRange> {
    const TypeSourceInfo* SourceInfo = Cast->getTypeInfoAsWritten();
    if (!SourceInfo)
      return std::nullopt;
    TypeLoc DestTypeLoc = SourceInfo->getTypeLoc();
    if (auto DestRefTypeLoc = DestTypeLoc.getAs<ReferenceTypeLoc>()) {
      TypeLoc Inner = DestRefTypeLoc.getPointeeLoc();
      return CharSourceRange::getTokenRange(Inner.getSourceRange());
    }
    return std::nullopt;
  };

  if (const auto* CCast = dyn_cast<CStyleCastExpr>(Cast)) {
    std::string function;
    if (IsForward) {
      auto ForwardTypeRange = GetForwardTypeRange(Cast);
      if (!ForwardTypeRange)
        return;
      function = (Twine{"std::forward<"} + Lexer::getSourceText(
            *ForwardTypeRange,
            SM, LangOpts) + Twine{">"}).str();
    } else
      function = "std::move";

    SourceLocation EndLoc = forwardSkipWhitespace(CCast->getRParenLoc().getLocWithOffset(1), SM, Result.Context);
    if (!EndLoc.isValid())
      return;

    FixIts.push_back(FixItHint::CreateReplacement(
        CharSourceRange::getCharRange(CCast->getLParenLoc(), EndLoc), function));
    FixIts.push_back(FixItHint::CreateInsertion(
        SourceExpr->getBeginLoc(), "("));
    FixIts.push_back(FixItHint::CreateInsertion(
        Lexer::getLocForEndOfToken(SourceExpr->getEndLoc(), 0, SM, LangOpts), ")"));
  } else if (const auto* NamedCast = dyn_cast<CXXNamedCastExpr>(Cast)) {
    std::string function;
    if (IsForward) {
      auto ForwardTypeRange = GetForwardTypeRange(Cast);
      if (!ForwardTypeRange)
        return;
      function = (Twine{"std::forward<"} + Lexer::getSourceText(
            *ForwardTypeRange,
            SM, LangOpts) + Twine{">"}).str();
    } else
      function = "std::move";

    FixIts.push_back(FixItHint::CreateReplacement(
        CharSourceRange::getTokenRange(NamedCast->getOperatorLoc(), NamedCast->getAngleBrackets().getEnd()), function));
  } else
    return;

  if (std::optional<FixItHint> Insertion = IncludeInserter.createIncludeInsertion(
      SM.getFileID(
          Cast->getBeginLoc()),
      "<utility>"))
    FixIts.push_back(std::move(*Insertion));

  if (Result.Nodes.getNodeAs<ConceptDecl>("concept"))
    FixIts.clear();

  if (IsForward)
    diag(Cast->getBeginLoc(), "use std::forward instead of cast expression") << FixIts;
  else
    diag(Cast->getBeginLoc(), "use std::move instead of cast expression") << FixIts;
}

} // namespace clang::tidy::readability
