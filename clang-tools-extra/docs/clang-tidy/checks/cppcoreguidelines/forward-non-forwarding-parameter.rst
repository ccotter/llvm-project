.. title:: clang-tidy - cppcoreguidelines-forward-non-forwarding-parameter

cppcoreguidelines-forward-non-forwarding-parameter
==================================================

Warns when ``std::forward`` is used on a non-forwarding reference.
``std::forward`` is a named cast that preserves the value category of
the input parameter. It is used in templated functions when the
parameter should be "forwarded" to another function or constructor
within the function template body. Using ``std::forward`` on
non-forwarding references can lead to unexpected or buggy behavior.

Example:

.. code-block:: c++

  void by_lvalue_ref(Object& Input) {
    // This will actually move Input, always. This is confusing
    // since the caller of by_lvalue_ref did not std::move, and
    // could lead to use after move bugs.

    // If a copy was intended, remove std::forward.
    // If a move is intended, change Input to 'Object&&' or 'Object'
    // and update the caller.
    If that is what is intended, use std::move.
    Object Copy(std::forward<Object>(Input));
  }

  void move_param(Object&& Input) {
    Object Copy(std::move(Input)); // Ok
    Object Copy2(std::forward<Object>(Input)); // Use std::move instead
  }

This check implements the ``std::forward`` specific enforcements of
`CppCoreGuideline ES.56 <http://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#es56-write-stdmove-only-when-you-need-to-explicitly-move-an-object-to-another-scope>
