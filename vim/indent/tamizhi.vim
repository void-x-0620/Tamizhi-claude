" Tamizhi indent: brace-based, same model as C
if exists('b:did_indent') | finish | endif
let b:did_indent = 1
setlocal cindent
setlocal cinoptions=L0,:0,l1,j1,(0,Ws
setlocal cinkeys=0{,0},0),!^F,o,O,e
let b:undo_indent = 'setl cin< cino< cink<'
