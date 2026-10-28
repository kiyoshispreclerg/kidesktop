# Testes do xisguard

Testa o xisguard junto com o Xnotify do servidor, em duas suítes:

- **matriz** (`matrix.py`): todas as combinações de estado inicial da sessão.
  Em cada combinação, muda cada variável em tempo de execução, testa, volta ao
  valor original e testa de novo.
- **cenários** (`scenarios.py`): sequências roteirizadas para o que não cabe
  num produto cartesiano. São as opções `no_pause`, `ignore.conf`,
  `notify_actions`, `--allow`/`--deny` e `SET_PERMISSIVE` com `persist`, e as
  respostas permanentes do diálogo, nos modos normal e seguro.

```sh
make -C xisguard matrix                       # as duas: 84 configurações + 25 cenários, ~12 min com -j 13
python3 xisguard/tests/matrix.py --suite scenarios -j 13   # só os cenários, ~2 min
python3 xisguard/tests/matrix.py --suite matrix --quick    # 24 configurações, o essencial
python3 xisguard/tests/matrix.py -k 'sys-rules_usr-1|cli_' -j 8
python3 xisguard/tests/matrix.py --list       # ids das configurações e cenários
```

No fim ele imprime o caminho do `report.md`. O código de saída é 0 só se
tudo passou. Os problemas encontrados ficam registrados em `FINDINGS.md`.

## Requisitos

- `bwrap` (bubblewrap) e user namespaces sem privilégio (padrão no Ubuntu 22.04)
- o build do fork em `~/Downloads/kiyoshi-forks/xserver/build-test-debug`:
  `hw/vfb/Xvfb` (padrão, sem janela) ou `hw/kdrive/ephyr/Xephyr` (`--xephyr`).
  Os dois têm Xnotify. Não passe `-extension XNOTIFY` para eles, porque essa
  opção *desliga* a extensão.
- `cc` e `libx11-dev` para o probe, `python3`

O xisguard e o probe são compilados automaticamente no início.

## O que varia

| variável | valores | como muda em runtime |
|---|---|---|
| `sys`: regras de sistema (`SYSCONFDIR/xnotify.conf*`) | `none`: sem arquivo nenhum (modo legado) · `base`: só `ALLOW XNOTIFY <xisguard>` · `rules`: base + regras para `xprobe-sys` | reescreve os arquivos e manda `RELOAD` pelo socket de controle, se o guard estiver rodando |
| `usr`: regras de usuário (`perms.conf`) | `0` sem arquivo · `1` com regras para `xprobe-user` | `ADD_RULE`/`REMOVE_RULE` pelo socket de controle, como o kiconf faz. Com o guard parado ou em modo seguro, edita o arquivo diretamente |
| `guard`: xisguard rodando | `0` · `1` | `SIGTERM` / inicia de novo |
| `perm`: modo permissivo no início | `0` · `1` · `fresh` (sem `xnotify.conf`, instalação nova, que começa permissivo) | `SET_PERMISSIVE` |
| `sec`: modo seguro | `0` · `1` | `SET_STATUS secure_mode` |
| `unk`: programa sem regra | `ask` (diálogo) · `quiet` · `kill` (`always_kill`) | `SET_STATUS quiet/always_kill` |
| `disabled`: Xnotify desligado | só em runtime | `DISABLE` / `ENABLE` |

As configurações com `guard=0` não variam `perm`/`sec`/`unk`, porque sem o
guard essas opções não existem. Nesse caso o `xnotify.conf` é escrito com tudo
desligado, e é com ele que o guard sobe quando é iniciado em runtime.

Cada configuração passa por estas rodadas:

1. `baseline`: como a sessão abriu
2. para cada variável e cada outro valor dela: `var=novo`, depois
   `var=original (back)`

As regras de sistema são variadas por último, e `sys=none` por último de
todos, porque perdê-las pode deixar a sessão inteira em modo legado e esconder
o efeito das outras variáveis.

Uma rodada lança três programas, cada um tentando cada ação (`SCREEN` e
`SELECTION` por padrão, veja `--actions`):

- `xprobe-sys`: tem regra de sistema quando `sys=rules`
- `xprobe-user`: tem regra de usuário quando `usr=1`
- `xprobe-none-N`: nunca tem regra. É um caminho novo a cada rodada, para não
  cair no throttle de 3 s por programa+ação do servidor.

Os três são cópias do mesmo `xprobe.c`. As regras casam pelo caminho do
executável, então para o servidor e para o guard eles são programas diferentes.
`SCREEN` lê com `GetImage` um pixel de uma janela de cor conhecida. Quando é
negado, o servidor devolve a imagem zerada. `SELECTION` faz `SetSelectionOwner`
e confere com `GetSelectionOwner`.

Resultado de cada probe: `ALLOW`, `DENY` ou `KILLED`, mais `+prompt` se o
diálogo foi mostrado para ele.

## As duas verificações

**Oráculo**: o resultado bate com o que a especificação diz (`Model.expect`
em `matrix.py`), nesta ordem:

1. servidor sem regras de sistema carregadas: tudo `ALLOW` (modo legado; o
   guard nem é aceito pelo servidor)
2. guard rodando com Xnotify desligado ou em modo permissivo: tudo `ALLOW`
3. `xprobe-sys` com regras de sistema carregadas: `ALLOW`
4. `xprobe-user` com guard rodando, regras de usuário e sem modo seguro: `ALLOW`
5. sem guard: `DENY`, sem diálogo
6. sem regra, com guard: `ask` → `DENY+prompt` (o diálogo falso responde
   "negar nesta sessão"), `quiet` → `DENY`, `kill` → `KILLED`

Quando o servidor relê as regras de sistema: ao iniciar, quando o guard inicia
(ele manda `RELOAD`), em todo `RELOAD` (inclusive ao desligar o modo
permissivo) e quando o guard morre (depois de 5 s sem heartbeat). Com o guard
parado, mudar os arquivos não tem efeito até uma dessas coisas acontecer, e o
oráculo considera isso correto.

**Volta**: depois de `(back)`, os resultados e o `GET_STATUS` do guard
(`permissive`, `secure_mode`, `quiet`, `always_kill`, `xnotify_enabled`,
`no_pause`) têm que ser idênticos aos do `baseline`. Com `--answer allow`, só os vereditos
precisam voltar: um programa que já recebeu grant de sessão não é perguntado de
novo. Essa verificação não usa o
oráculo: ela responde se voltar à opção anterior deixa tudo como estava, mesmo
onde a especificação é discutível.

**Arquivos**: no fim, com respostas "negar nesta sessão", nada pode ter sido
gravado. O `perms.conf` tem que estar igual ao do início, e nenhum
`50-xisguard.conf` pode aparecer nas regras de sistema.

### Onde o oráculo é uma escolha

Alguns pontos são decisão de projeto, não fato. Se a decisão for outra, mude
`Model`:

- remover regra de usuário (`REMOVE_RULE`) ou ligar o modo seguro deve tirar o
  acesso dos programas que ainda vão abrir. A limitação documentada (o servidor
  não retira o que já concedeu) vale para clientes já conectados, não para
  processos novos.
- ao desligar o modo permissivo, as regras de usuário continuam valendo.
- `sys=none` em runtime com o guard rodando: o servidor passa ~5 s negando
  tudo (e perguntando), enquanto ainda considera vivo o guard que ele passou a
  recusar. Depois de mudar regras de sistema, o kit espera 6,5 s antes de
  testar, então essa janela não aparece nos resultados: ela tornaria a matriz
  não determinística.
- adicionar regras de sistema em runtime quando a sessão abriu sem nenhuma
  (`sys=none`): o oráculo espera que o `RELOAD` do guard as carregue. O servidor
  recusa o guard em modo legado, então isso só acontece reiniciando o X.

## Como funciona por dentro

Cada configuração roda em seu próprio `bwrap`:

- `/tmp` privado. O diretório da configuração é montado em `/tmp/w`, então os
  caminhos são curtos e iguais em todas (as regras citam `/tmp/w/bin/...`).
- o `SYSCONFDIR` do servidor (`/opt/xlibre-test/etc/X11`, veja `--sysconfdir`)
  é substituído por `/tmp/w/sysconf`, sem root
- PID namespace próprio: quando o worker termina, tudo o que ele iniciou morre
  junto. Nada é morto por nome, e a sessão real não é tocada.
- rede própria no modo Xvfb, então todas as configurações usam `:1` sem
  conflito. Com `--xephyr`, cada slot usa `:70+N` e abre uma janela na sua tela.
- `HOME=/tmp/w/home`, `XDG_RUNTIME_DIR=/tmp/w/run`, `LANG=C`
- `fakebin/` vem antes no `PATH`. O `xisserve` falso responde o diálogo com o
  valor em `/tmp/w/answer` (padrão `99`, "negar nesta sessão", ou `1` com
  `--answer allow`) e registra cada chamada em `prompts.log`. O aviso "Xnotify
  DISABLED" ele deixa aberto, como um usuário que não clica. O `pkexec` falso
  registra a chamada em `pkexec.log` e executa o comando, como se a senha
  tivesse sido aceita.

O xisguard é uma cópia do binário do repo em `/tmp/w/bin/xisguard`, que é o
caminho que a regra `ALLOW XNOTIFY` das regras de sistema autoriza.

## Cenários

Cada cenário roda no mesmo sandbox de uma configuração da matriz e grava uma
lista de verificações: o que foi olhado, o que se esperava e o que saiu. No
`report.md`, a seção "Scenarios" lista primeiro as verificações que falharam.

| cenário | o que verifica |
|---|---|
| `no_pause-start0`, `no_pause-start1` | o programa fica pausado (SIGSTOP) enquanto o diálogo está aberto só sem `no_pause`. Testa com o valor no início, mudado em runtime e de volta |
| `ignore_conf` | `ignore.conf` só marca os REPORTs (`LIST_ACTIVE` `ignored`) e nunca concede nada; `LIST/ADD/REMOVE_IGNORED` |
| `notify_actions` | o filtro do `LIST_ACTIVE` segue `notify_actions`; `NONE` não desliga o diálogo; volta ao padrão |
| `cli_deny`, `cli_deny_all`, `cli_allow`, `cli_allow_and_deny` | `--deny`/`--allow` vencem as regras de usuário (não as de sistema), valem já na primeira tentativa, deny vence allow, nada é salvo, e somem ao reiniciar sem eles |
| `permissive_persist` | `SET_PERMISSIVE` sem e com `persist`: `GET_STATUS`, `xnotify.conf` e o estado depois de reiniciar o guard |
| `answer-<resposta>-normal/secure` | cada resposta do diálogo (`allow`, `deny`, `trust`, `allow_exact`, `trust_exact`, `allow_session`, `trust_session`): o que é gravado (`perms.conf` no normal, `50-xisguard.conf` via `pkexec` no seguro); o mesmo programa de novo, com outra ação e com outros argumentos; depois de reiniciar o guard; com o guard parado |
| `answer-allow-secure-pkexec_cancelled` | polkit cancelado: vale só para a sessão, nada é gravado |
| `guard_restart_first_request` | o primeiro pedido depois de reiniciar o guard (FINDINGS #6) |

Detalhes que os cenários precisam:

- **Pausa:** o `xisserve` falso anota no `prompts.log` o estado em `/proc` do
  programa que pediu (`state=T` = pausado).
- **Argumentos estáveis:** o probe recebe a ação e o retry por variável de
  ambiente (`XPROBE_ACTION`, `XPROBE_RETRY_MS`), então os argumentos são
  sempre `--hold-ms N`. É isso que permite testar regras EXACT (`exe|args`)
  com outra ação e com outros argumentos.
- **Restart do guard:** depois de cada restart, um programa descartável
  "absorve" o primeiro pedido, que se perde (FINDINGS #6). Sem isso, esse
  bug mascararia as verificações seguintes. O cenário
  `guard_restart_first_request` testa o #6 sem esse desvio.
- **Cancelamento do polkit:** com o arquivo `/tmp/w/pkexec_fail`, o `pkexec`
  falso termina com 126, como um prompt cancelado.

Para acrescentar um cenário, use o decorador `@scenario(...)` em
`scenarios.py`, com as ferramentas de `ScenarioWorker`: `probe`,
`expect_probe`, `check`, `new_exe`, `answer`, `restart_guard`, `ctl`, `active`.

## Sessão real (`realsession.py`)

A matriz usa xisserve/pkexec falsos e Xvfb. `realsession.py` roda as peças
instaladas: o Xephyr de `/opt/xlibre-test` (veja `--xserver`), o
`/usr/local/bin/xisguard` (`--guard`) e o popup de verdade do
`xisserve --question`, respondido por AT-SPI (o botão é escolhido pela
posição, então qualquer idioma serve). A janela do Xephyr abre na sua tela.

```sh
python3 xisguard/tests/realsession.py                       # ~4 min, servidor instalado
python3 xisguard/tests/realsession.py --xserver ~/Downloads/kiyoshi-forks/xserver/build-test-debug/hw/kdrive/ephyr/Xephyr
python3 xisguard/tests/realsession.py -k no_rules
```

Ele verifica o comportamento desejado das regras de sistema:

| cenário | o que verifica |
|---|---|
| `rules` | com regras de sistema: sem guard só vale o sistema; com guard valem sistema + usuário e o resto pergunta; continua assim depois de ligar/desligar o permissivo duas vezes e de reiniciar o guard; o servidor nunca recusa o guard |
| `rules_removed_at_runtime` | apagar as regras + `RELOAD` deixa tudo permissivo (só REPORTs); restaurá-las + `RELOAD` volta às regras |
| `no_rules` | sem regras de sistema: tudo permitido, nada pergunta, regras de usuário não importam, o guard recebe REPORTs, com permissivo ligado ou não; adicionar regras + `RELOAD` passa a aplicá-las |
| `guard_rule_only` | só `ALLOW XNOTIFY <guard>` já basta para as regras de usuário valerem |

O `perms.conf` de teste tem `ALLOW ALL <xisguard>` e `ALLOW ALL <xisserve>`,
como na sessão de quem já confiou nos dois (o primeiro é o achado 11). As
regras de sistema são trocadas com `bwrap`, sem root; `HOME`, runtime dir e
D-Bus (`dbus-run-session`) são próprios. Resultado em
`/tmp/xisguard-real/<data>/report.md`, com `xserver.log` e `guard.log`.

Requer, além do da matriz: `xisserve` no `PATH`, `dbus-run-session` e
`python3-pyatspi`.

## Achados

`FINDINGS.md` registra os problemas que a matriz e os cenários encontraram,
com status, causa provável e como reproduzir cada um. Atualize-o a cada
correção e rodada.

## Resultados

Ficam em `--out` (padrão `$TMPDIR/xisguard-matrix/<data>/`):

- `report.md`: falhas agrupadas (um bug que aparece em 40 configurações vira
  uma linha), depois a tabela de rodadas de cada configuração com
  `**obtido** (want esperado)` nas divergências
- `results.json`: tudo, incluindo o estado do modelo em cada rodada
- `<config>/`: `guard.log`, `xserver.log`, `prompts.log`, `pkexec.log`,
  `worker.log`, `result.json` e os diretórios `home/` e `sysconf/` como
  ficaram no fim

## Opções

| opção | |
|---|---|
| `--suite all\|matrix\|scenarios` | qual suíte rodar (padrão: as duas) |
| `-j N` | configurações em paralelo (padrão: metade dos CPUs, até 4). Quase tudo é espera, então `-j 8` funciona bem. |
| `-k REGEX` | filtra pelo id da configuração |
| `--quick` | sem `quiet`/`kill`, sem `fresh` e sem modo seguro com permissivo |
| `--answer deny\|allow` | resposta do diálogo, sempre só para a sessão. Com `allow`, os probes tentam de novo por 2 s, e o oráculo passa a esperar `ALLOW+prompt` e, para programas fixos, o grant valer até o próximo `RELOAD` |
| `--actions SCREEN,SELECTION` | ações testadas |
| `--xephyr` | assistir em janelas em vez de Xvfb |
| `--xserver`, `--sysconfdir` | outro build do servidor |

## O que não está coberto

- a interface do kiconf: o kit fala direto com o mesmo socket de controle
- o xisserve, o polkit e o Xorg de verdade
- `SUBSCRIBE` e os eventos que ele empurra (`REPORT`, `STATE`, `PERMISSIVE`)
- `always_kill`/`quiet` combinados com respostas permanentes, e `REVOKE_ACCESS`

Para acrescentar uma variável à matriz: um nível em `Config`/`all_configs`, o
efeito em `Model.expect`, e como mudá-la em `Worker.set_var` e
`Worker.toggles`.
