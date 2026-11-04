0 4 ident from
5 9 ident pump
10 16 kw import
17 21 ident Pump
21 22 punct ,
23 31 ident dose_for
31 31 newline
33 37 ident rule
38 64 str "a dose is never negative"
65 68 kw for
69 70 ident p
70 71 punct :
72 76 ident Pump
76 77 punct ,
78 85 ident reading
85 86 punct :
87 90 ident int
90 91 punct :
91 91 newline
92 96 indent
96 104 kw requires
105 112 ident reading
113 115 punct >=
116 117 int 0
117 117 newline
122 125 kw let
126 127 ident d
128 129 punct =
130 138 ident dose_for
138 139 punct (
139 140 ident p
140 141 punct ,
142 149 ident reading
149 150 punct )
150 150 newline
155 160 kw prove
161 162 ident d
163 165 punct >=
166 167 int 0
167 167 newline
169 169 dedent
169 173 ident rule
174 197 str "dosing has no effects"
197 198 punct :
198 198 newline
199 203 indent
203 210 ident effects
211 215 ident pump
215 216 punct :
217 221 ident none
221 221 newline
226 233 ident effects
234 238 ident pump
238 239 punct .
239 244 ident alarm
244 245 punct :
246 253 ident console
253 254 punct ,
255 260 ident clock
260 260 newline
262 262 dedent
262 266 ident rule
267 287 str "dosing can't crash"
287 288 punct :
288 288 newline
289 293 indent
293 299 ident proven
300 304 ident pump
304 305 punct .
305 313 ident dose_for
313 313 newline
318 324 ident proven
325 329 ident pump
329 330 punct .
330 334 ident Pump
334 335 punct .
335 339 ident step
339 339 newline
376 376 dedent
376 378 kw fn
379 383 ident rule
383 384 punct (
384 385 ident x
385 386 punct :
387 390 ident int
390 391 punct )
392 394 punct ->
395 398 ident int
398 399 punct :
399 399 newline
400 404 indent
404 410 kw return
411 412 ident x
412 412 newline
413 413 dedent
413 413 eof
