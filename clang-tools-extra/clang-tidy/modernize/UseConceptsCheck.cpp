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

#include <optional>
#include <tuple>

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace modernize {

void UseConceptsCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(functionTemplateDecl(has(functionDecl(hasReturnTypeLoc(typeLoc().bind("return"))).bind("function"))), this);
}

std::optional<std::tuple<TemplateArgumentLoc, TemplateArgumentLoc>> checkIsEnableIf(TypeLoc TheType) {
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
    return std::make_optional(std::make_tuple(Specialization.getArgLoc(0), Specialization.getArgLoc(1)));

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

static SourceRange getArgRange(const TemplateArgumentLoc& Arg) {
  return SourceRange(Arg.getBeginLoc(), End);
}

void UseConceptsCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Function = Result.Nodes.getNodeAs<FunctionDecl>("function");
  const auto *ReturnType = Result.Nodes.getNodeAs<TypeLoc>("return");
  assert(Function);
  assert(ReturnType);
  llvm::errs() << "\n\nType functionDecl name=" << Function->getName() << "\n";

  Function->dump();

  auto& SM = Result.Context->getSourceManager();

  std::optional<std::tuple<TemplateArgumentLoc, TemplateArgumentLoc>> EnableIf = checkIsEnableIf(*ReturnType);
  if (EnableIf) {
    llvm::errs() << "GOT IT\n";
    TemplateArgumentLoc EnableCondition = std::get<0>(*EnableIf);
    TemplateArgumentLoc  EnableType = std::get<1>(*EnableIf);
    //EnableCondition.getArgument().dump();
    llvm::errs() << "KIND=" << EnableType.getArgument().getKind() << "\n";
    EnableCondition.getSourceExpression()->dump();

    // TemplateArgumentLoc's SourceRange End is the location of the last token
    // (per UnqualifiedId docs). E.g., in `enable_if<AAA && BBB>`, the End
    // location will be the first 'B' in 'BBB'.
    llvm::errs() << "cond sr: " << EnableCondition.getSourceRange().printToString(SM) << "\n";
    llvm::errs() << "type sr : " << EnableType.getSourceRange().printToString(SM) << "\n";
    llvm::errs() << "type sr2: " << EnableType.getTypeSourceInfo()->getTypeLoc().getSourceRange().printToString(SM) << "\n";
    llvm::errs() << "type endloc: " << EnableType.getTypeSourceInfo()->getTypeLoc().getEndLoc().printToString(SM) << "\n";
  } else {
    llvm::errs() << "NULL cond\n";
  }

  //diag(MatchedDecl->getLocation(), "function %0 is insufficiently awesome")
  //    << MatchedDecl;
}

} // namespace modernize
} // namespace tidy
} // namespace clang
