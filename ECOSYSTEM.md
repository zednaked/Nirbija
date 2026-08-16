# O que falta no ecossistema livre

Levantamento de 16/08/2026. Metade medida, metade não — e as duas estão
marcadas, porque misturar as duas é como esse tipo de documento apodrece.

- **O lado livre é contado**, com `nirbija_scan` nesta máquina. Os números
  saem do comando no fim do arquivo.
- **O lado iPad é de conhecimento**, até maio de 2026, sem verificação contra a
  App Store. Trate como direção, não como inventário.

## O que está instalado aqui

286 plugins, entre 224 LV2, 29 CLAP, 31 VST3 e 2 internos — com muita duplicata,
já que Dragonfly, Zam, Surge e Airwindows aparecem em dois ou três formatos.

Depois que o scan passou a ler a classificação que cada formato declara:

| Tipo | Quantos |
|---:|---|
| efeito | 148 |
| MIDI | 45 |
| analisador | 44 |
| utilitário | 31 |
| instrumento | 18 |

Esses cinco números **são o mapa**. Efeito e análise somam 192 de 286; o que faz
música — instrumento — são 18.

## Onde o livre já ganha

Começando por aqui, senão o resto vira lamento.

| Área | O que existe | Contra o iPad |
|---|---|---|
| Medição | x42: EBU, BBC, DIN, K-12/14/20, true-peak, DR-14, correlação, histograma, escopo de fase | Nada no iPad chega perto |
| Efeito de mixagem | Calf (52), Zam (25), Airwindows Consolidated, CHOW Tape, Dragonfly | Saturado, e melhor |
| Síntese | Surge XT | Melhor que quase todo synth pago de iPad |
| Modular | Cardinal (VCV Rack) | Drambo é mais divertido; Cardinal é mais fundo |
| Guitarra | Neural Amp Modeler | Empata com BIAS FX, e treina modelo próprio |
| Processamento MIDI | 45 plugins — filtro, mapa, transposição, escala, strum, legato | Mais completo que qualquer coisa de iPad |

**Não construa efeito.** A área está resolvida e a competição é de graça.

## Onde o iPad ganha

O padrão é nítido: o iPad não ganha em DSP. Ganha em **coisas que produzem
ideia** e em **coisas feitas pra dedo**.

E o contraste aparece nos números acima. Dos 46 plugins MIDI instalados aqui,
praticamente todos **processam** notas que já existem. Quase nenhum **gera** —
o Step Sequencer que fecha o Tier 1 abaixo é literalmente o quadragésimo sexto,
e entrou nessa conta ao ser escrito.

### Tier 1 — buraco grande, encaixe direto

**1. Sequenciadores MIDI como plugin.** A Rozeta Sequencer Suite (Bram Bos) é o
que faz o AUM ser AUM: Bassline, Rhythm, Arpeggio, XOX, Particles, LFO, Scaler,
Collider. Plugin que gera, não que filtra.

| iPad | Livre |
|---|---|
| Rozeta Bassline (303-style) | ✅ **Step Sequencer** (`nirbija.stepseq`) |
| Rozeta Rhythm / XOX | Stochas, e o Step Sequencer empilhado |
| Rozeta Particles (probabilístico) | — |
| Fugue Machine (multi-playhead) | — |
| Playbeat / Riffer (generativo) | — |
| Atom 2 (piano roll AUv3) | — |

**Metade feita.** O Step Sequencer é interno, monofônico, com grade própria, e
o host não precisou de uma linha: `set_transport`, `take_midi_output` e a cadeia
em `channel_strip.cpp` já estavam ligados. Isso também mostrou que a monofonia
não limita — `sessions/jam-goth.json` empilha quatro deles num strip antes do
DrumGizmo e sai uma bateria de quatro vozes.

O que falta do desenho é o resto da suíte: probabilidade por passo, múltiplos
cabeçotes de leitura, geração por regra. `design/sequencer.md` cobre o que foi
feito; o restante é escopo novo.

**2. Plugin MIDI scriptável.** ✅ **Feito** — `nirbija.script`, Lua embutido.

O desenho de `design/scripting.md` foi seguido na variante C: o script **não
roda no caminho do áudio**. Ele compila tabelas na thread da UI, e a thread de
áudio só indexa. Cobre escala, acorde, curva de velocity e roteamento de canal;
**não** cobre script que reage à nota que acabou de chegar, e isso está dito no
próprio plugin.

Sandbox por lista de permissão (sem `io`, `os`, `package`, `load`, `require`) e
orçamento de instruções, porque sessão se troca entre pessoas e script vem
junto.

**Tier 1 fechado.**

### Tier 2 — buraco real, mais trabalho

| Buraco | Referência | Estado livre |
|---|---|---|
| Sampler de performance | Koala Sampler | sfizz quer arquivo SFZ, DrumGizmo quer kit. Nada de "grava e toca" |
| Arpejador | Rozeta Arpeggio | recebe nota e gera acorde arpejado. O Step Sequencer já tem metade das peças |
| Looper como plugin | Loopy Pro | SooperLooper (standalone, envelhecido), Luppp. Nada em LV2/CLAP |
| Acorde e escala | Scaler 2, Chordjam | x42 força escala; nada composicional |

### Tier 3 — não vale

Efeito, medição, síntese subtrativa, simulação de amplificador. Saturados e bons.

## A rota de entrega que o Nirbija tem e ninguém mais

O scan mostra três plugins de formato `internal` — File Player, Looper e agora
o Step Sequencer, que foi por essa rota justamente pra provar a ideia antes de
empacotar. Isso é
um veículo que nenhum outro projeto livre de áudio tem: dá pra embarcar
ferramenta junto com o host, sem pedir instalação e sem empacotar nada.

| Rota | Alcance | Custo |
|---|---|---|
| CLAP/LV2 separado | Todo o ecossistema — Ardour, Carla, Reaper, Bitwig | Cada um é um projeto: build, empacotamento, editora, release |
| Interno do Nirbija | Só o Nirbija | Herda UI, sessão e MIDI que já existem. Zero empacotamento |

Recomendação: **prova a ideia como interno, extrai pra CLAP se ficar boa.** O
DSP é o mesmo; muda a casca.

## O buraco que não é DSP

Havia 286 plugins aqui e o picker era uma lista plana com busca por texto —
achar um reverb exigia saber como o reverb se chamava.

Isso foi consertado junto com este documento: os três formatos declaram sua
própria classificação (classe LV2, features do CLAP, `subCategories` do VST3), e
nada disso estava sendo lido. Agora o picker filtra por tipo e mostra a
categoria que o plugin dá a si mesmo.

Vale como princípio: **antes de escrever plugin novo, torne o que já existe
encontrável.** 286 plugins que ninguém acha são menos úteis que 30 organizados.

## Refazer a medição

```sh
cmake --build build --target nirbija_scan
./build/tests/nirbija_scan | tail -8          # a contagem por tipo
./build/tests/nirbija_scan | grep -c ''       # quantos ao todo
```

Uma pilha de `unknown` na contagem quer dizer que algum backend parou de ler a
classificação do formato — é o alarme desse arquivo.
