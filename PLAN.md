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
4. **Host VST3** — SDK vendorizado, `IComponent`/`IAudioProcessor`/`IEditController`.
5. **UI mixer** — strips verticais, fader+medidor, slots de insert, master, patch view.
6. **GUI de plugin** — embedding X11: suil (LV2), extensão `gui` (CLAP), `IPlugView` (VST3).
7. **MIDI** — portas JACK MIDI, roteamento por canal, MIDI learn.
8. **Gravador + looper** — ring buffer realtime → thread de escrita, WAV/FLAC;
   loops com lançamento quantizado.
9. **Sessão** — serialização do grafo + blobs opacos de estado dos plugins.

Ordem é dependência real, não preferência: 6 precisa de 5, 5 precisa de 2–4,
todos precisam de 1.

## Como validar cada fase

Cada backend de plugin ganha um teste CLI que carrega um plugin instalado de
verdade, processa um bloco de áudio e confere que a saída não é silêncio nem
NaN. Nada de mock de plugin — o valor do host está justamente em aguentar
plugin de terceiro mal comportado.
