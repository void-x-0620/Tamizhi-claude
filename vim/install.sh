#!/bin/sh
# Install Tamizhi Vim support into ~/.vim (or $VIMDIR). Neovim: VIMDIR=~/.config/nvim ./install.sh
set -e
dest="${VIMDIR:-$HOME/.vim}"
here="$(cd "$(dirname "$0")" && pwd)"
for d in ftdetect ftplugin indent syntax colors; do
    mkdir -p "$dest/$d"
    cp "$here/$d/tamizhi.vim" "$dest/$d/"
done
echo "Installed to $dest"
echo "Add to your vimrc:  colorscheme tamizhi"
