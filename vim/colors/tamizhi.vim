" Tamizhi colour scheme (dark, works with 256-colour and truecolor terminals)
hi clear
if exists('syntax_on') | syntax reset | endif
let g:colors_name = 'tamizhi'
set background=dark

hi Normal       guifg=#e6dfd0 guibg=#1b1d23 ctermfg=253 ctermbg=234
hi Comment      guifg=#6b7280 ctermfg=243 gui=italic cterm=italic
hi Todo         guifg=#1b1d23 guibg=#e5c07b ctermfg=234 ctermbg=180 gui=bold cterm=bold
hi Conditional  guifg=#e06c75 ctermfg=168 gui=bold cterm=bold
hi Repeat       guifg=#e06c75 ctermfg=168 gui=bold cterm=bold
hi Statement    guifg=#c678dd ctermfg=176 gui=bold cterm=bold
hi StorageClass guifg=#d19a66 ctermfg=173 gui=bold cterm=bold
hi Keyword      guifg=#e5c07b ctermfg=180 gui=bold cterm=bold
hi Structure    guifg=#e5c07b ctermfg=180 gui=bold cterm=bold
hi Function     guifg=#61afef ctermfg=75
hi Include      guifg=#c678dd ctermfg=176 gui=bold cterm=bold
hi Type         guifg=#56b6c2 ctermfg=73
hi Boolean      guifg=#d19a66 ctermfg=173
hi Typedef      guifg=#56b6c2 ctermfg=73 gui=bold cterm=bold
hi Label        guifg=#e06c75 ctermfg=168
hi Constant     guifg=#c678dd ctermfg=176 gui=italic cterm=italic
hi Character    guifg=#98c379 ctermfg=114
hi Number       guifg=#d19a66 ctermfg=173
hi Float        guifg=#d19a66 ctermfg=173
hi String       guifg=#98c379 ctermfg=114
hi SpecialChar  guifg=#56b6c2 ctermfg=73
hi Special      guifg=#e5c07b ctermfg=180
hi Operator     guifg=#abb2bf ctermfg=249
hi Delimiter    guifg=#7f848e ctermfg=102
hi Identifier   guifg=#e6dfd0 ctermfg=253

hi Visual       guibg=#3b4252 ctermbg=238
hi CursorLine   guibg=#23262e ctermbg=235 cterm=NONE
hi LineNr       guifg=#4b5263 ctermfg=240
hi CursorLineNr guifg=#e5c07b ctermfg=180
hi MatchParen   guifg=#ffffff guibg=#44475a ctermfg=231 ctermbg=60 gui=bold
hi Search       guifg=#1b1d23 guibg=#e5c07b ctermfg=234 ctermbg=180
hi StatusLine   guifg=#e6dfd0 guibg=#2c313a ctermfg=253 ctermbg=236 gui=NONE cterm=NONE
hi StatusLineNC guifg=#7f848e guibg=#23262e ctermfg=102 ctermbg=235 gui=NONE cterm=NONE
hi VertSplit    guifg=#2c313a guibg=#2c313a ctermfg=236 ctermbg=236
hi Pmenu        guifg=#e6dfd0 guibg=#2c313a ctermfg=253 ctermbg=236
hi PmenuSel     guifg=#1b1d23 guibg=#61afef ctermfg=234 ctermbg=75
hi ErrorMsg     guifg=#e06c75 ctermfg=168 gui=bold cterm=bold
hi Error        guifg=#e06c75 ctermfg=168 gui=underline cterm=underline
