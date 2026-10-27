0 6 kw extern
7 8 ident f
8 9 punct (
9 10 ident x
10 11 punct :
12 15 ident int
15 16 punct )
16 16 newline
17 23 kw extern
24 26 kw fn
27 28 ident g
28 29 punct (
29 30 ident x
30 31 punct :
32 35 ident int
35 36 punct )
37 39 punct ->
40 43 ident int
43 43 newline
44 50 kw extern
51 53 kw fn
54 55 ident h
55 56 punct (
56 57 ident x
57 58 punct :
59 62 ident int
62 63 punct )
63 64 punct :
64 64 newline
65 69 kw test
70 74 ident name
74 75 punct :
75 75 newline
76 80 indent
80 84 kw pass
84 84 newline
85 85 dedent
85 89 kw test
90 94 str "ok"
94 95 punct :
95 95 newline
96 100 indent
100 106 kw assert
107 108 int 1
109 111 punct ==
112 113 int 1
113 114 punct ,
115 120 str "msg"
120 120 newline
121 121 dedent
121 121 eof
