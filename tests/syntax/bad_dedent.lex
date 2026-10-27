0 2 kw fn
3 4 ident f
4 5 punct (
5 6 punct )
6 7 punct :
7 7 newline
8 10 indent
10 11 ident x
12 13 punct =
14 15 int 1
15 15 newline
16 20 indent
20 21 ident y
22 23 punct =
24 25 int 2
25 25 newline
27 27 dedent
27 27 dedent
27 28 ident z
29 30 punct =
31 32 int 3
32 32 newline
33 33 dedent
33 33 eof
error E0103 26 27 inconsistent indentation
  label 26 27 this line's indentation doesn't match any enclosing block
  help indent with multiples of 4 spaces so each line lines up with a block above it
