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
