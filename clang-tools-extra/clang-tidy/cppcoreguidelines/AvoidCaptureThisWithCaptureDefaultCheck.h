//===--- AvoidCaptureThisWithCaptureDefaultCheck.h - clang-tidy -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CPPCOREGUIDELINES_AVOIDCAPTURETHISWITHCAPTUREDEFAULTCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CPPCOREGUIDELINES_AVOIDCAPTURETHISWITHCAPTUREDEFAULTCHECK_H

#include "../ClangTidyCheck.h"

namespace clang {
namespace tidy {
namespace cppcoreguidelines {

/// Capture defaults lambas defined in in member functions can be misleading
/// about whether capturing data member is by value or reference. For example,
/// [=] will capture local variables by value but member variables by
/// reference. CppCoreGuideline F.54 suggests to always be explicit
/// and never specify a capture default when also capturing this.
///
/// For the user-facing documentation see:
/// http://clang.llvm.org/extra/clang-tidy/checks/cppcoreguidelines/avoid-capture-this-with-capture-default.html
class AvoidCaptureThisWithCaptureDefaultCheck : public ClangTidyCheck {
public:
  AvoidCaptureThisWithCaptureDefaultCheck(StringRef Name,
                                          ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
  bool isLanguageVersionSupported(const LangOptions &LangOpts) const override {
    return LangOpts.CPlusPlus11;
  }
};

} // namespace cppcoreguidelines
} // namespace tidy
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CPPCOREGUIDELINES_AVOIDCAPTURETHISWITHCAPTUREDEFAULTCHECK_H
