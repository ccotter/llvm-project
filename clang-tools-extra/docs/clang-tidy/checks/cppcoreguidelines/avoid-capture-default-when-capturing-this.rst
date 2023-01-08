.. title:: clang-tidy - cppcoreguidelines-avoid-capture-default-when-capturing-this

cppcoreguidelines-avoid-capture-default-when-capturing-this
=========================================================

Warns when lambda specify a capture default and capture ``this``. The check also
offers fix-its.

Capture-defaults in member functions can be misleading about
whether data members are captured by value or reference. For example,
specifying the capture default ``[=]`` will still capture data members
by reference.

Examples:

.. code-block:: c++

      struct AClass {
        int DataMember;
        void misleadingLogic() {
          int local = 0;
          member = 0;
          auto f = [=]() {
            local += 1;
            member += 1;
          };
          f();
          // Here, local is 0 but member is 1
        }

        void clearLogic() {
          int local = 0;
          member = 0;
          auto f = [this, local]() {
            local += 1;
            member += 1;
          };
          f();
          // Here, local is 0 but member is 1
        }
      };

This check implements
`CppCoreGuideline F.54 <http://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#f54-if-you-capture-this-capture-all-variables-explicitly-no-default-capture>`_.
