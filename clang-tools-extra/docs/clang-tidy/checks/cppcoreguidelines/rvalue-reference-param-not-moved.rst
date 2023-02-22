.. title:: clang-tidy - cppcoreguidelines-rvalue-reference-param-not-moved

cppcoreguidelines-rvalue-reference-param-not-moved
==================================================

Warns when an rvalue reference function parameter is never moved within
the function body.

Rvalue reference parameters indicate a parameter that should be moved with
``std::move`` from within the function body. Any such parameter that is
never moved is confusing and potentially indicative of a buggy program.

Example:

.. code-block:: c++

  void logic(std::string&& Input) {
    std::string Copy(Input); // Oops - forgot to std::move
  }

Options
-------

.. option:: AllowAnyMoveExpr

   If set to `true`, the check accepts ``std::move`` calls containing any
   subexpression containing the parameter. CppCoreGuideline F.18 officially
   mandates that the parameter itself must be moved. Default is `false`.

  .. code-block:: c++

    // 'p' is flagged by this check if and only if AllowAnyMoveExpr is false
    void move_members_of(pair<Obj, Obj>&& p) {
      pair<Obj, Obj> other;
      other.first = std::move(p.first);
      other.second = std::move(p.second);
    }

    // 'p' is never flagged by this check
    void move_whole_pair(pair<Obj, Obj>&& p) {
      pair<Obj, Obj> other = std::move(p);
    }

.. option:: IgnoreUnnamedParams

   If set to `true`, the check ignores unnamed rvalue reference parameters.
   Default is `false`.

This check implements
`CppCoreGuideline F.18 <http://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#f18-for-will-move-from-parameters-pass-by-x-and-stdmove-the-parameter>`_.
