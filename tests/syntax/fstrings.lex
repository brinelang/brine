0 3 kw let
4 5 ident f
6 7 punct =
8 36 fstr lit "a" expr "x"@12 lit "b" expr "y"@16:">8.2" lit "{lit}" expr " "@31 expr "z"@34
37 38 punct +
39 43 fstr
44 45 punct +
46 57 fstr expr "d[\"k\"]"@49
57 57 newline
58 61 kw let
62 63 ident g
64 65 punct =
66 92 fstr lit "n " expr "f(a, [1, {2: 3}])"@71 lit " e"
92 92 newline
93 93 eof
error E0111 30 33 empty expression in f-string
  label 30 33 expected an expression
error E0102 33 35 unclosed `{` in f-string
  label 33 35 expected `}`
  help use `{{` to write a literal brace
error E0102 41 42 single `}` in f-string
  label 41 42 unmatched
  help use `}}` to write a literal brace
