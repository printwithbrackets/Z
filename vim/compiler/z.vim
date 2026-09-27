" Compiler for Z: build the current file with the `z` compiler that ships
" beside this file, so `:make` works from any directory.
"
" Override the compiler with, for example:
"     let g:z_compiler = '/usr/local/bin/z'

if exists("b:current_compiler")
  finish
endif
let b:current_compiler = "z"

" <sfile> is .../z/vim/compiler/z.vim, so the compiler binary is three
" directories up. Fall back to $PATH when this tree has not been built.
let s:root = expand("<sfile>:p:h:h:h")
let s:z = get(g:, "z_compiler", s:root .. "/z")
if !executable(s:z)
  let s:z = "z"
endif

" --error-format=gcc asks for one `file:line:col: severity: message` line per
" problem and per note, and nothing else. That is the shape errorformat wants,
" so the quickfix list needs no multi-line state machine and each note becomes
" its own entry -- a "did you mean" line that can be jumped to directly.
"
" --color=never because the quickfix list is not the terminal, and escape codes
" in a message are literal characters in it.
execute "setlocal makeprg=" .. escape(s:z, " ")
      \ .. " build\ %:S --error-format=gcc --color=never"

setlocal errorformat=%f:%l:%c:\ %t%*[^:]:\ %m

let b:undo_ftplugin = get(b:, "undo_ftplugin", "") .. "|setl makeprg< errorformat<"

" vim: ts=8 sw=2 sts=2 et fdm=marker
