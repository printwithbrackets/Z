" Shared helpers for the Z runtime files.
"
" The compiler binary sits beside the `vim/` directory this file lives in, one
" level up from `autoload/`, so `z build` works from a checkout without
" anything on $PATH. `g:z_compiler` overrides it.

let s:root = expand('<sfile>:p:h:h:h')

" The Z compiler to run, or the name to find on $PATH if the tree is not built.
function! z#Compiler() abort
  let l:bin = get(g:, 'z_compiler', s:root .. '/z')
  return executable(l:bin) ? l:bin : 'z'
endfunction

" The compiler's version, or an empty string if it cannot be run. Used by
" :Zversion, which is worth having because a runtime file from one checkout and
" a compiler from another is a confusing way to lose an afternoon.
function! z#Version() abort
  let l:out = systemlist(z#Compiler() .. ' --version 2>&1')
  return empty(l:out) ? '' : l:out[0]
endfunction

" Compiles `file` and returns the diagnostics as quickfix items.
"
" `--error-format=json` rather than `gcc` because the quickfix list wants the
" diagnostic's code (`return_owned_local`, `undefined_variable`) and the span of
" each note, and the one-line gcc format has neither: it prints
" `file:line:col: severity: message` and flattens every note onto its own line,
" which is what makes it good for `errorformat` and bad for anything that wants
" to know which note belongs to which problem.
"
" The compiler writes diagnostics to stderr and emits nothing at all when the
" build is clean, so `2>&1` is load-bearing and an empty result means no
" problems rather than a failed capture. `-o /dev/null` because this wants the
" diagnostics, not the binary.
function! z#Diagnostics(file) abort
  let l:cmd = z#Compiler() .. ' build ' .. shellescape(a:file)
        \ .. ' -o /dev/null --error-format=json --color=never 2>&1'
  let l:items = []
  for l:line in systemlist(l:cmd)
    if empty(trim(l:line))
      continue
    endif
    try
      let l:d = json_decode(l:line)
    catch
      " One unparsable line should not lose the other hundred, and it is not
      " worth failing the command over -- a compiler that cannot print JSON is
      " a problem the message itself will show.
      call add(l:items, {'filename': a:file, 'lnum': 1, 'col': 1, 'valid': 1,
            \ 'type': 'E', 'text': trim(l:line)})
      continue
    endtry

    let l:span = get(l:d, 'span', {})
    let l:file = get(l:span, 'file', a:file)
    " The code is the part that survives copy and paste into a search, so it
    " leads. `message` alone does not say whether a bug is the null-deref check
    " or the ownership rule, and those are two different files to go read.
    let l:text = printf('[%s] %s', get(l:d, 'code', '?'), get(l:d, 'message', ''))
    let l:free = []
    for l:note in get(l:d, 'notes', [])
      let l:nspan = get(l:note, 'span', {})
      if empty(l:nspan)
        " A note with nowhere to point is context: "in function 'main'". It
        " belongs to the message rather than in a list of its own.
        call add(l:free, get(l:note, 'message', ''))
        continue
      endif
      " A note that names a span is a second place to look, and the whole point
      " of a note is the place. "declared here" is worth a line of its own so it
      " can be jumped to and read in context.
      call add(l:items, {'filename': get(l:nspan, 'file', l:file),
            \ 'lnum': get(l:nspan, 'line', 1), 'col': get(l:nspan, 'col', 1),
            \ 'valid': 1, 'type': 'I', 'text': '[note] ' . get(l:note, 'message', '')})
    endfor
    if !empty(l:free)
      let l:text .= '  (' . join(l:free, '; ') . ')'
    endif
    call insert(l:items, {'filename': l:file, 'lnum': get(l:span, 'line', 1),
          \ 'col': get(l:span, 'col', 1), 'valid': 1,
          \ 'type': s:Type(get(l:d, 'severity', 'error')), 'text': l:text}, 0)
  endfor
  return l:items
endfunction

" severity -> quickfix type. `error` and `warning` are the two the compiler
" emits; `info` is for the note entries above.
function! s:Type(severity) abort
  return a:severity ==# 'warning' ? 'W' : (a:severity ==# 'info' ? 'I' : 'E')
endfunction

" Puts items in the quickfix list and refreshes the gutter signs from it, then
" returns the number of real problems (errors and warnings, not notes).
function! z#Report(items, title) abort
  call setqflist([], 'r', {'title': a:title, 'items': a:items})
  call z#Signs()
  return len(filter(copy(a:items), 'v:val.type !=# "I"'))
endfunction

" The same, into the window's location list instead of the quickfix list. One
" file's diagnostics per window is what a location list is for; the quickfix
" list accumulates across every file, which is right for a project build and
" wrong when switching between two files you are editing.
function! z#ReportLocal(items, title) abort
  call setloclist(0, [], 'r', {'title': a:title, 'items': a:items})
  call z#SignsFrom(a:items)
  return len(filter(copy(a:items), 'v:val.type !=# "I"'))
endfunction

" Gutter signs, from the quickfix list.
"
" `:make` would populate the list on its own, but it goes through `errorformat`
" and so knows nothing about which file a note belongs to. Doing this from the
" list means a plain `:make` gets signs too, as long as something ran
" z#Report or z#Signs afterwards.
function! z#Signs() abort
  call z#SignsFrom(getqflist())
endfunction

" Places a gutter sign per error and warning in `items`. Notes are left out:
" a note is a pointer to where else to look, not somewhere this file is wrong.
function! z#SignsFrom(items) abort
  if !has('signs')
    return
  endif
  call s:DefineSigns()
  execute 'silent! sign unplace zerr'
  execute 'silent! sign unplace zwarn'
  for l:item in a:items
    if !get(l:item, 'valid', 0) || get(l:item, 'type', '') !=# 'E'
          \ && get(l:item, 'type', '') !=# 'W'
      continue
    endif
    " `:sign place` needs a path, and a quickfix item carries one as a buffer
    " number -- `filename` is not a key getqflist() hands back, so asking for it
    " gets an empty string and `:sign place ... file=` then fails on it.
    let l:file = get(l:item, 'filename', '')
    if empty(l:file) && get(l:item, 'bufnr', 0) > 0
      let l:file = bufname(l:item.bufnr)
    endif
    if empty(l:file)
      continue
    endif
    let l:sign = l:item.type ==# 'E' ? 'zerr' : 'zwarn'
    " `silent!`, because a sign is decoration. `:sign place` refuses a line
    " that is past the end of a file the editor has not read, and it refuses
    " outright in a headless nvim -- neither is a reason for `:Zcheck` to fail
    " and lose the quickfix list it had already built.
    execute 'silent! sign place ' . l:sign . ' line=' . l:item.lnum
          \ . ' file=' . fnameescape(l:file)
  endfor
endfunction

" Signs are defined at the last moment rather than at load time: a user who has
" not set `g:z_signs_*` gets the defaults, and defining them once at startup
" would pin those before their config had a chance to change them.
function! s:DefineSigns() abort
  if exists('s:signs_defined')
    return
  endif
  let s:signs_defined = 1
  " `escape`, not `shellescape`: sign text is an Ex argument, and shellescape
  " would hand `:sign define` the quote characters as part of the text, which
  " is a two-character sign reading '>>'. Sign text has to be one or two
  " display cells, so the default is two.
  execute 'sign define zerr text=' . escape(get(g:, 'z_signs_error', '>>'), ' \')
        \ . ' texthl=DiagnosticSignError'
  execute 'sign define zwarn text=' . escape(get(g:, 'z_signs_warning', '>>'), ' \')
        \ . ' texthl=DiagnosticSignWarn'
endfunction

" Clears the quickfix list and the gutter signs. `:Zclearfix`, for when a stale
" list is more confusing than an empty one.
function! z#Clear() abort
  call setqflist([], 'r')
  call z#Signs()
endfunction