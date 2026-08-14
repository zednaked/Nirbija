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

```
bind = $mainMod SHIFT, N, exec, env __GLX_VENDOR_LIBRARY_NAME=mesa $HOME/dev/Nirbija/build/src/ui/nirbija

windowrule = float, class:^(nirbija-plugin)$
windowrule = center, class:^(nirbija-plugin)$
```

A regra de flutuante importa: as editoras de plugin são janelas X11 próprias, e
tiladas elas ficam esticadas com área morta em volta do desenho do plugin. Elas
carregam a classe `nirbija-plugin` justamente para poderem ser alvo de regra.
