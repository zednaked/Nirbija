# Sequenciador de passos — desenho

Preenche o Tier 1 do `ECOSYSTEM.md`: dos 45 plugins MIDI instalados nesta
máquina, quase todos **processam** notas que já existem. Este **gera**.

Modelo: Rozeta Bassline e Rozeta Rhythm (Bram Bos), o que faz o AUM valer a pena
no iPad. Não é clone — é a mesma categoria.

## O achado que torna isso barato

**O host não precisa de nada novo.** A plumbing já existe e já está ligada:

| O que | Onde |
|---|---|
| Posição, andamento, e se o transporte pulou | `core/plugin.h:56` `TransportInfo`, entregue por `set_transport()` antes de cada bloco |
| Plugin devolver MIDI que ele gerou | `core/plugin.h:146` `take_midi_output()` |
| Esse MIDI alimentar o **próximo insert do strip** | `core/channel_strip.cpp:69` |

O comentário em `plugin.h:143` já diz, escrito antes deste documento existir:

> a step sequencer or arpeggiator is a plugin whose whole output is MIDI, and it
> is worth nothing if the host never reads it

Ou seja: sequenciador no slot 1, synth no slot 2, e o strip liga os dois. É
exatamente o modelo do AUM, e já funciona.

## Forma

Plugin **interno** (`PluginFormat::Internal`), como o Looper e o File Player.
Zero áudio: `audio_inputs = 0`, `audio_outputs = 0`, `kind = MidiEffect`. O
`kind_from_ports` já classifica isso certo sozinho.

Sai como `nirbija.stepseq`, ao lado de `nirbija.looper`.

## Modelo de dados

```
passos       16 (fixo no primeiro corte; 1..64 depois)
por passo    nota, velocidade, gate (fração do passo), probabilidade, ativo
globais      divisão (1/4, 1/8, 1/16, tercinas), swing, transposição,
             comprimento (quantos passos antes de voltar), canal MIDI
```

Tudo em `float`/`uint8_t` de tamanho fixo, num array. **Nada de `std::vector`
crescendo em tempo real** — vale a mesma regra dura do resto do projeto: a
thread realtime não aloca, não trava mutex, não faz I/O.

## O laço, e a única parte difícil

Cada `process()` recebe um bloco de `frames` amostras e sabe, por
`TransportInfo`, em que batida o bloco **começa**. O trabalho é: descobrir quais
passos caem dentro deste bloco, e emitir note-on/note-off no frame certo.

```
beats_por_passo = 4 / divisao
passo_no_inicio = transport.beats / beats_por_passo
passo_no_fim    = (transport.beats + frames/sample_rate * bpm/60) / beats_por_passo
```

Para cada fronteira de passo entre os dois, converte a batida de volta pra
frame e emite. O note-off do passo anterior sai no frame calculado pelo gate.

Três coisas que dão bug e valem escrever antes de codar:

1. **`transport.changed`.** Existe exatamente pra isso (`plugin.h:67`): o
   transporte pulou ou começou. Todo note-on pendente vira note-off imediato e
   o contador ressincroniza a partir de `beats`. Sem isso, parar e voltar deixa
   nota presa.
2. **Posição vem do transporte, nunca de contador próprio.** Contar blocos
   funciona até o primeiro pulo e desanda em silêncio. `beats` é a verdade.
3. **Parar é emitir note-off, não parar de emitir.** `playing` indo pra `false`
   com uma nota no ar é o jeito clássico de deixar um synth zumbindo.

## Parâmetros antes de UI

`parameters()` / `parameter_value()` / `set_parameter()` já são a interface
(`plugin.h:152`), e o `ParamEditor.qml` genérico desenha slider pra qualquer
plugin sem editora. Então:

**Expor tudo como parâmetro dá uma UI de graça no primeiro dia**, e ainda ganha
MIDI learn e automação sem uma linha a mais. A grade de UI própria vem depois,
quando o desenho estiver provado.

São ~5 parâmetros globais + 5 por passo × 16 = 85. Muito pra um editor genérico
ficar agradável, mas o suficiente pra **provar o plugin antes de investir em
QML**.

## Estado

`save_state()` / `load_state()` com o array de passos. Formato: texto simples,
uma linha por passo, como o resto dos plugins internos faz. Sessão carrega, o
padrão volta se o blob estiver truncado — `parse_number` (`plugin.h:92`) existe
justamente porque `std::stod` joga exceção em arquivo editado à mão.

## Ordem de construção

1. **Passos fixos, divisão fixa em 1/16, nota e velocidade por passo.** Prova o
   laço contra o transporte, que é a única parte difícil. Testável offline, sem
   JACK, como `graph_routing` e `looper` já são.
2. Gate, probabilidade, comprimento, transposição.
3. Swing e divisões com tercina.
4. UI própria em QML — grade de 16, arrastar pra nota, tocar pra ligar.
5. Só então: extrair pra CLAP, se tiver ficado bom.

O teste do passo 1 é o de sempre neste projeto: roda um número conhecido de
blocos com transporte sintético, conta os eventos que saíram e confere os
frames. Sem servidor de áudio, sem plugin externo.

## Por que não CLAP direto

Ver `ECOSYSTEM.md`, seção da rota de entrega. Como interno herda UI, sessão e
roteamento MIDI que já existem e custa zero empacotamento; a extração pra CLAP
depois é só trocar a casca, porque o DSP e o estado são os mesmos. Provar
primeiro, distribuir depois.
