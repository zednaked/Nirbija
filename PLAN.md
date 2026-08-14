# Nirbija — plano

Host de plugins + mixer para Linux desktop, no espírito do AUM (Kymatica, iOS).
Layout e fluxo de trabalho inspirados; código, assets, ícones e marca são próprios.

## Decisões travadas

| Eixo | Escolha | Porquê |
|---|---|---|
| Plataforma | Linux desktop nativo | latência real, acesso a PipeWire/JACK e aos plugins nativos |
| Linguagem | C++20 | os três SDKs de plugin (CLAP, LV2, VST3) são C/C++ nativos |
| Build | CMake ≥ 3.28 | `FetchContent` para SDKs vendorizados |
| Áudio | JACK API sobre PipeWire | `pipewire-jack` já instalado (3.1608.0); patchbay de graça |
| UI | Qt6 QML (6.11) | GPU-composited, touch-first, combina com faders/strips do AUM |
| Plugins | CLAP + LV2 + VST3 | os três, atrás de uma interface comum |
| Licença | **GPLv3** | consequência do SDK VST3 (a alternativa é acordo com a Steinberg) |

## Restrições descobertas nesta máquina

- Sessão é **Wayland/Hyprland**. Praticamente toda GUI de plugin em Linux é
  X11-only. O app roda sob **XWayland** (`QT_QPA_PLATFORM=xcb`) para conseguir
  reparentar as janelas de plugin. Sem isso, só UI genérica gerada de parâmetros.
- `suil` não está instalado (`suil-0.pc` não encontrado) — necessário na fase 7
  para hospedar UI de LV2. `lilv 0.28.0` presente.
- Headers CLAP não estão no sistema — vão vendorizados via `FetchContent`.
- Plugins reais já disponíveis pra teste: Dragonfly (LV2 e VST3), Odin2 (LV2).

## Arquitetura

```
ui (Qt6 QML, thread da UI)
      │  comandos lock-free (SPSC ring)  ▲ snapshots de medidor (atomics)
      ▼                                  │
engine (thread realtime do JACK)
      ├── AudioGraph — nós, ordem topológica, buffers pré-alocados
      ├── ChannelStrip — gain, pan, mute/solo, inserts, medidor
      └── PluginInstance (interface)
              ├── Lv2Instance   (lilv)
              ├── ClapInstance  (clap headers)
              └── Vst3Instance  (VST3 SDK)
```

Regra dura: a thread realtime não aloca, não trava mutex, não faz I/O. Toda
mudança de estrutura (adicionar canal, carregar plugin) é preparada na thread
da UI e entregue pronta por mensagem; o descarte volta pela fila de lixo.

## Fases

0. **Scaffold** — CMake, layout de diretórios, descoberta de deps, git. ← atual
1. **Engine** — cliente JACK, grafo, strip DSP, fila de comandos, medidores.
2. **Host LV2** — interface `PluginInstance` + backend lilv, provado por CLI
   processando áudio pelo DragonflyHallReverb.
3. **Host CLAP** — scan de `~/.clap` e `/usr/lib/clap`, params, estado.
4. **UI mixer** — strips verticais, fader+medidor, slots de insert, master, patch view.
5. **GUI de plugin** — embedding X11: suil (LV2), extensão `gui` (CLAP).
6. **MIDI** — portas JACK MIDI, roteamento por canal, MIDI learn.
7. **Gravador + looper** — ring buffer realtime → thread de escrita, WAV/FLAC;
   loops com lançamento quantizado.
8. **Sessão** — serialização do grafo + blobs opacos de estado dos plugins.
9. **Host VST3** — adiado (ver abaixo).

Ordem é dependência real, não preferência: 5 precisa de 4, 4 precisa de 2–3,
todos precisam de 1.

## Fase 5 — embedding de GUI: funcionando

Editoras nativas de CLAP e LV2 desenham dentro do host. Verificado com Surge XT
(CLAP) e Dragonfly Hall Reverb (LV2).

Três coisas precisaram estar certas ao mesmo tempo, e cada uma sozinha dava
janela preta:

1. **A janela pai tem que ser Xlib pura.** Um `QWindow` do xcb não serve: os
   toolkits de plugin realizam a view contra o handle recebido e esperam uma
   janela X11 que possam dominar. `PluginWindow` cria a dela com
   `XCreateSimpleWindow`, bombeia os próprios eventos e não passa pelo sistema
   de janelas do Qt.
2. **O host precisa rodar o event loop do plugin CLAP.** No Linux o plugin não
   roda loop próprio: ele registra timers e file descriptors no host pelas
   extensões `clap.timer-support` e `clap.posix-fd-support`, e espera ser
   chamado de volta. Sem isso a editora cria a janela e nunca pinta um pixel —
   foi exatamente o sintoma. Servidos em `ClapInstance::pump_main_thread`.
3. **A janela filha precisa ser mapeada pelo host.** O plugin cria a dela e
   frequentemente a deixa sem mapear (`map_state=0`); `adoptChild()` mapeia e
   adota o tamanho dela.

Diagnóstico: `NIRBIJA_DEBUG_EMBED=1` imprime a árvore de janelas filhas com
tamanho e `map_state` — foi o que apontou o item 3.

### GLX nesta máquina

Editora LV2 que usa OpenGL (DPF/Pugl, robtk/x42) falhava em
`Failed to realize Pugl view`. Não era bug nosso: `glXCreateContext` falha com
`BadValue` neste XWayland, para qualquer programa. Provado com um probe de 20
linhas fora do projeto.

Contorno: `__GLX_VENDOR_LIBRARY_NAME=mesa`. Com isso a Dragonfly desenha. O
host avisa disso quando uma UI LV2 recusa instanciar, já que o plugin não diz
nada.

### Ainda torto

- O tamanho da janela fica a cargo do compositor. O Hyprland tila as janelas e
  ignora `XResizeWindow` e `XSizeHints`, então sobra área morta em volta da
  editora. Numa regra de janela flutuante isso desaparece.
- `force_x11_platform()` troca o app para xcb quando a sessão é Wayland, senão
  `winId()` devolve um ponteiro e a primeira chamada X do plugin morre com
  `BadWindow`. Escape: `NIRBIJA_ALLOW_WAYLAND=1`, perdendo as editoras.
- `nirbija_gui_probe "<nome>"` abre a editora de um plugin isolada, para teste.

## VST3 — adiado de propósito

Decisão de 14/08/2026: fica para quando o resto estiver de pé. Motivos:

- É o backend mais caro dos três (SDK vendorizado, API COM-like, arranjo de bus
  a negociar) e o que menos ensina sobre a arquitetura do host — CLAP e LV2 já
  provaram que a interface `PluginInstance` aguenta formatos diferentes.
- Carimba **GPLv3** no projeto. Enquanto ele não entra, a licença fica em aberto.
- Nada depende dele: a UI, o MIDI, o gravador e a sessão são todos indiferentes
  ao formato do plugin.

O que já está pronto para recebê-lo: a opção `NIRBIJA_VST3` no CMake (default
`OFF`), o ramo em `make_all_backends()`, e `PluginFormat::Vst3` na interface.
Falta o `src/hosting/vst3_backend.cpp` — `IComponent`/`IAudioProcessor`/
`IEditController`, arranjo de bus, sincronismo de parâmetro.

## Como validar cada fase

Cada backend de plugin ganha um teste CLI que carrega um plugin instalado de
verdade, processa um bloco de áudio e confere que a saída não é silêncio nem
NaN. Nada de mock de plugin — o valor do host está justamente em aguentar
plugin de terceiro mal comportado.
