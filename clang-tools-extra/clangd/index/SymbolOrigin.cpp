//===--- SymbolOrigin.cpp ----------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SymbolOrigin.h"

#include <array>

namespace clang {
namespace clangd {

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, SymbolOrigin O) {
  if (O == SymbolOrigin::Unknown)
    return OS << "unknown";
  constexpr static std::array<char, 17> Sigils = { "AOSMIRP7BL012345" };
  for (unsigned I = 0; I < sizeof(Sigils); ++I)
    if (static_cast<uint16_t>(O) & 1u << I)
      OS << Sigils[I];
  return OS;
}

} // namespace clangd
} // namespace clang
