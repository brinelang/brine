0 3 kw let
4 5 ident u
6 7 punct =
8 13 str "open"
13 13 newline
14 17 kw let
18 19 ident v
20 21 punct =
22 26 fstr expr "a"@25
26 26 newline
27 30 kw let
31 32 ident w
33 34 punct =
35 37 str ""
37 37 newline
37 37 eof
error E0102 8 13 unterminated string literal
  label 8 13 string starts here
  help close the string with a matching quote on the same line
error E0102 24 26 unclosed `{` in f-string
  label 24 26 expected `}`
  help use `{{` to write a literal brace
error E0102 22 26 unterminated f-string
  label 22 26 f-string starts here
error E0102 35 37 unterminated string literal
  label 35 37 string starts here
  help close the string with a matching quote on the same line
