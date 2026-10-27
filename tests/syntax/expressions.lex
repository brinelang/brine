0 2 kw fn
3 4 ident f
4 5 punct (
5 6 punct )
6 7 punct :
7 7 newline
8 12 indent
12 13 ident x
14 15 punct =
16 17 ident a
18 20 kw if
21 22 ident b
23 27 kw else
28 29 ident c
29 29 newline
34 35 ident x
36 37 punct =
38 39 ident a
40 42 kw if
43 44 ident b
44 44 newline
49 50 ident x
51 52 punct =
53 56 kw not
57 58 ident a
59 62 kw and
63 64 ident b
65 67 kw or
68 69 ident c
69 69 newline
74 75 ident x
76 77 punct =
78 79 ident a
80 81 punct <
82 83 ident b
84 86 punct <=
87 88 ident c
89 91 punct !=
92 93 ident d
93 93 newline
98 99 ident x
100 101 punct =
102 103 ident a
104 106 kw in
107 108 ident b
108 108 newline
113 114 ident x
115 116 punct =
117 118 ident a
119 122 kw not
123 125 kw in
126 127 ident b
127 127 newline
132 133 ident x
134 135 punct =
136 137 int 1
137 139 punct ..
139 140 int 2
140 140 newline
145 146 ident x
147 148 punct =
149 150 int 1
150 153 punct ..=
153 154 int 2
154 154 newline
159 160 ident x
161 162 punct =
163 164 ident a
165 166 punct |
167 168 ident b
169 170 punct ^
171 172 ident c
173 174 punct &
175 176 ident d
177 179 punct <<
180 181 ident e
182 184 punct >>
185 186 ident f
187 188 punct +
189 190 ident g
191 192 punct -
193 194 ident h
195 196 punct *
197 198 ident i
199 200 punct /
201 202 ident j
203 204 punct %
205 206 ident k
206 206 newline
211 212 ident x
213 214 punct =
215 216 punct -
216 217 ident a
218 219 punct +
220 221 punct ~
221 222 ident b
222 222 newline
227 228 ident x
229 230 punct =
231 232 ident f
232 233 punct (
233 234 ident a
234 235 punct ,
236 239 kw ref
240 241 ident b
241 242 punct ,
243 246 kw mut
247 248 ident c
248 249 punct ,
250 251 ident k
251 252 punct =
252 253 ident v
253 254 punct )
254 254 newline
259 260 ident x
261 262 punct =
263 266 ident sum
266 267 punct (
267 268 ident y
269 272 kw for
273 274 ident y
275 277 kw in
278 280 ident ys
281 283 kw if
284 285 ident y
286 287 punct >
288 289 int 0
289 290 punct )
290 290 newline
295 296 ident x
297 298 punct =
299 300 punct [
300 301 ident y
302 305 kw for
306 307 ident y
308 310 kw in
311 313 ident ys
313 314 punct ]
314 314 newline
319 320 ident x
321 322 punct =
323 324 punct [
324 325 ident y
326 329 kw for
330 331 ident y
332 334 kw in
335 337 ident ys
338 341 kw for
342 343 ident z
344 346 kw in
347 349 ident zs
349 350 punct ]
350 350 newline
355 356 ident x
357 358 punct =
359 360 punct [
360 361 ident y
362 365 kw for
366 367 ident y
368 370 kw in
370 371 punct ]
371 371 newline
376 377 ident x
378 379 punct =
380 381 ident a
381 382 punct .
382 383 ident b
383 384 punct .
384 385 ident c
385 386 punct (
386 387 ident d
387 388 punct )
388 389 punct [
389 390 ident e
390 391 punct ]
391 392 punct [
392 393 ident f
393 394 punct :
394 395 ident g
395 396 punct ]
396 397 punct [
397 398 punct :
398 399 ident h
399 400 punct ]
400 401 punct [
401 402 ident i
402 403 punct :
403 404 punct ]
404 405 punct [
405 406 ident j
406 407 punct ,
408 409 ident k
409 410 punct ]
410 410 newline
415 416 ident x
417 418 punct =
419 420 ident a
420 421 punct [
421 422 int 1
422 423 punct :
423 424 int 2
424 425 punct :
425 426 int 3
426 427 punct ]
427 427 newline
432 433 ident x
434 435 punct =
436 437 ident t
437 438 punct .
438 439 int 0
439 439 newline
444 445 ident x
446 447 punct =
448 449 ident t
449 450 punct .
450 453 float 0.1
453 453 newline
458 459 ident x
460 461 punct =
462 463 ident a
463 464 punct ?
464 465 punct .
465 466 ident b
466 467 punct ?
467 467 newline
472 473 ident x
474 475 punct =
476 477 punct (
477 478 int 1
478 479 punct ,
480 481 int 2
481 482 punct )
482 482 newline
487 488 ident x
489 490 punct =
491 492 punct (
492 493 punct )
493 493 newline
498 499 ident x
500 501 punct =
502 503 punct (
503 504 int 1
504 505 punct )
505 505 newline
510 511 ident x
512 513 punct =
514 515 punct [
515 516 int 1
516 517 punct ,
518 519 int 2
519 520 punct ,
520 521 punct ]
521 521 newline
526 527 ident x
528 529 punct =
530 531 punct {
531 532 int 1
532 533 punct :
534 535 int 2
535 536 punct ,
537 538 int 3
538 539 punct :
540 541 int 4
541 542 punct }
542 542 newline
547 548 ident x
549 550 punct =
551 552 punct {
552 553 punct }
553 553 newline
558 559 ident x
560 561 punct =
562 566 ident True
566 566 newline
571 572 ident x
573 574 punct =
575 580 ident False
580 580 newline
585 586 ident x
587 588 punct =
589 592 kw par
592 593 punct (
593 594 ident a
594 595 punct ,
596 597 ident b
597 598 punct )
598 598 newline
603 604 ident x
605 606 punct =
607 610 kw par
610 611 punct (
611 612 ident a
612 613 punct )
613 613 newline
618 619 ident x
620 621 punct =
622 625 kw par
626 627 punct [
627 628 ident y
629 632 kw for
633 634 ident y
635 637 kw in
638 640 ident ys
640 641 punct ]
641 641 newline
646 647 ident x
648 649 punct =
650 653 kw par
654 655 punct [
655 656 int 1
656 657 punct ,
658 659 int 2
659 660 punct ]
660 660 newline
665 666 ident x
667 668 punct =
669 672 kw par
673 676 ident sum
676 677 punct (
677 678 ident y
679 682 kw for
683 684 ident y
685 687 kw in
688 690 ident ys
690 691 punct )
691 691 newline
696 697 ident x
698 699 punct =
700 703 kw par
704 705 ident f
705 706 punct (
706 707 int 1
707 708 punct )
708 708 newline
713 714 ident x
715 716 punct =
717 720 kw par
721 722 int 1
722 722 newline
727 728 ident x
729 730 punct =
731 755 fstr lit "a" expr "b"@735 lit "c" expr "d"@739:">8" expr "e + 1"@745:"x "
755 755 newline
760 761 ident x
762 763 punct =
764 772 fstr expr "a b"@767
772 772 newline
777 778 ident x
779 780 punct =
781 786 fstr expr ""@784
786 786 newline
791 792 ident x
793 794 punct =
795 796 int 1
797 798 punct +
798 798 newline
803 804 ident x
805 806 punct =
807 808 punct )
808 808 newline
809 809 dedent
809 809 eof
error E0111 783 785 empty expression in f-string
  label 783 785 expected an expression
