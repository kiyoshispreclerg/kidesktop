# Protocolo de captura do kicomp

Como um gravador de tela (primeiro o kishot; qualquer outro pode vir depois, o
fork do OBS por exemplo) pede ao compositor uma imagem de uma saída **com
algumas janelas excluídas dela**, e como os dois combinam se isso é possível,
quando é necessário, qual buffer ler e quando é seguro lê-lo.

*Tradução de [CAPTURE-PROTOCOL.md](CAPTURE-PROTOCOL.md), que é o texto de
referência: se os dois divergirem, vale o inglês. Os nomes de propriedades,
mensagens, chaves de configuração e campos ficam como no original.*

**Estado: especificação, protocolo versão 1, na branch `capture-censor`.**
Implementado (kicomp 0.3.70, kiwm 0.6.3): a marca e todos os jeitos de pô-la (o
menu de janela e o `capture_exclude_classes` do kiwm, herança para diálogos, o
menu do tasklist do xispanel, o "Ocultar uma janela..." do kishot); o lado do
compositor por inteiro — descoberta, sessões, `TARGETS`, `STATE`, `HIDE_PID`, a
regra dos popups, a imagem nos renderers **XRender** e **GLX** (slots,
`REQUEST`/`FRAME`/`RELEASE`, `UNCHANGED`, preencher e ocultar, dano por slot para
redesenhar só o que mudou) e janelas em tela cheia mantidas sem redirecionamento;
o lado do kishot — descoberta, sessão, seqlock, slots em screenshots, preview e
gravações, `HIDE_PID` no lugar de esconder a própria janela, nenhuma lista
própria; e os interruptores (a aba do compositor no kiconf). Testado por
`kicomp/tests/capture-control.sh`, `capture-render.sh`, `capture-unredirect.sh`,
`kiwm/tests/capture-exclude.sh` e `kishot/tests/capture-session.sh`, e medido na
sessão real (GLX, dois monitores). Não implementado: a imagem no renderer
**EGL** (as saídas ficam `UNAVAILABLE` enquanto uma janela excluída aparece, e o
kishot as censura sozinho), `DMABUF`, slots de saídas com escala no kishot (ele
as censura sozinho), e um teste dos efeitos que desenham uma janela várias vezes
(expo, cubo, cover-switch, lâmpada mágica, wobbly): a substituição é no nó, então
estão cobertos por construção, não exercitados. Foi escrito antes para que os
dois lados fossem construídos contra o mesmo texto.

`slot`, em `FRAME` e `RELEASE`, é o **índice** (a partir de 0) na lista de XIDs
de slots daquela saída em `TARGETS` (palavras 9–12), não um XID.

## Por que existe

Hoje o kishot censura pintando retângulos pretos sobre *a geometria X da
janela*, na sua própria cópia da janela raiz. Isso erra sempre que o compositor
desenha a janela em outro lugar: o cubo, o expo, o cover switch, a lâmpada
mágica, janelas wobbly, uma animação de minimizar. Nada que o gravador consiga
ler do X diz para onde aqueles pixels foram, e um retângulo não acompanha uma
malha deformada. Só o compositor sabe, porque é ele quem desenha.

Então a exclusão pertence ao compositor, como no kwin_wayland: **o usuário
marca uma janela** (no menu da barra de título, por exemplo) **e o compositor
produz uma segunda imagem de cada saída em que aquela janela não está** (ou é
uma cor chapada). O gravador não decide o que fica escondido e não precisa de
uma lista de janelas; só pede a imagem e a lê no lugar de copiar a saída.

Fora do escopo: proteger uma janela de outros programas de captura, do
`GetImage` do próprio servidor X, ou da própria tela. Só a imagem que este
protocolo entrega é censurada. Veja [Segurança](#segurança).

## Termos

| termo | significado |
|---|---|
| **gravador** | o cliente que tira capturas ou grava (kishot) |
| **compositor** | o kicomp, dono de `_NET_WM_CM_S<n>` |
| **janela CM** | a janela dona de `_NET_WM_CM_S<n>`; onde o compositor publica o que sabe fazer |
| **saída** | um monitor RandR, como no "um pixmap por saída" do kicomp; identificada pelo nome RandR (`DP-1`) |
| **janela excluída** (ou censurada) | uma janela que leva a marca `_KIWM_CAPTURE_EXCLUDE`, ou um popup de uma; num WM com reparentamento, o frame, decoração incluída (uma barra de título diz qual documento está aberto) |
| **sessão** | o pedido contínuo de um gravador por imagens censuradas de algumas saídas |
| **janela de sessão** | uma janela que o gravador cria para a sessão; leva o pedido dele e recebe as respostas do compositor |
| **slot** | um dos poucos pixmaps por saída em que o compositor renderiza as imagens censuradas |

## Visão geral

```
o usuário marca uma janela -> _KIWM_CAPTURE_EXCLUDE nela (menu do kiwm, regra de classe, uma ferramenta)

gravador                                         compositor
   |  GetSelectionOwner(_NET_WM_CM_S0) ---------->  (nenhum: retângulos pretos próprios)
   |  lê _KICOMP_CAPTURE na janela CM ----------->  (ausente: retângulos pretos próprios)
   |  cria a janela de sessão S, põe propriedades nela
   |  ClientMessage _KICOMP_CAPTURE_OPEN --------->  valida, aloca os slots
   |  <---- propriedades em S + _KICOMP_CAPTURE_STATUS
   |
   |  a cada quadro, por saída:
   |    lê _KICOMP_CAPTURE_STATE em S
   |    nenhuma janela excluída visível -> copia a saída como sempre, relê STATE
   |    há alguma visível               -> REQUEST ----> montagem final, uma segunda vez
   |                                       <---- FRAME (slot, flags)
   |                                       copia o slot
   |                                       RELEASE ---->
   |  ClientMessage _KICOMP_CAPTURE_CLOSE -------->  libera os slots
```

O caso barato é o comum: enquanto nenhuma janela excluída está visível numa
saída, a tela **é** a imagem censurada, nada extra é renderizado, e o gravador
lê a saída exatamente como lê sem compositor — a cópia direta na GPU que torna
o kishot e o fork do OBS leves **continua**. Só enquanto há uma janela excluída
aparecendo o gravador lê um slot, e um `CopyArea` de um slot custa a ele o que
custava um `CopyArea` da raiz: os dois são pixmaps na GPU. O trabalho extra é do
compositor, e é pequeno (veja o [contrato de custo](#contrato-de-custo)).

## Marcar janelas e política

### Quais janelas: o usuário marca, com o gerenciador de janelas

Uma janela é **excluída** quando vale qualquer um destes:

1. **marcada**: a janela cliente dela leva **`_KIWM_CAPTURE_EXCLUDE`**
   (`CARDINAL`, formato 32). O valor é `1` quando a própria janela foi marcada e
   `2` quando herda a marca por ser um diálogo de uma janela marcada. Qualquer
   valor não-zero exclui; a diferença é só para o kiwm, para que desmarcar uma
   janela limpe o que ela repassou e nada além disso. Remover a propriedade
   desmarca.
2. **popup**: é uma janela override-redirect (um menu, uma dica) cujo
   `_NET_WM_PID` é igual ao de uma janela marcada. Esta é **derivada por quem lê
   as marcas** — o compositor, e o fallback do próprio gravador — e não é uma
   propriedade, porque uma janela override-redirect nunca é oferecida ao
   gerenciador de janelas: o kiwm só saberia dela quando já estivesse na tela, e
   uma marca escrita então chegaria um quadro atrasada. Janelas gerenciadas não
   têm esse problema (abaixo).

A marca é **propriedade do kiwm** (veja `kiwm/PROTOCOL.md`), não do compositor,
porque ela significa algo mesmo sem compositor nenhum: um gravador nesse caso
pinta retângulos pretos sobre as janelas que a levam, que é o que o kishot faz
hoje. Tudo o que este documento diz sobre o compositor é uma melhoria sobre
isso, e a propriedade é a mesma nos dois casos.

De onde vem a marca:

- **o menu de janela do kiwm** (`kiwm/menu.c`) tem uma entrada marcável, "Ocultar
  da captura de tela", no estilo de Maximizar/Restaurar: ela põe `1` no cliente
  ou o apaga, e (des)marca os diálogos da janela com `2`.
- **o `kiwm.conf`**, a lista que persiste entre sessões:

  ```ini
  # nomes WM_CLASS (instância ou classe, sem distinguir maiúsculas) que o kiwm sempre marca
  capture_exclude_classes = KeePassXC, Signal
  ```

  o kiwm marca uma janela que casa **quando trata o `MapRequest`, antes de
  mapeá-la**, de modo que a marca existe antes do primeiro quadro da janela; faz o
  mesmo com um diálogo que abre depois a partir de uma janela marcada. O kiconf
  ganha um campo para a lista, junto das outras configurações do kiwm.
- **o "Ocultar uma janela..." do kishot** põe `1` na janela que o usuário
  escolhe. O kishot **não tem botão de remover**: a lista de janelas ocultas dele
  é só de leitura, com um aviso de que, para parar de ocultar uma janela, o
  usuário desmarca "Ocultar da captura de tela" no menu da barra de título dela.
  Uma marca pode vir de uma regra de classe ou ser herdada por um diálogo, e o
  kishot desfazê-la seria desfeito de volta; desmarcar pertence ao lugar onde as
  marcas são gerenciadas.
- Qualquer ferramenta pode; a propriedade é a interface. Sob um gerenciador de
  janelas que não seja o kiwm só existem marcas manuais: sem lista de classes e
  sem marcas nos diálogos.

Ninguém mais guarda uma lista. O compositor e o gravador **não mantêm conjunto
de janelas próprio**: o que é excluído é o que leva a marca, mais a regra dos
popups. As marcas duram enquanto as janelas duram; a lista de classes é o que
sobrevive.

### Se, e com que aparência

Escolha do usuário, no `kicomp.conf`, seção principal:

```ini
# Oferecer captura censurada aos gravadores, e como fica uma janela excluída
# na imagem que eles recebem.
#   off      não: o protocolo de captura não é oferecido (o padrão)
#   black    a janela é desenhada como uma forma preta chapada
#   #rrggbb  ... como uma forma chapada dessa cor
#   hide     a janela não é desenhada; o que está atrás dela aparece, que é o
#            que o kwin_wayland faz com as janelas que exclui
capture_censor = off
# No máximo esta quantidade de sessões abertas ao mesmo tempo; cada uma custa
# slots (veja o contrato de custo).
capture_sessions = 2
```

`off` é o padrão porque não é de graça, e porque um gravador que não encontra o
protocolo faz algo perfeitamente bom no lugar (retângulos pretos na própria
cópia). Espera-se que o kiconf ganhe um interruptor para isso, e para o modo,
junto das outras configurações do compositor.

## Descoberta

### Há compositor?

`XGetSelectionOwner(_NET_WM_CM_S<n>)` para a tela sendo capturada. `None`: não
há compositor. O gravador censura como sempre censurou e para aqui.

### Ele oferece captura?

**`_KICOMP_CAPTURE`** (`CARDINAL[4]`, formato 32) **na janela CM** — a mesma
janela que leva `_KICOMP_EFFECTS`, e pela mesma razão:

| índice | significado |
|---|---|
| 0 | versão do protocolo, `1` |
| 1 | bits de recurso (abaixo) |
| 2 | modo de censura: `0` hide, `1` fill |
| 3 | cor de preenchimento como `0x00RRGGBB`; vale quando o modo é fill |

Bits de recurso:

| bit | nome | significado |
|---|---|---|
| 0 | `CENSOR` | o compositor sabe renderizar imagens censuradas. Sempre ligado na versão 1; reservado para que uma revisão futura ofereça captura simples sem ele |
| 1 | `DMABUF` | os pixmaps dos slots podem ser exportados com DRI3 `BuffersFromPixmap` (só o renderizador EGL pode dizer isso) |

**A ausência é toda a negociação**, como com `_KICOMP_EFFECTS`: sem compositor,
um que não conhece este protocolo, um compilado sem ele e um com
`capture_censor = off` se leem todos da mesma forma, e o gravador recorre à
própria censura. O gravador nunca precisa perguntar *por quê*.

Por que na janela CM e não na raiz: a propriedade morre com a janela. Um
compositor que cai não deixa um "sim" velho na raiz para o próximo gravador
acreditar; a janela CM é destruída junto com o cliente, e a propriedade também.

O gravador seleciona `PropertyChangeMask|StructureNotifyMask` na janela CM,
para saber quando a propriedade aparece (o usuário ligou), muda (o modo mudou)
ou some (desligaram, ou o compositor saiu — `DestroyNotify`). Um novo
compositor assumindo `_NET_WM_CM_S<n>` é uma nova janela CM: refazer a
descoberta. Em todos esses casos uma gravação em andamento passa à própria
censura **a partir do próximo quadro** (veja [Falha](#falha-e-fallback)).

### A versão

Um gravador que não conhece a versão maior em `_KICOMP_CAPTURE[0]` trata o
protocolo como ausente. Revisões futuras acrescentam capacidades como bits de
recurso e nunca mudam o significado de um existente.

## A sessão

O gravador cria uma **janela de sessão S** — uma janela `InputOnly` própria,
desmapeada —, põe nela um pequeno pedido como propriedades e envia `OPEN`. O
compositor responde na mesma janela. Como S pertence ao gravador:

- se o gravador morre, S morre com a conexão dele, o compositor vê
  `DestroyNotify` (ele seleciona `StructureNotify` em S) e libera tudo. Um
  gravador que travou não custa nada depois.
- nada da sessão vive na janela CM, então dois gravadores não se enxergam.

### Definido pelo gravador, em S, antes do `OPEN`

| propriedade | tipo | significado |
|---|---|---|
| `_KICOMP_CAPTURE_CLIENT` | `UTF8_STRING` | o nome do programa, para logs e o painel `stats` |
| `_KICOMP_CAPTURE_OUTPUTS` | `UTF8_STRING` | nomes RandR separados por NUL (`"DP-1\0HDMI-1\0"`). A posição nesta lista é o **índice de saída da sessão**, usado por todos os registros e mensagens abaixo |
| `_KICOMP_CAPTURE_SLOTS` | `CARDINAL` | slots por saída, 1–4; padrão 3. Uma captura única pede 1 |
| `_KICOMP_CAPTURE_HIDE_PID` | `CARDINAL` | opcional: um `_NET_WM_PID` cujas janelas ficam fora da imagem por completo — as do próprio gravador, para que ele não precise esconder a janela dele e esperar a tela repintar antes de capturar |

É só isso. O gravador **não envia lista de janelas**: o que é excluído é
conhecimento do compositor, igual para todo gravador. A única exceção é
`HIDE_PID`, que não é sobre privacidade: janelas daquele processo **nunca são
desenhadas** nas imagens desta sessão (sem conteúdo, sem sombra), seja qual for
o `capture_censor`, e contam em `visible` como qualquer janela excluída.

### Mensagens

Todas são `ClientMessage`, formato 32, enviadas com `XSendEvent` e **máscara de
eventos vazia**, o que entrega ao cliente que criou a janela de destino e não
exige seleção — do jeito que `_KICOMP_QUIT` já chega ao compositor. As mensagens
para o compositor vão para a **janela CM**; as para o gravador vão para **S**.
`data.l[0]` de toda mensagem ao compositor é S.

| mensagem | para | `l[0]` | `l[1]` | `l[2]` | `l[3]` | `l[4]` |
|---|---|---|---|---|---|---|
| `_KICOMP_CAPTURE_OPEN` | compositor | S | versão (`1`) | | | |
| `_KICOMP_CAPTURE_CLOSE` | compositor | S | | | | |
| `_KICOMP_CAPTURE_REQUEST` | compositor | S | saída | id do pedido | | |
| `_KICOMP_CAPTURE_RELEASE` | compositor | S | saída | slot | | |
| `_KICOMP_CAPTURE_STATUS` | gravador | estado | erro | saídas | versão | |
| `_KICOMP_CAPTURE_FRAME` | gravador | saída | slot | id do pedido | flags | |

### Respondido pelo compositor, em S

Escrito **antes** de enviar `STATUS`. Os dois seguem pela mesma conexão, então o
gravador que recebe `STATUS` já encontra as propriedades lá.

**`_KICOMP_CAPTURE_TARGETS`** (`CARDINAL[]`, formato 32): um registro de 13
palavras por saída da sessão, em ordem.

| palavra | significado |
|---|---|
| 0 | flags: bit 0 `AVAILABLE`, bit 1 `DMABUF` (os slots desta saída podem ser exportados) |
| 1, 2 | `x`, `y` da saída em coordenadas da raiz, como `int32` |
| 3, 4 | `w`, `h`: o tamanho dela em pixels da raiz (o tamanho *lógico*, aquele em que ficam as áreas e o ponteiro) |
| 5, 6 | `pw`, `ph`: o tamanho dos slots em pixels: **o tamanho final da saída**, o que ela de fato exibe. Numa saída HiDPI é maior que `w`, `h`; escale uma área por `pw/w`, `ph/h` |
| 7 | profundidade dos pixmaps dos slots, `24` |
| 8 | quantidade de slots `n` |
| 9–12 | os XIDs dos pixmaps dos slots; palavras não usadas valem `0` |

A imagem é **sempre na resolução final da saída**: o compositor nunca a reduz
ao tamanho lógico. Numa saída 1× `pw`×`ph` é igual a `w`×`h`.

**`STATUS`**: `estado` `1` a sessão está ativa; `0` foi recusada, ou terminou (o
gravador a esquece). `erro`:

| erro | significado |
|---|---|
| 0 | nenhum |
| 1 | versão de protocolo não suportada |
| 2 | desativado (`capture_censor = off`) |
| 3 | um nome de saída é desconhecido |
| 4 | sem recursos: sessões demais, ou os slots não puderam ser alocados |
| 5 | não permitido (o servidor recusou algo ao compositor) |

Uma sessão recusada é, para o gravador, o mesmo que captura ausente: seus
próprios retângulos pretos.

### Encerramento

O gravador envia `CLOSE` (ou simplesmente morre). O compositor libera os slots
e **apaga as propriedades de sessão que escreveu** *antes* de liberar os
pixmaps, pela razão que `_KICOMP_STOWED_PIXMAP` dá: um leitor que relê a
propriedade a cada uso nunca fica segurando um XID que o compositor já liberou.
Quando é o compositor que encerra a sessão (está saindo, ou `capture_censor`
foi desligado), ele faz o mesmo e envia `STATUS` com estado `0`.

## O que "excluída" significa na imagem

### Como é desenhada

A imagem é a cena da saída **montada de novo com aquelas janelas substituídas**,
na mesma ordem de empilhamento, o que faz os casos especiais antigos
desaparecerem: uma janela acima de uma excluída a cobre como na tela; uma
abaixo não é afetada; uma janela translúcida acima de uma excluída mostra o que
a exclusão deixou.

| modo | a janela excluída é |
|---|---|
| fill (`black`, `#rrggbb`) | desenhada com uma cor chapada no lugar do conteúdo: mesma forma (cantos arredondados, extensão shape), mesma geometria, transformação, opacidade, recorte e efeito da tela. A sombra dela é desenhada como de costume — uma sombra não diz nada sobre o conteúdo |
| hide | não desenhada: sem conteúdo, sem sombra, sem efeito que a anime. O que estava atrás dela aparece |

Nos dois, **todo lugar em que o compositor desenha o conteúdo da janela** é
substituído, não só o principal: uma célula do expo, uma face do cubo (inclusive
as viradas para longe), um cartão do cover switch, uma animação de lâmpada
mágica ou de minimizar, uma malha wobbly, uma imagem guardada (conteúdo de
`_KICOMP_STOWED_PIXMAP` desenhado como miniatura), uma animação de fechar de
uma janela que já não existe (`window_retain`). O teste cabe numa frase:
**nenhum pixel da imagem deriva do conteúdo de uma janela excluída.**

O que a imagem não contém: o ponteiro (o gravador o compõe, como hoje). Nada
mais é removido — as camadas do próprio kiwm (o painel do alt-tab, o wireframe
de mover/redimensionar) são janelas comuns na cena e estão nela.

### Dano

Uma janela ser excluída não é dano para o quadro normal: quando uma marca é posta
ou removida, ou uma janela excluída é mapeada, desmapeada ou sai de uma saída, a
região que ela cobria mostra os mesmos pixels na tela e outros na imagem. Então
**qualquer mudança no estado de exclusão de uma janela invalida a imagem das
saídas que ela toca**: a região afetada é redesenhada no mínimo, e `UNCHANGED`
nunca é respondido através de uma mudança assim. (O KWin força um repaint total da
sua visão filtrada pelo mesmo motivo.)

### Quando não dá para fazer

Uma saída para a qual o compositor não consegue renderizar uma imagem censurada
avisa em `STATE` (`UNAVAILABLE`, abaixo) e o gravador censura aquela saída por
conta própria. Os casos:

- a saída está **sem redirecionamento** (uma janela em tela cheia desenhada
  direto na tela, `unredirect.c`): a tela é essa janela e mais nada, então não
  há o que compor e o compositor **mantém a saída sem redirecionamento** (kicomp
  0.3.70; um rascunho anterior mandava recompor, o que custava a um jogo a
  exibição direta por causa de uma imagem de uma cor só). O que está atrás da
  janela não está na imagem, marcado ou não: `visible` é `0` se a janela em tela
  cheia não está excluída (o gravador copia a tela), e `1` se está, e então a
  imagem é **chapada** — a cor de preenchimento, ou preto no modo `hide`, que não
  tem um "o que há atrás" para mostrar;
- o renderizador caiu em um fallback ou não consegue desenhar um alvo fora da
  tela para a sessão;
- a saída sumiu (desconexão a quente).

## Qual buffer ler

**`_KICOMP_CAPTURE_STATE`** (`CARDINAL[]`, formato 32, em S): três palavras por
saída da sessão, em ordem, mantidas em dia pelo compositor.

| palavra | significado |
|---|---|
| 0 | `epoch`: aumenta toda vez que a próxima palavra passa de 0 para não-0 ou o contrário |
| 1 | `visible`: quantas janelas excluídas contribuem pixels para a imagem desta saída agora. `0` significa que a tela **é** a imagem censurada |
| 2 | flags: bit 0 `UNAVAILABLE` |

A decisão do gravador, por saída e por quadro:

| `STATE` | ler | custo para o compositor |
|---|---|---|
| `UNAVAILABLE` | a saída, e então censurar por conta própria (retângulos pretos, como sem compositor) | nenhum |
| `visible` = 0 | a saída, exatamente como sem compositor; depois reler `STATE` e **descartar o quadro se `epoch` mudou** | nenhum |
| `visible` > 0 | um slot: `REQUEST`, esperar `FRAME`, ler, `RELEASE` | uma montagem final extra da saída |

### A regra de ordenação que torna `visible` = 0 seguro

O gravador não pergunta nada ao compositor quando `visible` é 0, então entre ele
ler `STATE` e ler a saída uma janela excluída pode aparecer (o usuário marca a
entrada do menu, ou um aplicativo abre uma janela que casa com uma regra de
classe). Por isso o compositor segue uma **ordenação de um só lado**:

> **Quando `visible` passa de 0 para não-0, o compositor escreve `STATE`
> primeiro e pinta depois o primeiro quadro que contém a janela.** Na volta, de
> não-0 para 0, pode escrever `STATE` tarde (depois do quadro que removeu a
> janela) — o gravador só lê slots um pouco mais.

A metade do gravador é um seqlock:

```
lê STATE             -> epoch e, visible 0
copia a saída        -> (CopyArea / GetImage da área da saída)
lê STATE de novo     -> epoch e'; se e' != e, descarta o quadro e usa REQUEST
```

Isso é sólido porque o servidor processa os pedidos de cada conexão em ordem: a
cópia é processada antes da segunda leitura; se a cópia viu um quadro com a
janela, esse quadro foi apresentado depois do `ChangeProperty` do compositor,
que foi processado antes, então a segunda leitura vê a nova época. Uma janela
pode aparecer na tela por no máximo um quadro *descartado*.

## Obter um quadro: `REQUEST`, `FRAME`, `RELEASE`

Pull, não push: o compositor monta uma imagem censurada só quando o gravador
pede, então um gravador a 30 fps numa saída de 144 Hz custa 30 montagens extras
por segundo, não 144.

`REQUEST` (`saída`, `id do pedido`): o gravador quer uma imagem censurada
daquela saída. Todo pedido recebe **exatamente um** `FRAME` com o mesmo `id do
pedido`:

- no próximo repaint da saída, com a imagem montada num slot livre — ou
  imediatamente, se a saída não mudou desde a última imagem que o compositor fez
  para esta sessão, com o mesmo slot e a flag `UNCHANGED`;
- ou com `slot` = `0xFFFFFFFF` e uma flag de falha.

Flags de `FRAME`:

| bit | nome | significado |
|---|---|---|
| 0 | `UNCHANGED` | nada mudou desde a última imagem desta saída; mesmo conteúdo, mesmo slot |
| 1 | `BUSY` | sem slot livre (o gravador segura todos); tente de novo |
| 2 | `UNAVAILABLE` | a saída não pode ser renderizada censurada agora; leia `STATE` |

### Segurar um slot

Um slot **entregue** por `FRAME` fica **seguro** até o gravador enviar
`RELEASE` para ele, e o compositor **não escreve num slot seguro**. "Seguro" é
uma contagem: `UNCHANGED` entrega o mesmo slot de novo e o segura de novo, e
cada entrega é liberada por seu próprio `RELEASE`. Com `n` slots o gravador pode
segurar no máximo `n - 1`; o que sobra é onde o compositor renderiza.

O compositor escolhe o slot livre entregue há mais tempo, para que um slot
tenha a maior idade possível ao ser reutilizado — quanto maior o intervalo,
menos o trabalho de GPU que o leu ainda pode estar rodando.

### O que o gravador faz com um slot

Um slot é um pixmap comum (`depth` 24) **do compositor** — desenhe a partir
dele, nunca o libere, nunca desenhe nele; um `XFreePixmap` de outro cliente o
destrói para todo mundo. As mesmas regras de `_KICOMP_STOWED_PIXMAP`: leia as
propriedades a cada uso em vez de guardar um XID; tolere o erro mesmo assim (um
tratador de erros permissivo em volta da leitura é todo o tratamento que
precisa); e o que você recebe é a imagem do momento do `FRAME`.

A base é a cópia na GPU que o gravador já tem, com outra origem:

```
xcb_copy_area(slot -> meu pixmap de quadro, no deslocamento da área)   // por saída, juntando
... ponteiro, como antes ...
ida e volta (GetInputFocus)
RELEASE
```

O kishot já copia a raiz para um anel de pixmaps próprios, então uma saída
censurada não custa a ele nada que ele não fizesse; uma área com várias saídas
exige um `CopyArea` por saída no lugar certo. Os slots estão nos pixels finais
da saída (`pw`×`ph`), então numa saída HiDPI o gravador escala o recorte por
`pw/w`. Um consumidor que amostra a saída como textura GL (o fork do OBS) liga o
pixmap do slot do mesmo jeito que liga o da raiz, quando `visible` > 0.

**Zero-copy a partir do slot** (`DMABUF`): com o bit de recurso e a flag do
registro, o gravador pode exportar um slot com DRI3 `BuffersFromPixmap` e
importá-lo direto, pulando a cópia própria. Ele abre mão de compor o ponteiro e
de juntar várias saídas na GPU, então só vale para uma saída, sem ponteiro. O
compositor aloca os slots **sem modificadores de compressão** (sem DCC), que é
o que codificadores e VPP conseguem importar; o gravador passa o modificador que
a exportação devolve em vez de supor `DRM_FORMAT_MOD_INVALID`.

Regra do `RELEASE`: **só envie depois de uma ida e volta que siga o último
pedido que lê o slot**, para que o servidor tenha pelo menos começado a leitura.
Quando a leitura passa por uma importação dma-buf, o gravador também não deve
liberar antes de o consumidor (o codificador) ter terminado.

## Sincronização

- **`FRAME` significa completo.** O compositor o envia quando o trabalho de GPU
  do slot terminou — ele espera na própria fence, de forma assíncrona (um sync
  fd observado no laço principal), não com um `glFinish` que travaria todas as
  outras saídas. Um gravador pode ler um slot com ou sem sincronização implícita
  e ver uma imagem pronta. Fences explícitas entregues ao gravador ficam para uma
  versão futura (não cabem num `ClientMessage`; precisariam de fences DRI3).
- **Escrita-após-leitura** (o compositor reutilizando um slot que o servidor
  ainda está copiando) é coberta pela ida e volta do `RELEASE`, mais a fence por
  buffer do kernel, mais a escolha do menos recentemente entregue acima. O
  compositor não deve confiar só na primeira.
- **Ordem no fio**: propriedades primeiro, depois a mensagem que aponta para
  elas — do lado do compositor para `TARGETS`, `STATE` e `STATUS`.
- **Carimbos de tempo**: o gravador carimba um quadro quando `FRAME` chega. Uma
  imagem é montada no repaint da saída e anunciada assim que o trabalho de GPU
  acaba, então isso está a um quadro da verdade; um gravador que quiser mais
  precisão poderá receber MSC/UST numa propriedade no futuro.

## Contrato de custo

O que o compositor deve garantir, para que um gravador possa ligar isto sem
pensar. São requisitos a implementar e depois **medir**; nenhum é uma afirmação
sobre qualquer build.

1. **Sem sessão, sem custo.** Sem slots, sem código no caminho do quadro além de
   um teste.
2. **Uma sessão sem nada excluído visível custa só contabilidade**: as palavras
   de `STATE`, mantidas a partir de eventos de janela, e os slots recebidos no
   `OPEN`.
3. **Montar só quando pedido, e só se a saída mudou** desde a última imagem
   daquela sessão (`UNCHANGED` caso contrário).
4. **Só a montagem final é feita duas vezes.** Tudo o que não depende das
   janelas excluídas é feito uma vez e compartilhado pelas duas imagens (tabela
   abaixo).
5. **A memória são os slots**: `n × pw × ph × 4` bytes por saída pedida (1440×900,
   três slots, cerca de 15 MB), alocados no `OPEN` e liberados no `CLOSE`; uma
   sessão de captura única pede um slot. `capture_sessions` limita o total.

### O que é compartilhado e o que é duplicado

| trabalho | feito |
|---|---|
| texturas das janelas, os dma-bufs delas, dano, o espelho de janelas | uma vez |
| geometria, estado de animações e efeitos, o grafo da cena | uma vez |
| sombras e qualquer intermediário que não leia uma janela excluída | uma vez |
| a montagem final: percorrer a cena e desenhar a saída | **duas vezes**: uma como é mostrada, outra com a substituição |
| um intermediário que *lê* uma janela excluída (um blur do que está atrás de uma janela translúcida sobre uma delas) | por imagem, só onde difere |

Então o pior caso é uma montagem final a mais de uma saída, e só nas saídas em
que uma janela excluída aparece e só enquanto um gravador roda. Qualquer coisa
que torne isso maior (um efeito que recalcula por imagem) é um bug desse efeito.

**Jeito recomendado de fazer a segunda montagem barato:** deixar o quadro real
terminar; copiá-lo para o slot (um blit na GPU); e então redesenhar no slot
**só as regiões que as janelas excluídas tocam** — a janela, e no modo `hide`
também a sombra — com a substituição, desenhando ali tudo o que a cena tem
naquelas regiões, de modo que o que está acima de uma janela excluída continua
acima dela. O custo passa a crescer com a *área* das janelas excluídas, não com
a saída. Um compositor pode, em vez disso, montar a saída inteira uma segunda
vez; o contrato é a imagem, não o método.

De qualquer forma, um slot reutilizado não precisa ser refeito do zero: o
compositor pode guardar, por slot, o dano acumulado desde a última vez que aquele
slot foi atualizado (um diário de dano, o buffer age do GL) e refazer só isso. Os
streams de screencast do KWin fazem exatamente isso.

**O que o kicomp faz (0.3.69), e o que mediu.** O diário de dano por slot, não a
cópia: cada slot guarda o dano que o quadro real recebeu desde a última vez que
aquele slot foi desenhado (mais o dano que o quadro real descarta por ninguém
ver — sob uma janela opaca — quando algo está oculto, e o retângulo de uma
janela cuja exclusão mudou), e a segunda montagem redesenha só isso; o que é
desenhado duas vezes é o que mudou, como na tela. Um `REQUEST` é atendido na
mesma volta do laço em que chega, não no próximo repaint da saída (esperar esse
repaint e o vblank dele eram 16 dos 22 ms que uma imagem levava). No GLX o slot
é ligado como textura (texture-from-pixmap) e desenhado por um framebuffer
object, depois `glFlush`: o uso do buffer pelo servidor é ordenado pelo
sincronismo implícito do kernel. Um *drawable* GLX pixmap dá os mesmos pixels,
mas gasta ~6 ms no flush do buffer frontal; ler de volta para a memória do
sistema e fazer `PutImage` funciona em qualquer lugar e é o último recurso. Cada
um é conferido no primeiro uso (alguns pixels lidos dos dois lados) e o próximo
é usado se o servidor não vê o desenho. Na sessão real (AMD Renoir, GLX,
gravação a 60 fps de uma saída 1920×1080 com um emulador animando): com uma
janela excluída aparecendo, o kicomp foi de 2,9 a 4,2 % de um núcleo, o servidor
X ficou igual, a GPU de 23 a 34 %, nenhum quadro perdido, uma imagem pronta
0,2–0,6 ms depois do seu `REQUEST`.

## Segurança

- **O compositor é livre.** Ele vê todas as janelas por ser o compositor
  (redirecionamento Composite é o que ele é), e nada neste protocolo lhe dá um
  acesso que ele já não tenha; ele não precisa ser autorizado.
- **Tudo é um drawable X.** O compositor nunca move pixels para fora do X: um
  slot é um pixmap nomeado por um XID; lê-lo é um pedido X do gravador
  (`CopyArea`, `GetImage`, `BuffersFromPixmap`). O servidor continua sendo,
  portanto, o ponto de controle de acesso sobre quem pode ler a tela, e o
  xisguard também onde ele intermedeia esses pedidos. O compositor não decide
  quem pode capturar e não deve ganhar um segundo jeito, mais fraco, de fazê-lo.
- **O que verificar ao implementar** (não é uma pergunta de projeto): que um
  gravador autorizado a capturar a tela também possa ler um slot. Ler o slot deve
  valer, para a política, o mesmo que ler a raiz; se as regras do xisguard
  distinguirem a leitura do pixmap de outro cliente, os slots precisam ser
  reconhecidos como equivalentes à tela.
- **O `STATUS` com erro 5** existe para quando o compositor é impedido de algo de
  que precisa (criar os pixmaps, por exemplo): a sessão falha limpamente e o
  gravador recorre ao fallback.
- **O que não protege.** A própria tela mostra as janelas excluídas; o `GetImage`
  da raiz, a captura de qualquer outro programa, as miniaturas do xispanel a
  partir de `_KICOMP_STOWED_PIXMAP` — todos ainda as veem. Isto é uma propriedade
  de uma imagem entregue a um gravador. Quem quiser esconder uma janela de toda
  captura precisa que o servidor faça isso.
- **Sessões forjadas.** `OPEN` nomeia uma janela, e qualquer cliente pode enviar
  a mensagem com qualquer id de janela. O compositor só escreve propriedades em S
  e envia eventos a ela, e os slots que ele nomeia são pixmaps que qualquer
  cliente já pode usar, então o pior que um forjador consegue é incomodar;
  `capture_sessions` limita até isso.

## Falha e fallback

A regra do gravador é que **ele nunca copia a saída sem máscara enquanto algo
que deveria ser excluído possa estar visível**. Concretamente:

| evento | o gravador faz |
|---|---|
| sem compositor, sem `_KICOMP_CAPTURE`, sessão recusada | seus próprios retângulos pretos na cópia da saída — o comportamento que tem hoje |
| `STATE` diz `UNAVAILABLE` para uma saída | o mesmo, para aquela saída |
| `FRAME` demora mais que o limite (sugestão: 100 ms) ou responde `BUSY` | repete o quadro anterior; não lê a saída |
| `visible` > 0 e a sessão ou o slot sumiu (`BadPixmap`, `DestroyNotify` na janela CM, `STATUS` 0) | a partir deste quadro censura com os próprios retângulos; nunca lê a saída sem máscara |
| uma leitura com `visible` = 0 cuja segunda leitura de `STATE` difere | descarta |

O gravador monta seus próprios retângulos pelas mesmas regras do compositor: as
janelas que levam a marca, mais a regra dos popups. Um gravador que recorreu a retângulos pretos enquanto a política do
compositor é `hide` não está fazendo o que o usuário pediu; ele diz isso na
linha de status em vez de escurecer em silêncio.

## Para os três programas

### O kicomp deve

- publicar `_KICOMP_CAPTURE` na janela CM se e só se `capture_censor` não for
  `off`, com o modo e a cor; mantê-la em dia; apagá-la ao desligar;
- manter o conjunto de janelas excluídas a partir do espelho de janelas que já
  tem: a marca `_KIWM_CAPTURE_EXCLUDE` nas janelas cliente (ele observa a
  propriedade) e a regra dos popups (`_NET_WM_PID`, override-redirect),
  reavaliado quando uma janela é mapeada e quando qualquer das duas muda — de
  modo que uma janela seja excluída desde o primeiro quadro em que é pintada;
- no `OPEN`: validar, alocar os slots como pixmaps X (o renderizador EGL como
  buffers GBM nomeados com `PixmapFromBuffers` — a mecânica que já tem para os
  quadros; XRender e GLX com pixmaps comuns do servidor) **no tamanho final da
  saída**, escrever `TARGETS` e `STATE`, enviar `STATUS`;
- manter `STATE` em dia com a ordenação de um só lado;
- no `REQUEST`, fazer a segunda montagem final num slot livre — uma flag no nó
  da cena, não uma segunda cena — e enviar `FRAME` quando a GPU terminar;
- deixar em paz uma saída sem redirecionamento (acima) e responder por ela como
  uma cor chapada quando a janela em tela cheia é excluída;
- liberar tudo no `CLOSE`, no `DestroyNotify` de S e ao sair.

### O kiwm deve

- acrescentar a entrada marcável "Ocultar da captura de tela" ao menu de janela,
  criando ou apagando `_KIWM_CAPTURE_EXCLUDE` na janela cliente, e (des)marcando
  os diálogos dela com `2`;
- ler `capture_exclude_classes` do `kiwm.conf` e marcar as janelas que casam com
  `1` **antes de mapeá-las** (no `MapRequest`), e os diálogos de uma janela
  marcada com `2`, também antes de mapeá-los;
- documentar a propriedade em `kiwm/PROTOCOL.md`. Fora isso o kiwm não participa:
  não conhece sessões nem slots.

### O kishot deve

- rodar a descoberta no início de cada captura, e observar a janela CM enquanto
  grava;
- **não manter lista própria de janelas nem de classes** (`censor_classes`,
  `censor_popups` e a página de configurações Janelas ocultas deixam de existir):
  a lista de janelas ocultas na janela principal é as janelas que levam a marca
  naquele momento;
- manter a própria censura (`ks_censor.c`) como fallback e como único caminho sem
  compositor, construída a partir das marcas mais a regra dos popups; tirá-la de
  uma saída só enquanto uma sessão estiver entregando imagens censuradas;
- fazer o "Ocultar uma janela..." pôr `_KIWM_CAPTURE_EXCLUDE` com valor `1`, **sem
  botão de remover**: a lista é só de leitura e avisa que, para parar de ocultar
  uma janela, o usuário desmarca "Ocultar da captura de tela" no menu da barra de
  título dela;
- enquanto houver sessão aberta, enviar `HIDE_PID` com o próprio PID e parar de
  esconder a própria janela ao redor de uma captura (o esconder-e-esperar-um-repaint
  que faz hoje continua como fallback);
- pegar a saída como faz hoje quando `visible` é 0 e ler um slot quando não é,
  por saída, com o seqlock e as regras de liberação acima;
- mostrar o que está acontecendo — "janelas ocultas: compositor (hide)", "…:
  retângulos pretos" — na linha de status, tomando o modo de `_KICOMP_CAPTURE`;
- usar o mesmo caminho para uma captura e para a prévia ao vivo da aba Gravação
  (uma sessão com um slot).

### Nada muda

No servidor X e em todos os outros clientes.

## Referência: o KWin

O KWin (Wayland, Plasma 6.6) tem "Excluir da captura". O que ele faz, lido dos
fontes e dos merge requests
([!8442](https://invent.kde.org/plasma/kwin/-/merge_requests/8442),
[!9171](https://invent.kde.org/plasma/kwin/-/merge_requests/9171),
[`filteredsceneview.cpp`](https://invent.kde.org/plasma/kwin/-/raw/master/src/plugins/screencast/filteredsceneview.cpp),
[`outputscreencastsource.cpp`](https://invent.kde.org/plasma/kwin/-/raw/master/src/plugins/screencast/outputscreencastsource.cpp)):

- a propriedade é `Window::excludeFromCapture`, definida pelo menu da barra de
  título, pelo menu do Gerenciador de Tarefas, por uma regra de janela ou pela API
  de scripts; janelas transientes são marcadas **recursivamente**, então menus e
  diálogos vão junto;
- cada stream de screencast tem uma **`FilteredSceneView`**: uma segunda pintura
  da cena, com camada e rastreio de dano próprios, em que um predicado
  (`addWindowFilter`) descarta as janelas excluídas enquanto a ordem de
  empilhamento é montada. Ela renderiza **direto no buffer do consumidor** (o
  dma-buf do PipeWire), no tamanho final em pixels da saída, com o cursor como uma
  visão separada;
- `pidToHide`: as janelas do aplicativo que pediu a captura podem ficar de fora, e
  é assim que o Spectacle se mantém fora da própria gravação;
- quando a flag de uma janela muda, a visão chama
  `addDeviceRepaint(Region::infinite())`, porque a região que a janela libera não
  está no dano normal;
- a flag começou só para screencasts ("quem quer uma captura de tela pode
  minimizar a janela"); as capturas de tela passaram a respeitá-la depois, por
  consistência e privacidade.

Onde este protocolo concorda: o grafo da cena é compartilhado e só a pintura final
é feita duas vezes, com um predicado de janela no passo da ordem de empilhamento;
transientes seguem a janela-mãe; a imagem é no tamanho final da saída; capturas de
tela são cobertas desde o início. Onde vai além: o KWin cria a visão filtrada para
todo stream, haja ou não janela excluída, e só tem a aparência "não desenhada";
aqui nada é renderizado enquanto nenhuma janela excluída está visível (`visible` =
0), e `fill` existe ao lado de `hide`. Onde o KWin faz melhor, e o que este
documento tirou dele: esconder as janelas do próprio gravador (`HIDE_PID`),
invalidar a imagem quando o estado de exclusão muda (Dano), o diário de dano por
slot, e um menu do gerenciador de tarefas como um segundo lugar para marcar
(abaixo).

## Perguntas em aberto

- **Buffers fornecidos pelo gravador.** O KWin renderiza no próprio buffer do
  consumidor, então não há cópia nenhuma; aqui o gravador copia o slot para o
  pixmap de quadro dele, uma cópia de quadro inteiro na GPU por quadro (em 4K,
  cerca de 33 MB). Uma versão futura poderia deixar o gravador entregar ao
  compositor os pixmaps do próprio anel, e o compositor renderizar a imagem
  censurada direto neles — só com EGL, e o recorte e a junção de várias saídas
  teriam de ir para o compositor. Os slots continuam sendo a base porque
  funcionam com qualquer renderizador.
- **Um segundo lugar para marcar.** O KDE também oferece a marca no menu do
  Gerenciador de Tarefas. Uma entrada no menu da tasklist do xispanel poria e
  removeria a mesma propriedade, e é o lugar natural para o "desmarcar" que o
  kishot não tem.
- **A regra dos popups está em dois programas.** Casar uma janela
  override-redirect com uma marcada pelo `_NET_WM_PID` acontece no compositor e no
  fallback do kishot; deveria ser uma função só em `shared/`, não duas que
  divergem.
- **Sincronização explícita e carimbos de tempo**: uma versão futura, como fences
  DRI3 e uma propriedade para MSC/UST.
- **Um segundo gravador.** A fonte XSHM do fork do OBS também poderia falar isto,
  e como a marca é do kiwm ela também não precisa de lista própria; o modelo de
  slots combina com a estrutura de copiar-e-ler-depois dela, e a cópia direta na
  GPU dela fica intacta sempre que nada está excluído.

## Átomos

| átomo | em | tipo | definido por |
|---|---|---|---|
| `_KIWM_CAPTURE_EXCLUDE` | uma janela cliente | `CARDINAL` | o kiwm (menu, classes do `kiwm.conf`, diálogos), o kishot, qualquer ferramenta |
| `_KICOMP_CAPTURE` | janela CM | `CARDINAL[4]` | compositor |
| `_KICOMP_CAPTURE_CLIENT` | S | `UTF8_STRING` | gravador |
| `_KICOMP_CAPTURE_OUTPUTS` | S | `UTF8_STRING` | gravador |
| `_KICOMP_CAPTURE_SLOTS` | S | `CARDINAL` | gravador |
| `_KICOMP_CAPTURE_HIDE_PID` | S | `CARDINAL` | gravador |
| `_KICOMP_CAPTURE_TARGETS` | S | `CARDINAL[]` | compositor |
| `_KICOMP_CAPTURE_STATE` | S | `CARDINAL[]` | compositor |

Mensagens: `_KICOMP_CAPTURE_OPEN`, `_CLOSE`, `_REQUEST`, `_RELEASE` (gravador →
janela CM), `_STATUS`, `_FRAME` (compositor → S).
