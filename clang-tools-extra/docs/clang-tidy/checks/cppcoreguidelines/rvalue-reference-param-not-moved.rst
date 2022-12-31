.. title:: clang-tidy - cppcoreguidelines-rvalue-reference-param-not-moved

cppcoreguidelines-rvalue-reference-param-not-moved
==================================================

Warns when an rvalue reference function parameter is never moved within
the function body.

Rvalue reference parameters indicate a parameter that should be moved with
``std::move`` from within the function body. Any such parameter that is
never moved is confusing and potentially indicative of a buggy program.

Examples:

.. code-block:: c++

  void logic(std::string&& Input) {
    std::string Copy(Input); // Oops - forgot to std::move
  }

This check implements
`CppCoreGuideline F.18 <http://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#f18-for-will-move-from-parameters-pass-by-x-and-stdmove-the-parameter>`_.
