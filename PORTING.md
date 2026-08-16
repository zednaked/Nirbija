# Nirbija fora do Linux — o que custaria

Levantamento feito em 16/08/2026, logo depois da v0.1.0. **Não é um plano de
trabalho**: é o mapa medido, guardado pra não precisar redescobrir se um dia a
demanda aparecer. A decisão atual é **não portar** — ver "Por que não agora".

Números vêm de contagem no código, não de estimativa. As linhas de comando que
os produzem estão no fim, pra poder refazer a medição quando o código andar.

## O tamanho da coisa

| Área | Linhas | Portável hoje? |
|---|---:|---|
| QML | 3.972 | **sim**, Qt roda nos três |
| UI em C++ (models, sessão, skin) | 3.310 | sim, **menos** `plugin_window.cpp` (343) e `platform.h` (27) |
| `core/` — grafo, strip, looper, recorder, player | 3.467 | sim, **menos** `engine.cpp` (731) |
| `hosting/` — CLAP, LV2, VST3 | 3.430 | sim, **menos** ~16 linhas de embedding |
| testes | 2.287 | os offline sim |

Isto é: a esmagadora maioria do projeto já é portátil. O que prende é pequeno em
linhas e grande em dificuldade — as duas coisas não andam juntas aqui.

## O que já está no lugar certo

Duas escolhas antigas envelheceram bem e economizam a maior parte do trabalho de
interface:

`src/core/plugin.h:76`

```cpp
virtual bool attach(uintptr_t parent_window) = 0;
```

O contrato de embedding já é neutro. `uintptr_t` carrega um `Window` do X11 hoje,
carregaria um `HWND` no Windows e um `NSView*` no Mac **sem mudar assinatura**.
Só as implementações mudam.

`src/ui/plugin_window.h` — a interface pública de `PluginWindow` é
`open()`, `close()`, `isOpen()` e um sinal `closed()`. Nada de X11 vaza pra
quem usa. O específico está todo nos membros privados (`void* display_`,
`unsigned long window_`) e no `.cpp`.

Ou seja: a fronteira de portabilidade já existe e está desenhada no lugar certo.
Portar é **preencher outra implementação atrás dela**, não refatorar o projeto.

## Os três muros, em ordem de dificuldade

### 1. Embedding de editora — o muro de verdade

`src/ui/plugin_window.cpp`, 343 linhas de Xlib puro. Usa 26 funções X
distintas: cria a janela com `XCreateSimpleWindow`, bombeia os próprios eventos
(`XNextEvent`/`XPending`/`XCheckTypedWindowEvent`), adota a janela filha via
`XQueryTree` + `XMapWindow`, e instala `XSetErrorHandler` para o erro de um
plugin não derrubar o mixer.

Isso não abstrai — **reescreve por plataforma**. Nada aqui tem equivalente
portável.

E os três backends pedem X11 por nome:

| Arquivo | Linha | Hoje | Mac | Windows |
|---|---|---|---|---|
| `clap_backend.cpp` | 83, 87, 412 | `CLAP_WINDOW_API_X11` | `CLAP_WINDOW_API_COCOA` | `CLAP_WINDOW_API_WIN32` |
| `vst3_backend.cpp` | 1188, 1198 | `kPlatformTypeX11EmbedWindowID` | `kPlatformTypeNSView` | `kPlatformTypeHWND` |
| `lv2_backend.cpp` | 186, 205 | `LV2_UI__X11UI` | `LV2_UI__CocoaUI` | `LV2_UI__WindowsUI` |

São ~16 linhas de constante, mas cada troca puxa uma implementação de janela
inteira atrás.

**Mac é bem pior que Windows.** Enfiar um `NSView` de plugin dentro de uma
janela Qt exige código Objective-C++ e um ciclo de vida de view que briga com o
do Qt. `HWND` no Windows é reparentável de forma parecida com o que já se faz
aqui — é o caminho mais curto dos dois.

### 2. Áudio — `engine.cpp`, 731 linhas

Usa 31 símbolos do JACK, dos quais a maioria são tipos e trampolins. A API real
é pequena e comum: abrir/fechar cliente, registrar/remover portas, pegar buffer
de porta, conectar/desconectar, callbacks de processo, sample rate e buffer
size, e leitura/escrita de MIDI.

JACK **compila** no Mac e no Windows, então tecnicamente rodaria. Mas ninguém
usa JACK nessas plataformas: o caminho honesto é CoreAudio no Mac e
WASAPI/ASIO no Windows, atrás de uma interface — ou RtAudio/miniaudio, que já
cobrem os três.

A boa notícia: dessas 731 linhas, o grosso é grafo, ordem topológica e regra de
tempo real. O que é JACK mesmo é a casca fina em volta.

### 3. Caminhos de scan — trivial

Seis linhas com diretório do Linux cravado:

```
clap_backend.cpp:29   /usr/lib/clap, /usr/local/lib/clap, ~/.clap
vst3_backend.cpp:1249 /usr/lib/vst3, /usr/local/lib/vst3, ~/.vst3
```

Mac quer `/Library/Audio/Plug-Ins/{CLAP,VST3}` e o equivalente em `~`; Windows
quer `%COMMONPROGRAMFILES%\{CLAP,VST3}`. Meia hora de trabalho.

## O que fica mais fácil fora do Linux

Vale registrar, porque é contra-intuitivo: **o pedaço mais difícil do host
some**.

`clap_backend.cpp:418 pump_main_thread()` existe porque no Linux o plugin CLAP
não roda loop de eventos próprio — ele registra timers e file descriptors no
host (`CLAP_EXT_TIMER_SUPPORT`, `CLAP_EXT_POSIX_FD_SUPPORT`) e espera ser
chamado de volta. Foi o item 2 do `PLAN.md`, o que fazia a editora criar janela
e nunca pintar um pixel.

No Mac e no Windows o plugin usa o run loop nativo. Todo esse mecanismo, e o
`Linux::IRunLoop` equivalente no VST3, deixam de ser necessários.

LV2, por outro lado, praticamente não existe nessas plataformas. O valor real
lá seria **CLAP + VST3 só**.

## Custo que não é código

Costuma ser esquecido e costuma ser o que trava:

- **macOS** — sem conta Apple Developer (US$ 99/ano) e notarização, o Gatekeeper
  bloqueia o app pra qualquer pessoa que baixe. Na prática não é opcional.
- **Windows** — sem certificado de assinatura (US$ 200–400/ano), o SmartScreen
  assusta todo mundo no primeiro download.
- **Matriz de teste** triplica, e plugin é onde bug de host aparece: cada
  formato × cada plataforma × cada toolkit de editora.
- **GPLv3** não impede nada nos três. As pluginterfaces do VST3 continuam
  compatíveis.

## Por que não agora

1. Mac e Windows já têm host de plugin bom e barato. Linux não tem equivalente
   ao AUM — é exatamente o buraco que Nirbija preenche.
2. É o que o produto já diz de si: `content.json` lista
   `compat: linux, pipewire, jack` e `does not: run on windows or macos`. É
   posicionamento, não desculpa.
3. A v0.1.0 acabou de sair e ainda não tem usuário. Portar antes de saber se
   alguém quer é montar três matrizes de teste sem retorno.

**Se um dia for**: Windows primeiro. `HWND` é muito mais simples que `NSView`,
e a burocracia é mais barata. A ordem seria caminhos de scan → áudio →
embedding, porque cada um é testável sozinho e o último é o caro.

## Refazer a medição

```sh
# linhas por área
wc -l src/core/*.cpp src/core/*.h src/hosting/*.cpp src/ui/*.cpp src/ui/qml/*.qml

# superfície da API JACK
grep -rho 'jack_[a-z_]*' src/core/engine.cpp | sort -u

# superfície do Xlib
grep -rho 'X[A-Z][A-Za-z]*' src/ui/plugin_window.cpp | sort -u

# onde X11 está cravado nos backends
grep -rn 'X11\|kPlatformType\|X11UI' src/hosting/*.cpp

# caminhos de scan cravados
grep -rn '"/usr/lib\|"\.clap"\|"\.vst3"' src/hosting/*.cpp
```
