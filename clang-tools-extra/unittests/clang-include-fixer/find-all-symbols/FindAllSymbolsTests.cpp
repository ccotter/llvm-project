//===-- FindAllSymbolsTests.cpp - find all symbols unit tests ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FindAllSymbolsAction.h"
#include "HeaderMapCollector.h"
#include "SymbolInfo.h"
#include "SymbolReporter.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/FileSystemOptions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/PCHContainerOperations.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "gtest/gtest.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace clang {
namespace find_all_symbols {

static const char HeaderName[] = "symbols.h";

class TestSymbolReporter : public SymbolReporter {
public:
  ~TestSymbolReporter() override {}

  void reportSymbols(llvm::StringRef FileName,
                     const SymbolInfo::SignalMap &NewSymbols) override {
    for (const auto &Entry : NewSymbols)
      Symbols[Entry.first] += Entry.second;
  }

  int seen(const SymbolInfo &Symbol) const {
    auto it = Symbols.find(Symbol);
    return it == Symbols.end() ? 0 : it->second.Seen;
  }

  int used(const SymbolInfo &Symbol) const {
    auto it = Symbols.find(Symbol);
    return it == Symbols.end() ? 0 : it->second.Used;
  }

private:
  SymbolInfo::SignalMap Symbols;
};

class FindAllSymbolsTest : public ::testing::Test {
public:
  int seen(const SymbolInfo &Symbol) { return Reporter.seen(Symbol); }

  int used(const SymbolInfo &Symbol) { return Reporter.used(Symbol); }

  bool runFindAllSymbols(StringRef HeaderCode, StringRef MainCode) {
    llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> InMemoryFileSystem(
        new llvm::vfs::InMemoryFileSystem);
    llvm::IntrusiveRefCntPtr<FileManager> Files(
        new FileManager(FileSystemOptions(), InMemoryFileSystem));

    std::string FileName = "symbol.cc";

    const std::string InternalHeader = "internal/internal_header.h";
    const std::string TopHeader = "<top>";
    // Test .inc header path. The header for `IncHeaderClass` should be
    // internal.h, which will eventually be mapped to <top>.
    std::string IncHeader = "internal/private.inc";
    std::string IncHeaderCode = "class IncHeaderClass {};";

    HeaderMapCollector::RegexHeaderMap RegexMap = {
        {R"(internal_.*\.h$)", TopHeader.c_str()},
    };

    std::string InternalCode =
        "#include \"private.inc\"\nclass Internal {};";
    SymbolInfo InternalSymbol("Internal", SymbolInfo::SymbolKind::Class,
                              TopHeader, {});
    SymbolInfo IncSymbol("IncHeaderClass", SymbolInfo::SymbolKind::Class,
                         TopHeader, {});
    InMemoryFileSystem->addFile(
        IncHeader, 0, llvm::MemoryBuffer::getMemBuffer(IncHeaderCode));
    InMemoryFileSystem->addFile(InternalHeader, 0,
                                llvm::MemoryBuffer::getMemBuffer(InternalCode));

    std::unique_ptr<tooling::FrontendActionFactory> Factory(
        new FindAllSymbolsActionFactory(&Reporter, &RegexMap));

    tooling::ToolInvocation Invocation(
        {std::string("find_all_symbols"), std::string("-fsyntax-only"),
         std::string("-std=c++11"), FileName},
        Factory->create(), Files.get(),
        std::make_shared<PCHContainerOperations>());

    InMemoryFileSystem->addFile(HeaderName, 0,
                                llvm::MemoryBuffer::getMemBuffer(HeaderCode));

    std::string Content = "#include\"" + std::string(HeaderName) +
                          "\"\n"
                          "#include \"" +
                          InternalHeader + "\"";
#if !defined(_MSC_VER) && !defined(__MINGW32__)
    // Test path cleaning for both decls and macros.
    const std::string DirtyHeader = "./internal/./a/b.h";
    Content += "\n#include \"" + DirtyHeader + "\"";
    const std::string CleanHeader = "internal/a/b.h";
    const std::string DirtyHeaderContent =
        "#define INTERNAL 1\nclass ExtraInternal {};";
    InMemoryFileSystem->addFile(
        DirtyHeader, 0, llvm::MemoryBuffer::getMemBuffer(DirtyHeaderContent));
    SymbolInfo DirtyMacro("INTERNAL", SymbolInfo::SymbolKind::Macro,
                          CleanHeader, {});
    SymbolInfo DirtySymbol("ExtraInternal", SymbolInfo::SymbolKind::Class,
                           CleanHeader, {});
#endif // _MSC_VER && __MINGW32__
    Content += "\n" + MainCode.str();
    InMemoryFileSystem->addFile(FileName, 0,
                                llvm::MemoryBuffer::getMemBuffer(Content));
    Invocation.run();
    EXPECT_EQ(1, seen(InternalSymbol));
    EXPECT_EQ(1, seen(IncSymbol));
#if !defined(_MSC_VER) && !defined(__MINGW32__)
    EXPECT_EQ(1, seen(DirtySymbol));
    EXPECT_EQ(1, seen(DirtyMacro));
#endif  // _MSC_VER && __MINGW32__
    return true;
  }

protected:
  TestSymbolReporter Reporter;
};

TEST_F(FindAllSymbolsTest, VariableSymbols) {
  static const std::array<char, 132> Header = { R"(
      extern int xargc;
      namespace na {
      static bool SSSS = false;
      namespace nb { const long long *XXXX; }
      })" };
  static const std::array<char, 83> Main = { R"(
      auto y = &na::nb::XXXX;
      int main() { if (na::SSSS) return xargc; }
  )" };
  runFindAllSymbols(Header.begin(), Main.begin());

  SymbolInfo Symbol =
      SymbolInfo("xargc", SymbolInfo::SymbolKind::Variable, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("SSSS", SymbolInfo::SymbolKind::Variable, HeaderName,
                      {{SymbolInfo::ContextType::Namespace, "na"}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("XXXX", SymbolInfo::SymbolKind::Variable, HeaderName,
                      {{SymbolInfo::ContextType::Namespace, "nb"},
                       {SymbolInfo::ContextType::Namespace, "na"}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
}

TEST_F(FindAllSymbolsTest, ExternCSymbols) {
  static const std::array<char, 114> Header = { R"(
      extern "C" {
      int C_Func() { return 0; }
      struct C_struct {
        int Member;
      };
      })" };
  static const std::array<char, 83> Main = { R"(
      C_struct q() {
        int(*ptr)() = C_Func;
        return {0};
      }
  )" };
  runFindAllSymbols(Header.begin(), Main.begin());

  SymbolInfo Symbol =
      SymbolInfo("C_Func", SymbolInfo::SymbolKind::Function, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol =
      SymbolInfo("C_struct", SymbolInfo::SymbolKind::Class, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
}

TEST_F(FindAllSymbolsTest, CXXRecordSymbols) {
  static const std::array<char, 256> Header = { R"(
      struct Glob {};
      struct A; // Not a definition, ignored.
      class NOP; // Not a definition, ignored
      namespace na {
      struct A {
        struct AAAA {};
        int x;
        int y;
        void f() {}
      };
      };  //
      )" };
  static const std::array<char, 57> Main = { R"(
      static Glob glob;
      static na::A::AAAA* a;
  )" };
  runFindAllSymbols(Header.begin(), Main.begin());

  SymbolInfo Symbol =
      SymbolInfo("Glob", SymbolInfo::SymbolKind::Class, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("A", SymbolInfo::SymbolKind::Class, HeaderName,
                      {{SymbolInfo::ContextType::Namespace, "na"}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("AAA", SymbolInfo::SymbolKind::Class, HeaderName,
                      {{SymbolInfo::ContextType::Record, "A"},
                       {SymbolInfo::ContextType::Namespace, "na"}});
  EXPECT_EQ(0, seen(Symbol));
  EXPECT_EQ(0, used(Symbol));
}

TEST_F(FindAllSymbolsTest, CXXRecordSymbolsTemplate) {
  static const std::array<char, 357> Header = { R"(
      template <typename T>
      struct T_TEMP {
        template <typename _Tp1>
        struct rebind { typedef T_TEMP<_Tp1> other; };
      };
      // Ignore specialization.
      template class T_TEMP<char>;

      template <typename T>
      class Observer {
      };
      // Ignore specialization.
      template <> class Observer<int> {};
      )" };
  static const std::array<char, 51> Main = { R"(
      extern T_TEMP<int>::rebind<char> weirdo;
  )" };
  runFindAllSymbols(Header.begin(), Main.begin());

  SymbolInfo Symbol =
      SymbolInfo("T_TEMP", SymbolInfo::SymbolKind::Class, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
}

TEST_F(FindAllSymbolsTest, DontIgnoreTemplatePartialSpecialization) {
  static const std::array<char, 217> Code = { R"(
      template<class> class Class; // undefined
      template<class R, class... ArgTypes>
      class Class<R(ArgTypes...)> {
      };

      template<class T> void f() {};
      template<> void f<int>() {};
      )" };
  runFindAllSymbols(Code.begin(), "");
  SymbolInfo Symbol =
      SymbolInfo("Class", SymbolInfo::SymbolKind::Class, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  Symbol = SymbolInfo("f", SymbolInfo::SymbolKind::Function, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
}

TEST_F(FindAllSymbolsTest, FunctionSymbols) {
  static const std::array<char, 321> Header = { R"(
      namespace na {
      int gg(int);
      int f(const int &a) { int Local; static int StaticLocal; return 0; }
      static void SSSFFF() {}
      }  // namespace na
      namespace na {
      namespace nb {
      template<typename T>
      void fun(T t) {};
      } // namespace nb
      } // namespace na";
      )" };
  static const std::array<char, 141> Main = { R"(
      int(*gg)(int) = &na::gg;
      int main() {
        (void)na::SSSFFF;
        na::nb::fun(0);
        return na::f(gg(0));
      }
  )" };
  runFindAllSymbols(Header.begin(), Main.begin());

  SymbolInfo Symbol =
      SymbolInfo("gg", SymbolInfo::SymbolKind::Function, HeaderName,
                 {{SymbolInfo::ContextType::Namespace, "na"}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("f", SymbolInfo::SymbolKind::Function, HeaderName,
                      {{SymbolInfo::ContextType::Namespace, "na"}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("SSSFFF", SymbolInfo::SymbolKind::Function, HeaderName,
                      {{SymbolInfo::ContextType::Namespace, "na"}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("fun", SymbolInfo::SymbolKind::Function, HeaderName,
                      {{SymbolInfo::ContextType::Namespace, "nb"},
                       {SymbolInfo::ContextType::Namespace, "na"}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
}

TEST_F(FindAllSymbolsTest, NamespaceTest) {
  static const std::array<char, 193> Header = { R"(
      int X1;
      namespace { int X2; }
      namespace { namespace { int X3; } }
      namespace { namespace nb { int X4; } }
      namespace na { inline namespace __1 { int X5; } }
      )" };
  static const std::array<char, 113> Main = { R"(
      using namespace nb;
      int main() {
        X1 = X2;
        X3 = X4;
        (void)na::X5;
      }
  )" };
  runFindAllSymbols(Header.begin(), Main.begin());

  SymbolInfo Symbol =
      SymbolInfo("X1", SymbolInfo::SymbolKind::Variable, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("X2", SymbolInfo::SymbolKind::Variable, HeaderName,
                      {{SymbolInfo::ContextType::Namespace, ""}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("X3", SymbolInfo::SymbolKind::Variable, HeaderName,
                      {{SymbolInfo::ContextType::Namespace, ""},
                       {SymbolInfo::ContextType::Namespace, ""}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("X4", SymbolInfo::SymbolKind::Variable, HeaderName,
                      {{SymbolInfo::ContextType::Namespace, "nb"},
                       {SymbolInfo::ContextType::Namespace, ""}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("X5", SymbolInfo::SymbolKind::Variable, HeaderName,
                      {{SymbolInfo::ContextType::Namespace, "na"}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
}

TEST_F(FindAllSymbolsTest, DecayedTypeTest) {
  static const std::array<char, 40> Header = { "void DecayedFunc(int x[], int y[10]) {}" };
  static const std::array<char, 46> Main = { R"(int main() { DecayedFunc(nullptr, nullptr); })" };
  runFindAllSymbols(Header.begin(), Main.begin());
  SymbolInfo Symbol = SymbolInfo(
      "DecayedFunc", SymbolInfo::SymbolKind::Function, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
}

TEST_F(FindAllSymbolsTest, CTypedefTest) {
  static const std::array<char, 95> Header = { R"(
      typedef unsigned size_t_;
      typedef struct { int x; } X;
      using XX = X;
      )" };
  static const std::array<char, 115> Main = { R"(
      size_t_ f;
      template<typename T> struct vector{};
      vector<X> list;
      void foo(const XX&){}
  )" };
  runFindAllSymbols(Header.begin(), Main.begin());

  SymbolInfo Symbol = SymbolInfo("size_t_", SymbolInfo::SymbolKind::TypedefName,
                                 HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("X", SymbolInfo::SymbolKind::TypedefName, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol =
      SymbolInfo("XX", SymbolInfo::SymbolKind::TypedefName, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
}

TEST_F(FindAllSymbolsTest, EnumTest) {
  static const std::array<char, 203> Header = { R"(
      enum Glob_E { G1, G2 };
      enum class Altitude { high='h', low='l'};
      enum { A1, A2 };
      class A {
      public:
        enum A_ENUM { X1, X2 };
      };
      enum DECL : int;
      )" };
  static const std::array<char, 177> Main = { R"(
      static auto flags = G1 | G2;
      static auto alt = Altitude::high;
      static auto nested = A::X1;
      extern DECL whatever;
      static auto flags2 = A1 | A2;
  )" };
  runFindAllSymbols(Header.begin(), Main.begin());

  SymbolInfo Symbol =
      SymbolInfo("Glob_E", SymbolInfo::SymbolKind::EnumDecl, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(0, used(Symbol));

  Symbol =
      SymbolInfo("G1", SymbolInfo::SymbolKind::EnumConstantDecl, HeaderName,
                 {{SymbolInfo::ContextType::EnumDecl, "Glob_E"}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol =
      SymbolInfo("G2", SymbolInfo::SymbolKind::EnumConstantDecl, HeaderName,
                 {{SymbolInfo::ContextType::EnumDecl, "Glob_E"}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol =
      SymbolInfo("Altitude", SymbolInfo::SymbolKind::EnumDecl, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
  Symbol =
      SymbolInfo("high", SymbolInfo::SymbolKind::EnumConstantDecl, HeaderName,
                 {{SymbolInfo::ContextType::EnumDecl, "Altitude"}});
  EXPECT_EQ(0, seen(Symbol));
  EXPECT_EQ(0, used(Symbol));

  Symbol = SymbolInfo("A1", SymbolInfo::SymbolKind::EnumConstantDecl,
                      HeaderName, {{SymbolInfo::ContextType::EnumDecl, ""}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
  Symbol = SymbolInfo("A2", SymbolInfo::SymbolKind::EnumConstantDecl,
                      HeaderName, {{SymbolInfo::ContextType::EnumDecl, ""}});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
  Symbol = SymbolInfo("", SymbolInfo::SymbolKind::EnumDecl, HeaderName, {});
  EXPECT_EQ(0, seen(Symbol));
  EXPECT_EQ(0, used(Symbol));

  Symbol = SymbolInfo("A_ENUM", SymbolInfo::SymbolKind::EnumDecl, HeaderName,
                      {{SymbolInfo::ContextType::Record, "A"}});
  EXPECT_EQ(0, seen(Symbol));
  EXPECT_EQ(0, used(Symbol));

  Symbol = SymbolInfo("X1", SymbolInfo::SymbolKind::EnumDecl, HeaderName,
                      {{SymbolInfo::ContextType::EnumDecl, "A_ENUM"},
                       {SymbolInfo::ContextType::Record, "A"}});
  EXPECT_EQ(0, seen(Symbol));

  Symbol = SymbolInfo("DECL", SymbolInfo::SymbolKind::EnumDecl, HeaderName, {});
  EXPECT_EQ(0, seen(Symbol));
}

TEST_F(FindAllSymbolsTest, IWYUPrivatePragmaTest) {
  static const std::array<char, 73> Header = { R"(
    // IWYU pragma: private, include "bar.h"
    struct Bar {
    };
  )" };
  static const std::array<char, 17> Main = { R"(
    Bar bar;
  )" };
  runFindAllSymbols(Header.begin(), Main.begin());

  SymbolInfo Symbol =
      SymbolInfo("Bar", SymbolInfo::SymbolKind::Class, "bar.h", {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
}

TEST_F(FindAllSymbolsTest, MacroTest) {
  static const std::array<char, 80> Header = { R"(
    #define X
    #define Y 1
    #define MAX(X, Y) ((X) > (Y) ? (X) : (Y))
  )" };
  static const std::array<char, 64> Main = { R"(
    #ifdef X
    int main() { return MAX(0,Y); }
    #endif
  )" };
  runFindAllSymbols(Header.begin(), Main.begin());
  SymbolInfo Symbol =
      SymbolInfo("X", SymbolInfo::SymbolKind::Macro, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("Y", SymbolInfo::SymbolKind::Macro, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("MAX", SymbolInfo::SymbolKind::Macro, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
}

TEST_F(FindAllSymbolsTest, MacroTestWithIWYU) {
  static const std::array<char, 127> Header = { R"(
    // IWYU pragma: private, include "bar.h"
    #define X 1
    #define Y 1
    #define MAX(X, Y) ((X) > (Y) ? (X) : (Y))
  )" };
  static const std::array<char, 64> Main = { R"(
    #ifdef X
    int main() { return MAX(0,Y); }
    #endif
  )" };
  runFindAllSymbols(Header.begin(), Main.begin());
  SymbolInfo Symbol =
      SymbolInfo("X", SymbolInfo::SymbolKind::Macro, "bar.h", {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("Y", SymbolInfo::SymbolKind::Macro, "bar.h", {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));

  Symbol = SymbolInfo("MAX", SymbolInfo::SymbolKind::Macro, "bar.h", {});
  EXPECT_EQ(1, seen(Symbol));
  EXPECT_EQ(1, used(Symbol));
}

TEST_F(FindAllSymbolsTest, NoFriendTest) {
  static const std::array<char, 94> Header = { R"(
    class WorstFriend {
      friend void Friend();
      friend class BestFriend;
    };
  )" };
  runFindAllSymbols(Header.begin(), "");
  SymbolInfo Symbol =
      SymbolInfo("WorstFriend", SymbolInfo::SymbolKind::Class, HeaderName, {});
  EXPECT_EQ(1, seen(Symbol));

  Symbol =
      SymbolInfo("Friend", SymbolInfo::SymbolKind::Function, HeaderName, {});
  EXPECT_EQ(0, seen(Symbol));

  Symbol =
      SymbolInfo("BestFriend", SymbolInfo::SymbolKind::Class, HeaderName, {});
  EXPECT_EQ(0, seen(Symbol));
}

} // namespace find_all_symbols
} // namespace clang
