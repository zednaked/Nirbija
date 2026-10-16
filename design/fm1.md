# FM-1 — desenho

O FM-1 com a Jangada é um instrumento de verdade: quatro tracks, sequencer,
efeitos, tudo dentro de uma caixa ligada por USB. Hoje, para o Nirbija, ele é
só um par de portas: áudio que chega e MIDI que sai. O som que a música usa
mora no aparelho, e reabrir a sessão uma semana depois não traz ele de volta.

Isto faz do FM-1 um instrumento do mixer como os de dentro. Ele entra numa
strip pelo picker, abre um editor com os parâmetros das quatro tracks, e a
sessão guarda o projeto dele inteiro. Abrir a sessão devolve o FM-1 ao
estado daquela música, e as cenas trocam os sons dele na virada do compasso.

O mockup navegável que fechou o visual está em
<https://claude.ai/artifact/FTpf75Me74mFdKebBEaubP>.

## O que foi decidido, e por quem

As respostas abaixo são do Thiago, uma por pergunta. Elas são o contrato;
o resto do documento é como cumpri-lo.

| Pergunta | Resposta |
|---|---|
| O que a sessão guarda do FM-1? | O projeto inteiro: sons, padrões e mix das 4 tracks |
| Como ele aparece no mixer? | Uma strip, com as 4 tracks dentro do editor |
| As cenas mexem no FM-1? | Sim, os sons por cena |
| A strip mostra os parâmetros? | Sim, como o editor de um plugin |
| Visual | O do mockup, aprovado ("tá ótimo") |

## Vocabulário

- **FM-1**: o aparelho com a Jangada (USB `1209:0001`; as portas aparecem
  com o nome que o firmware dá a elas, hoje "Felucca"). Firmware sem o
  protocolo v8 é tratado como desconectado, com o motivo escrito na strip.
- **Projeto do FM-1**: o objeto 0 do backup da Jangada (`JNG2`, ~3–4 KB):
  as quatro tracks com sons, padrões, mix, e o patch FM6 de cada uma. É o que
  vai para a sessão.
- **Som** de uma track: o frame do `TRACK_SOUND` (protocolo v8, cmd 96):
  engine, os valores pelas chaves estáveis e, numa track FM6, o patch. É o
  que uma cena troca.
- **Link**: a thread que conversa com o FM-1 (um pedido por vez, como o
  protocolo exige) e o par de filas que a liga à thread de áudio.

## A peça: `Fm1Instance`

Um built-in como o Drone: `src/core/fm1.{h,cpp}`, uid `nirbija.fm1`, na lista
do `InternalBackend` (`src/core/plugin.cpp:47`), tipo Instrument. O editor é
`src/ui/qml/Fm1Editor.qml`, carregado como os outros (`Main.qml:793`).

- **Áudio**: `process()` passa a entrada da strip para a saída. A entrada da
  strip é a captura USB do FM-1. Ao entrar no picker, a strip procura a porta
  de captura do FM-1 e se liga nela sozinha; se não achar, o slot de entrada
  fica no "no input" de sempre e a strip diz por quê.
- **Notas**: o que chega pelo `queue_midi()` sai para o FM-1 no mesmo frame,
  pela porta MIDI do FM-1 (abaixo). Canal n vai para a track n do aparelho,
  como ele já entende.
- **Relógio**: uma chave na strip liga o `clock_out` do engine
  (`engine.cpp:230`) na entrada do FM-1. O FM-1 segue com GLO > CLK USB.

## As portas MIDI do FM-1

Plugin nenhum tem porta JACK hoje; quem registra é o engine. O FM-1 precisa
de uma porta de saída e uma de entrada que carreguem SysEx, e o `drain` de
hoje joga SysEx fora (`engine.cpp:26`).

Uma interface opcional nova no `PluginInstance`:

```cpp
// Um aparelho do outro lado de um par de portas MIDI do engine. Bytes crus,
// SysEx inteiro; as filas são da thread de áudio de um lado e do Link do outro.
struct ExternalMidi {
  std::string match;            // parte do nome da porta do aparelho
  RtByteRing<8192>* to_device;  // Link -> áudio -> porta de saída
  RtByteRing<8192>* from_device;// porta de entrada -> áudio -> Link
};
virtual ExternalMidi* external_midi() { return nullptr; }
```

`RtByteRing` é nova: uma fila de bytes sem lock, ao lado do `RtQueue`
(`rt_queue.h`), que guarda frames inteiros com o tamanho na frente.

O engine, ao ligar uma instância que devolve isso, registra `fm1_out` e
`fm1_in` para a strip, liga nas portas do aparelho que casam com `match`, e a
cada bloco:

- escreve as notas do `take_midi_output()` da instância e depois um frame
  inteiro da fila `to_device` (nunca meio SysEx: se não couber no bloco,
  espera o próximo);
- copia tudo o que chega em `fm1_in` para `from_device`, SysEx inclusive.

A thread de áudio só move bytes. Montar e entender frame é do Link.

## O Link

Uma `std::thread` por instância, no molde do `BleMidi` (`ble_midi.h`):

- **Aperto de mão**: `INFO`; protocolo < 8 = "Jangada antiga: atualize para
  usar o FM-1 aqui". Depois `WATCH 3` e `PING` a cada segundo, que é o que
  mantém os pushes vivos (o aparelho desliga o WATCH 3 s sem pedido).
- **Um pedido por vez**, com timeout de 200 ms e uma repetição. Os pushes
  (`CHANGED`, `RELOAD`, `STEP_CHANGED`, `TRACK_CHANGED`) chegam a qualquer
  hora e nunca são resposta.
- **Desconectado** é um estado normal: sem porta, sem resposta ao `INFO` ou
  ao `PING`. A strip mostra, o editor fica cinza, e o Link tenta de novo a
  cada 2 s.

## Os parâmetros

`parameters()` lista, para cada uma das tracks 1–3: os 8 do EDIT do engine
dela, ATK DEC SUS REL, DST CHO DLY REV, LFO RATE e WAVE, e um parâmetro
**SOM** (abaixo). A track 4 é a bateria: por enquanto só os sends; o
protocolo não expõe os níveis do kit, e o editor diz isso em vez de inventar.

- **Id**: `track << 8 | chave`, com a chave estável da Jangada (`keys.h`). Uma
  sessão salva hoje mira o mesmo parâmetro depois de um firmware novo. O Link
  aprende chave → índice do próprio `TRACK_SOUND` (os pares vêm em ordem de
  índice) e manda `TRACK_PARAM` com o índice.
- **Nome, faixa, passo**: do `ENG_DESC` (cmd 98) do engine. Os enums (WAVE, FTYP, SRC…) são
  `stepped`, e a cena pula eles na virada.
- **Troca de engine** muda o que os 8 do EDIT são. A lista de parâmetros é
  refeita, e o host é avisado como quando um CLAP reescaneia os seus.
- **Mão no aparelho**: o `CHANGED` da track selecionada no FM-1 vira
  `take_touched()`. A mão ganha, como em qualquer plugin. Mexer num knob do FM-1
  com Gravar cena aceso grava na cena.

## A sessão

`save_state()` devolve o projeto do FM-1, e não monta na hora: o Link mantém
uma cópia. Ela é pega com `BK_LIST` e `BK_GET` do objeto 0 (16 pedidos de 256
bytes, menos de meio segundo):

- quando a sessão é salva à mão;
- quando o FM-1 mudou (um push qualquer) e passou 30 s sem mudar de novo.

Não com mais frequência: enquanto um backup está aberto, o FM-1 segura o
autosave dele e recusa gravar banco FM6 por 15 s.

`take_state_dirty()` diz sim depois de cada cópia nova, e a sessão se escreve
como se escreve para qualquer plugin.

`load_state()` guarda o projeto e o Link devolve com `BK_PUT` (begin, os
pedaços, commit): o FM-1 carrega na hora. O commit pede o song do FM-1
parado; tocando, a strip mostra "Pare o FM-1 para receber o projeto" e o Link
devolve assim que ele parar. Desconectado, devolve quando ele chegar. Nunca
pela metade: o commit confere o CRC, e um corte deixa o projeto antigo.

O formato do blob é o `JNG2` como o FM-1 dá, com um cabeçalho do Nirbija
(versão, tamanho, CRC). Uma sessão de um FM-1 com firmware mais novo que o
aparelho de agora pode ser recusada pelo commit (rc 2); a strip diz isso.

## As cenas

Nada novo no conductor (`scene_conductor.h`). O FM-1 entra nas cenas pelos
dois caminhos que todo plugin já tem:

- **Parâmetros** viram alvos `param` como os de um CLAP: andam no fade,
  pulam se `stepped`. Uma mudança chega no `set_parameter_rt()`, uma vez por
  bloco; o Link junta os valores e manda o último de cada parâmetro que
  mudou, todos num `TRACK_PARAMS`, assim que a resposta anterior chega.
- **SOM** é um parâmetro `stepped` por track, cujo valor é um índice numa
  tabela de sons guardada no estado da instância. Com Gravar cena aceso,
  trocar o engine ou o preset de uma track no FM-1 (ou o botão "Gravar som"
  no editor) pega o `TRACK_SOUND` da track, guarda na tabela e toca o SOM com
  o índice novo. Na virada, a cena põe o SOM; a instância vê o índice mudar e
  o Link manda o `TRACK_SOUND` guardado. O som inteiro troca no 1 do
  compasso, como um padrão de sequencer.

A tabela de sons vai no blob da sessão junto do projeto.

## Limites, e o que a Jangada precisa

- **Vazão**: um pedido por vez, resposta em ~5–10 ms. Com um valor por
  pedido, um fade de 10 parâmetros atualizaria cada um a cada ~50–100 ms.
  O Link usa o **`TRACK_PARAMS` (cmd 97)**: todos os valores que mudaram
  desde o último pedido, de todas as tracks, num frame só (até 127). Um fade
  inteiro anda a cada ~5–10 ms.
- **Descrever um engine**: o `DESC` só descreve a track selecionada no
  FM-1. O Link usa o **`ENG_DESC` (cmd 98)**, que descreve qualquer engine
  sem mudar a tela do aparelho.
- **Bateria**: os níveis do kit não estão no protocolo. Fica para quando
  estiverem.
- **Troca de som com clique**: o `TRACK_SOUND` faz panic na track (como um
  preset). A cena troca no 1 do compasso, onde costuma haver nota nova;
  medir no aparelho antes de prometer "sem clique".

## Ordem

1. ~~Jangada: `TRACK_PARAMS` e `ENG_DESC`~~ feitos no protocolo v8 da
   Jangada 1.0.1 (cmds 97 e 98), com teste de host.
2. `ExternalMidi` no engine e um FM-1 de mentira nos testes (o protocolo
   num objeto C++, respondendo pelas filas), com os testes do Link:
   aperto de mão, timeout, desconexão, pushes no meio de uma resposta.
3. `Fm1Instance` com áudio, notas e relógio, sem editor: já é útil.
4. Sessão: pegar, guardar, devolver, song tocando, desconectado.
5. Parâmetros e `Fm1Editor.qml`, o visual do mockup.
6. SOM nas cenas.
7. No FM-1 de verdade: latência das notas, cliques na troca de som, o
   projeto devolvido depois de desligar e ligar.
