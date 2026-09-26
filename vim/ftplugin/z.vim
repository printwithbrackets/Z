" Vim ftplugin for Z
" Filenames:    *.z

if exists("b:did_ftplugin")
  finish
endif
let b:did_ftplugin = 1

setlocal commentstring=//\ %s
setlocal formatoptions-=t formatoptions+=croql

" Indentation {{{1
" Z is brace-based, so indentation is driven by brace depth, plus a
" continuation indent for unclosed parens and dangling operators.
setlocal indentexpr=<SID>GetZIndent()

" Scan one line, updating absolute depths. String literals (including
" $"..." interpolations) and comments are skipped, so braces inside them never
" affect indentation -- matching how the lexer treats them.
function! s:ScanLine(line, braces, parens, incomment) abort
  let l:s = a:line
  let l:i = 0
  let l:len = strlen(l:s)
  let l:braces = a:braces
  let l:parens = a:parens
  let l:incomment = a:incomment

  while l:i < l:len
    if l:incomment
      let l:m = match(l:s, '\*/', l:i)
      if l:m < 0
        break
      endif
      let l:incomment = 0
      let l:i = l:m + 2
      continue
    endif

    " Nearest bracket, quote or comment opener, whichever comes first.
    let l:m = match(l:s, '["{}()\[\]]', l:i)
    let l:cm = match(l:s, '\%("//\|/\*\)', l:i)
    if l:cm >= 0 && (l:m < 0 || l:cm < l:m)
      let l:m = l:cm
    endif
    if l:m < 0
      break
    endif
    let l:tok = l:s[l:m]

    if l:tok ==# '"'
      let l:i = s:SkipString(l:s, l:m)
    elseif l:tok ==# '/' && l:s[l:m + 1] ==# '/'
      break " line comment: the rest of the line is prose
    elseif l:tok ==# '/' && l:s[l:m + 1] ==# '*'
      let l:incomment = 1 " block comment, may or may not close on this line
      let l:i = l:m + 2
    elseif index(['{', '(', '['], l:tok) >= 0
      if l:tok ==# '{'
        let l:braces += 1
      else
        let l:parens += 1
      endif
      let l:i = l:m + 1
    else
      if l:tok ==# '}'
        let l:braces -= 1
      else
        let l:parens -= 1
      endif
      let l:i = l:m + 1
    endif
  endwhile

  return [l:braces, l:parens, l:incomment]
endfunction

" Index just past the closing quote of the string starting at `start`.
function! s:SkipString(str, start) abort
  let l:i = a:start + 1
  let l:len = strlen(a:str)
  while l:i < l:len
    if a:str[l:i] ==# '\'
      let l:i += 2
    elseif a:str[l:i] ==# '"'
      return l:i + 1
    else
      let l:i += 1
    endif
  endwhile
  return l:len
endfunction

" The last character of `line` that is code rather than comment or string, so
" a trailing `*/` or `// note` is never mistaken for a dangling operator.
function! s:LastCodeChar(line) abort
  let l:s = a:line
  let l:i = 0
  let l:len = strlen(l:s)
  let l:last = ''
  while l:i < l:len
    if l:s[l:i] ==# '"'
      let l:i = s:SkipString(l:s, l:i)
      let l:last = '"'
    elseif l:s[l:i] ==# '/' && l:s[l:i + 1] ==# '/'
      break " line comment: the rest is prose
    elseif l:s[l:i] ==# '/' && l:s[l:i + 1] ==# '*'
      let l:m = match(l:s, '\*/', l:i + 2)
      if l:m < 0
        let l:i = l:len
      else
        let l:i = l:m + 2
      endif
    elseif l:s[l:i] ==# '*' && l:s[l:i + 1] ==# '/'
      let l:i += 2 " end of a block comment, not code
    elseif l:s[l:i] =~# '\s'
      let l:i += 1
    else
      let l:last = l:s[l:i]
      let l:i += 1
    endif
  endwhile
  return l:last
endfunction

" Classify how a line trails off: 2 = dangling operator, so the next line is
" always a continuation; 1 = dangling `,` or `.`, which only continues outside
" a block (inside one it means "next item in this list", e.g. match arms).
" A trailing `{` opens a block instead, and a trailing `(`/`[` is already
" handled by the paren depth.
function! s:IsContinuation(line) abort
  let l:last = s:LastCodeChar(a:line)
  if l:last ==# '' || l:last ==# '{'
    return 0
  endif
  if l:last =~# '[-+*/%=<>!&|?:]'
    return 2
  endif
  return l:last =~# '[,.]' ? 1 : 0
endfunction

" Scanned state per line, so reindenting a buffer stays linear in its length
" instead of walking from the top for every line. The state lives in
" buffer-local variables: the functions are script-local and shared by every Z
" buffer, so keeping the state in `s:` would leak one file's brace depth into
" the next. Any edit bumps 'b:changedtick', which drops the cache -- correct
" first, fast second.
function! s:StateAt(lnum) abort
  if !exists("b:z_indent_scanned")
    let b:z_indent_state = {}
    let b:z_indent_carry = [0, 0, 0]
    let b:z_indent_scanned = 0
    let b:z_indent_tick = -1
  endif

  if b:z_indent_tick != b:changedtick
    let b:z_indent_state = {}
    let b:z_indent_carry = [0, 0, 0]
    let b:z_indent_scanned = 0
    let b:z_indent_tick = b:changedtick
  endif
  if has_key(b:z_indent_state, a:lnum)
    return b:z_indent_state[a:lnum]
  endif

  let l:braces = b:z_indent_carry[0]
  let l:parens = b:z_indent_carry[1]
  let l:incomment = b:z_indent_carry[2]
  let l:cont = 0

  let l:i = b:z_indent_scanned + 1
  while l:i <= a:lnum
    let l:cont = s:IsContinuation(getline(l:i - 1))
    " Cache the state at the *start* of this line, before scanning it.
    let b:z_indent_state[l:i] = [l:braces, l:parens, l:incomment, l:cont]
    let l:res = s:ScanLine(getline(l:i), l:braces, l:parens, l:incomment)
    let l:braces = l:res[0]
    let l:parens = l:res[1]
    let l:incomment = l:res[2]
    let b:z_indent_scanned = l:i
    let l:i += 1
  endwhile
  let b:z_indent_carry = [l:braces, l:parens, l:incomment]

  return b:z_indent_state[a:lnum]
endfunction

function! s:GetZIndent() abort
  let l:lnum = v:lnum
  if l:lnum < 1
    return -1 " keep whatever indent the line already has
  endif
  let l:width = shiftwidth()
  let l:res = s:StateAt(l:lnum)
  let l:braces = l:res[0]
  let l:parens = l:res[1]
  let l:incomment = l:res[2]
  let l:cont = l:res[3]

  let l:line = getline(l:lnum)
  let l:plain = l:line =~# '^\s*$' || l:line =~# '^\s*[/*]' || l:incomment

  let l:indent = l:braces * l:width
  if l:parens > 0
    let l:indent += l:width
  elseif !l:plain && (l:cont == 2 || (l:cont == 1 && l:braces <= 0))
    let l:indent += l:width
  endif

  " A line that opens with a closer belongs to the level it closes.
  if l:braces > 0 && l:line =~# '^\s*[})]'
    let l:indent -= l:width
  endif
  if l:parens > 0 && l:line =~# '^\s*[)\]]'
    let l:indent -= l:width
  endif

  return l:indent > 0 ? l:indent : 0
endfunction
" }}}

let b:undo_ftplugin = "setl commentstring< formatoptions< indentexpr<"

" `:make` builds the buffer with this repo's compiler.
compiler z

" vim: ts=8 sw=2 sts=2 et fdm=marker
