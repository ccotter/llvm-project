//===--- ForwardNonForwardingParameterCheck.h - clang-tidy ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CPPCOREGUIDELINES_FORWARDNONFORWARDINGPARAMETERCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CPPCOREGUIDELINES_FORWARDNONFORWARDINGPARAMETERCHECK_H

#include "../ClangTidyCheck.h"

namespace clang::tidy::cppcoreguidelines {

/// Warns when std::forward is called on a non-forwarding reference. This
/// check implements the std::forward specific enforcement section of
/// CppCoreGuideline ES.56.
///
/// For the user-facing documentation see:
/// http://clang.llvm.org/extra/clang-tidy/checks/cppcoreguidelines/forward-non-forwarding-parameter.html
class ForwardNonForwardingParameterCheck : public ClangTidyCheck {
public:
  ForwardNonForwardingParameterCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::cppcoreguidelines

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CPPCOREGUIDELINES_FORWARDNONFORWARDINGPARAMETERCHECK_H
