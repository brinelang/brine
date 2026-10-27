0 2 kw fn
3 4 ident f
4 5 punct (
5 6 punct )
6 7 punct :
7 7 newline
8 9 indent
9 12 kw let
13 19 ident tabbed
20 21 punct =
22 23 int 1
23 23 newline
26 26 dedent
26 29 kw let
30 33 ident odd
34 35 punct =
36 37 int 2
37 37 newline
38 42 indent
42 43 ident x
44 45 punct =
46 47 punct (
47 48 int 1
49 50 punct +
59 60 int 2
60 61 punct )
61 61 newline
66 67 ident y
68 69 punct =
70 71 punct [
71 72 int 1
72 73 punct ,
76 77 int 2
77 78 punct ]
78 78 newline
83 84 ident z
85 86 punct =
87 88 int 1
89 90 punct +
101 102 int 2
102 102 newline
103 103 dedent
103 103 dedent
103 103 eof
error E0104 8 9 tabs are not allowed in indentation
  label 8 9 tab character here
  help brine uses spaces for indentation (4 per level); `brine fmt` fixes this automatically
error E0103 24 26 inconsistent indentation
  label 24 26 this line's indentation doesn't match any enclosing block
  help indent with multiples of 4 spaces so each line lines up with a block above it
