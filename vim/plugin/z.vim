" Z commands for Vim and Neovim.
"
" Loaded from plugin/ on startup, so these exist in any buffer once the runtime
" path is set -- no ftplugin needed.
"
" On Neovim the diagnostics render through Neovim's own diagnostics system: the
" message sits on the line it is about, the sign is in the gutter, and the
" built-in commands carry the rest:
"
"   :lua vim.diagnostic.open_float()   " the full text of the problem at hand
"   :lua vim.diagnostic.goto_next()    " and goto_prev() for the way back
"
" No quickfix window opens. `:Zcheck!` opens one anyway, filled with the same
" report, for the days a list is what the navigation wants.
"
"   :Zcheck        compile the current file and report the diagnostics
"   :Zcheck!       same, and also open the quickfix list
"   :Zlmake        same, into the location list (window under the same rule)
"   :Zclearfix     empty the quickfix list and remove the gutter signs
"   :Zsigns        re-place the gutter signs from the current quickfix list
"   :Zversion      print the compiler version
"
" `:make` and `:lmake` also work -- vim/compiler/z.vim sets makeprg and
" errorformat -- but they go through the one-line gcc error format, which has
" no diagnostic code and flattens notes into the list as unrelated entries.
" `:Zcheck` reads the JSON format instead and keeps them attached.
"
" `g:z_check_on_write` (default 0) runs :Zcheck after each write. Off by
" default: compiling on save is a choice, and a slow one, so it is opt-in.
"   let g:z_check_on_write = 1
" `g:z_check_on_write_quiet` (default 1) keeps the quickfix window from opening
" on every save and only opens it when something is wrong. With the inline
" diagnostics there is no window at all, and the summary line does the talking.
" `g:z_virtual_text` (default 1 on Neovim, always off on Vim) sends the
" diagnostics through vim.diagnostic; 0 restores the quickfix flow everywhere.
" `g:z_compiler` overrides the compiler binary; vim/compiler/z.vim uses it too.

if exists('g:loaded_z_plugin')
  finish
endif
let g:loaded_z_plugin = 1

function! s:Current() abort
  return expand('%:p')
endfunction

function! s:Check(bang, local) abort
  let l:file = s:Current()
  if empty(l:file)
    echohl WarningMsg
    echomsg 'Z: no file name'
    echohl None
    return
  endif

  let l:items = z#Diagnostics(l:file)
  let l:title = 'z: ' . fnamemodify(l:file, ':t')
  if a:local
    call z#ReportLocal(l:items, l:title)
  else
    call z#Report(l:items, l:title)
  endif

  if empty(l:items)
    echohl DiagnosticInfo
    echomsg 'Z: ' . l:title . ' -- no problems'
    echohl None
    return
  endif

  " Only errors and warnings are "problems"; the note entries exist to be
  " jumped to and are not worth counting in a summary.
  let l:errors = len(filter(copy(l:items), 'v:val.type ==# "E"'))
  let l:warns = len(filter(copy(l:items), 'v:val.type ==# "W"'))
  let l:msg = printf('Z: %d error(s), %d warning(s)', l:errors, l:warns)
  if l:errors > 0
    echohl DiagnosticError
  elseif l:warns > 0
    echohl DiagnosticWarn
  else
    echohl DiagnosticInfo
  endif
  echomsg l:msg
  echohl None

  " Inline mode already put the message on the problem's line, so there is no
  " window to open: the summary above and the on-line text carry it. The bang
  " opens the list anyway, the way back for anything `:cnext` does better.
  " `empty()` and not `!`, because the bang arrives as the string "!" and a
  " non-numeric string is falsy in a Vim condition, so `!a:bang` would open on
  " :Zcheck! -- the same inversion this replaces.
  if z#Inline()
    if !empty(a:bang)
      execute (a:local ? 'lopen' : 'copen')
    endif
    return
  endif

  " A bang suppresses the window (Vim convention: :Zcheck opens, :Zcheck!
  " does not); otherwise the list opens, location list for :Zlmake.
  if empty(a:bang)
    execute (a:local ? 'lopen' : 'copen')
  endif
endfunction

command! -bang -nargs=0 Zcheck :call <SID>Check(<q-bang>, 0)
command! -bang -nargs=0 Zlmake :call <SID>Check(<q-bang>, 1)
command! -nargs=0 Zclearfix :call z#Clear()
command! -nargs=0 Zsigns :call z#Signs()
command! -nargs=0 Zversion :echo 'Z compiler: ' . (empty(z#Version()) ? 'not found (' . z#Compiler() . ')' : z#Version())

" Check on write, registered per filetype rather than `<buffer>`. A plugin file
" is sourced at startup, when `<buffer>` would bind to whichever buffer happened
" to be current then -- usually none of the user's -- and the autocmd would never
" fire for a file opened later. A pattern on the extension follows the file
" instead of the startup order.
augroup z_check_on_write
  autocmd! * *.z
  autocmd BufWritePost *.z call z#CheckOnWrite()
augroup END