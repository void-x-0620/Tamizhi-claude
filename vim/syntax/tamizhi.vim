" Vim syntax file
" Language: Tamizhi (Tanglish)
if exists('b:current_syntax') | finish | endif

" Comments start with '#'
syn keyword tzTodo contained TODO FIXME XXX NOTE
syn match   tzComment "#.*$" contains=tzTodo,@Spell

" Control flow: @(c) aga irundha { } ^(c) aga irundha { } illana { }
syn match   tzCondMark "[@^]\ze\s*("
syn keyword tzConditional aga irundha illana
syn keyword tzRepeat      sutri ovvoru seithu
syn keyword tzSwitch      thervu
syn keyword tzCase        vazhakku iyalbu
syn keyword tzFlow        udai thodar
syn keyword tzException   muyarchi pidi erivi
syn keyword tzRepeat      ulle
syn keyword tzReturn      kudu

" Declarations
syn keyword tzDecl     edu
syn keyword tzImport   serku veli
syn keyword tzFuncKw   seyal   nextgroup=tzFuncName skipwhite
syn keyword tzStructKw kattamaipu inaippu pattiyal vagupu nextgroup=tzStructName skipwhite
syn keyword tzTypedefKw peyar nextgroup=tzStructName skipwhite
syn keyword tzModifier  nilai thangu maaridum
syn keyword tzAccess    pothu paathukaappu maraivu
syn keyword tzModifier  moolam
syn keyword tzCtor      uruvaakku
syn keyword tzOop       puthu ithu thandhai
syn keyword tzOperatorKw maatru alavu
syn keyword tzNull      illai
syn match   tzFuncName   contained "\h\w*"
syn match   tzStructName contained "\h\w*"

" Builtin output
syn keyword tzBuiltin pathi

" Types and literals
syn keyword tzType int float string bool void char
syn keyword tzType i8 i16 i32 i64 u8 u16 u32 u64 uchar short ushort uint long ulong
syn keyword tzType usize isize f32 f64 ldouble List Map Set Fn
syn keyword tzBoolean true false

syn match tzNumber "\<\d\+\>"
syn match tzNumber "\<0[xX]\x\+\>"
syn match tzNumber "\<0[bB][01]\+\>"
syn match tzNumber "\<0[oO][0-7]\+\>"
syn match tzFloat  "\<\d\+\.\d\+\%([eE][-+]\=\d\+\)\=\>"
syn match tzFloat  "\<\d\+[eE][-+]\=\d\+\>"
syn match tzCharEsc contained "\\\%([nrt0\\'\"]\|x\x\x\)"
syn match tzChar "'\%([^\\']\|\\.\|\\x\x\x\)'" contains=tzCharEsc

" Strings, escapes and printf-style format specifiers
syn match  tzEscape contained "\\[nrt\\\"]"
syn match  tzFormat contained "%[-+ 0#]*\d*\(\.\d\+\)\?\(ll\|l\)\?[dfsc%]"
syn region tzString start=+"+ skip=+\\"+ end=+"+ oneline contains=tzEscape,tzFormat

" Operators
syn match tzOperator "[-+*/%=<>!&|^]=\?"
syn match tzOperator "&&\|||\|<<=\?\|>>=\?\|++\|--\|[~?]"
syn match tzDelim    "[(){}\[\];,.:]"

" Function calls: name followed by '('
syn match tzCall "\<\h\w*\ze\s*("  contains=tzKeywordGuard
syn match tzKeywordGuard contained "\<\%(aga\|irundha\|sutri\|pathi\|thervu\|maatru\|alavu\|thandhai\)\>"

hi def link tzComment     Comment
hi def link tzTodo        Todo
hi def link tzCondMark    Conditional
hi def link tzConditional Conditional
hi def link tzRepeat      Repeat
hi def link tzFlow        Statement
hi def link tzException   Exception
hi def link tzSwitch      Conditional
hi def link tzCase        Label
hi def link tzTypedefKw   Typedef
hi def link tzModifier    StorageClass
hi def link tzAccess      StorageClass
hi def link tzCtor        Keyword
hi def link tzOop         Constant
hi def link tzOperatorKw  Keyword
hi def link tzNull        Constant
hi def link tzChar        Character
hi def link tzCharEsc     SpecialChar
hi def link tzReturn      Statement
hi def link tzDecl        StorageClass
hi def link tzImport      Include
hi def link tzFuncKw      Keyword
hi def link tzStructKw    Structure
hi def link tzFuncName    Function
hi def link tzStructName  Type
hi def link tzBuiltin     Function
hi def link tzType        Type
hi def link tzBoolean     Boolean
hi def link tzNumber      Number
hi def link tzFloat       Float
hi def link tzString      String
hi def link tzEscape      SpecialChar
hi def link tzFormat      Special
hi def link tzOperator    Operator
hi def link tzDelim       Delimiter
hi def link tzCall        Function
hi def link tzKeywordGuard NONE

let b:current_syntax = 'tamizhi'
