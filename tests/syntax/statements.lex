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
153 154 ident x
155 157 punct &=
158 159 int 3
159 159 newline
164 165 ident x
166 168 punct |=
169 170 int 4
170 170 newline
175 176 ident x
177 179 punct ^=
180 181 int 5
181 181 newline
186 187 ident x
188 191 punct <<=
192 193 int 1
193 193 newline
198 199 ident x
200 203 punct >>=
204 205 int 1
205 205 newline
210 212 kw if
213 214 ident x
214 215 punct :
216 222 kw return
223 224 int 1
224 224 newline
229 231 kw if
232 233 ident x
233 234 punct :
235 237 kw if
238 239 ident y
239 240 punct :
241 245 kw pass
245 245 newline
250 255 kw while
256 257 ident x
257 258 punct :
258 258 newline
259 267 indent
267 276 kw invariant
277 278 ident x
279 280 punct >
281 282 int 0
282 282 newline
291 296 kw break
296 296 newline
301 301 dedent
301 309 kw requires
310 311 ident x
311 311 newline
316 319 kw for
320 321 ident i
322 324 kw in
325 326 int 0
326 328 punct ..
328 329 ident n
329 330 punct :
330 330 newline
331 339 indent
339 348 kw invariant
349 350 ident i
351 353 punct >=
354 355 int 0
355 355 newline
364 372 kw continue
372 372 newline
377 377 dedent
377 380 kw par
381 384 kw for
385 386 ident i
387 389 kw in
390 392 ident xs
392 393 punct :
393 393 newline
394 402 indent
402 406 kw pass
406 406 newline
411 411 dedent
411 414 kw for
415 416 ident a
416 417 punct ,
418 419 ident b
420 422 kw in
423 424 ident m
424 425 punct .
425 430 ident items
430 431 punct (
431 432 punct )
432 433 punct :
433 433 newline
434 442 indent
442 446 kw pass
446 446 newline
451 451 dedent
451 454 kw for
455 456 ident a
457 458 ident b
459 461 kw in
462 463 ident c
463 464 punct :
464 464 newline
465 473 indent
473 477 kw pass
477 477 newline
482 482 dedent
482 488 kw return
488 488 newline
489 489 dedent
489 489 eof
