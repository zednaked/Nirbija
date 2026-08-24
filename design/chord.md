# Acorde e escala — desenho

Fecha o Tier 2 do `ECOSYSTEM.md` que ainda estava em branco: "x42 força
escala; nada composicional". Modelo: Scaler 2 e ChordJam (Bram Bos, o mesmo
autor da suíte Rozeta que inspirou `design/sequencer.md`) — uma tecla dispara
um acorde inteiro, dentro de uma tonalidade escolhida, sem exigir teoria
musical de quem toca.

## Por que não é um script Lua

`nirbija.script` (`design/scripting.md`) já sabe montar `note_map` de escala e
`velocity_curve` — tecnicamente dava pra escrever isto em Lua hoje. Mas
`ECOSYSTEM.md` é direto sobre onde o lado livre perde: não é DSP, é **coisa
feita pra dedo**. Script é código; isto é arrastar um ponto de divisão no
teclado e ver o grau acender. A diferença de propósito é a razão de ser dois
plugins, não um com mais uma tabela.

## Forma

Interno (`PluginFormat::Internal`), zero áudio: `audio_inputs = 0`,
`audio_outputs = 0`, `kind = MidiEffect`. UID `nirbija.chord`, ao lado de
`nirbija.stepseq` e `nirbija.arp`.

Mais perto do Arpeggiator que do Step Sequencer: tem entrada para acompanhar.
`queue_midi` roda na thread de áudio, igual ao Arpeggiator
(`arpeggiator.h:16`) — sem atômico, sem lock, um array de tamanho fixo. O
Step Sequencer não tem esse problema porque não ouve nada; este ouve o
teclado inteiro.

## O gesto: teclado partido em duas zonas

Um ponto de divisão (`split_point`, nota MIDI) corta o teclado em duas:

| Zona | O que faz |
|---|---|
| Abaixo do split — **gatilho** | cada tecla é um grau da escala corrente. Tocar dispara o acorde diatônico daquele grau, com voicing próprio, numa oitava de performance separada |
| Acima do split — **passagem** | toca como uma melodia normal; se `passthrough_mode` pede, cada nota é arredondada para a nota da escala mais próxima antes de sair |

É o gesto do GarageBand Autoplay e do Scaler: uma mão faz harmonia com uma
tecla de cada vez, a outra toca melodia sem conseguir errar a tonalidade.

## Escala → acorde por grau

Escala é um conjunto fixo de intervalos a partir da fundamental (`root`,
0–11). Maior: `{0,2,4,5,7,9,11}`. O acorde de cada grau é a mesma conta que
qualquer harmonia tonal faz: empilhar terças **dentro da escala**, a partir
da nota daquele grau — grau 1 pega os graus 1-3-5 da escala, grau 2 pega
2-4-6, e assim por diante. Não é tabela escrita à mão por escala; é uma
função de `(escala, grau) → três ou quatro notas`, e cada modo/escala nova
no enum ganha os próprios acordes de graça.

Escalas do primeiro corte, mesmo vocabulário que `scripting.md` já usa para
`quantize_to_scale`: maior, menor natural, menor harmônica, menor melódica,
os sete modos gregos, pentatônica maior, pentatônica menor, blues. Uma
pentatônica não tem sete graus regulares — o gatilho usa quantos graus a
escala tiver e as teclas acima do maior grau ficam mudas, não erram nota.

## O que soa quando uma tecla do gatilho é tocada

`queue_midi` recebe a nota. Se está abaixo do split:

1. `(nota − root) mod 12` acha o grau mais próximo na escala corrente —
   igual ao `quantize_to_scale` do script, só que a saída é um acorde, não
   uma nota.
2. Monta as notas do acorde daquele grau (passo anterior), transpõe para a
   `oitava de performance` (parâmetro, independente da oitava em que a tecla
   foi tocada — apertar essa tecla numa oitava grave ou aguda dispara o
   mesmo acorde no registro configurado, só a inversão sonora muda por
   voicing) e aplica o voicing corrente (ver abaixo).
3. Emite um note-on por voz, guardado num array fixo indexado pela nota
   gatilho — o mesmo padrão do `held_` do Arpeggiator
   (`arpeggiator.h:99`-`107`): quando a tecla solta, o array diz quais notas
   soltar, sem precisar lembrar teoria de novo no note-off.

Nota fora da escala abaixo do split (ex.: tecla preta numa escala que não a
tem) não dispara nada — silêncio, não o grau mais próximo por engano. É
melhor uma tecla muda do que um acorde errado.

Acima do split, sem gatilho: passa a nota, quantizada ou não conforme
`passthrough_mode`.

## Voicing

Três coisas independentes, cada uma um parâmetro:

- **Inversão** — fundamental, primeira, segunda (a nota mais grave do
  acorde muda, as outras se reacomodam pra cima).
- **Vozes** — tríade (3 notas) ou tétrade com sétima diatônica (4 notas).
  Sétima também vem da escala, não de tabela por acorde: é o próximo grau
  empilhado depois do quinto.
- **Spread** — fechado (as vozes cabem numa oitava) ou aberto (a do meio
  sobe uma oitava, jeito clássico de parar de soar "apertado" no grave).

## Modelo de dados

```
globais   root, escala, split_point, oitava de performance,
          inversão, vozes (3/4), spread, passthrough_mode, canal MIDI
por voz   nenhum — vozes são calculadas, não armazenadas
```

Nada por passo, nada que cresça: ao contrário do sequenciador, este plugin
não guarda um padrão, guarda uma **regra**. `held_` e o array de notas
soantes por gatilho são a única estrutura de tamanho fixo em jogo, do mesmo
tamanho de decisão que `arpeggiator.h:112` já tomou (`kMaxHeld * vozes`).

## Parâmetros antes de UI

Mesma aposta do `design/sequencer.md`: os ~8 parâmetros globais bastam para o
`ParamEditor.qml` genérico desenhar sliders e o plugin já tocar no primeiro
dia, com MIDI learn de graça. A UI própria — teclado desenhado com o split
arrastável e o grau ativo acendendo — vem depois de o gesto estar provado.

## Estado

`save_state()` / `load_state()`: uma linha por parâmetro global, texto
simples, mesma convenção dos outros internos. `parse_number` cobre blob
truncado ou editado à mão (`plugin.h:105`).

## Ordem de construção

1. **Root fixo em C, escala maior, tríades fechadas, sem inversão, sem
   passthrough.** Prova o mapeamento grau→acorde e o pareamento
   note-on/note-off por tecla gatilho. Testável offline como
   `tests/arpeggiator_test.cpp` já é: nota sintética entra, confere as que
   saem e em que frame.
2. Demais escalas e modos.
3. Inversão, sétima, spread.
4. Passagem quantizada acima do split.
5. UI própria em QML.
6. Extrair pra CLAP se ficar bom — mesma lógica de sempre.

## Por que não CLAP direto

Ver `ECOSYSTEM.md`, seção da rota de entrega, e a mesma decisão em
`design/sequencer.md`. Como interno herda UI, sessão e MIDI que já existem e
custa zero empacotamento; a extração é só trocar a casca depois, porque a
regra escala→acorde não muda com o formato.
