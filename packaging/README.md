# Instalação no desktop

`Nirbija.desktop` é o lançador. Copie para `~/.local/share/applications/` e
ajuste o caminho em `Exec=` se o repo não estiver em `~/dev/Nirbija`.

Duas coisas nele não são decoração:

- `env __GLX_VENDOR_LIBRARY_NAME=mesa` — nesta máquina o vendor GLX padrão faz
  `glXCreateContext` falhar com `BadValue` sob XWayland, e toda editora de
  plugin em OpenGL abre preta. Forçar a Mesa resolve. Se a sua máquina não tem
  esse problema, pode tirar.
- `StartupWMClass=nirbija` — casa a janela com o lançador.

## Hyprland

Testado no Hyprland 0.56, cuja sintaxe de regra usa `match:class` e campos com
valor — a forma antiga `windowrule = float, class:...` é recusada com
`invalid field float: missing a value`.

```
bind = $mainMod SHIFT, N, exec, env __GLX_VENDOR_LIBRARY_NAME=mesa $HOME/dev/Nirbija/build/src/ui/nirbija

windowrule {
    name = nirbija_plugin_editor
    match:class = ^(nirbija-plugin)$
    float = true
    center = true
}
```

A regra de flutuante importa: as editoras de plugin são janelas X11 próprias, e
tiladas elas ficam esticadas com área morta em volta do desenho do plugin. Elas
carregam a classe `nirbija-plugin` justamente para poderem ser alvo de regra.
