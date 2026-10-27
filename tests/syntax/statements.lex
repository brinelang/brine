0 2 kw fn
3 4 ident f
4 5 punct (
5 6 punct )
6 7 punct :
7 7 newline
8 12 indent
12 15 kw let
16 17 ident x
17 17 newline
22 25 kw var
26 27 ident y
27 28 punct :
29 32 ident int
32 32 newline
37 40 kw let
41 42 punct (
42 43 ident a
43 44 punct ,
45 46 ident b
46 47 punct )
48 49 punct =
50 51 ident t
51 51 newline
56 59 kw let
60 61 ident c
61 62 punct ,
63 64 ident d
65 66 punct =
67 68 ident t
68 68 newline
73 76 kw var
77 78 ident e
78 79 punct :
80 81 punct [
81 84 ident int
84 85 punct ]
86 87 punct =
88 91 kw ref
92 93 ident z
93 93 newline
98 99 ident x
100 102 punct +=
103 104 int 1
104 104 newline
109 110 ident x
111 113 punct -=
114 115 int 1
115 115 newline
120 121 ident x
122 124 punct *=
125 126 int 2
126 126 newline
131 132 ident x
133 135 punct /=
136 137 int 2
137 137 newline
142 143 ident x
144 146 punct %=
147 148 int 2
148 148 newline
153 155 kw if
156 157 ident x
157 158 punct :
159 165 kw return
166 167 int 1
167 167 newline
172 174 kw if
175 176 ident x
176 177 punct :
178 180 kw if
181 182 ident y
182 183 punct :
184 188 kw pass
188 188 newline
193 198 kw while
199 200 ident x
200 201 punct :
201 201 newline
202 210 indent
210 219 kw invariant
220 221 ident x
222 223 punct >
224 225 int 0
225 225 newline
234 239 kw break
239 239 newline
244 244 dedent
244 252 kw requires
253 254 ident x
254 254 newline
259 262 kw for
263 264 ident i
265 267 kw in
268 269 int 0
269 271 punct ..
271 272 ident n
272 273 punct :
273 273 newline
274 282 indent
282 291 kw invariant
292 293 ident i
294 296 punct >=
297 298 int 0
298 298 newline
307 315 kw continue
315 315 newline
320 320 dedent
320 323 kw par
324 327 kw for
328 329 ident i
330 332 kw in
333 335 ident xs
335 336 punct :
336 336 newline
337 345 indent
345 349 kw pass
349 349 newline
354 354 dedent
354 357 kw for
358 359 ident a
359 360 punct ,
361 362 ident b
363 365 kw in
366 367 ident m
367 368 punct .
368 373 ident items
373 374 punct (
374 375 punct )
375 376 punct :
376 376 newline
377 385 indent
385 389 kw pass
389 389 newline
394 394 dedent
394 397 kw for
398 399 ident a
400 401 ident b
402 404 kw in
405 406 ident c
406 407 punct :
407 407 newline
408 416 indent
416 420 kw pass
420 420 newline
425 425 dedent
425 431 kw return
431 431 newline
432 432 dedent
432 432 eof
