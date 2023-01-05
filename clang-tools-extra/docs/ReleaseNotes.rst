====================================================
Extra Clang Tools |release| |ReleaseNotesTitle|
====================================================

.. contents::
   :local:
   :depth: 3

Written by the `LLVM Team <https://llvm.org/>`_

.. only:: PreRelease

  .. warning::
     These are in-progress notes for the upcoming Extra Clang Tools |version| release.
     Release notes for previous releases can be found on
     `the Download Page <https://releases.llvm.org/download.html>`_.

Introduction
============

This document contains the release notes for the Extra Clang Tools, part of the
Clang release |release|. Here we describe the status of the Extra Clang Tools in
some detail, including major improvements from the previous release and new
feature work. All LLVM releases may be downloaded from the `LLVM releases web
site <https://llvm.org/releases/>`_.

For more information about Clang or LLVM, including information about
the latest release, please see the `Clang Web Site <https://clang.llvm.org>`_ or
the `LLVM Web Site <https://llvm.org>`_.

Note that if you are reading this file from a Git checkout or the
main Clang web page, this document applies to the *next* release, not
the current one. To see the release notes for a specific release, please
see the `releases page <https://llvm.org/releases/>`_.

What's New in Extra Clang Tools |release|?
==========================================

Some of the major new features and improvements to Extra Clang Tools are listed
here. Generic improvements to Extra Clang Tools as a whole or to its underlying
infrastructure are described first, followed by tool-specific sections.

Major New Features
------------------

...

Improvements to clangd
----------------------

Inlay hints
^^^^^^^^^^^

Diagnostics
^^^^^^^^^^^

Semantic Highlighting
^^^^^^^^^^^^^^^^^^^^^

Compile flags
^^^^^^^^^^^^^

Hover
^^^^^

Code completion
^^^^^^^^^^^^^^^

Signature help
^^^^^^^^^^^^^^

Cross-references
^^^^^^^^^^^^^^^^

Objective-C
^^^^^^^^^^^

Miscellaneous
^^^^^^^^^^^^^

Improvements to clang-doc
-------------------------

Improvements to clang-query
---------------------------

The improvements are...

Improvements to clang-rename
----------------------------

The improvements are...

Improvements to clang-tidy
--------------------------

- Preprocessor-level module header parsing is now disabled by default due to
  the problems it caused in C++20 and above, leading to performance and code
  parsing issues regardless of whether modules were used or not. This change
  will impact only the following checks:
  :doc:`modernize-replace-disallow-copy-and-assign-macro
  <clang-tidy/checks/modernize/replace-disallow-copy-and-assign-macro>`,
  :doc:`bugprone-reserved-identifier
  <clang-tidy/checks/bugprone/reserved-identifier>`, and
  :doc:`readability-identifier-naming
  <clang-tidy/checks/readability/identifier-naming>`. Those checks will no
  longer see macros defined in modules. Users can still enable this
  functionality using the newly added command line option
  `--enable-module-headers-parsing`.

- Remove configuration option `AnalyzeTemporaryDestructors`, which was deprecated since
  :program:`clang-tidy` 16.

- Improved `--dump-config` to print check options in alphabetical order.

New checks
^^^^^^^^^^

- New :doc:`bugprone-multi-level-implicit-pointer-conversion
  <clang-tidy/checks/bugprone/multi-level-implicit-pointer-conversion>` check.

  Detects implicit conversions between pointers of different levels of
  indirection.

- New :doc:`performance-enum-size
  <clang-tidy/checks/performance/enum-size>` check.

  Recommends the smallest possible underlying type for an ``enum`` or ``enum``
  class based on the range of its enumerators.
  Detect implicit and explicit casts of ``enum`` type into ``bool`` where ``enum`` type
  doesn't have a zero-value enumerator.

- New :doc:`bugprone-unsafe-functions
  <clang-tidy/checks/bugprone/unsafe-functions>` check.

  Checks for functions that have safer, more secure replacements available, or
  are considered deprecated due to design flaws.
  This check relies heavily on, but is not exclusive to, the functions from
  the *Annex K. "Bounds-checking interfaces"* of C11.

- New :doc:`cppcoreguidelines-avoid-capturing-lambda-coroutines
  <clang-tidy/checks/cppcoreguidelines/avoid-capturing-lambda-coroutines>` check.

  Flags C++20 coroutine lambdas with non-empty capture lists that may cause
  use-after-free errors and suggests avoiding captures or ensuring the lambda
  closure object has a guaranteed lifetime.

- New :doc:`cppcoreguidelines-misleading-capture-default-by-value
  <clang-tidy/checks/cppcoreguidelines/misleading-capture-default-by-value>` check.

  Warns when lambda specify a by-value capture default and capture ``this``.

- New :doc:`cppcoreguidelines-rvalue-reference-param-not-moved
  <clang-tidy/checks/cppcoreguidelines/rvalue-reference-param-not-moved>` check.

  Warns when an rvalue reference function parameter is never moved within
  the function body.

- New :doc:`llvmlibc-inline-function-decl
  <clang-tidy/checks/llvmlibc/inline-function-decl>` check.

  Checks that all implicit and explicit inline functions in header files are
  tagged with the ``LIBC_INLINE`` macro.

- New :doc:`modernize-type-traits
  <clang-tidy/checks/modernize/type-traits>` check.

  Converts standard library type traits of the form ``traits<...>::type`` and
  ``traits<...>::value`` into ``traits_t<...>`` and ``traits_v<...>`` respectively.

- New :doc:`performance-avoid-endl
  <clang-tidy/checks/performance/avoid-endl>` check.

  Finds uses of ``std::endl`` on streams and replaces them with ``'\n'``.

- New :doc:`modernize-use-constraints
  <clang-tidy/checks/modernize/use-constraints>` check.

  Replace ``enable_if`` with C++20 requires clauses.

- New :doc:`readability-avoid-unconditional-preprocessor-if
  <clang-tidy/checks/readability/avoid-unconditional-preprocessor-if>` check.

  Finds code blocks that are constantly enabled or disabled in preprocessor
  directives by analyzing ``#if`` conditions, such as ``#if 0`` and
  ``#if 1``, etc.

- New :doc:`readability-operators-representation
  <clang-tidy/checks/readability/operators-representation>` check.

  Enforces consistent token representation for invoked binary, unary and
  overloaded operators in C++ code.

New check aliases
^^^^^^^^^^^^^^^^^

Changes in existing checks
^^^^^^^^^^^^^^^^^^^^^^^^^^

Removed checks
^^^^^^^^^^^^^^

Improvements to include-fixer
-----------------------------

The improvements are...

Improvements to clang-include-fixer
-----------------------------------

The improvements are...

Improvements to modularize
--------------------------

The improvements are...

Improvements to pp-trace
------------------------

Clang-tidy Visual Studio plugin
-------------------------------
