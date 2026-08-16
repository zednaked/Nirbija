# Plugin MIDI scriptável — desenho

O outro Tier 1 do `ECOSYSTEM.md`. Modelo: Mozaic (Bram Bos) e StreamByter
(Audeonic), que deixam qualquer pessoa escrever um plugin MIDI dentro do host,
sem compilar nada. Equivalente livre: nenhum.

É alavanca de segunda ordem. Em vez de escrever oito sequenciadores, escreve-se
um plugin e a comunidade escreve os oito.

## A parte difícil, dita de cara

**MIDI é processado na thread realtime.** A regra dura do projeto está no
`PLAN.md` e não abre exceção: não aloca, não trava mutex, não faz I/O.

Um interpretador de script normal viola as três. Lua aloca e roda coletor de
lixo; Python nem se discute. Rodar um `lua_State` dentro de `process()` é o
caminho óbvio, e é o errado — a primeira pausa de GC vira xrun audível.

Isso torna este projeto **materialmente mais difícil que o sequenciador**, e a
diferença é toda aqui. As três saídas:

### A. Interpretador na thread realtime, com memória fixa

VM de bytecode própria: arena pré-alocada, sem heap, sem GC, orçamento de
instruções por bloco. Script que estoura o orçamento é cortado e o plugin
silencia com aviso, em vez de estourar o deadline de áudio.

- **A favor**: latência zero, semântica simples, o script vê o frame exato.
- **Contra**: é escrever uma linguagem. Semanas, não dias.

### B. Script numa thread de trabalho, filas nos dois sentidos

O `process()` só empurra evento pra uma fila SPSC e lê da outra. O script roda
noutra thread, sem restrição de alocação.

- **A favor**: qualquer linguagem serve, inclusive Lua inteiro. Dias, não
  semanas. O projeto já tem a peça: `core/rt_queue.h`.
- **Contra**: **latência de um bloco no mínimo**, e jitter. Pra filtro e mapa
  não se nota. Pra arpejador e sequenciador, que precisam colocar nota num frame
  específico, é audível e desqualifica metade dos casos de uso.

### C. Híbrido — o recomendado

O script **não roda** no caminho do áudio. Ele roda quando o transporte não
está tocando, ou fora do bloco, e o que ele produz é uma **tabela** que a thread
realtime lê sem interpretar nada: mapa de nota pra nota, curva de velocidade,
grade de passos, lista de eventos agendados por batida.

Isto é: o script é um **compilador de configuração**, não um processador de
evento. Ele responde a "quando o usuário mudar isto, recalcule aquela tabela".

- **A favor**: RT continua trivial e determinística — ler tabela é indexar
  array. Qualquer linguagem serve. Cobre a maioria do que Mozaic faz na prática:
  escalas, acordes, curvas de velocidade, padrões, arpejos gerados de antemão.
- **Contra**: não cobre script que decide **reagindo** a cada nota que chega
  ("se a última nota foi grave, transponha a próxima"). Esse caso fica de fora,
  e é honesto dizer que fica.

**Recomendação: C, e dizer no README o que ele não faz.** A alternativa é a A,
que é um projeto de linguagem antes de ser um projeto de música. Se a demanda
por reação em tempo real aparecer, A pode substituir C depois sem quebrar os
scripts que só geram tabela.

## Linguagem

Lua, embutida. Motivos: interpretador pequeno, embutir é trivial, muita gente já
sabe, e licença MIT convive com GPLv3. No desenho C ela nunca chega perto da
thread de áudio, então o GC dela deixa de ser problema.

Alternativa se Lua pesar: uma linguagem de expressão minúscula, do tamanho de
uma calculadora com `if` e laço. Menos poder, muito menos superfície.

## O que o script vê

Deliberadamente pequeno. Ele produz tabela; não pilota o host.

```lua
-- chamado quando um parâmetro muda ou o estado é carregado.
-- devolve as tabelas que a thread realtime vai ler.
function build(params)
  local map = {}                    -- 128 entradas: nota -> nota, ou nil
  for n = 0, 127 do
    map[n] = quantize_to_scale(n, params.root, params.scale)
  end
  return { note_map = map }
end
```

Tipos de tabela que valem no primeiro corte:

| Tabela | Tamanho | Faz |
|---|---|---|
| `note_map` | 128 | transposição, escala, dobra |
| `velocity_curve` | 128 | dinâmica, compressão de toque |
| `channel_map` | 16 | roteamento |
| `pattern` | N passos | mesma estrutura do `design/sequencer.md` |

A thread realtime consome isso com uma indexação. Nada mais.

## Troca de tabela sem travar

O script roda na thread da UI e produz um bloco novo. A entrega segue o padrão
que o projeto já usa em todo lugar (`PLAN.md`, "Regra dura"): monta pronto fora,
publica por ponteiro atômico, e o descarte volta pela fila de lixo. A thread
realtime nunca espera e nunca libera memória.

## Editora

Campo de texto em QML, botão de aplicar, painel de erro. Sem realce de sintaxe
no começo. O `PluginGui` (`plugin.h:97`) é pra editora nativa embutida por X11 —
isto aqui é interno, então desenha em QML direto, como o resto da UI.

O script vai no `save_state()`, junto com a sessão. Sessão que carrega, carrega
o script.

## Segurança, sem drama e sem ingenuidade

Um script vem junto com um arquivo de sessão, e sessão se troca entre pessoas.
Então:

- Lua **sem** `io`, `os`, `package`, `require`, `load`, `dofile`. Sandbox por
  lista de permissão, não por lista de proibição.
- Limite de instruções por `build()`, via debug hook, pra laço infinito não
  travar a UI.
- Sem acesso a rede nem a disco. O script transforma número em número.

Isto não é defesa contra atacante determinado — é a diferença entre "abri a
sessão de alguém" e "rodei o programa de alguém". Vale dizer no README que
sessão de terceiro carrega script, do mesmo jeito que macro de planilha.

## Ordem de construção

1. `note_map` só, com Lua embutido e sandbox. Prova a cadeia inteira —
   editar, rodar, publicar tabela, ouvir mudar — no menor escopo possível.
2. `velocity_curve` e `channel_map`.
3. Limite de instruções e relatório de erro decente.
4. `pattern`, compartilhando estrutura com `design/sequencer.md`.
5. Scripts de exemplo, que é o que faz alguém entender pra que serve.

O passo 1 é testável offline: constrói o plugin, carrega um script conhecido,
empurra notas, confere as que saíram. Sem servidor de áudio.
