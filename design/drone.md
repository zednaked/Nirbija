# Drone — desenho

Um instrumento para drone music: seis cordas que nunca param de soar, um
pedal que sobe e desce o conjunto em segundos, e um clima lento por cima.
Interno (`nirbija.drone`), na mesma rota que o Step Sequencer, o FX Pad e o
Sampler: prova a ideia dentro do host, extrai para CLAP se ficar boa.

## Por que existe

`ECOSYSTEM.md` mede onde o lado livre perde: não é em DSP, é em **coisa que
produz ideia** e **coisa feita pra dedo**. Um drone dá pra montar hoje com
Surge XT mais um reverb longo, e é assim que todo mundo faz — e é por isso que
quase ninguém faz. São vinte parâmetros espalhados por três janelas para
chegar num som que tem quatro decisões: que notas, quanto de cada, quanto
desafinam entre si, e quanto tempo o som leva pra chegar e pra ir embora.

Este plugin é essas quatro decisões, e mais nada, numa janela só.

## O que é um drone, para este desenho

- **Não tem note-on nem note-off.** O som está sempre lá; o que muda é quanto
  dele passa. Um sintetizador que para quando a tecla solta não é um drone.
- **Bate.** Duas cordas quase na mesma nota produzem um batimento lento, e
  esse batimento é o conteúdo. Afinação em cents é controle de primeira
  classe, não um "fine tune" escondido.
- **Trava.** Em entonação justa a quinta é exatamente 3:2 e o conjunto
  ressoa; em temperamento igual ela bate devagar contra a própria escala.
  Tanpura, harmônio e shruti box são justos. O plugin nasce justo, com a
  opção de temperar.
- **Deriva.** Deixado sozinho por um minuto, um drone de verdade não fica
  parado: afinação e peso das vozes passeiam devagar. Isso aqui se chama
  `Drift` (quanto) e `Tide` (quão devagar).
- **Chega e vai embora devagar.** O `Swell` é o instrumento. Sobe em `Rise`
  segundos, desce nos mesmos segundos, e a sala (`Space`) fica soando depois
  que ele desce, porque o reverb vem *depois* do swell na cadeia. Nasce em
  0,75: inserido num strip, o drone sobe sozinho em quatro segundos. Começou
  em zero, e um instrumento mudo no strip parece quebrado antes de parecer à
  espera.

## Forma

`audio_inputs = 0`, `audio_outputs = 2`, `has_midi_input = true`,
`kind = Instrument`. Uma nota MIDI **re-enraíza** o drone: a fundamental passa
a ser aquela nota e as seis cordas deslizam para lá em `Glide` segundos.
Note-off não faz nada. Não há voz por tecla; um acorde tocado às pressas vira
um deslize lento até a última tecla, o que é o comportamento certo para um
drone e o errado para qualquer outra coisa.

Mono funciona: um strip de um canal recebe o lado esquerdo.

## Parâmetros

37 parâmetros, por id, expostos em `parameters()` — então o MIDI learn
genérico do host mapeia qualquer um, e o editor só precisa de um `setParam`
por id e um snapshot.

| Id | Corda (×6, stride 4) | Faixa |
|---:|---|---|
| 4v+0 | Interval — semitons da fundamental, inteiro | −24..+24 |
| 4v+1 | Detune — cents | −50..+50 |
| 4v+2 | Level | 0..1 |
| 4v+3 | Shape — 0 seno, 0,5 triângulo, 1 dente de serra (polyBLEP) | 0..1 |

| Id | Global | Faixa |
|---:|---|---|
| 24 | Swell | 0..1 |
| 25 | Rise — segundos, tanto para subir quanto para descer | 0,05..30 |
| 26 | Root — nota MIDI | 24..84 |
| 27 | Just — 1 justo, 0 temperado | 0/1 |
| 28 | Drift — até ±12 cents e ±40 % de nível por corda | 0..1 |
| 29 | Tide — exponencial, 0,01..1 Hz | 0..1 |
| 30 | Cutoff — exponencial, 40 Hz..16 kHz | 0..1 |
| 31 | Resonance | 0..1 |
| 32 | Motion — quanto o filtro respira com a maré, até ±2,5 oitavas | 0..1 |
| 33 | Space — wet e decay juntos; no topo, feedback 0,98 | 0..1 |
| 34 | Grit — tanh antes do filtro, drive 1..9 | 0..1 |
| 35 | Width — L e R a alguns cents em direções opostas, cordas alternadas | 0..1 |
| 36 | Glide — exponencial, 0,05..8 s | 0..1 |

Rise, Tide e Glide são exponenciais porque a diferença entre 0,05 s e 0,1 s
se ouve e a entre 20 s e 20,05 s não; uma barra linear gastaria nove décimos
do curso lá em cima.

Intonação justa, cinco limites, uma razão por semitom:
`1, 16/15, 9/8, 6/5, 5/4, 4/3, 45/32, 3/2, 8/5, 5/3, 9/5, 15/8`. Oitavas por
`ldexp`. A quinta é 3/2 exata e o teste confere isso como número, antes de
ouvir qualquer coisa.

## Cadeia de áudio

```
6 × (osc L, osc R) · gain · pan  →  Σ · 0,4
  →  tanh(· drive) / √drive          Grit; também é o limitador suave da soma
  →  SVF TPT passa-baixa             cutoff · 2^(motion · respiração · 2,5)
  →  · swell²                        rampa linear ao quadrado: nasce do nada
  →  8 combs amortecidos ‖ 2 allpass Space; L e R com comprimentos diferentes
  →  dry · (1 − wet/2) + room · wet
```

Regra que manda mais aqui que em qualquer outro plugin do host: **tom
sustentado mostra toda costura.** Então:

- ganho de cada corda e posição do swell são suavizados **por amostra**;
- toda afinação (fundamental, intervalo, detune, deriva) passa por um
  glide por amostra com a constante de `Glide` — mexer numa corda enquanto
  ela soa não clica, desliza;
- filtro, forma de onda, grit, space, width e motion são suavizados por
  bloco com τ = 60 ms. O SVF é o TPT (Cytomic), que aceita coeficiente
  mudando por bloco sem transiente e é estável em qualquer cutoff — o
  Chamberlin do FX Pad precisaria de clamp em 0,35·sr e nunca abriria de
  verdade;
- a deriva é um passeio aleatório por corda (alvo novo a cada período da
  maré, com jitter de 0,5..1,5×, one-pole até ele), calculado por bloco e
  entregue ao glide por amostra;
- cada corda começa numa fase diferente por lado, para seis cordas não
  cruzarem zero juntas na primeira amostra.

O teste (`tests/drone_test.cpp`) mede o maior degrau amostra-a-amostra
durante o swell subindo e descendo e falha acima de 0,12 — o que seis tons
graves produzem sozinhos fica bem abaixo disso.

## Leituras para o editor

Atômicos escritos pela thread de áudio ao fim de cada bloco, lidos pela UI a
40 ms via `insertDroneSnapshot`: ganho efetivo por corda (com deriva e
swell), posição do swell, respiração do filtro (−1..1), pico do bloco e a
fundamental como está soando agora (deslizando).

## O editor

`DroneEditor.qml`, popup não modal e arrastável como o FX Pad.

**Seis cordas.** Cada uma é uma linha vertical desenhada num Canvas, vibrando
no seu **modo harmônico**: `round(2^(intervalo/12))` barrigas, com nós
marcados — a oitava vibra em duas barrigas, a décima segunda em três. A
amplitude é o ganho efetivo (então a corda para quando o swell desce), o
balanço lento respira na taxa do detune (o que faz o drone bater é o que faz
o desenho respirar), e uma barra atravessada mostra o nível. Cor por classe
de altura — uma quinta é sempre a mesma cor, em qualquer corda — e peso por
oitava.

Gesto: **tocar** a corda onde ela deve soar é o nível (absoluto, como o pad
do FX Pad, pelo mesmo motivo — pular é o ponto). **Deslizar de lado** é
detune em cents; o eixo é decidido pelo primeiro movimento. **Roda** move um
semitom, Shift+roda um cent. Teclado: setas para nível e detune, PgUp/PgDn
para intervalo, Home zera o detune. Abaixo, o detune em cents e uma barra de
forma de onda (SINE/TRI/SAW).

**Swell.** A barra alta à direita, com uma linha clara marcando onde o som
realmente está — o swell demora `Rise` segundos, e ver a linha subindo atrás
do dedo é o que faz o atraso ser intencional em vez de defeito.

**Sky.** O filtro como pad XY: cutoff no eixo X, ressonância no Y, uma faixa
mostrando até onde `Motion` deixa respirar e um ponto que passeia com a
maré. É o segundo elemento que se move sozinho.

**Clima.** Oito barras pequenas: Drift, Tide, Motion, Space, Grit, Width,
Glide, Rise. Tide, Glide e Rise mostram tempo (25 s, 378 ms), o resto,
percentual.

**Brilho.** Luz por baixo da porta, na cor da fundamental, tão forte quanto o
drone está alto. Com a janela meio vista do outro lado da sala escura, diz
que o drone está vivo.

**MAP.** Como no FX Pad: arma, toca num controle, gira o knob. Uma corda
mapeia o nível; o Sky mapeia cutoff à esquerda da marca e ressonância à
direita.

## Conjuntos de cordas

Sete presets, no botão **STRINGS** do cabeçalho: Open fifths (o de fábrica),
Tanpura, Sub and octaves, Harmonic series, Major shimmer, Minor fog, Unison
beat. Um preset é **só as cordas e a afinação** — intervalo, detune, nível e
forma das seis, mais Just. Fundamental, swell e clima ficam onde estão, então
dá pra trocar o conjunto por baixo de um drone soando e ele desliza para o
novo, sem clique. A tabela mora em `drone.cpp`; o teste confere que cada um
cai exato e que "Harmonic series" em afinação justa é 1, 2, 3, 4, 5, 6 vezes
a corda mais grave.

## Sequenciador em cima

Não há linha de código para isso, e é por desenho: o strip entrega o MIDI de
um insert ao seguinte, e o Drone lê nota como fundamental. Um Step Sequencer
acima dele no mesmo strip vira um sequenciador de fundamentais, e com `Glide`
alto cada passo é um deslize. `sessions/jam-drone-seq.json` demonstra
(D2 A1 G1 C2, quatro segundos cada) e `tests/drone_test.cpp` monta a cadeia
num `ChannelStrip` e confere que as duas notas chegam como raiz.

## CLAP

`src/clap/drone_clap.cpp` embrulha o mesmo `drone.cpp` num plugin CLAP —
descritor, fábrica e quatro extensões: params (37, com os inteiros marcados
como stepped e texto por parâmetro), state (o mesmo blob de texto), uma saída
estéreo e uma entrada de notas (dialeto CLAP e MIDI). Sem editor: o host
desenha os parâmetros. Sai em `build/clap/Nirbija Drone.clap`; copiar para
`~/.clap` e ele aparece no Ardour, no Reaper, no Bitwig — e no próprio
Nirbija, como um segundo Drone de formato CLAP.

Não é instalado com o host de propósito: o `cmake --install` deita o app e
nada mais, e um plugin no caminho CLAP do sistema é decisão separada.

`parse_number`/`format_number` saíram de `plugin.cpp` para
`core/state_text.cpp` para que o CLAP os leve sem arrastar backends, registro
e JACK. `tests/drone_clap.cpp` carrega o `.clap` pelo backend CLAP do próprio
host (via `CLAP_PATH`) e confere portas, parâmetros, nota que re-enraíza e
estado ida e volta — um host que não escreveu o wrapper, lendo o que ele
promete. De passagem, o backend CLAP passou a preencher `has_midi_input`, que
nunca preenchia.

## O que ficou de fora, de propósito

- **Voz por tecla.** Um drone que responde a acordes é um sintetizador de
  pads; o Surge XT já é melhor nisso.
- **Sequenciamento das cordas.** O Step Sequencer em cima do strip manda
  notas, e a nota re-enraíza. Isso já é um sequenciador de fundamentais.
- **Reverb por convolução, granular, feedback externo.** Space é Schroeder
  e é o bastante para o drone soar numa sala; para um espaço específico
  existe o Dragonfly no slot seguinte.

## Como verificar

```sh
cmake --build build-asan --target nirbija_drone && ./build-asan/tests/nirbija_drone
cmake --build build --target nirbija_editor_probe
NIRBIJA_SESSION=/tmp/probe.json QT_QPA_PLATFORM=offscreen \
  ./build/tests/nirbija_editor_probe nirbija.drone /tmp/drone.png 4 24=1 25=0.5
```

O segundo comando é a ferramenta manual que sobe o mixer inteiro, coloca o
plugin num strip, abre o editor e tira um retrato — o master fica mudo, então
nada toca. Foi assim que o layout foi acertado sem clicar em nada.
