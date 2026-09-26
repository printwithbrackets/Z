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

execute "setlocal makeprg=" . escape(s:z, " ") . '\ build\ %:S'

" Diagnostics look like `file:line:col: error: msg`, followed by the source
" line and a caret line; the trailing `%-G%m` discards those two.
setlocal errorformat=%f:%l:%c:\ error:\ %m,%-G%m

let b:undo_ftplugin = get(b:, "undo_ftplugin", "") .. "|setl makeprg< errorformat<"

" vim: ts=8 sw=2 sts=2 et fdm=marker
