# Cenas e fila — desenho

Uma música feita no Nirbija hoje é uma sessão que você conduz com a mão:
liga um strip, sobe um fader, troca o padrão do sequencer, e cada gesto é
seu. Isto dá a essa condução uma memória. Uma **cena** guarda o que muda de
uma parte da música para a outra; a **fila** põe as cenas em ordem e anda
sozinha, compasso a compasso, e você pula, segura ou volta quando quiser.

Não é uma timeline de DAW. Não há curvas desenhadas e nada acontece dentro
de uma parte: a cena diz onde chegar, o fade diz em quantos compassos, e o
resto é tocar.

O mockup navegável que fechou o visual está em
<https://claude.ai/artifact/PzVRL91NcmJSfL1SrsxPcC>.

## O que foi decidido, e por quem

As respostas abaixo são do Thiago, uma por pergunta. Elas são o contrato;
o resto do documento é como cumpri-lo.

| Pergunta | Resposta |
|---|---|
| Quem decide quando a parte muda? | A fila anda sozinha; eu pulo ou seguro a qualquer hora |
| O que uma cena muda? | Liga/desliga strip, volume, padrão do sequencer, parâmetros de plugin — inclusive de terceiros |
| Como a cena sabe o que guardar? | Só o que eu mexer enquanto ela grava |
| Parâmetros mudam como? | Só na passagem, de A para B durante o fade. Dentro da parte ficam parados |
| Quando a cena entra? | No 1 do próximo compasso |
| Quais strips ela toca? | Só os marcados "segue as cenas" |
| Tamanho do fade? | Um por cena, em compassos |
| Mão e cena no mesmo knob? | A mão ganha, até a próxima troca |
| Fim da fila? | A última cena fica tocando |
| Disparo? | Clique e teclado do PC. MIDI learn fica para depois |
| Onde fica? | Uma faixa fixa no topo, acima dos strips |
| Prioridade | Tem que ser lindo |

## Vocabulário

- **Cena**: nome, cor, duração em compassos (ou ∞), fade em compassos, e um
  conjunto de **alvos**. Um alvo é `(strip, o quê, valor)`.
- **O quê** de um alvo é um destes: `gate` (o liga/desliga da cena), `level`
  (o fader), `pattern` (o padrão de um Step Sequencer no strip) ou
  `param` (um parâmetro de um insert, por id).
- **Fila**: a ordem das cenas na faixa. Não há fila separada da lista; a
  ordem da faixa é a ordem da música.
- **Segue as cenas**: um booleano por strip. Um strip que não segue é
  invisível para toda cena, mesmo que uma cena antiga tenha alvos nele.
- **Mão**: um alvo que o jogador tomou durante a cena corrente. A cena não
  o toca de novo até a próxima troca.

## A faixa

Acima dos strips, abaixo da `TopBar` (`Main.qml:403`). `masterStrip` e
`mixerArea` hoje ancoram em `topBar.bottom` (`Main.qml:428`, `:442`) e
passam a ancorar na faixa. A faixa corre a largura toda, por cima do master
também: ela é da música, não de um strip.

Cada cena é um bloco com largura proporcional aos compassos — Intro 8,
Groove 16 — com mínimo legível; uma cena ∞ vale 8 na conta. A faixa lê como
a forma da música antes de qualquer som. No bloco:

- nome, `⌥n`, "16 comp · fade 2";
- uma cunha no começo, do tamanho do fade relativo à duração;
- na cena que toca: preenchimento que avança com o compasso, uma cabeça de
  leitura com brilho, os quatro tempos acendendo;
- na cena armada: borda tracejada e "entra no 1" piscando;
- as cenas já tocadas ficam esmaecidas.

À esquerda, `CENAS` e três botões: **Fila** (a fila anda sozinha),
**Segurar** (a cena corrente se repete), **Gravar cena**. À direita, uma
linha de estado: `Groove · compasso 5/16 · Quebra em 12 comp`.

Cores, fontes e alturas vêm do `Skin` (`skin.h:44-80`); o matiz de cada
cena sai da mesma família de `Qt.hsla` que os editores já usam para seus
parâmetros. Nada de cor nova fora do `Skin`.

Nos strips, cada controle que a cena corrente guarda ganha um ponto na cor
do strip; um controle na mão ganha o ponto amarelo `solo` e o strip mostra o
selo **mão**. Um strip que não segue as cenas tem a faixa de cor do topo
tracejada. É assim que "só o que eu mexer" fica visível sem abrir nada.

## Tempo

Tudo o que muda, muda numa linha de compasso. Um compasso é `numerator`
semínimas, como no resto do motor (`TransportInfo`, `plugin.h:58-77`).

O único lugar do motor que já sabe "há uma linha de compasso no frame X
deste bloco" é `schedule_metronome` (`engine.cpp:1122-1140`), com
`dsp::boundary_frame` (`dsp.h:99-125`), chamado logo depois de
`graph_->set_transport` (`engine.cpp:951`, `:959`). O **regente** das cenas
mora ao lado dele e usa a mesma conta.

Com o transporte parado não há compasso: clicar numa cena aplica na hora,
com a rampa curta que cada controle já tem. É o que se espera quando se está
montando a música, antes de tocar.

## O regente, na thread de áudio

`SceneConductor` é do `Engine`, roda uma vez por bloco antes do render, e
não aloca, não trava, não escreve log.

**O que ele recebe.** A thread da UI monta uma `SceneTable` — os alvos
já resolvidos para ponteiros e ids (strip do grafo, `PluginInstance*`, id de
parâmetro, se é degrau) em arrays de tamanho fixo — e a entrega pelo
`commands_` do motor (`engine.h:282`) como um ponteiro. A tabela velha volta
pela mesma via e é liberada na thread da UI. Armar uma cena é um comando:
`Arm(scene_index)`. Tirar a mão e soltar a mão também:
`Hand(strip, what, id)`.

**Na linha de compasso.** Se há cena armada, ou a cena corrente acabou e a
fila está andando, o regente troca de cena **no frame da linha**, limpa as
mãos, e para cada alvo de um strip que segue as cenas:

| O quê | Como |
|---|---|
| `pattern` | Não é rampa: é o `kNextPattern` do sequencer. Ver abaixo |
| `param` de degrau | Pula na linha (tipo de filtro, liga/desliga de um efeito) |
| `level` | Rampa linear na posição do fader, de onde está até o alvo, em `fade × compasso` |
| `gate` | Rampa do ganho de cena do strip, em curva de seno, mesma duração |
| `param` contínuo | Rampa linear no valor do parâmetro, mesma duração |

Fade zero vira a rampa curta que cada coisa já tem; nada nunca pula de
verdade.

**Por que não reaproveitar o mute.** O mute é do jogador e é uma chave de
10 ms (`kMuteSeconds`, `channel_strip.cpp:14-19`). O liga/desliga da cena
é outra coisa: um **ganho de cena** por strip, separado do fader e do mute,
multiplicado na mesma volta por amostra (`channel_strip.cpp:320-336`) e
andado por um `dsp::LinearRamp` (`dsp.h:22-47`, hoje sem uso no strip) com
duração em amostras. O botão "on" do strip na UI é esse ganho; o `M`
continua sendo o mute, e os dois se somam sem se pisar.

**Fader.** O regente escreve o alvo de `gain_` por bloco
(`channel_strip.h:90-97`); o suavizador de 15 ms que já existe transforma os
degraus de bloco em linha. Um bloco de 256 quadros a 48 kHz é 5,3 ms, abaixo
do suavizador, então a rampa sai lisa sem mudar o strip.

**Padrão.** A troca de padrão já é exata na amostra: a UI guarda
`next_pattern_` e o sequencer o aplica no próximo compasso
(`step_sequencer.cpp:702-732`, `:854-856`, `:1457-1460`). O regente escreve
`kNextPattern` no bloco em que recebe `Arm`, e os dois caem na mesma linha.
O caso de borda é o bloco que contém a própria linha; o regente roda antes do
render, então o sequencer vê o pedido no `begin_block` desse mesmo bloco.
Um teste fixa isso (ver Testes).

## Parâmetros de plugin

Aqui está o trabalho de verdade. O contrato hoje é que `set_parameter` roda
na thread da UI (`plugin.h:158-160`), e cada formato faz uma coisa:

| Formato | Hoje | Da thread de áudio? |
|---|---|---|
| Internos | store atômico relaxado (`drone.cpp:683-691`) | sim |
| LV2 | store atômico na porta de controle (`lv2_backend.cpp:798-807`) | sim |
| CLAP | `param_queue_`, fila SPSC cujo produtor é a UI (`clap_backend.cpp:452-465`, `:1050`) | não |
| VST3 | `setParamNormalized` no controller + `param_edits_`, SPSC da UI (`vst3_backend.cpp:985-993`, `:1338`) | não |

Então:

1. **`set_parameter_rt(id, value)`** entra em `PluginInstance`, chamável só
   do regente. Nos internos e no LV2 é o mesmo store. No CLAP e no VST3 é
   uma **segunda fila**, cujo produtor é a thread de áudio, drenada no mesmo
   ponto do `process` que já transforma a fila da UI em eventos
   (`clap_backend.cpp:364-369`; `vst3_backend.cpp:335-340`, `:850`). Uma
   fila por produtor mantém `RtQueue` SPSC (`rt_queue.h:11`).
2. **Um valor por bloco, no offset 0**, como já é hoje. Os plugins suavizam
   seus próprios parâmetros, e um passo de 5 ms numa rampa de segundos não
   é ouvido. Frame dentro do bloco fica para quando um teste mostrar que
   precisa.
3. **O editor do plugin precisa ver a rampa.** No CLAP o plugin atualiza a
   própria interface a partir dos eventos de `process`. No VST3 o controller
   só sabe o que `setParamNormalized` contou a ele, e isso é thread da UI: o
   regente publica o último valor de cada alvo andado num array atômico, e o
   timer da UI empurra para o controller o que mudou. Sem isso, o knob do
   Vital ficaria parado enquanto o som anda.
4. **Degrau.** `ParameterInfo` (`plugin.h:106-112`) ganha `stepped`. Os três
   backends já têm a informação e a jogam fora: `CLAP_PARAM_IS_STEPPED`
   (`clap_backend.cpp:381-398`), `stepCount > 0` ou `kIsList` no VST3
   (`vst3_backend.cpp:964-977`), `lv2:integer`, `lv2:toggled` ou
   `lv2:enumeration` no LV2 (`lv2_backend.cpp:183-193`). Um parâmetro de
   degrau nunca é rampado.

## Gravar por toque

"Gravar cena" liga um modo na UI; enquanto ele está ligado, todo gesto num
controle de um strip que segue as cenas escreve (ou reescreve) o alvo na
cena corrente, e o ponto do controle acende. O gesto também vale na hora:
gravar é tocar.

O que conta como gesto:

- **Fase 1:** fader, botão ON/OFF e troca de padrão do sequencer.
- **Fase 2:** o `ParamEditor`/`ParamBar` genérico (`ParamEditor.qml:148-151`
  → `Mixer.setInsertParameter`) e os editores dos internos.
- **Fase 3, o editor do próprio plugin:** hoje nenhum backend conta ao host
  qual parâmetro foi mexido — só um `take_state_dirty()` por plugin
  (`plugin.h:262-271`).
  - VST3: `performEdit` já vê o id (`vst3_backend.cpp:1276`), na thread da
    UI; basta repassar. `beginEdit`/`endEdit` (`:1272`, `:1281`) deixam de
    ser vazios para marcar o gesto.
  - LV2: `write_port` (`lv2_backend.cpp:510`) já vê o índice da porta, na
    thread da UI.
  - CLAP: `PARAM_VALUE` e `PARAM_GESTURE_BEGIN/END` chegam em
    `out_event_push` (`clap_backend.cpp:702-737`) na thread de áudio e hoje
    são descartados. Uma fila SPSC de "tocados" leva o id para a UI.

  Com isso, `PluginInstance` ganha `take_touched(uint32_t* ids, size_t n)`,
  polled no mesmo timer que já pega o estado sujo
  (`mixer_model.cpp:1986`).

Fora do modo gravar, o mesmo gesto vira **mão**: a UI manda `Hand` ao
regente antes de aplicar o valor, o regente para aquela rampa, e o controle
é do jogador até a próxima troca.

## Identidade

Uma cena aponta para coisas que precisam continuar sendo as mesmas depois de
salvar, reabrir, reordenar strips e mover inserts. Hoje nenhuma delas tem id
estável: strips são índice, nome e `graphSlot`, que só vale na execução
(`mixer_session.cpp:176`); ligações entre strips vão por nome (`:186-204`);
inserts são posição + formato + uid do plugin (`:229-247`).

Por isso strips e inserts ganham um campo `uid`, 64 bits em hex, criado
quando falta (sessões antigas ganham o seu na primeira abertura). Um alvo é
`{strip: uid, insert: uid | null, what, id?, value}`. Um alvo cujo strip ou
insert não existe mais é mantido no arquivo e ignorado ao tocar, e o bloco
da cena mostra "2 alvos sem destino", do mesmo jeito que um plugin que falta
é nomeado na tela e o resto carrega.

## Sessão

Uma seção nova no topo, escrita em `buildSession` junto de
`mixer_session.cpp:368-370` e lida com padrões em `readSession` (`:787`):

```json
"scenes": {
  "auto": true,
  "items": [
    { "name": "Groove", "hue": 0.83, "bars": 16, "fade": 1,
      "targets": [
        { "strip": "9f2c…", "what": "gate",    "value": 1 },
        { "strip": "9f2c…", "what": "level",   "value": 0.72 },
        { "strip": "9f2c…", "insert": "41ab…", "what": "pattern", "value": 0 },
        { "strip": "3e10…", "insert": "77d0…", "what": "param", "id": 1204, "value": 0.45 }
      ] }
  ]
}
```

`bars: 0` é ∞. Cada canal ganha `"followScenes": true`. `kSessionVersion`
(`:28`) **não** sobe: a checagem é de igualdade estrita e rejeitaria as
sessões de hoje, e chaves desconhecidas já são ignoradas na leitura. Uma
sessão com cenas aberta numa versão antiga só perde as cenas.

Qual cena toca, o compasso e a mão não são salvos. Abrir uma sessão abre na
primeira cena, parada.

## Desfazer

Editar cenas — gravar, renomear, reordenar, mudar duração ou fade, apagar —
é estrutural e chama `pushUndo()` (`mixer_model.cpp:2040`). Como `scenes`
passa a estar em `buildSession`, desfazer volta a lista de cenas junto com o
resto.

Tocar uma cena não entra no desfazer, assim como mover um fader não entra
hoje (`mixer_model.cpp:623-648`). É performance, não edição.

## Teclado

`Alt+1` … `Alt+9` armam as cenas 1 a 9; `Alt+H` segura; `Alt+R` grava.
Nenhum `Alt` está em uso (`Main.qml:65-108`), e `Espaço` continua tocando e
parando.

O Computer Keyboard escuta o app inteiro por um filtro em `qApp`
(`mixer_model.cpp:106`, `:163-179`) e ignora modificadores, então `Alt+H`
tocaria o H do teclado musical. O filtro passa a deixar de fora qualquer
tecla com `Alt` ou `Ctrl`. Os dígitos não estão no mapa do teclado
(`KeyboardEditor.qml:110-143`), mas a regra vale para todos, porque é a
certa.

Tudo entra no `ShortcutSheet` (F1).

## Fases

1. **Faixa, fila e strips.** A faixa, `gate`, `level` e `pattern`, o
   regente no motor, gravar por toque nos controles do Nirbija, mão, sessão,
   `uid`s, teclado. Já faz uma música inteira com liga/desliga, volume e
   padrões, que é o pedido mínimo.
2. **Parâmetros de internos e LV2.** `set_parameter_rt` nos dois, `stepped`
   no LV2, gravar pelo `ParamEditor` genérico e pelos editores internos.
3. **CLAP e VST3.** A segunda fila, `stepped` nos dois, o eco para o
   controller do VST3, `take_touched` nos três backends para gravar pelo
   editor do próprio plugin.

Fica fora: curvas dentro de uma parte, fade diferente por strip, MIDI learn
das cenas, ramificações na fila ("depois de mim vai para X").

## A fase 1, como ficou

O que a implementação decidiu onde este documento deixava em aberto, ou
mudou no caminho:

- **A interface fala inglês**, como o resto do app: `SCENES`, `Queue`,
  `Hold`, `● Record scene`, e `ON`/`OFF` no strip.
- **Um strip segue as cenas por padrão.** Uma cena só mexe no que guarda,
  então seguir não custa nada até alguém gravar algo nele; "Ignore scenes"
  no menu do título é a exceção, como o Drone.
- **O insert não ganhou uid.** Em memória a cena guarda o tag de corrente
  do sequencer (`ChannelStrip::insert_by_tag`, lido sob o contador de
  sequência da corrente, como o render lê); na sessão, a posição dele entre
  os plugins vivos, que é como o strip já os lista. Um alvo cujo plugin
  sumiu fica na cena, contado como "gone" no bloco, e não toca nada.
- **CC conta como mão.** Um knob do controlador é um gesto de alguém: grava
  quando a gravação está ligada, e toma o fader da cena quando não está.
- **Escolher uma cena parado é dizer onde começar.** O play começa por ela,
  contando o compasso 1 a partir dali; o Rewind depois de tocar volta para
  a primeira.
- **O padrão é enfileirado um compasso antes, mas não logo depois de uma
  linha.** O sequencer conta o compasso a partir do começo do último bloco,
  e no bloco seguinte a uma linha ainda acha que ela está à frente: um
  `kNextPattern` escrito ali trocava na hora.
- **Ainda não desenhado:** o ponto de "a cena guarda" no slot do
  sequencer. O bloco da cena conta o padrão entre os controles que ela tem.

## Testes

Todos `quick`, offline, no mesmo estilo dos que já medem o maior degrau entre
amostras:

- **Sem clique.** Uma cena que desliga um strip com fade de 1 compasso, e
  outra com fade 0: o maior degrau entre amostras fica abaixo do limite que
  os testes de fader e mute já usam.
- **Na linha.** A cena armada começa no frame exato da linha de compasso,
  inclusive quando a linha cai no meio do bloco e quando cai no primeiro
  frame.
- **Padrão e cena juntos.** Armar com um `pattern` no último bloco antes da
  linha e no bloco que contém a linha: o padrão novo e o começo da rampa
  caem no mesmo frame.
- **Mão.** Um `Hand` no meio de uma rampa para a rampa naquele valor; a
  troca seguinte devolve o controle à cena.
- **Fila.** Oito compassos de Intro com a fila andando chegam na cena 2 no
  compasso 9; com Segurar, voltam ao compasso 1 da Intro; na última cena,
  ficam.
- **Sessão.** Ida e volta de uma sessão com cenas; uma sessão sem `scenes`
  abre com lista vazia; um alvo sem destino sobrevive à ida e volta.
- **Degrau.** Um parâmetro `stepped` com fade de 4 compassos muda uma vez,
  na linha.
- **Custo.** `render_bench` com 16 strips e uma cena de 64 alvos no meio de
  um fade continua dentro da margem de hoje.
