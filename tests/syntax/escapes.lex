0 3 kw let
4 5 ident s
6 7 punct =
8 65 str "tab\there 😀      end"
65 65 newline
66 69 kw let
70 71 ident t
72 73 punct =
74 92 str "single \"q\" ' ok"
92 92 newline
93 93 eof
error E0101 29 33 invalid unicode escape
  label 29 33 bad escape
error E0101 34 42 invalid unicode escape
  label 34 42 bad escape
error E0101 43 53 invalid unicode escape
  label 43 53 bad escape
error E0101 54 56 unknown escape sequence
  label 54 56 unknown escape
  help valid escapes: \n \t \r \0 \\ \" \' \{ \} \u{XXXX}
error E0101 57 60 unknown escape sequence
  label 57 60 unknown escape
  help valid escapes: \n \t \r \0 \\ \" \' \{ \} \u{XXXX}
