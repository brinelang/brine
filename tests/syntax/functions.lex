0 2 kw fn
3 4 ident f
4 5 punct (
5 6 ident a
6 7 punct ,
8 9 ident b
9 10 punct :
11 14 ident int
14 15 punct )
15 16 punct :
16 16 newline
17 21 indent
21 25 kw pass
25 25 newline
26 26 dedent
26 28 kw fn
29 30 ident g
30 31 punct (
31 32 ident x
32 33 punct :
34 37 ident int
37 38 punct )
39 41 punct ->
42 43 punct :
43 43 newline
44 48 indent
48 52 kw pass
52 52 newline
53 53 dedent
53 55 kw fn
56 57 ident h
57 58 punct [
58 59 ident T
59 60 punct ,
61 62 ident U
62 63 punct ]
63 64 punct (
64 65 ident x
65 66 punct :
67 68 ident T
68 69 punct ,
70 73 kw ref
74 75 ident y
75 76 punct :
77 78 ident U
78 79 punct ,
80 83 kw mut
84 85 ident z
85 86 punct :
87 90 ident int
90 91 punct )
92 94 punct ->
95 96 punct (
96 97 ident T
97 98 punct ,
99 100 punct [
100 101 ident U
101 102 punct ]
102 103 punct ,
104 105 punct {
105 108 ident str
108 109 punct :
110 113 ident int
113 114 punct }
114 115 punct )
115 116 punct :
116 116 newline
117 121 indent
121 129 kw requires
130 131 ident x
132 133 punct >
134 135 int 0
135 135 newline
140 147 kw ensures
148 154 ident result
154 154 newline
159 163 kw pass
163 163 newline
164 164 dedent
164 166 kw fn
167 168 ident k
168 169 punct (
169 170 punct )
171 174 ident int
174 175 punct :
175 175 newline
176 180 indent
180 184 kw pass
184 184 newline
185 185 dedent
185 187 kw fn
188 189 ident m
189 190 punct (
190 191 punct )
192 193 punct {
198 202 kw pass
203 205 kw fn
206 207 ident n
207 208 punct (
208 209 punct )
214 218 kw pass
219 219 newline
219 219 eof
