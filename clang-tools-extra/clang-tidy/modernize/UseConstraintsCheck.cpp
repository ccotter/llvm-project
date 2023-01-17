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

struct EnableIfData {
  TemplateSpecializationTypeLoc Loc;
  TypeLoc Outer;
};

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
                  .bind("function")))
          .bind("functionTemplate"),
      this);
}

static std::optional<TemplateSpecializationTypeLoc>
matchEnableIfSpecialization_impl_type(TypeLoc TheType) {
  llvm::errs() << "matchEnableIfSpecialization_impl_type name=("
               << TheType.getType().getAsString() << ")\n";
  TheType.getTypePtr()->dump();
  if (const auto Dep = TheType.getAs<DependentNameTypeLoc>()) {
    llvm::errs() << "FOR DEPENDENT ID="
                 << Dep.getTypePtr()->getIdentifier()->getName()
                 << " nested kind="
                 << Dep.getTypePtr()->getQualifier()->getKind() << "\n";
    Dep.getTypePtr()->getQualifier()->getAsType()->dump();
    if (Dep.getTypePtr()->getIdentifier()->getName() != "type" ||
        Dep.getTypePtr()->getKeyword() != ETK_Typename) {
      return std::nullopt;
    }
    return matchEnableIfSpecialization_impl_type(
        Dep.getQualifierLoc().getTypeLoc());
  } else if (const auto Elaborated = TheType.getAs<ElaboratedTypeLoc>()) {
    return std::nullopt;
  } else if (const auto Specialization =
                 TheType.getAs<TemplateSpecializationTypeLoc>()) {
    std::string Name = TheType.getType().getAsString();
    if (Name.find("enable_if<") == std::string::npos) {
      return std::nullopt;
    }
    int NumArgs = Specialization.getNumArgs();
    if (NumArgs != 1 && NumArgs != 2) {
      return std::nullopt;
    }
    return std::make_optional(Specialization);
  }
  return std::nullopt;
}

static std::optional<TemplateSpecializationTypeLoc>
matchEnableIfSpecialization_impl_t(TypeLoc TheType) {
  llvm::errs() << "matchEnableIfSpecialization_impl_t name=("
               << TheType.getType().getAsString() << ")\n";
  TheType.getTypePtr()->dump();
  if (const auto Dep = TheType.getAs<DependentNameTypeLoc>()) {
    return std::nullopt;
  } else if (const auto Elaborated = TheType.getAs<ElaboratedTypeLoc>()) {
    llvm::errs() << "ELAB??\n";
    return matchEnableIfSpecialization_impl_t(Elaborated.getNamedTypeLoc());
  } else if (const auto Specialization =
                 TheType.getAs<TemplateSpecializationTypeLoc>()) {
    std::string Name = TheType.getType().getAsString();
    if (Name.find("enable_if_t<") == std::string::npos) {
      return std::nullopt;
    }
    if (!Specialization.getTypePtr()->isTypeAlias()) {
      return std::nullopt;
    }
    if (const auto *AliasedType = llvm::dyn_cast<DependentNameType>(
            Specialization.getTypePtr()->getAliasedType())) {
      if (AliasedType->getIdentifier()->getName() != "type" ||
          AliasedType->getKeyword() != ETK_Typename) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
    int NumArgs = Specialization.getNumArgs();
    if (NumArgs != 1 && NumArgs != 2) {
      return std::nullopt;
    }
    return std::make_optional(Specialization);
  }
  return std::nullopt;
}

static std::optional<TemplateSpecializationTypeLoc>
matchEnableIfSpecialization_impl(TypeLoc TheType) {
  llvm::errs() << "matchEnableIfSpecialization_impl name=("
               << TheType.getType().getAsString() << ")\n";
  std::optional<TemplateSpecializationTypeLoc> EnableIf;
  EnableIf = matchEnableIfSpecialization_impl_type(TheType);
  if (EnableIf) {
    return EnableIf;
  }
  return matchEnableIfSpecialization_impl_t(TheType);
}

static std::optional<EnableIfData>
matchEnableIfSpecialization(TypeLoc TheType) {
  llvm::errs() << "matchEnableIfSpecialization name=("
               << TheType.getType().getAsString() << ")\n";
  if (const auto Qualified = TheType.getAs<QualifiedTypeLoc>()) {
    llvm::errs() << "First Qual\n";
  }
  if (const auto Pointer = TheType.getAs<PointerTypeLoc>()) {
    llvm::errs() << "First Ptr\n";
    TheType = Pointer.getPointeeLoc();
  } else if (const auto Reference = TheType.getAs<ReferenceTypeLoc>()) {
    llvm::errs() << "First Ref\n";
    TheType = Reference.getPointeeLoc();
  }
  if (const auto Qualified = TheType.getAs<QualifiedTypeLoc>()) {
    llvm::errs() << "Second Qual\n";
    TheType = Qualified.getUnqualifiedLoc();
  }

  std::optional<TemplateSpecializationTypeLoc> EnableIf =
      matchEnableIfSpecialization_impl(TheType);
  if (EnableIf) {
    return std::make_optional(EnableIfData{std::move(*EnableIf), TheType});
  } else {
    return std::nullopt;
  }
}

static std::tuple<std::optional<EnableIfData>, const Decl *>
matchTrailingTemplateParam(const FunctionTemplateDecl *FunctionTemplate) {
  // For non-type trailing param, match very specifically
  // 'template <..., enable_if_type<Condition, Type> = Default>' where
  // enable_if_type is 'enable_if' or 'enable_if_t'. E.g., 'template <typename
  // T, enable_if_t<is_same_v<T, bool>, int*> = nullptr>
  //
  // Otherwise, match a trailing default type arg.
  // E.g., 'template <typename T, typename = enable_if_t<is_same_v<T, bool>>>'

  llvm::errs() << "matchTrailingTemplateParam\n";
  FunctionTemplate->dump();
  const TemplateParameterList *TemplateParams =
      FunctionTemplate->getTemplateParameters();
  if (TemplateParams->size() == 0) {
    return {};
  }
  const NamedDecl *LastParam =
      TemplateParams->getParam(TemplateParams->size() - 1);
  if (const auto *LastTemplateParam =
          llvm::dyn_cast<NonTypeTemplateParmDecl>(LastParam)) {
    llvm::errs() << "\nGOT NonTypeTemplateParmDecl hasDefaultArgument="
                 << LastTemplateParam->hasDefaultArgument()
                 << " name=" << LastTemplateParam->getName() << "\n";
    LastTemplateParam->dump();

    if (!LastTemplateParam->hasDefaultArgument() ||
        !LastTemplateParam->getName().empty()) {
      return {};
    }
    llvm::errs() << "default expr=\n";
    LastTemplateParam->getDefaultArgument()->dump();

    return std::make_tuple(
        matchEnableIfSpecialization(
            LastTemplateParam->getTypeSourceInfo()->getTypeLoc()),
        LastTemplateParam);
  } else if (const auto *LastTemplateParam =
                 llvm::dyn_cast<TemplateTypeParmDecl>(LastParam)) {
    llvm::errs() << "GOT TemplateTypeParmDecl hasDefault="
                 << LastTemplateParam->hasDefaultArgument() << " hasname="
                 << (LastTemplateParam->getIdentifier() != nullptr) << "\n";
    LastTemplateParam->dump();
    if (LastTemplateParam->hasDefaultArgument() &&
        LastTemplateParam->getIdentifier() == nullptr) {
      return std::make_tuple(
          matchEnableIfSpecialization(
              LastTemplateParam->getDefaultArgumentInfo()->getTypeLoc()),
          LastTemplateParam);
    }
  }
  return {};
}

template <typename T>
static SourceLocation getRAngleFileLoc(const SourceManager &SM,
                                       const T &Element) {
  // getFileLoc handles the case where the RAngle loc is part of a synthesized
  // '>>', which ends up allocating a 'scratch space' buffer in the source
  // manager.
  return SM.getFileLoc(Element.getRAngleLoc());
}

static SourceRange
getConditionRange(ASTContext &Context,
                  const TemplateSpecializationTypeLoc &EnableIf) {
  // TemplateArgumentLoc's SourceRange End is the location of the last token
  // (per UnqualifiedId docs). E.g., in `enable_if<AAA && BBB>`, the End
  // location will be the first 'B' in 'BBB'.
  const LangOptions &LangOpts = Context.getLangOpts();
  const SourceManager &SM = Context.getSourceManager();
  if (EnableIf.getNumArgs() > 1) {
    TemplateArgumentLoc NextArg = EnableIf.getArgLoc(1);
    return SourceRange(
        EnableIf.getLAngleLoc().getLocWithOffset(1),
        utils::lexer::findPreviousTokenKind(NextArg.getSourceRange().getBegin(),
                                            SM, LangOpts, tok::comma));
  } else {
    return SourceRange(EnableIf.getLAngleLoc().getLocWithOffset(1),
                       getRAngleFileLoc(SM, EnableIf));
  }
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
      getRAngleFileLoc(SM, EnableIf));
}

static std::optional<std::string>
getTypeText(ASTContext &Context,
            const TemplateSpecializationTypeLoc &EnableIf) {
  if (EnableIf.getNumArgs() > 1) {
    const LangOptions &LangOpts = Context.getLangOpts();
    const SourceManager &SM = Context.getSourceManager();
    bool Invalid = false;
    std::string Text =
        Lexer::getSourceText(
            CharSourceRange::getCharRange(getTypeRange(Context, EnableIf)), SM,
            LangOpts, &Invalid)
            .trim()
            .str();
    if (Invalid) {
      return std::nullopt;
    }
    return std::make_optional(std::move(Text));
  } else {
    return std::make_optional("void");
  }
}

static std::optional<SourceLocation>
findInsertionForConstraint(const FunctionDecl *Function, ASTContext &Context) {
  SourceManager &SM = Context.getSourceManager();
  const LangOptions &LangOpts = Context.getLangOpts();

  if (const auto *Constructor = llvm::dyn_cast<CXXConstructorDecl>(Function)) {
    if (Constructor->init_begin() != Constructor->init_end()) {
      const CXXCtorInitializer *FirstInit = *Constructor->init_begin();
      return utils::lexer::findPreviousTokenKind(FirstInit->getSourceLocation(),
                                                 SM, LangOpts, tok::colon);
    }
  }
  if (Function->isDeleted()) {
    SourceRange ParamsRange = Function->getParametersSourceRange();
    if (!ParamsRange.isValid()) {
      return std::nullopt;
    }
    SourceLocation EndParens = utils::lexer::findNextAnyTokenKind(
        ParamsRange.getEnd(), SM, LangOpts, tok::r_paren, tok::r_paren);
    return utils::lexer::findNextAnyTokenKind(EndParens, SM, LangOpts,
                                              tok::equal, tok::equal);
  }
  const Stmt *Body = Function->getBody();
  if (!Body) {
    return std::nullopt;
  }
  return Body->getBeginLoc();
}

static Token getPreviousToken(SourceLocation &Location, const SourceManager &SM,
                              const LangOptions &LangOpts,
                              bool SkipComments = false) {
  Token Token;
  Token.setKind(tok::unknown);

  Location = Location.getLocWithOffset(-1);
  if (Location.isInvalid())
    return Token;

  auto StartOfFile = SM.getLocForStartOfFile(SM.getFileID(Location));
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

bool isPrimaryExpression(const Expr *Expression) {
  // This function is an incomplete approximation of checking whether
  // an Expr is a primary expression. In particular, if this function
  // returns true, the expression is a primary expression. The converse
  // is not necessarily true.

  if (const auto *Cast = llvm::dyn_cast<ImplicitCastExpr>(Expression)) {
    Expression = Cast->getSubExprAsWritten();
  }
  if (llvm::dyn_cast<ParenExpr>(Expression)) {
    return true;
  }
  if (llvm::dyn_cast<DependentScopeDeclRefExpr>(Expression)) {
    return true;
  }

  return false;
}

static std::optional<std::string> getConditionText(const Expr *ConditionExpr,
                                                   SourceRange ConditionRange,
                                                   ASTContext &Context) {
  SourceManager &SM = Context.getSourceManager();
  const LangOptions &LangOpts = Context.getLangOpts();

  SourceLocation PrevTokenLoc = ConditionRange.getEnd();
  if (PrevTokenLoc.isInvalid()) {
    return std::nullopt;
  }
  Token PrevToken = getPreviousToken(PrevTokenLoc, SM, LangOpts);
  bool EndsWithDoubleSlash =
      PrevToken.is(tok::comment) &&
      Lexer::getSourceText(CharSourceRange::getCharRange(
                               PrevTokenLoc, PrevTokenLoc.getLocWithOffset(2)),
                           SM, LangOpts) == "//";

  bool Invalid = false;
  llvm::StringRef ConditionText = Lexer::getSourceText(
      CharSourceRange::getCharRange(ConditionRange), SM, LangOpts, &Invalid);
  if (Invalid) {
    return std::nullopt;
  }

  auto AddParens = [&](llvm::StringRef Text) -> std::string {
    if (isPrimaryExpression(ConditionExpr)) {
      return Text.str();
    } else {
      return "(" + Text.str() + ")";
    }
  };

  if (EndsWithDoubleSlash) {
    return std::make_optional(AddParens(ConditionText.str()));
  } else {
    return std::make_optional(AddParens(ConditionText.trim().str()));
  }
}

static std::vector<FixItHint> handleReturnType(const FunctionDecl *Function,
                                               const TypeLoc &ReturnType,
                                               const EnableIfData &EnableIf,
                                               ASTContext &Context) {
  SourceManager &SM = Context.getSourceManager();

  TemplateArgumentLoc EnableCondition = EnableIf.Loc.getArgLoc(0);

  SourceRange ConditionRange = getConditionRange(Context, EnableIf.Loc);
  llvm::errs() << "cond sr: " << ConditionRange.printToString(SM) << "\n";

  std::optional<std::string> ConditionText = getConditionText(
      EnableCondition.getSourceExpression(), ConditionRange, Context);
  std::optional<std::string> TypeText = getTypeText(Context, EnableIf.Loc);
  if (!TypeText) {
    return {};
  }

  SmallVector<const Expr *, 3> ExistingConstraints;
  Function->getAssociatedConstraints(ExistingConstraints);
  if (ExistingConstraints.size() > 0) {
    // FIXME - Support adding new constraints to existing ones. Do we need to
    // consider subsumption?
    return {};
  }

  std::optional<SourceLocation> ConstraintInsertionLoc =
      findInsertionForConstraint(Function, Context);
  if (!ConstraintInsertionLoc) {
    return {};
  }
  std::vector<FixItHint> FixIts;
  FixIts.push_back(FixItHint::CreateReplacement(
      CharSourceRange::getTokenRange(EnableIf.Outer.getSourceRange()),
      *TypeText));
  FixIts.push_back(FixItHint::CreateInsertion(
      *ConstraintInsertionLoc, "requires " + *ConditionText + " "));
  return FixIts;
}

static std::vector<FixItHint>
handleTrailingTemplateType(const FunctionTemplateDecl *FunctionTemplate,
                           const FunctionDecl *Function,
                           const Decl *LastTemplateParam,
                           const EnableIfData &EnableIf, ASTContext &Context) {
  SourceManager &SM = Context.getSourceManager();
  const LangOptions &LangOpts = Context.getLangOpts();

  TemplateArgumentLoc EnableCondition = EnableIf.Loc.getArgLoc(0);

  SourceRange ConditionRange = getConditionRange(Context, EnableIf.Loc);

  std::optional<std::string> ConditionText = getConditionText(
      EnableCondition.getSourceExpression(), ConditionRange, Context);
  if (!ConditionText) {
    return {};
  }

  SmallVector<const Expr *, 3> ExistingConstraints;
  Function->getAssociatedConstraints(ExistingConstraints);
  if (ExistingConstraints.size() > 0) {
    // FIXME - Support adding new constraints to existing ones. Do we need to
    // consider subsumption?
    return {};
  }

  SourceRange RemovalRange;
  const TemplateParameterList *TemplateParams =
      FunctionTemplate->getTemplateParameters();
  if (!TemplateParams || TemplateParams->size() == 0) {
    return {};
  }

  if (TemplateParams->size() == 1) {
    RemovalRange =
        SourceRange(TemplateParams->getTemplateLoc(),
                    getRAngleFileLoc(SM, *TemplateParams).getLocWithOffset(1));
  } else {
    RemovalRange =
        SourceRange(utils::lexer::findPreviousTokenKind(
                        LastTemplateParam->getSourceRange().getBegin(), SM,
                        LangOpts, tok::comma),
                    getRAngleFileLoc(SM, *TemplateParams));
  }

  std::optional<SourceLocation> ConstraintInsertionLoc =
      findInsertionForConstraint(Function, Context);
  if (!ConstraintInsertionLoc) {
    return {};
  }
  std::vector<FixItHint> FixIts;
  FixIts.push_back(
      FixItHint::CreateRemoval(CharSourceRange::getCharRange(RemovalRange)));
  FixIts.push_back(FixItHint::CreateInsertion(
      *ConstraintInsertionLoc, "requires " + *ConditionText + " "));
  return FixIts;
}

void UseConstraintsCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *FunctionTemplate =
      Result.Nodes.getNodeAs<FunctionTemplateDecl>("functionTemplate");
  const auto *Function = Result.Nodes.getNodeAs<FunctionDecl>("function");
  const auto *ReturnType = Result.Nodes.getNodeAs<TypeLoc>("return");
  if (!FunctionTemplate || !Function || !ReturnType) {
    return;
  }

  // Check for
  //   template <...>
  //   enable_if<Condition, ReturnType> function();
  //
  //   template <..., enable_if<Condition, Type> = Type{}>
  //   function();
  //
  //   template <..., typename = enable_if<Condition, void>>
  //   function();

  std::optional<EnableIfData> EnableIf;
  EnableIf = matchEnableIfSpecialization(*ReturnType);
  if (EnableIf.has_value()) {
    std::vector<FixItHint> FixIts =
        handleReturnType(Function, *ReturnType, *EnableIf, *Result.Context);
    diag(ReturnType->getBeginLoc(),
         "use C++20 requires constraints instead of enable_if")
        << FixIts;
    return;
  }
  const Decl *LastTemplateParam = nullptr;
  std::tie(EnableIf, LastTemplateParam) =
      matchTrailingTemplateParam(FunctionTemplate);
  if (EnableIf.has_value() && LastTemplateParam) {
    std::vector<FixItHint> FixIts = handleTrailingTemplateType(
        FunctionTemplate, Function, LastTemplateParam, *EnableIf,
        *Result.Context);
    diag(LastTemplateParam->getSourceRange().getBegin(),
         "use C++20 requires constraints instead of enable_if")
        << FixIts;
    return;
  }
}

} // namespace modernize
} // namespace tidy
} // namespace clang
