#!/bin/sh

git ls-files -z \
  '*.c' '*.cc' '*.cpp' '*.cxx' \
  '*.h' '*.hh' '*.hpp' '*.hxx' \
| xargs -0 clang-format-20 -n
