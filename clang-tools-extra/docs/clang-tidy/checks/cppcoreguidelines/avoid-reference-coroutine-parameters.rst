.. title:: clang-tidy - cppcoreguidelines-avoid-reference-coroutine-parameters

cppcoreguidelines-avoid-reference-coroutine-parameters
======================================================

Warns when a coroutine accepts reference parameters. After a coroutine suspend point,
references could be dangling and no longer valid. Instead, pass parameters as values.

This check implements
[CppCoreGuideline CP.53](http://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines).

Examples:

.. code-block:: c++

  std::future<int> someCoroutine(int& val) {
    co_await ...;
    // When the coroutine is resume, 'val' might no longer be valid.
    if (val) ...
  }
