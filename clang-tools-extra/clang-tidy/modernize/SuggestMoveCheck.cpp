//===--- SuggestMoveCheck.cpp - clang-tidy --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SuggestMoveCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"

#include "../utils/ExprSequence.h"

using namespace clang::ast_matchers;

using clang::tidy::utils::ExprSequence;
using clang::tidy::utils::StmtToBlockMap;

namespace clang {
namespace tidy {
namespace modernize {

void SuggestMoveCheck::registerMatchers(MatchFinder *Finder) {
  // FIXME: Add matchers.

  auto src = declRefExpr(hasDeclaration(anyOf(
                             varDecl(hasAncestor(compoundStmt().bind("scope")))
                                 .bind("srcDecl"),
                             parmVarDecl(hasAncestor(functionDecl(hasAnyBody(
                                             compoundStmt().bind("scope")))))
                                 .bind("srcDecl"))))
                 .bind("src");

  auto copyCtors =
      cxxConstructExpr(
          isExpansionInMainFile(), hasArgument(0, src),
          hasDeclaration(
              decl(cxxConstructorDecl(isCopyConstructor()).bind("copyCtor"))))
          .bind("root");
  auto copyAssigns =
      cxxOperatorCallExpr(
          isExpansionInMainFile(), hasArgument(1, src), isAssignmentOperator(),
          hasDeclaration(
              cxxMethodDecl(isCopyAssignmentOperator()).bind("copyAssign")))
          .bind("root");

  auto allRefs = declRefExpr(isExpansionInMainFile(),
                             hasDeclaration(varDecl().bind("decl")))
                     .bind("allRefs");
  auto allStmts = stmt(isExpansionInMainFile()).bind("allStmts");
  auto bitwiseMoveable =
      classTemplateSpecializationDecl(
          hasName("BloombergLP::bslmf::IsBitwiseMoveable"),
          hasTemplateArgument(0,
                              refersToType(qualType().bind("bitwiseMoveable"))))
          .bind("classSpecialization");
  Finder->addMatcher(copyCtors, this);
  Finder->addMatcher(copyAssigns, this);
  Finder->addMatcher(allRefs, this);
  Finder->addMatcher(allStmts, this);
  Finder->addMatcher(bitwiseMoveable, this);
}

namespace {

template <typename T>
static std::string getExprText(const clang::ASTContext &astContext,
                               const clang::SourceManager &sourceManager,
                               const T *arg) {
  return clang::Lexer::getSourceText(
             clang::CharSourceRange::getTokenRange(arg->getSourceRange()),
             sourceManager, astContext.getLangOpts(), nullptr)
      .str();
}

// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// // See https://llvm.org/LICENSE.txt for license information.
// // SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
using namespace clang;
class FindEscaped {
public:
  static const VarDecl *isEscaped(const Stmt *S) {
    // Check for '&'. Any VarDecl whose address has been taken we treat as
    // escaped.
    // FIXME: What about references?
    if (auto *LE = llvm::dyn_cast<LambdaExpr>(S)) {
      return findLambdaReferenceCaptures(LE);
    }

    const UnaryOperator *U = llvm::dyn_cast<UnaryOperator>(S);
    if (!U)
      return nullptr;
    if (U->getOpcode() != UO_AddrOf)
      return nullptr;

    const Expr *E = U->getSubExpr()->IgnoreParenCasts();
    if (const DeclRefExpr *DR = llvm::dyn_cast<DeclRefExpr>(E))
      if (const VarDecl *VD = llvm::dyn_cast<VarDecl>(DR->getDecl()))
        return VD;

    return nullptr;
  }

  // Treat local variables captured by reference in C++ lambdas as escaped.
  static const VarDecl *findLambdaReferenceCaptures(const LambdaExpr *LE) {
    const CXXRecordDecl *LambdaClass = LE->getLambdaClass();
    llvm::DenseMap<const ValueDecl *, FieldDecl *> CaptureFields;
    FieldDecl *ThisCaptureField;
    LambdaClass->getCaptureFields(CaptureFields, ThisCaptureField);

    for (const LambdaCapture &C : LE->captures()) {
      if (!C.capturesVariable())
        continue;

      ValueDecl *VD = C.getCapturedVar();
      const FieldDecl *FD = CaptureFields[VD];
      if (!FD || !isa<VarDecl>(VD))
        continue;

      // If the capture field is a reference type, it is capture-by-reference.
      if (FD->getType()->isReferenceType())
        return cast<VarDecl>(VD);
    }
    return nullptr;
  }
};

} // namespace

class FindMoveCandidates {
  clang::ASTContext &d_ctx;
  clang::SourceManager &d_sm;

  SuggestMoveCheck &d_cb;

public:
  FindMoveCandidates(clang::ASTContext *ctx, clang::SourceManager *sm,
                     SuggestMoveCheck *cb)
      : d_ctx(*ctx), d_sm(*sm), d_cb(*cb) {}

  const clang::CXXRecordDecl *
  getCXXRecordDecl(const clang::DeclRefExpr *ref) const {
    auto refType = ref->getDecl()->getType().getTypePtr();
    if (!refType) {
      llvm::errs() << "Unexpected missing CxxRecordDecl for\n";
      refType->dump();
      assert(false);
    }

    const clang::CXXRecordDecl *recordDecl = refType->getAsCXXRecordDecl();
    assert(recordDecl);
    return recordDecl;
  }

  bool hasMoveConstructor(const clang::DeclRefExpr *ref) const {
    return getCXXRecordDecl(ref)->hasMoveConstructor();
  }
  bool hasMoveAssignment(const clang::DeclRefExpr *ref) const {
    return getCXXRecordDecl(ref)->hasMoveAssignment();
  }

  bool isStmtWithinCycle(const SuggestMoveCheck::Scope &scope,
                         const clang::Stmt *stmt) const {
    // Checks whether the Stmt is contained in a SCC in the CFG.

    std::unordered_set<const clang::CFGBlock *> visited;
    auto dfs = [&](auto dfs, const clang::CFGBlock *at) {
      if (visited.count(at)) {
        return true;
      }
      visited.insert(at);

      for (clang::CFGBlock::const_succ_iterator itr = at->succ_begin();
           itr != at->succ_end(); ++itr) {
        if (itr->isReachable()) {
          if (dfs(dfs, *itr)) {
            return true;
          }
        }
      }
      return false;
    };

    const clang::CFGBlock *block = scope.blockMap->blockContainingStmt(stmt);
    return dfs(dfs, block);
  }

  bool isMoveAlwaysSafe(const clang::DeclRefExpr *ref,
                        const SuggestMoveCheck::CopiedFromData &data) const {
    // If the ref occurs in the last expression statement of the compound
    // statement that declares the variable, then it should always be safe to do
    // a move. This assumes that the the move constructor/assignment operator
    // implement "move semantics" for the given object. We also restrict the
    // kind of expression statement that can appear.

    return false; // NOT IMPLEMENTED YET
  }

  enum MoveSafety {
    Never,
    Maybe,
    Always,
  };

  MoveSafety isTypeSafeToMove(const clang::DeclRefExpr *ref) const {
    return isTypeSafeToMove(ref->getType());
  }

  MoveSafety isTypeSafeToMove(clang::QualType type) const {
    type = type.getDesugaredType(d_ctx);
    if (d_cb.bitwiseMoveable().count(type.getTypePtr())) {
      return Always;
    }
    if (type.isTrivialType(d_ctx)) {
      return Always;
    }

    if (const clang::CXXRecordDecl *recordDecl = type->getAsCXXRecordDecl()) {
      return isTypeSafeToMove(recordDecl);
    }

    return Maybe;
  }

  MoveSafety isTypeSafeToMove(const clang::CXXRecordDecl *recordDecl) const {
    // BAS value semantic types, strings, and containers of "safe to move" types
    // are "safe to move types." Objects that manage resources, e.g. a mutex
    // lock, may change behavior if moved. E.g.,
    //
    //   {
    //     shared_ptr<scoped_lock<mutex>> lock = ...;
    //
    //     {
    //       shared_ptr<scoped_lock<mutex>> lock2(lock);
    //     }
    //
    //     critical_section();
    //   }
    //
    // This code will run with the mutex held when critical_section is called.
    // If lock is moved into lock2, then critical_section will be run without
    // the mutex held.

    static const std::unordered_set<std::string> neverSafe = {
        "std::unique_lock",
        "std::scoped_lock",
    };
    static const std::unordered_set<std::string> safeTypes = {
        "std::basic_string", "bsl::basic_string"};
    static const std::unordered_map<std::string, int> safeContainers = []() {
      std::vector<std::tuple<std::string, int>> types = {

          // Not a container, and technically could have a custom deleter...
          // should we remove this as not safe?
          {"shared_ptr", 1},

          {"array", 1},
          {"vector", 1},
          {"deque", 1},
          {"forward_list", 1},
          {"list", 1},

          {"set", 1},
          {"map", 2},
          {"multiset", 1},
          {"multimap", 2},

          {"unordered_set", 1},
          {"unordered_map", 2},
          {"unordered_multiset", 1},
          {"unordered_multimap", 2},

          {"stack", 1},
          {"queue", 1},
          {"priority_queu", 1},
      };

      std::unordered_map<std::string, int> safe;
      for (const auto &[type, n] : types) {
        safe["std::" + type] = n;
        safe["bsl::" + type] = n;
      }
      return safe;
    }();
    static const std::unordered_set<std::string> safeRecursiveTemplateTypes = {
        "std::tuple", "std::pair", "bsl::pair", "std::variant",
        "std::optional"};

    std::string name = recordDecl->getQualifiedNameAsString();

    auto reduceMoveSafety = [](MoveSafety a, MoveSafety b) -> MoveSafety {
      if (a == Never || b == Never) {
        return Never;
      }
      if (a == Maybe || b == Maybe) {
        return Maybe;
      }
      return Always;
    };

    if (safeContainers.count(name)) {
      const auto *classSpecialization =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(recordDecl);
      assert(classSpecialization);

      MoveSafety reducedSafety = Always;
      const clang::TemplateArgumentList &templateArgs =
          classSpecialization->getTemplateArgs();
      int numToExamine = safeContainers.find(name)->second;
      for (int i = 0; i != numToExamine; ++i) {
        reducedSafety = reduceMoveSafety(
            reducedSafety, isTypeSafeToMove(templateArgs.get(i).getAsType()));
      }
      return reducedSafety;
    }

    if (safeRecursiveTemplateTypes.count(name)) {
      const auto *classSpecialization =
          llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(recordDecl);
      assert(classSpecialization);

      MoveSafety reducedSafety = Always;
      const clang::TemplateArgumentList &templateArgs =
          classSpecialization->getTemplateArgs();
      int numTemplateTypes = templateArgs.size();
      for (int i = 0; i != numTemplateTypes; ++i) {
        const auto &templateArg = templateArgs.get(i);
        if (templateArg.getKind() == clang::TemplateArgument::Pack) {
          for (auto itr = templateArg.pack_begin();
               itr != templateArg.pack_end(); ++itr) {
            reducedSafety = reduceMoveSafety(
                reducedSafety, isTypeSafeToMove(itr->getAsType()));
          }
        } else {
          reducedSafety = reduceMoveSafety(
              reducedSafety, isTypeSafeToMove(templateArg.getAsType()));
        }
      }
      return reducedSafety;
    }

    if (safeTypes.count(name)) {
      return Always;
    }

    if (neverSafe.count(name)) {
      return Never;
    }

    return Maybe;
  }

  clang::SmallVector<const clang::Stmt *, 1>
  getParentStmts(const clang::Stmt *S) const {
    using namespace clang;
    auto *Context = &d_ctx;
    SmallVector<const Stmt *, 1> Result;

    TraversalKindScope RAII(*Context, TK_AsIs);
    DynTypedNodeList Parents = Context->getParents(*S);

    SmallVector<DynTypedNode, 1> NodesToProcess(Parents.begin(), Parents.end());

    while (!NodesToProcess.empty()) {
      DynTypedNode Node = NodesToProcess.back();
      NodesToProcess.pop_back();

      if (const auto *S = Node.get<Stmt>()) {
        Result.push_back(S);
      } else {
        Parents = Context->getParents(Node);
        NodesToProcess.append(Parents.begin(), Parents.end());
      }
    }

    return Result;
  }

  const clang::Stmt *getOuterStmt(const clang::Stmt *stmt) const {
    // OuterStmt is a bespoke concept that defines the smallest
    // clang::Stmt in an AST that can be considered safe to apply
    // the move optimization. The simplest example where this comes
    // in handy is to detect code like `consumes_strings(a,a`) where
    // the function call aceepts both parameters as `std::string`,
    // and we do not want to consider this asa  candidate for move
    // since there is no sequencing in argument evaluation order.
    // The OuterStmt would be the outer most clang::CallExpr.
    // If two DeclRefExpr (`a` in this case) map to the same OuterStmt
    // as would happen here, then neither are not candidates for move.

    for (const clang::Stmt *parent : getParentStmts(stmt)) {
      if (llvm::dyn_cast<clang::CompoundStmt>(parent)) {
        return stmt;
      }
      const clang::Stmt *fullExpr = getOuterStmt(parent);
      if (fullExpr) {
        return fullExpr;
      }
    }
    return nullptr;
  }

  void generateReplacement(const clang::Stmt *stmt) {}

  std::vector<clang::FixItHint> generateFixIts(const clang::Stmt *stmt) const {
    return {clang::FixItHint::CreateReplacement(
        clang::CharSourceRange::getTokenRange(stmt->getSourceRange()),
        "std::move(" + getExprText(d_ctx, d_sm, stmt) + ")")};
  }

  void report(const clang::DeclRefExpr *ref, bool isSafe, bool isConstruct,
              bool hasMoveCtor, bool hasMoveAssign) const {
    std::vector<clang::FixItHint> FixIts = generateFixIts(ref);

    std::string note = "move";
    if (isSafe) {
      note += "-safe";
    } else {
      note += "-unsafe";
    }
    if (isConstruct && !hasMoveCtor) {
      note += "-needs-move-ctor";
    } else if (!isConstruct && !hasMoveAssign) {
      note += "-needs-move-assign";
    }
    bool reportRemoveConstNote = false;
    if (ref->getType().isConstQualified()) {
      reportRemoveConstNote = true;
      note += "-remove-const";
    }

    if (note == "move-safe") {
    }

    d_cb.diag(ref->getBeginLoc(), "use std::move to avoid copy") << FixIts;

    if (reportRemoveConstNote) {
      // Notes are output based on the previously emitted diagnostic. We wait to
      // emit the note until after emitting the warning.
      // reportDiagnostic(
      //    *ref->getDecl(),
      //    "std::move candidate: Consider removing const to allow the variable
      //    to be moved "
      //    "[remove-const]",
      //    clang::DiagnosticsEngine::Note);
    }
  }

  void checkUsage(const clang::DeclRefExpr *ref,
                  const SuggestMoveCheck::CopiedFromData &data) const {
    const clang::VarDecl *decl = data.decl;
    const SuggestMoveCheck::Scope &scope = d_cb.scopes().find(decl)->second;

    if (decl->getStorageClass() != clang::SC_Auto &&
        decl->getStorageClass() != clang::SC_None) {
      return;
    }
    if (decl->getTLSKind() != clang::VarDecl::TLS_None) {
      return;
    }

    if (d_cb.escaped().count(decl)) {
      // Don't consider variables whose address is taken.
      return;
    }

    auto usageRange = d_cb.varUsages().equal_range(decl);
    std::vector<const clang::DeclRefExpr *> usages;
    std::transform(usageRange.first, usageRange.second,
                   std::back_inserter(usages),
                   [](const auto &itr) { return itr.second; });
    std::nth_element(usages.begin(), usages.end() - 1, usages.end(),
                     [&](const auto *a, const auto *b) {
                       return scope.sequence->inSequence(a, b);
                     });

    const clang::DeclRefExpr *lastUsage = usages[usages.size() - 1];
    if (lastUsage != ref) {
      // Definitely not the last reference.
      return;
    }

    auto sharesAnyOuterStmt = [&](const clang::Stmt *stmt) {
      const clang::Stmt *fullStmt = getOuterStmt(stmt);
      for (auto itr = usageRange.first; itr != usageRange.second; ++itr) {
        if (stmt == itr->second) {
          continue;
        }
        if (getOuterStmt(itr->second) == fullStmt) {
          return true;
        }
      }
      return false;
    };
    if (sharesAnyOuterStmt(ref)) {
      // If the last reference occurs in a OuterStmt that contains another
      // reference to the same variable, then we will assume there is a
      // potential non-sequence ordering in the order of evaluation. E.g.,
      // function call has no order of evaluation of its arguments.
      return;
    }

    if (isStmtWithinCycle(scope, ref)) {
      // If we're in a cycle in the CFG, give up. We could try to be
      // smarter, but that's too complicated for this tool's intent.
      return;
    }

    bool alwaysSafe = isMoveAlwaysSafe(ref, data);

    MoveSafety safety = isTypeSafeToMove(ref);
    if (safety == Never) {
      return;
    }

    bool isSafe = alwaysSafe || safety == Always;
    report(ref, isSafe, data.isConstruct, hasMoveConstructor(ref),
           hasMoveAssignment(ref));
  }

  void postProcessing() {
    for (const auto &[ref, decl] : d_cb.variablesCopiedFrom()) {
      std::string filename = d_sm.getFilename(ref->getBeginLoc()).str();
      checkUsage(ref, decl);
    }
  }
};

void SuggestMoveCheck::check(const MatchFinder::MatchResult &Result) {
  clang::ASTContext &ctx = *Result.Context;

  if (d_ctx) {
    assert(d_ctx == Result.Context);
  }
  if (d_sm) {
    assert(d_sm == Result.SourceManager);
  }

  d_ctx = Result.Context;
  d_sm = Result.SourceManager;

  if (Result.Nodes.getNodeAs<clang::CXXConstructorDecl>("copyCtor") ||
      Result.Nodes.getNodeAs<clang::CXXMethodDecl>("copyAssign")) {
    const bool isConstruct =
        Result.Nodes.getNodeAs<clang::CXXConstructorDecl>("copyCtor");
    const clang::DeclRefExpr *src =
        Result.Nodes.getNodeAs<clang::DeclRefExpr>("src");
    const clang::VarDecl *srcDecl =
        Result.Nodes.getNodeAs<clang::VarDecl>("srcDecl");
    d_variablesCopiedFrom[src] = {isConstruct, srcDecl};

    clang::CompoundStmt *scope = const_cast<clang::CompoundStmt *>(
        Result.Nodes.getNodeAs<clang::CompoundStmt>("scope"));
    clang::CFG::BuildOptions options;
    std::unique_ptr<clang::CFG> cfg =
        clang::CFG::buildCFG(nullptr, scope, &ctx, options);
    auto sequence = std::make_unique<ExprSequence>(cfg.get(), scope, &ctx);
    auto blockMap = std::make_unique<StmtToBlockMap>(cfg.get(), &ctx);
    d_scopes[srcDecl] =
        Scope{scope, std::move(cfg), std::move(sequence), std::move(blockMap)};
  } else if (const auto *ref =
                 (Result.Nodes.getNodeAs<clang::DeclRefExpr>("allRefs"))) {
    const clang::VarDecl *decl = Result.Nodes.getNodeAs<clang::VarDecl>("decl");
    d_varUsages.emplace(decl, ref);
  } else if (const auto *ref =
                 (Result.Nodes.getNodeAs<clang::Stmt>("allStmts"))) {
    if (const clang::VarDecl *varDecl = FindEscaped::isEscaped(ref)) {
      d_escaped.insert(varDecl);
    }
  } else if (const auto *type =
                 (Result.Nodes.getNodeAs<clang::QualType>("bitwiseMoveable"))) {
    const auto *recordDecl =
        Result.Nodes.getNodeAs<clang::ClassTemplateSpecializationDecl>(
            "classSpecialization");
    assert(recordDecl);

    if (isTrueType(recordDecl)) {
      d_bitwiseMoveable.insert(type->getTypePtr());
    }
  } else {
    assert(false && "Unhandled match");
  }
}

void SuggestMoveCheck::onEndOfTranslationUnit() {
  FindMoveCandidates finder(d_ctx, d_sm, this);
  finder.postProcessing();
  d_ctx = nullptr;
  d_sm = nullptr;
}

} // namespace modernize
} // namespace tidy
} // namespace clang
