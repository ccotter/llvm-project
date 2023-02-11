.. title:: clang-tidy - bugprone-string-constructor

bugprone-string-constructor
===========================

Finds string constructors that are suspicious and probably errors.

A common mistake is to swap arguments to the 'fill' string-constructor.

Examples:

.. code-block:: c++

  std::string str('x', 50); // should be str(50, 'x')

Constructing a string using the 'fill' constructor where both arguments
are implicitly cast is confusing and likely indicative of a bug.

.. code-block: c++

  int x;
  char ch;
  char buf[10];
  std::string str1(ch, 5); // Args possibly swapped?
  std::string str2('a', x); // Args possibly swpaped?
  std::string str3(buf[1], 5); // First arg should be '&buf[1]'?

Calling the string-literal constructor with a length bigger than the literal is
suspicious and adds extra random characters to the string.

Examples:

.. code-block:: c++

  std::string("test", 200);   // Will include random characters after "test".
  std::string_view("test", 200);

Creating an empty string from constructors with arguments is considered
suspicious. The programmer should use the empty constructor instead.

Examples:

.. code-block:: c++

  std::string("test", 0);   // Creation of an empty string.
  std::string_view("test", 0);

Options
-------

.. option::  WarnOnLargeLength

   When `true`, the check will warn on a string with a length greater than
   :option:`LargeLengthThreshold`. Default is `true`.

.. option::  LargeLengthThreshold

   An integer specifying the large length threshold. Default is `0x800000`.

.. option:: StringNames

    Default is `::std::basic_string;::std::basic_string_view`.

    Semicolon-delimited list of class names to apply this check to.
    By default `::std::basic_string` applies to ``std::string`` and
    ``std::wstring``. Set to e.g. `::std::basic_string;llvm::StringRef;QString`
    to perform this check on custom classes.
