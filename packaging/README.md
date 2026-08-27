# Instalação no desktop

```sh
cmake --install build --prefix ~/.local
```

Põe o binário, o lançador e o ícone no lugar — quatro arquivos. Se
`~/.local/bin` estiver no `PATH`, `Exec=nirbija` resolve sozinho.

Duas coisas no `.desktop` não são decoração:

- `Icon=nirbija` e `StartupWMClass=nirbija` — o primeiro acha o ícone no tema
  hicolor, o segundo casa a janela com o lançador.
- `Exec=nirbija`, sem `env` na frente, **de propósito**. Nesta máquina o vendor
  GLX padrão faz `glXCreateContext` falhar com `BadValue` sob XWayland e toda
  editora de plugin em OpenGL abre preta; forçar a Mesa resolve. Mas forçar em
  todo mundo tira o driver de hardware de quem está bem, então fica opt-in. Se
  as editoras abrirem pretas:

  ```
  Exec=env __GLX_VENDOR_LIBRARY_NAME=mesa nirbija
  ```

## Distribuir

`dist.sh` monta o que sai numa release, tudo em `dist/`:

```sh
packaging/dist.sh src        # tarball de fontes, do que o git tem commitado
packaging/dist.sh bin        # árvore binária, para uma máquina com o mesmo Qt
packaging/dist.sh appimage   # AppImage, Qt junto dentro
```

A árvore binária traz um `install.sh` que instala em `~/.local` (ou no prefixo
que você passar) e reescreve só a linha `Exec=` com o caminho absoluto. Ela
linka contra o Qt e o JACK da máquina que compilou, então só viaja para uma
distro igual — qualquer outra quer o AppImage ou os fontes.

O AppImage não força Mesa. Se precisar:

```sh
__GLX_VENDOR_LIBRARY_NAME=mesa ./nirbija-0.1.0-x86_64.AppImage
```

## Hyprland

Nothing to paste. Under Hyprland the app asks the compositor to float plugin
editors itself, at startup, over its IPC socket — the rule is named
`nirbija_plugin_editor` and matches class `nirbija-plugin`. Set
`NIRBIJA_NO_WM_RULES=1` to keep the compositor's config entirely your own.

The rule matters because editors are X11 windows of their own: tiled, the
plugin keeps drawing at its own size and the rest of the tile is dead space.

Hyprland 0.56 moved its config to Lua, and its `keyword` command now refuses a
windowrule outright — `keyword can't work with non-legacy parsers. Use eval.`
So the rule goes in as Lua, with `keyword windowrulev2` left as the fallback for
older builds. Written by hand it reads:

```lua
hl.window_rule({
    name = "nirbija_plugin_editor",
    match = { class = "^(nirbija-plugin)$" },
    float = true,
    center = true,
})
```

A bind, if you want one — this is the only part still worth adding by hand:

```lua
hl.bind("SUPER SHIFT, N", hl.dsp.exec_cmd("nirbija"))
```

## Other compositors

Only Hyprland can be told at runtime, so everywhere else the rule is still
manual. Sway, in `~/.config/sway/config`:

```
for_window [class="nirbija-plugin"] floating enable, move position center
```

## Traces go to the journal

`NIRBIJA_DEBUG_EMBED=1` and friends print through Qt's logging, which on a
systemd distro is routed to the journal rather than to the terminal — a
redirect of stderr catches nothing and the flag looks broken. Read them with
`journalctl --user -f`, or force the old behaviour:

```sh
QT_FORCE_STDERR_LOGGING=1 NIRBIJA_DEBUG_EMBED=1 nirbija
```
