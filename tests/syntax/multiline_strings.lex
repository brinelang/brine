0 5 kw const
6 7 ident A
8 9 punct =
10 41 str "two\n  lines\n"
41 41 newline
42 47 kw const
48 49 ident B
50 51 punct =
52 92 fstr expr "x"@57 lit " and " expr "d[\"k\"]"@65 lit "\n" expr "\", \".join(y)"@76
92 92 newline
93 98 kw const
99 100 ident C
101 102 punct =
103 124 str "meant to\nspan lines"
124 124 newline
125 127 kw fn
128 132 ident main
132 133 punct (
133 134 punct )
134 135 punct :
135 135 newline
136 140 indent
140 143 kw let
144 148 ident case
149 150 punct =
151 152 int 1
152 152 newline
157 160 kw let
161 165 ident test
166 167 punct =
168 172 ident case
172 172 newline
173 173 dedent
173 173 eof
error E0102 103 112 a string literal in `"` can't span lines
  label 103 112 string starts here
  help for text over several lines, write `"""` at both ends
