0 3 kw let
4 5 ident f
6 7 punct =
8 43 fstr lit "a" expr "x"@12 lit "b" expr "y"@16:">8.2" lit "{lit}" expr " "@31 expr "z\" + f\""@34
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
