# Achados da matriz de testes

Registro dos problemas que `matrix.py` encontrou, para acompanhar a correção de
cada um. Edite este arquivo à mão: status, data e notas. Depois de cada
correção, rode a matriz de novo e anote o resultado na seção do achado.

Status possíveis: `aberto` · `em andamento` · `corrigido` · `não é bug`
(decisão de projeto: ajustar `Model.expect`) · `quebrou` (a correção causou
regressão: descreva).

## Histórico de rodadas

| data | commit | resposta | passaram | relatório | notas |
|---|---|---|---|---|---|
| 2026-10-25 | f5fad50 + kit não commitado | deny | 40/84 | `/tmp/xisguard-matrix/20261025-primeira-rodada/report.md` | primeira rodada; 7 de 9216 células variaram entre execuções (achado 6) |
| 2026-10-25 | f5fad50 + kit não commitado | deny | 40/84 + 18/25 cenários | `/tmp/xisguard-matrix/20261025-cenarios/report.md` | suíte de cenários adicionada (achados 7–10); matriz idêntica à rodada anterior |

Para registrar uma nova rodada: `make -C xisguard matrix MATRIX_ARGS="-j 12"` e
uma linha acima. O relatório fica em `--out`, copie-o se quiser guardar.

## Resumo

| # | achado | onde | status |
|---|---|---|---|
| 1 | Perder as regras de sistema deixa o servidor em modo legado até reiniciar o X | servidor (Xnotify) | aberto |
| 2 | Desligar o modo permissivo apaga as regras de usuário no servidor | xisguard | em andamento |
| 3 | Modo seguro e `REMOVE_RULE` não retiram acesso; desligar o seguro não reativa regras | xisguard | aberto |
| 4 | Regras de sistema adicionadas em runtime só valem após reiniciar o X, se a sessão abriu sem nenhuma | servidor | aberto |
| 5 | Janela de ~5 s negando tudo ao remover as regras de sistema | servidor | aberto (talvez aceitável) |
| 6 | O primeiro pedido depois que o guard reinicia se perde | servidor ou xisguard | aberto |
| 7 | `--deny` (inclusive `--deny ALL`) não vence as regras de usuário | xisguard | corrigido |
| 8 | `--allow` nega a primeira tentativa, e só concede para aqueles argumentos exatos | xisguard (+ servidor) | corrigido |
| 9 | Regras EXACT (`exe\|args`) do `perms.conf` se perdem ao reiniciar o guard | xisguard | corrigido |
| 10 | "Deny" permanente no modo seguro continua perguntando toda vez | xisguard | corrigido |

---

## 1. Perder as regras de sistema deixa o servidor em modo legado até reiniciar o X

**Sintoma:** com o guard rodando, se `xnotify.conf*` some (ou fica vazio), o
servidor passa a permitir tudo. Quando os arquivos voltam e o guard manda
`RELOAD`, nada muda: o servidor continua permitindo tudo.

**Causa (lida no código, ainda não confirmada com correção):**
`XnotifyLoadConfig()` desliga `security_mode` quando não carrega nenhuma regra.
`is_trusted_sender()` retorna `FALSE` sempre que `security_mode` está desligado,
então o servidor passa a ignorar todos os comandos do guard, inclusive o
`RELOAD` que o faria sair do modo legado. Ver `Xext/xnotify/xnotify.c`.

**Gravidade:** a maior. É uma perda total de proteção, sem aviso, até reiniciar
o X.

**Reproduzir:**
```sh
python3 xisguard/tests/matrix.py -k '^sys-rules_usr-0_guard-1_perm-0_sec-0_unk-ask$'
```
Rodadas `sys=none` e as seguintes: todas `ALLOW`. Passos `sys=base (back)` e
`sys=rules (back)` deveriam voltar a negar.

**Direções possíveis (decidir):**
- manter `security_mode` ligado enquanto o guard estiver vivo, mesmo sem regras
  de sistema;
- aceitar comandos do guard em modo legado se vierem de um executável já
  autenticado no início da sessão;
- documentar como limitação e fazer o xisguard avisar quando o servidor entrar
  em modo legado (ele já recebe `STATUS`).

**Status:** aberto

**Notas / histórico:**
-

---

## 2. Desligar o modo permissivo apaga as regras de usuário no servidor

**Sintoma:** com `usr=1` (regras em `perms.conf`), ligar e depois desligar o
permissivo (`SET_PERMISSIVE`) faz o programa com regra de usuário passar a ser
negado. Nos testes: o primeiro uso depois do desligamento é negado (e perguntado,
ou morto, conforme o modo).

**Causa (lida no código):** desligar manda `{"command":"RELOAD"}` ao servidor,
que apaga a tabela de regras inteira, incluindo as regras de usuário enviadas
pelo guard. O `SET_PERMISSIVE` não chama `send_all_permissions_to_xserver()`
depois. Compare com `request_server_reload()` (`xisguard.c`, ~1388), que só
reaplica o permissivo, também sem reenviar as regras.

**Reproduzir:**
```sh
python3 xisguard/tests/matrix.py -k 'usr-1_guard-1_perm-0_sec-0'
```
Passos `permissive=0` e `permissive=0 (back)`, coluna `user:*`.

**Correção provável:** depois do `RELOAD` em `SET_PERMISSIVE` (desligar), e
também em `request_server_reload()`, chamar `send_all_permissions_to_xserver()`.

**Status:** em andamento

**Notas / histórico:**
- 2026-10-26: corrigido no xisguard.c (matriz 40→48/84 ao corrigir); falta testar na sessão real.

---

## 3. Modo seguro e `REMOVE_RULE` não retiram acesso; desligar o seguro não reativa regras

**Sintoma (três formas do mesmo problema):**
- `SET_STATUS secure_mode=1` esvazia as regras de usuário no guard, mas o
  servidor continua com as que já recebeu: o programa de usuário continua
  `ALLOW`.
- `REMOVE_RULE` de uma regra de usuário não tira o acesso (a limitação já está
  documentada em XISGUARD.md, mas vale também para programas que ainda vão
  abrir, não só para clientes já conectados).
- `SET_STATUS secure_mode=0` recarrega `perms.conf` no guard, mas não reenvia as
  regras ao servidor.

**Causa:** nenhum desses caminhos faz `RELOAD` + `send_all_permissions_to_xserver()`.
O `SET_STATUS` só chama `load_user_config()`.

**Decisão de projeto pendente:** o esperado é que o acesso siga a regra
atual? A matriz assume que sim (`Model.expect`). Se a decisão for outra
(mantém até a sessão acabar), mude o `Model` e marque `não é bug`.

**Reproduzir:** passos `secure=1`, `secure=0 (back)`, `usr=0`, `usr=0 (back)`,
coluna `user:*`.

**Status:** aberto

**Notas / histórico:**
-

---

## 4. Regras de sistema adicionadas em runtime só valem após reiniciar o X, se a sessão abriu sem nenhuma

**Sintoma:** sessão que abre com `sys=none` (modo legado). Mesmo escrevendo as
regras e mandando `RELOAD` pelo guard, o servidor continua permitindo tudo.

**Causa:** mesma do achado 1 (guard não autenticado em modo legado).

**Nota:** provavelmente se resolve junto com o 1. Se a decisão do 1 for "é
limitação", o README deve dizer que, sem nenhum arquivo de regras de sistema no
início, o xisguard não protege nada.

**Status:** aberto

**Notas / histórico:**
-

---

## 5. Janela de ~5 s negando tudo ao remover as regras de sistema

**Sintoma:** ao ir de `sys=rules/base` para `sys=none` com o guard rodando, o
servidor primeiro nega tudo (e pergunta) por ~5 s, e só depois entra em modo
legado.

**Causa:** o servidor recarrega as regras, não acha nenhuma, e desliga o
`security_mode`; mas considera o guard vivo até o heartbeat expirar (5 s) e só
então reconfigura. O guard recusa/ignora o estado intermediário.

**Impacto:** baixo isoladamente; relevante junto com o achado 1.

**Nota sobre o kit:** `matrix.py` espera 6,5 s depois de mudar regras de
sistema para não depender dessa janela. Se o achado 1 for corrigido, vale
reduzir ou remover essa espera e testar a janela de propósito.

**Status:** aberto (talvez aceitável)

**Notas / histórico:**
-

---

## 6. O primeiro pedido depois que o guard reinicia se perde

**Sintoma:** com `guard=0` e depois `guard=1` (guard reiniciado), o primeiro
programa sem regra a pedir permissão é negado sem diálogo e sem ser
perguntado. É intermitente: na repetição de uma mesma configuração, o programa
afetado muda (`sys:SCREEN` numa rodada, `sys:SELECTION` na outra, e
`KILLED`/`DENY` trocam). Foi a única fonte de variação entre execuções
(7 de 9216 células).

**Causa (hipótese, não confirmada):** o `notify_sock` do servidor continua
conectado ao arquivo de socket antigo do guard (que foi apagado no
desligamento). O primeiro `send()` falha, o servidor fecha o socket e descarta a
mensagem; a seguinte, depois de reabrir, funciona.

**Como confirmar:** olhar `xserver.log` e `guard.log` da configuração
`sys-base_usr-0_guard-1_perm-0_sec-0_unk-ask` em torno de `guard=1 (back)`.

**Reproduzir de forma determinística:** cenário `guard_restart_first_request`
(`python3 xisguard/tests/matrix.py --suite scenarios -k restart_first`). Os
outros cenários contornam este bug com um probe descartável depois de cada
restart do guard (`absorb_first_request` em `scenarios.py`). Quando ele for
corrigido, dá para remover esse contorno.

**Correção provável:** no servidor, em `Xnotify()`/`XnotifyReport()`, quando o
`send()` falha com `ECONNREFUSED`/`ENOENT`, reabrir o socket e tentar de novo
uma vez antes de descartar. Hoje só fecha e retorna.

**Status:** aberto

**Notas / histórico:**
-

---

## 7. `--deny` (inclusive `--deny ALL`) não vence as regras de usuário

**Sintoma:** `xisguard --deny SCREEN` com `ALLOW SCREEN <prog>` no
`perms.conf`: o programa continua conseguindo SCREEN. O README diz que os
overrides da linha de comando "take priority over perms.conf rules". O mesmo
acontece com `--deny ALL`.

**Causa (confirmada no `guard.log`):** o override só é consultado em
`handle_message()`, ou seja, quando o servidor pergunta. Mas na inicialização
`send_all_permissions_to_xserver()` já mandou as regras de usuário como
`ALLOW` ao servidor, que concede direto, sem perguntar ao guard.

**Correção provável:** em `send_all_permissions_to_xserver()`, não enviar
`ALLOW` para ações em `cli_deny_mask`; ou mandar `DENY_ACTION` para elas depois
da sincronização. Decidir também se `--deny` deve valer contra regras de
sistema: a suíte assume que não (`cli_deny`, primeira verificação).

**Reproduzir:** cenários `cli_deny` e `cli_deny_all`.

**Status:** corrigido

**Notas / histórico:**
- 2026-10-26: corrigido no xisguard.c (cenários cli_deny/cli_deny_all passam); falta testar na sessão real.

---

## 8. `--allow` nega a primeira tentativa, e só concede para aqueles argumentos exatos

**Sintoma:** com `xisguard --allow SCREEN`, um programa sem regra tem a
primeira captura negada. O README diz que o guard "respond immediately".
Pelo `guard.log`, o guard concede para `exe|args`, ou seja, só para aquela
linha de comando exata. O mesmo programa aberto com outros argumentos é negado
de novo na primeira tentativa.

**Causa:** o servidor nega antes de perguntar (`XnotifyIsAllowed` devolve
`FALSE` e notifica o guard). A resposta `ALLOW` do override chega depois e só
vale para tentativas seguintes. E `handle_message()` monta a chave com os
argumentos (`exe|args`), que é o que o override envia.

**Correção provável:** na inicialização (e depois de cada `RELOAD`/sincronização),
mandar `ALLOW_ACTION` para cada ação em `cli_allow_mask`, como o modo
permissivo faz com todas (`permissive_reapply()`). Assim o servidor nem
pergunta.

**Reproduzir:** cenário `cli_allow`.

**Status:** corrigido

**Notas / histórico:**
- 2026-10-26: corrigido no xisguard.c (cenário cli_allow passa); falta testar na sessão real.

---

## 9. Regras EXACT (`exe|args`) do `perms.conf` se perdem ao reiniciar o guard

**Sintoma:** responder "Allow EXACT" ou "Trust EXACT" no modo normal funciona
na sessão. Depois que o guard reinicia (ou no próximo login), o programa volta
a ser perguntado.

**Causa (confirmada no `guard.log`):** a regra é gravada inteira
(`ALLOW SCREEN /prog|--hold-ms 1200`), mas `load_user_config()` separa os
campos com `strtok` por espaço. O padrão vira `/prog|--hold-ms`, e é isso que
vai ao servidor (`Sent: ALLOW SCREEN for /prog|--hold-ms`). Quase toda linha
de comando real tem espaços nos argumentos.

No modo seguro funciona: o parser do servidor lida com os espaços.

**Correção provável:** em `load_user_config()`, o padrão é o resto da linha
depois da ação (não o próximo token). Ver como `XnotifyLoadFile()` do servidor
faz e usar a mesma regra. Conferir também `REMOVE_RULE`/`LIST_RULES`, que
comparam o padrão.

**Reproduzir:** cenários `answer-allow_exact-normal` e
`answer-trust_exact-normal`, verificação "after a guard restart".

**Status:** corrigido

**Notas / histórico:**
- 2026-10-26: corrigido no xisguard.c (cenários *_exact-normal passam); falta testar na sessão real.

---

## 10. "Deny" permanente no modo seguro continua perguntando toda vez

**Sintoma:** no modo seguro, responder "Deny" grava `DENY SCREEN <prog>` nas
regras de sistema via pkexec, corretamente. Mas toda vez que o programa tenta
de novo, o diálogo aparece outra vez. No modo normal a regra é respeitada sem
perguntar.

**Causa:** o servidor nega pela regra, mas regra de `DENY` não impede que ele
notifique o guard (`XnotifyIsAllowed` notifica sempre que não concede). No modo
normal o guard acha o `DENY` no `perms.conf` e responde sem diálogo. No modo
seguro o guard não carrega regra nenhuma em memória, então trata como programa
desconhecido.

**Correção provável (escolher):**
- servidor: não notificar o guard quando existe uma regra `DENY` explícita para
  o exe/ação (só notificar quando não há regra);
- guard: no modo seguro, carregar também as regras de sistema (o diretório vem
  de `GET_CONFIG_PATH`) só para consulta.

**Reproduzir:** cenário `answer-deny-secure`.

**Status:** corrigido

**Notas / histórico:**
- 2026-10-26: corrigido no xisguard.c (cenário answer-deny-secure passa); falta testar na sessão real.

---

## O que os cenários confirmaram que funciona

Serve de base para pegar regressões: se algum destes passar a falhar, é algo
novo.

- `no_pause`: pausa (SIGSTOP) só quando desligado; vale no início, em
  runtime e de volta, e é persistido.
- `ignore.conf` nunca concede permissão; `LIST/ADD/REMOVE_IGNORED` e a flag
  `ignored` do `LIST_ACTIVE` funcionam.
- `notify_actions` filtra `LIST_ACTIVE` e não desliga o diálogo; persistido.
- `--allow X --deny X`: deny vence. Overrides não são salvos e somem ao
  reiniciar sem eles.
- `SET_PERMISSIVE` com e sem `persist`: `GET_STATUS`, `xnotify.conf` e o
  estado depois de reiniciar estão corretos.
- Respostas Allow, Trust e Deny (normal), Allow, Trust e EXACT (seguro), e as
  de sessão: gravam no lugar certo, valem para o mesmo programa, Trust cobre
  outras ações, as de sessão não sobrevivem ao restart.
- Polkit cancelado no modo seguro: vale só para a sessão, nada é gravado.

## Não cobertos (ainda)

Ver "O que não está coberto" no README. Os achados acima são só do que a
matriz e os cenários exercitam hoje.
