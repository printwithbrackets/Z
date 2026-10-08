" Compiler for Z: build the current file with the `z` compiler that ships
" beside this file, so `:make` works from any directory.
"
" Override the compiler with, for example:
"     let g:z_compiler = '/usr/local/bin/z'
"
" `:Zcheck` (vim/plugin/z.vim) is the better command: this one has to go
" through `errorformat`, and the one-line gcc format the compiler can print
" carries no diagnostic code and no note spans. See the note at the bottom.

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
" Every space in the value needs escaping, not just the ones inside the path:
" :setlocal splits an unescaped space into a new option, and "build" is not an
" option, so a bare separator hands it to :setlocal as one and it errors out.
execute "setlocal makeprg="
      \ .. escape(s:z .. " build %:S --error-format=gcc --color=never", " \t")

" %t reads the severity's first letter, so error/warning/note arrive as E/W/N
" and the quickfix window can group them. `%*[^:]` swallows the rest of the
" severity word, because `: error:` after `t` would otherwise become part of
" the message.
setlocal errorformat=%f:%l:%c:\ %t%*[^:]:\ %m

" Follow a quickfix entry into another file rather than asking about it: a
" diagnostic in an imported file is the normal case, not the rare one, and
" `:cnext` stopping to ask is the wrong default.
if !exists('g:z_switchbuf_set')
  let g:z_switchbuf_set = 1
  set switchbuf=useopen,uselast
endif

let b:undo_ftplugin = get(b:, "undo_ftplugin", "") .. "|setl makeprg< errorformat<"

" vim: ts=8 sw=2 sts=2 et fdm=marker