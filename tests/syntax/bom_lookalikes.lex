119 121 kw fn
122 126 ident main
126 127 punct (
127 128 punct )
128 129 punct :
129 129 newline
130 134 indent
134 137 kw let
138 139 ident a
141 142 punct =
143 144 int 1
144 144 newline
149 154 ident print
154 155 punct (
158 160 ident hi
163 164 punct )
164 164 newline
169 174 ident print
174 175 punct (
175 176 ident a
184 185 int 1
185 186 punct )
186 186 newline
187 187 dedent
187 187 eof
error E0101 139 141 unexpected character U+00A0 (no-break space)
  label 139 141 not valid here
  help write a plain space instead
  fix 139 141 " " write a plain space instead
error E0101 155 158 unexpected character `“` (U+201C left double quotation mark)
  label 155 158 not valid here
  help write `"` instead
  fix 155 158 "\"" write `"` instead
error E0101 160 163 unexpected character `”` (U+201D right double quotation mark)
  label 160 163 not valid here
  help write `"` instead
  fix 160 163 "\"" write `"` instead
error E0101 176 179 unexpected character U+200B (zero-width space)
  label 176 179 not valid here
  help delete it: it can't be seen, but it is there
  fix 176 179 "" delete it: it can't be seen, but it is there
error E0101 180 183 unexpected character `–` (U+2013 en dash)
  label 180 183 not valid here
  help write `-` instead
  fix 180 183 "-" write `-` instead
