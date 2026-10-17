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
    " A diagnostic with no code (plain diag_error) shows the message alone
    " rather than a "[?]" bracket that searches for nothing.
    let l:code = get(l:d, 'code', '')
    let l:text = empty(l:code) ? get(l:d, 'message', '')
          \ : printf('[%s] %s', l:code, get(l:d, 'message', ''))
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

" On Neovim the diagnostics go through Neovim's own diagnostics system rather
" than through the quickfix list and our own sign placement: the message is
" drawn on the line the problem is on, the float carries the full text, and
" vim.diagnostic.goto_next() does the walking. g:z_virtual_text = 0 restores
" the quickfix flow. The lua block sits at column zero: the end marker has to,
" and `lua` inside an `if` is fine with the block untouched.
if has('nvim')
lua << LUA
  -- One namespace holds everything this plugin placed, so clearing a buffer
  -- is setting an empty list on it again.
  local ns = vim.api.nvim_create_namespace('z-diagnostics')
  local sev = {
    E = vim.diagnostic.severity.ERROR,
    W = vim.diagnostic.severity.WARN,
    I = vim.diagnostic.severity.INFO,
  }
  -- Takes the quickfix-shaped list z#Diagnostics builds and files one
  -- diagnostic per item on the buffer the item names, so a note that points at
  -- the declaration in an imported file lands on that file's own copy.
  function _G.z_diag_set(items)
    local per_buf = {}
    for _, it in ipairs(items) do
      local buf = it.bufnr
      if not (buf and buf > 0) and it.filename and it.filename ~= '' then
        buf = vim.fn.bufnr(it.filename)
      end
      if buf and buf > 0 and vim.api.nvim_buf_is_loaded(buf) and it.lnum and it.lnum > 0 then
        -- The underline runs from the span start to the last printed cell of
        -- the line, because the compiler's span length did not survive the
        -- quickfix shape. A message that past the line's end clamps to it.
        local col = tonumber(it.col) or 1
        local line = vim.api.nvim_buf_get_lines(buf, it.lnum - 1, it.lnum, false)[1]
        local endc = line and #vim.trim(line) or col
        if endc < col then
          endc = col
        end
        local d = {
          lnum = it.lnum - 1,
          col = col - 1,
          end_lnum = it.lnum - 1,
          end_col = endc,
          severity = sev[it.type] or sev.I,
          message = it.text,
          source = 'z',
        }
        local diags = per_buf[buf]
        if diags == nil then
          diags = {}
          per_buf[buf] = diags
        end
        table.insert(diags, d)
      end
    end
    for buf, diags in pairs(per_buf) do
      vim.diagnostic.set(ns, buf, diags, {})
    end
  end
  function _G.z_diag_clear()
    -- Every buffer this plugin ever touched, not just the one with focus: the
    -- command is :Zclearfix and a report that spans imported files leaves
    -- diagnostics behind on each of their buffers.
    local diags = vim.diagnostic.get(nil, {namespace = ns})
    for _, d in ipairs(diags) do
      vim.diagnostic.set(ns, d.bufnr, {}, {})
    end
  end
LUA
endif

" Diagnostics land inline on Neovim, or in the gutter everywhere else. The
" quickfix/location list itself is filled by the reporters either way, so old
" habits such as `:cnext` keep working; the window opening is what differs.
" Inside the inline branch SignsFrom only clears the plugin's own leftovers,
" because the diagnostics system keeps its own sign and doubling up on one
" column is a fight over which wins.
function! z#Show(items) abort
  if z#Inline()
    call luaeval('_G.z_diag_set(_A)', a:items)
  endif
  call z#SignsFrom(a:items)
endfunction

" Whether the editor's own diagnostics carry the report. Checked per call, like
" every other user flag here, so it can flip during a session. Public because
" the commands in plugin/z.vim branch on it without touching the lists.
function! z#Inline() abort
  return has('nvim') && get(g:, 'z_virtual_text', 1)
        \ && luaeval('vim.diagnostic ~= nil')
endfunction

" One-line counts, the echo the list window replaced on Neovim.
function! s:EchoSummary(items) abort
  let l:errors = len(filter(copy(a:items), 'v:val.type ==# "E"'))
  let l:warns = len(filter(copy(a:items), 'v:val.type ==# "W"'))
  if l:errors > 0
    echohl DiagnosticError
  elseif l:warns > 0
    echohl DiagnosticWarn
  else
    echohl DiagnosticInfo
  endif
  echomsg printf('Z: %d error(s), %d warning(s)', l:errors, l:warns)
  echohl None
endfunction

" Puts items in the quickfix list and the report on screen, then returns the
" number of real problems (errors and warnings, not notes).
function! z#Report(items, title) abort
  call setqflist([], 'r', {'title': a:title, 'items': a:items})
  call z#Show(a:items)
  return len(filter(copy(a:items), 'v:val.type !=# "I"'))
endfunction

" The same, into the window's location list instead of the quickfix list. One
" file's diagnostics per window is what a location list is for; the quickfix
" list accumulates across every file, which is right for a project build and
" wrong when switching between two files you are editing.
function! z#ReportLocal(items, title) abort
  call setloclist(0, [], 'r', {'title': a:title, 'items': a:items})
  call z#Show(a:items)
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
" In inline mode this only clears the plugin's own leftovers, because the
" diagnostics system places the sign itself and two severities in one cell
" would be a fight over which wins.
function! z#SignsFrom(items) abort
  if !has('signs')
    return
  endif
  call s:DefineSigns()
  " One group for both severities, cleared as a whole: `:sign unplace` by name
  " is Vim-only grammar and Neovim rejects it, so names never travel through
  " Ex commands here -- only the function calls below, which both editors share.
  call sign_unplace('z')
  if z#Inline()
    return
  endif
  for l:item in a:items
    if !get(l:item, 'valid', 0) || get(l:item, 'type', '') !=# 'E'
          \ && get(l:item, 'type', '') !=# 'W'
      continue
    endif
    " `sign_place` needs a path, and a quickfix item carries one as a buffer
    " number -- `filename` is not a key getqflist() hands back, so asking for it
    " gets an empty string and placing it would fail on it.
    let l:file = get(l:item, 'filename', '')
    if empty(l:file) && get(l:item, 'bufnr', 0) > 0
      let l:file = bufname(l:item.bufnr)
    endif
    if empty(l:file)
      continue
    endif
    let l:sign = l:item.type ==# 'E' ? 'zerr' : 'zwarn'
    " A sign is decoration, so placing one never fails the caller: the id is
    " bumped per sign rather than derived from the line, because two problems
    " on one line are two signs and one id cannot serve both.
    let s:sign_id += 1
    call sign_place(s:sign_id, 'z', l:sign, l:file, {'lnum': l:item.lnum})
  endfor
endfunction

" Signs are defined at the last moment rather than at load time: a user who has
" not set `g:z_signs_*` gets the defaults, and defining them once at startup
" would pin those before their config had a chance to change them.
let s:sign_id = 0
function! s:DefineSigns() abort
  if exists('s:signs_defined')
    return
  endif
  let s:signs_defined = 1
  " The function call, not the Ex command: `:sign define` with a multi-byte
  " glyph parses differently between Vim and Neovim, and the text arrives
  " verbatim here either way. Sign text has to be one or two display cells,
  " so the default is two.
  call sign_define('zerr', {'text': get(g:, 'z_signs_error', '>>'),
        \ 'texthl': 'DiagnosticSignError'})
  call sign_define('zwarn', {'text': get(g:, 'z_signs_warning', '>>'),
        \ 'texthl': 'DiagnosticSignWarn'})
endfunction

" Clears the quickfix list, the diagnostics and the gutter signs. `:Zclearfix`,
" for when a stale list is more confusing than an empty one.
function! z#Clear() abort
  call setqflist([], 'r')
  if z#Inline()
    call luaeval('_G.z_diag_clear()')
  endif
  call z#Signs()
endfunction

" The `BufWritePost` hook, and the only place that reads g:z_check_on_write.
"
" Deliberately a function rather than an `if` in plugin/z.vim: a plugin file is
" sourced at startup, which is *before* a LazyVim spec's `config()` runs, so a
" flag set there is not yet true and the autocmd would never be registered at
" all. Reading it per write means the flag can be set whenever, which is the only
" time that works.
function! z#CheckOnWrite() abort
  if !get(g:, 'z_check_on_write', 0)
    return
  endif
  if empty(expand('%:p')) || &buftype !=# ''
    return
  endif
  let l:items = z#Diagnostics(expand('%:p'))
  call z#Report(l:items, 'z: ' . fnamemodify(expand('%:p'), ':t'))
  if z#Inline()
    " The report is on the line and the sign is in the gutter, so the window
    " that would have opened has nothing left to say. A one-line count stands
    " in for it, which also makes a save that fixed everything visible.
    call s:EchoSummary(l:items)
    return
  endif
  if empty(l:items)
    return
  endif
  " Quiet mode: only interrupt for a real error. A warning belongs in the list
  " and gets a sign in the gutter, but it is not worth the quickfix window
  " appearing over the file on every save.
  let l:errors = len(filter(copy(l:items), 'v:val.type ==# "E"'))
  if l:errors == 0 && get(g:, 'z_check_on_write_quiet', 1)
    return
  endif
  copen
endfunction