# Program icon

`xmd.rc` gives the program the icon `xmd.ico`. That icon is made from the
game's own title image, so it is not in this repository. Before you build,
put any `.ico` file here as `xmd.ico` (for example one you make from your own
copy of the game).

# Button-prompt pictures

`xmd.rc` also packs `glyphs.bin` into the program: the button pictures for
keyboard and mouse, PlayStation and Nintendo pads. It is a generated file
(8 MB) and not in this repository. Make it before you build:

    python port/tools/make_glyphs.py
