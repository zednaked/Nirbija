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

## Fase 5 — embedding de GUI: onde parou

Feito e funcionando:

- Interface `PluginGui` no núcleo (`attach`/`detach`/`idle`/`preferred_size`).
- CLAP: extensão `gui` com `CLAP_WINDOW_API_X11`. A negociação inteira funciona —
  `create` → `set_parent` → `get_size` → `set_size` → `show` retornam sucesso, e
  a janela hospedeira abre no tamanho que o próprio plugin pediu (638x680 no
  Surge XT).
- LV2: X11UI carregada direto do binário da UI, sem suil (suil só é necessário
  para envolver UI de toolkit diferente do host).
- `force_x11_platform()`: o app troca para xcb quando a sessão é Wayland, senão
  `winId()` devolve um ponteiro e a primeira chamada X do plugin morre com
  `BadWindow`. Escape: `NIRBIJA_ALLOW_WAYLAND=1`.
- `nirbija_gui_probe "<nome>"`: ferramenta manual que abre a editora de um
  plugin isolada.

**Não funciona ainda: a editora não desenha.** A janela abre no tamanho certo e
fica preta.

- CLAP (Surge XT): todas as chamadas retornam sucesso, nenhum pixel aparece.
- LV2/DPF (Dragonfly): falha antes, em `Failed to realize Pugl view`.

Tentado sem sucesso: mapear a janela antes do `attach`; informar o tamanho ao
plugin com `set_size`; sincronizar o X (`processEvents` + `sync`) antes de
entregar o handle.

Hipótese para a próxima investida: um `QWindow` do xcb não é o pai que esses
toolkits esperam. Hosts que funcionam (Carla, Ardour) criam a janela pai com
Xlib puro e/ou implementam o protocolo XEmbed. O próximo passo é trocar
`PluginWindow` por uma janela Xlib criada à mão, embrulhada num container Qt.

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
