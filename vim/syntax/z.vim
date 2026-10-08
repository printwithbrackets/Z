" Vim syntax file
" Language:     Z
" Filenames:    *.z
"
" The keyword list mirrors src/lexer.c (KEYWORDS[]), the escape sequences mirror
" the decoder in lex_string_escape() there, the builtins mirror
" Z_INTRINSIC_LIST and Z_BUILTIN_LIST in src/limits.h, the duration units mirror
" duration_unit_us() in src/lexer.c, and the surface names mirror the defaults
" in surface_new() (src/surface.c).
"
" NOTE 1: `:syn` does not support line continuation, so every item is one line.
" NOTE 2: every pattern is in single quotes. In a double-quoted Vim string `\<`
"           loses its backslash, which does not raise an error -- the pattern
"           simply never matches, so a rule written that way looks fine and
"           highlights nothing. Checked with:
"             :echo strlen("\<trim\>")   " 6, the `\<` became `<`
"             :echo strlen('\<trim\>')   " 8, as intended
" NOTE 3: specific rules come after the general ones they override -- `new
"           Point(` is a type rather than a call, `.length` is a property rather
"           than a method, a builtin is a runtime call rather than the
"           program's own. `zBuiltin` and `zDestructor` link to `Function`,
"           the same group `zFunction` uses, so where two of those rules cover
"           the same text the difference is a name and not a colour.

if exists("b:current_syntax")
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

syn case match

" Lowest priority: punctuation and operators {{{1
syn match zDelimiter '[][{}();,]'
" Never use `\=` in this pattern: it is the optional atom, so it would match
" the empty string and win at every column, eating the whole file.
syn match zOperator '<<=\|>>=\|[-+*/%&|^]=\|==\|!=\|<=\|>=\|=>\|->\|&&\|||\|<<\|>>\|++\|--\|[-+*/%&|^~]\|[<>=!?:]'

" Literals and names {{{1
" Every radix, with `_` allowed as a digit separator. The radix alternatives
" come first so `0x` is not read as the decimal `0` followed by an `x`.
syn match   zNumber '\<0[xX][0-9a-fA-F_]\+\>\|0[bB][01_]\+\>\|0[oO][0-7_]\+\>\|[0-9]\%([0-9_]\)*\>'
" A float. The fraction is only a float when a digit follows the point, which is
" what leaves `Console.WriteLog` and `p.x` alone.
syn match   zFloat '\<[0-9]\%([0-9_]\)*\.\%([0-9]\|_[0-9]\)*\>'
" A duration is a number with a unit stuck to it: h, m, s, and the two
" two-character ones. The lookahead is the point -- `2h21m37` is one duration in
" three parts, so the unit is not required to end the number, but `37` on its
" own is an integer and must not pick up a unit from whatever follows it.
syn match   zDuration '\<[0-9]\%([0-9_]\)*\%(ms\|us\|h\|m\|s\)\ze\%([0-9_]\|[^0-9A-Za-z_]\|\>\)'
" Z names user types in PascalCase (structs, classes, enums and their
" variants), which is what a type reference looks like in practice.
syn match   zTypeName '\<\u\w*\>'
" A plain call, then the contextual keywords: `base(`/`get;` must beat the
" generic call rule, while a method named `Get()` is unaffected.
syn match   zFunction '\<\h\w*\ze\s*('
syn match   zKeyword  '\%(\.\)\@<!\<\%(base\|get\|set\)\>\ze\s*[;(]'
syn match   zFunction '\<print\>\ze\s*('
" C#-style declarations put the type first: `int fib(`, `T maxOf<T>(`.
syn match   zFunction '\%(\%(\<\%(int\|bool\|string\|float\|void\)\>\|\<\u\w*\)\>\s\+\)\@<=\h\w*\%(\s*<[^<>]*>\)\=\ze\s*('
" Operator overloads are spelled `op_Add`, `op_Mul`, ...
syn match   zFunction '\<op_\w\+\>'
" Method calls: `.Sum(` -- `\zs` keeps the dot out of the highlight.
syn match   zFunction '\.\zs\h\w*\ze\s*('
syn match   zProperty '\.\zslength\>'
" `new Point(1, 2)` instantiates a type, so colour the name as one. Defined
" after the call rules so it wins the tie with them.
syn match   zTypeName '\%(\<new\>\s\+\)\@<=\<\u\w*\>'
" A destructor declaration is `~Type(`. The `~` has to be written `\~`, or it is
" the previous-substitute atom rather than the character, and the name has to
" follow it so `~5` still reads as the bitwise-not operator.
syn match   zDestructor '\%(\.\)\@<!\~\zs\u\w*'
" The two surface names a program can write. `print` is reachable only through
" one of them, and both word orders are listed because case folding does not
" join `WriteLog` to `LogWrite`.
syn match   zBuiltin 'Console\.\zs\%(WriteLog\|LogWrite\)\>'
" The names that resolve to the runtime rather than to something the program
" could have written: the intrinsics, then Z_BUILTIN_LIST. Longest name first,
" so the alternation cannot stop early and leave `sub` matching inside
" `substring`. The lookbehind keeps `v.contains(x)` -- a method of the
" program's own type -- off this list.
syn match   zBuiltin '\%(\.\)\@<!\<\%(last_index_of_byte\|float_to_string\|str_buf_append\|index_of_byte\|int_to_string\|str_buf_new\|starts_with\|write_bytes\|read_string\|drop_value\|trim_start\|read_line\|pad_right\|read_byte\|ends_with\|contains\|char_str\|str_free\|pad_left\|index_of\|io_errno\|trim_end\|to_text\|hash_of\|char_at\|str_dup\|reverse\|replace\|repeat\|io_eof\|input\|upper\|split\|clamp\|close\|lower\|open\|join\|hold\|exit\|sqrt\|trim\|min\|sin\|pow\|len\|sub\|cos\|abs\|gcd\|max\|lcm\|die\)\>'
" `\x` and `\u` carry their own digits, which belong to the escape rather than
" being highlighted as separate numbers. Spelled out because the length varies
" and a tail pattern would run to the end of the line.
syn match   zEscape '\\\%(x\x\x\|u\x\x\x\x\|[ntr0\\{}"]\)'

" Strings {{{1
" Interpolated strings are matched before plain ones by column, not by order:
" the `$"..."` opener starts one column before the plain-string region.
" Holes are `{expr}`, and their contents are highlighted as ordinary Z. The
" member lists are spelled out: a `contains=@cluster` reference does not take
" effect here, and the lists are the only thing keeping holes nestable.
syn region zInterpString matchgroup=zStringQuote start=+\$"+ end=+"+ contains=zEscape,zInterpHole
syn region zString matchgroup=zStringQuote start=+"+ skip=+\\\\\|\\"+ end=+"+ contains=zEscape
syn region zInterpHole matchgroup=zInterpDelim start=+{+ end=+}+ contained contains=zType,zTypeName,zBoolean,zStorageClass,zConditional,zRepeat,zStatement,zReturn,zModifier,zKeyword,zNumber,zFloat,zDuration,zString,zInterpString,zEscape,zFunction,zProperty,zBuiltin,zDestructor,zOperator,zDelimiter,zComment,zInterpHole2
syn region zInterpHole2 matchgroup=zInterpDelim start=+{+ end=+}+ contained contains=zType,zTypeName,zBoolean,zStorageClass,zConditional,zRepeat,zStatement,zReturn,zModifier,zKeyword,zNumber,zFloat,zDuration,zString,zInterpString,zEscape,zFunction,zProperty,zBuiltin,zDestructor,zOperator,zDelimiter,zComment,zInterpHole3
syn region zInterpHole3 matchgroup=zInterpDelim start=+{+ end=+}+ contained contains=zType,zTypeName,zBoolean,zStorageClass,zConditional,zRepeat,zStatement,zReturn,zModifier,zKeyword,zNumber,zFloat,zDuration,zString,zInterpString,zEscape,zFunction,zProperty,zBuiltin,zDestructor,zOperator,zDelimiter,zComment

" Comments come last: `//` and `/*` start at the same column as the `/`
" operator, and whichever is defined last is the one that wins.
syn keyword zTodo contained TODO FIXME XXX NOTE
syn region  zComment start="/\*" end="\*/" contains=zTodo,@Spell fold
syn match   zComment "//.*$" contains=zTodo,@Spell

" Highest priority: keywords {{{1
" `fn`/`method`/`closure` are type constructors -- `fn(int) -> int` reads as a
" type -- and must be keywords rather than the call rule below, which would
" otherwise match `fn(` as a call. `null` sits with the other literals.
" `break`/`continue` leave the current construct like `return` does. `const`
" declares storage the way `var` does, and `extern`/`export` modify a
" declaration the way `virtual`/`override` do. `move` modifies a value the same
" way: `return move x;` hands one on rather than copying it, and reading the
" source afterwards is an error. `auto` is a synonym for `var`.
syn keyword zType        int bool float string void fn method closure
syn keyword zBoolean     true false null
syn keyword zStorageClass var auto const
syn keyword zConditional if else
syn keyword zRepeat      while for foreach
syn keyword zStatement   in match
syn keyword zReturn      return break continue
syn keyword zModifier    virtual override extern export move
syn keyword zKeyword     new struct class interface enum this import

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
hi def link zFloat           Float
hi def link zDuration        Number
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
hi def link zBuiltin         Function
hi def link zDestructor      Function
hi def link zOperator        Operator
hi def link zDelimiter       Delimiter

let b:current_syntax = "z"

let &cpo = s:cpo_save
unlet s:cpo_save

" vim: ts=8 sw=2 sts=2 et fdm=marker