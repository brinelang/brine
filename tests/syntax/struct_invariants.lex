0 6 kw struct
7 11 ident Ring
11 12 punct :
12 12 newline
13 17 indent
17 22 ident items
22 23 punct :
24 25 punct [
25 28 ident int
28 29 punct ]
29 29 newline
34 37 ident cap
37 38 punct :
39 42 ident int
42 42 newline
47 56 kw invariant
57 58 int 0
59 60 punct <
61 65 ident self
65 66 punct .
66 69 ident cap
69 69 newline
74 83 kw invariant
84 87 ident len
87 88 punct (
88 92 ident self
92 93 punct .
93 98 ident items
98 99 punct )
100 102 punct <=
103 107 ident self
107 108 punct .
108 111 ident cap
111 111 newline
113 113 dedent
113 119 kw struct
120 121 ident K
121 122 punct :
122 122 newline
123 127 indent
127 136 kw invariant
136 137 punct :
138 141 ident int
141 141 newline
142 142 dedent
142 142 eof
