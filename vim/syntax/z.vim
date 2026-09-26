" Vim syntax file
" Language:     Z
" Filenames:    *.z
"
" The keyword list mirrors src/lexer.c (KEYWORDS[]), the escape sequences
" mirror the string decoder there, and the contextual keywords (base/get/set)
" mirror the identifier checks in src/parser.c.
"
" NOTE 1: `:syn` does not support line continuation, so every item is one line.
" NOTE 2: when two items start at the same column, the one defined LAST wins
"         regardless of how much text each matches. Items below are therefore
"         ordered from lowest to highest priority: keep that order when editing.

if exists("b:current_syntax")
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

syn case match

" Lowest priority: punctuation and operators {{{1
syn match zDelimiter "[][{}();,]"
" Never use `\=` in this pattern: it is the optional atom, so it would match
" the empty string and win at every column, eating the whole file.
syn match zOperator "<<=\|>>=\|[-+*/%&|^]=\|==\|!=\|<=\|>=\|=>\|->\|&&\|||\|<<\|>>\|++\|--\|[-+*/%&|^~]\|[<>=!?:]"

" Literals and names {{{1
syn match   zNumber   "\<\d\+\>"
" Z names user types in PascalCase (structs, classes, enums and their
" variants), which is what a type reference looks like in practice.
syn match   zTypeName "\<\u\w*\>"
" A plain call, then the contextual keywords: `base(`/`get;` must beat the
" generic call rule, while a method named `Get()` is unaffected.
syn match   zFunction "\<\h\w*\ze\s*("
syn match   zKeyword  "\%(\.\)\@<!\<\%(base\|get\|set\)\>\ze\s*[;(]"
syn match   zFunction "\<print\>\ze\s*("
" C#-style declarations put the type first: `int fib(`, `T maxOf<T>(`.
syn match   zFunction "\%(\%(\<\%(int\|bool\|string\|void\)\>\|\<\u\w*\)\>\s\+\)\@<=\h\w*\%(\s*<[^<>]*>\)\=\ze\s*("
" Operator overloads are spelled `op_Add`, `op_Mul`, ...
syn match   zFunction "\<op_\w\+\>"
" Method calls: `.Sum(` -- `\zs` keeps the dot out of the highlight.
syn match   zFunction "\.\zs\h\w*\ze\s*("
syn match   zProperty "\.\zslength\>"
" `new Point(1, 2)` instantiates a type, so colour the name as one. Defined
" after the call rules so it wins the tie with them.
syn match   zTypeName "\%(\<new\>\s\+\)\@<=\<\u\w*\>"
syn match   zEscape   "\\\%(n\|t\|r\|0\|\\\\\|\"\|.\)"

" Strings {{{1
" Interpolated strings are matched before plain ones by column, not by order:
" the `$"..."` opener starts one column before the plain-string region.
" Holes are `{expr}`, and their contents are highlighted as ordinary Z. The
" member lists are spelled out: a `contains=@cluster` reference does not take
" effect here, and the lists are the only thing keeping holes nestable.
syn region zInterpString matchgroup=zStringQuote start=+\$"+ end=+"+ contains=zEscape,zInterpHole
syn region zString matchgroup=zStringQuote start=+"+ skip=+\\\\\|\\"+ end=+"+ contains=zEscape
syn region zInterpHole matchgroup=zInterpDelim start=+{+ end=+}+ contained contains=zType,zTypeName,zBoolean,zStorageClass,zConditional,zRepeat,zStatement,zReturn,zModifier,zKeyword,zNumber,zString,zInterpString,zEscape,zFunction,zProperty,zOperator,zDelimiter,zComment,zInterpHole2
syn region zInterpHole2 matchgroup=zInterpDelim start=+{+ end=+}+ contained contains=zType,zTypeName,zBoolean,zStorageClass,zConditional,zRepeat,zStatement,zReturn,zModifier,zKeyword,zNumber,zString,zInterpString,zEscape,zFunction,zProperty,zOperator,zDelimiter,zComment,zInterpHole3
syn region zInterpHole3 matchgroup=zInterpDelim start=+{+ end=+}+ contained contains=zType,zTypeName,zBoolean,zStorageClass,zConditional,zRepeat,zStatement,zReturn,zModifier,zKeyword,zNumber,zString,zInterpString,zEscape,zFunction,zProperty,zOperator,zDelimiter,zComment

" Comments come last: `//` and `/*` start at the same column as the `/`
" operator, and whichever is defined last is the one that wins.
syn keyword zTodo contained TODO FIXME XXX NOTE
syn region  zComment start="/\*" end="\*/" contains=zTodo,@Spell fold
syn match   zComment "//.*$" contains=zTodo,@Spell

" Highest priority: keywords {{{1
" `fn`/`method` are type constructors -- `fn(int) -> int` reads as a type --
" and must be keywords rather than the call rule below, which would otherwise
" match `fn(` as a call. `null` sits with the other literals. `break`/`continue`
" leave the current construct like `return` does. `const` declares storage the
" way `var` does, and `extern`/`export` modify a declaration the way
" `virtual`/`override` do.
syn keyword zType        int bool string void fn method
syn keyword zBoolean     true false null
syn keyword zStorageClass var const
syn keyword zConditional if else
syn keyword zRepeat      while for foreach
syn keyword zStatement   in match
syn keyword zReturn      return break continue
syn keyword zModifier    virtual override extern export
syn keyword zKeyword     new struct class enum this import

syn sync minlines=50
syn sync maxlines=200

hi def link zTodo           Todo
hi def link zComment         Comment
hi def link zString          String
hi def link zStringQuote     Delimiter
hi def link zInterpString    String
hi def link zInterpDelim     Special
hi def link zEscape          SpecialChar
hi def link zNumber          Number
hi def link zBoolean         Boolean
hi def link zType            Type
hi def link zTypeName        Type
hi def link zStorageClass    StorageClass
hi def link zModifier        StorageClass
hi def link zConditional     Conditional
hi def link zRepeat          Repeat
hi def link zStatement       Statement
hi def link zReturn          Statement
hi def link zKeyword         Keyword
hi def link zFunction        Function
hi def link zProperty        Identifier
hi def link zOperator        Operator
hi def link zDelimiter       Delimiter

let b:current_syntax = "z"

let &cpo = s:cpo_save
unlet s:cpo_save

" vim: ts=8 sw=2 sts=2 et fdm=marker
