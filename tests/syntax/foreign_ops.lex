0 2 kw if
3 4 ident a
5 7 punct /
8 9 ident b
10 12 kw and
13 14 ident c
15 17 kw or
18 19 kw not
19 20 ident d
22 23 ident e
23 25 punct +=
26 27 ident f
27 29 punct -=
36 37 punct :
37 37 newline
38 42 indent
42 46 kw pass
46 46 newline
47 47 dedent
47 47 eof
error E0105 5 7 `//` is not an operator in brine
  label 5 7 use `/`
  note `/` on integers already rounds down like Python's `//`; comments start with `#`
  fix 5 7 "/" replace with `/`
error E0105 10 12 `&&` is not an operator in brine
  label 10 12 use `and`
  fix 10 12 "and" replace with `and`
error E0105 15 17 `||` is not an operator in brine
  label 15 17 use `or`
  fix 15 17 "or" replace with `or`
error E0105 18 19 `!` is not an operator in brine
  label 18 19 use `not`
  fix 18 19 "not " replace with `not`
error E0106 20 21 semicolons are not used in brine
  label 20 21 remove this
  fix 20 21 "" remove the semicolon
error E0105 23 25 brine has no increment/decrement operators
  label 23 25 not supported
  help write `x += 1` instead
error E0105 27 29 brine has no increment/decrement operators
  label 27 29 not supported
  help write `x -= 1` instead
error E0101 30 31 unexpected character `@`
  label 30 31 not valid here
error E0101 32 33 unexpected character `$`
  label 32 33 not valid here
error E0101 34 36 unexpected character `é`
  label 34 36 not valid here
