" Tamizhi filetype settings
if exists('b:did_ftplugin') | finish | endif
let b:did_ftplugin = 1
setlocal commentstring=#\ %s
setlocal comments=:#
setlocal formatoptions-=t formatoptions+=croql
setlocal expandtab shiftwidth=4 softtabstop=4 tabstop=4
setlocal matchpairs+=<:>
let b:undo_ftplugin = 'setl cms< com< fo< et< sw< sts< ts< mps<'
