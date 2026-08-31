# kicomp

Compositor opcional do `kiwm`, seguindo `kiwm/kiwm-kicomp-projeto.md`.

Começou como o protótipo da Fase 5 — a mesma cena que você veria sem
composição, com transparência real (alpha de janelas de 32 bits e
`_NET_WM_WINDOW_OPACITY`) como única diferença. Hoje já tem também:

- shape aplicada na composição (cantos arredondados, clientes com shape);
- interface de efeitos: *geometry change*, *fade in/out*, *scale in/out*;
- relógio de frames por output para as animações;
- configuração em `kicomp.conf`.

Sem OpenGL ainda — o renderer é XRender, que dá conta de translação,
escala e alpha. Wobbly e blur é que vão pedir GL.

```sh
make
./kicomp
```

Opções:

| opção | efeito |
|---|---|
| `--replace` | assume o lugar de outro compositor em execução |
| `--single-drawable` | modo legado: **um** drawable para a tela inteira em vez de um por output |
| `--skip-wm-layers` | não compõe as camadas próprias do kiwm (`_KIWM_LAYER`: OSD do alt-tab, contorno de move/resize) |
| `--effects`, `--no-effects` | liga/desliga as animações |
| `--anim-ms=N` | unidade global de animação, em ms |
| `--renderer=NOME` | `auto` \| `xrender` |
| `-v`, `--verbose` | log detalhado (eventos, janelas, camadas, frames) |

Toda opção tem uma chave equivalente no `kicomp.conf` (abaixo); a linha
de comando sempre vence sobre o arquivo.

Ele sempre imprime no terminal, sem `-v`, o essencial: backend de render e
de apresentação, capabilities detectadas, e quantos drawables existem e
por quê — atualizado a cada mudança de output:

```
kicomp: kicomp 0.2.0 on :0 screen 0 (3840x1080)
kicomp: renderer=xrender presenter=copy
kicomp: capabilities: composite=1 overlay=1 damage=1 xfixes=1 render=1 randr=1 present=0 flip-per-crtc=0
kicomp: 2 drawables (one per output)
kicomp:   [0] DP-1         1920x1080+0+0 @ 143.98 Hz
kicomp:   [1] HDMI-1       1920x1080+1920+0 @ 60.00 Hz
kicomp: effects on, animation unit 160 ms
kicomp:   geometry   on , 160 ms
kicomp:   fade-in    on , 160 ms
kicomp:   fade-out   on , 160 ms
kicomp:   scale-in   off, 160 ms
kicomp:   scale-out  off, 160 ms
```

com `--single-drawable`:

```
kicomp: 1 drawable (legacy single-screen mode)
kicomp:   [0] screen       3840x1080+0+0 @ 60.00 Hz
```

O modo legado é o único lugar onde a regra "um drawable por output" é
desligada de propósito — cena, renderer, presenter e dirty state não
mudam, só passam a ter um output só, do tamanho da tela.

`kicomp` é opcional em todos os sentidos: `kiwm` não sabe que ele existe,
não precisa de nenhuma alteração para ser composto, e matar o `kicomp`
devolve a sessão ao caminho não-composto (seção 31 do documento).

## Configuração

`$XDG_CONFIG_HOME/kicomp.conf`, ou `~/.config/kicomp.conf`. O arquivo é
opcional — toda chave tem um default que funciona. Mesmo formato do
`kiwm.conf` (`chave = valor`, `#` comenta), mais seções para os efeitos:

```ini
# ---- global ----
effects            = 1     # animações ligadas
animation_duration = 160   # a unidade de animação, em ms
renderer           = auto  # auto | xrender
presenter          = auto  # auto | copy
single_drawable    = 0     # 1 = modo legado, um drawable pra tela toda
skip_wm_layers     = 0     # 1 = não compõe o OSD/contorno do kiwm

# ---- efeitos ----
[effect:geometry]
enabled  = 1
duration = 1.0   # múltiplo de animation_duration, não milissegundos

[effect:fade-in]
enabled  = 1
duration = 1.0
windows  = 1     # janelas comuns
menus    = 1     # menus, popups, tooltips, notificações
docks    = 1     # paineis

[effect:fade-out]
enabled  = 1
duration = 1.0
events   = close     # close,minimize,desktop-leave... (ver Eventos)

[effect:scale-in]
enabled  = 0
duration = 1.0
from     = 0.8      # tamanho inicial, fração do final
origin   = window   # window | pointer | output

[effect:scale-out]
enabled  = 0
duration = 1.0
to       = 1.15     # fração do tamanho real; > 1 incha antes de sumir
origin   = window
```

Comentário na mesma linha (`origin = pointer  # ...`) e espaço à direita
do valor são aceitos — só precisa de um espaço antes do `#`.

**A unidade de animação.** Nenhum efeito tem tempo próprio em
milissegundos: cada um pede um *múltiplo* de `animation_duration` — `0.5`
para algo que deve parecer instantâneo, `1.0` para uma transição comum,
`2.0` para uma transição grande. Assim uma única chave acelera ou
desacelera o desktop inteiro de forma coerente, em vez de deixar um
punhado de animações reguladas independentemente. `animation_duration=0`
mantém os efeitos ligados mas termina todos imediatamente.

Cada `[effect:<nome>]` aceita sempre `enabled`, `duration` e `events`, e
além dessas as chaves que o próprio efeito entender — cada módulo parseia as
suas (`config_key` em `effect.h`), e uma chave que ele não conhece vira
aviso no terminal em vez de sumir em silêncio.

## Eventos

O core não entrega ao efeito transições do X (mapeou, desmapeou,
reconfigurou) e sim **o que aconteceu com a janela**, no vocabulário do
desktop:

```
open   close   minimize   restore   maximize   unmaximize
shade  unshade fullscreen unfullscreen  focus  unfocus  move
desktop-leave  desktop-enter
```

Cada efeito declara em quais deles responde, e isso é configuração:

```ini
[effect:geometry]
events = maximize,unmaximize,move   # sem shade: quem enrola é o shade

[effect:fade-out]
events = close,minimize             # some ao fechar E ao minimizar
```

`all` e `none` valem como lista inteira. O core filtra antes de chamar o
módulo, então um efeito nunca recebe um evento que o usuário não pediu —
é por isso que "enrolar no shade sem o geometry deslizando junto" é uma
linha de config, e não um caso especial dentro de um efeito.

**Como o core sabe.** Um unmap do X pode ser fechar, minimizar ou sair de
um desktop; um resize pode ser maximizar, enrolar, virar fullscreen ou só
redimensionar. A diferença está em propriedades (`_NET_WM_STATE`,
`WM_STATE` na janela cliente, `_NET_CURRENT_DESKTOP`/`_KIWM_OUTPUT_DESKTOP`
no root). Por isso a classificação é feita um instante depois: o loop
drena a fila inteira, e só então `windows_flush_events()` decide o que
aconteceu, com o lote todo em mãos.

E metade disso é responsabilidade do WM: o kiwm publica o estado **antes**
da geometria que o realiza (ver `kiwm/client.c`). Lê ao contrário, mas é
o que faz a ordem dos eventos dizer o que aconteceu — anunciado depois, o
`_NET_WM_STATE` chega depois do ConfigureNotify que ele explica, e a essa
altura a animação errada já começou (foi exatamente esse o bug do
geometry deslizando ao enrolar). Para WMs que não fazem isso, o kicomp
ainda dá um round-trip antes de classificar, que é o melhor esforço
possível de fora.

É também a heurística que a IPC da seção 32 vai substituir: o WM sabe de
primeira mão o que fez.

## Efeitos

A interface está em `src/effect.h` e é deliberadamente pequena: o core
conhece um efeito rodando (`CompEffect`/`CompEffectOps`: `update`,
`apply`, `finished`, `destroy`) e um módulo que decide quando começar um
(`CompEffectModule`, com um callback de evento). Ele nunca sabe *o que* o
efeito é.

Duas regras que um efeito precisa respeitar:

- **tempo, não frames** — `update()` recebe um instante monotônico e a
  duração vem da unidade global; o mesmo efeito leva o mesmo tempo num
  monitor de 60 Hz e num de 144 Hz;
- **por output** — `apply()` roda uma vez por output sendo pintado, com a
  cena daquele output, então o mesmo efeito pode estar no meio do
  caminho num monitor e terminado no outro.

E uma que ele nunca pode quebrar: **não toca no estado lógico do WM**
(seção 27). Um efeito altera `transform` e `opacity` de nós da cena, e
nada além disso — a janela realmente está onde o WM diz que está; ela só
parece ainda não ter chegado.

Adicionar um efeito = um arquivo em `src/effects/`, sua declaração em
`effect.h` e uma linha na tabela de `effect.c`. Nada mais no compositor
muda (seção 42).

### `fade-in` (seção 24.1)

Janela que aparece sobe do transparente. Chaves próprias: `windows`,
`menus`, `docks` — quais tipos de janela recebem o efeito
(`_NET_WM_WINDOW_TYPE`, lido da janela cliente dentro do frame). Ligado
por padrão.

A opacidade é *multiplicada*, não atribuída: um terminal meio
transparente por `_NET_WM_WINDOW_OPACITY` não vira opaco só porque estava
abrindo.

### `scale-in` (seção 24.2)

Janela que aparece cresce até o tamanho final. O destino é sempre a
geometria real; configurável é de onde ela cresce:

| chave | valores |
|---|---|
| `from` | tamanho inicial como fração do final (0.05–4.0, default 0.8; acima de 1 encolhe até o lugar) |
| `origin` | `window` (centro dela, default), `pointer` (onde está o mouse), `output` (centro do monitor) |
| `windows`, `menus`, `docks` | quais tipos recebem |

O ponto de origem é lido uma vez, quando o efeito começa — um `pointer`
que acompanhasse o mouse arrastaria a animação de lado. Desligado por
padrão: empilhado com o `fade-in` é questão de gosto, então quem quiser
escolhe.

### `fade-out` (seção 24.1) e `scale-out` (seção 24.3)

Os mesmos invertidos, no fechamento. O `scale-out` inverte também o
sentido: começa no tamanho real e vai até `to`, em direção à mesma
`origin` de onde o `scale-in` cresceria. `to` acima de 1 faz a janela
inchar um pouco antes de sumir, em vez de encolher. Os dois se compõem sem saber um
do outro — um escreve `transform`, o outro `opacity` —, que é o "zoom +
fade ao fechar" da seção 24.3.

São os primeiros efeitos que sobrevivem ao próprio assunto: quando eles
começam, o X já desmapeou a janela e o aplicativo pode já ter morrido. O
que continua sendo desenhado é o pixmap que o compositor nomeou enquanto
a janela existia, mantido vivo por `window_retain()` (ver `window.h`) até
a animação acabar. O `release` fica no `destroy` do efeito, então um
efeito cancelado — a janela voltou, o compositor está encerrando — libera
igual a um que terminou.

Por padrão respondem só a `close`. Quem quiser que minimizar também
dissolva põe `events = close,minimize` — e quando o efeito de `minimize`
existir, basta tirar dessa lista.

### `geometry` (seção 24.4)

O primeiro. Uma janela que pula para outro tamanho/lugar — maximizar,
restaurar, meia tela, snap — desliza e escala até lá em vez de
teleportar.

Ele **não** anima arrastos: um move/resize com o mouse chega como um
fluxo de configures, e animar isso deixaria a janela visivelmente atrás
do ponteiro. Sem a IPC da seção 32 (o WM é quem sabe que há um drag em
curso), o próprio fluxo é o sinal — `window.c` mede o intervalo entre
configures e marca a sequência como interativa. É a heurística que a IPC
vai substituir.

Enquanto uma janela está sendo transformada, o recorte de shape sai de
cena: a região está em coordenadas não transformadas e o XFixes não sabe
escalá-la, então cantos arredondados ficam quadrados por ~um sexto de
segundo. O renderer GL, que transforma a máscara junto, é onde isso
deixa de ser uma troca.

### O que falta, e o que cada um precisa

Os efeitos abaixo cabem todos no XRender — nenhum deles precisa de GL —
mas dois pedaços de infraestrutura ainda não existem:

**(a) janela que sobrevive ao próprio fim** — **feito**, junto com o
`fade-out`/`scale-out`: `window_retain()`/`window_release()`, e uma
entrada que vira *zombie* quando o X destrói a janela mas algum efeito
ainda a desenha (o pixmap é nosso até liberarmos).

**(b) recorte de origem no nó da cena.** Um `CompRect` dizendo "desenhe
só esta parte do pixmap", sem escala. É o que o shade precisa para
enrolar sem distorcer.

| efeito | precisa | como |
|---|---|---|
| `minimize`/`restore` | — | escala entre a geometria da janela e `_NET_WM_ICON_GEOMETRY` (a caixinha que a taskbar publica na janela cliente) |
| `shade`/`unshade` | (b) | crop animado da altura, sem escala nem distorção; detectado por `_NET_WM_STATE_SHADED` no cliente |
| `desktop-wall` | (a) | + agrupar janelas por `_NET_WM_DESKTOP` e ler `_KIWM_OUTPUT_DESKTOP` para saber a troca por output; translação da cena inteira, com `docks` opcional (default: acompanham) |

O `desktop-wall` é o único que mexe em mais de uma janela por vez — a
interface já suporta isso (um efeito não é obrigado a ter `window`), mas
ele precisa que as janelas do desktop que está saindo continuem
existindo, que é de novo o item (a).

## Pacing

Cada output tem seu próprio relógio de frames (`src/scheduler.c`),
rodando na taxa de atualização *dele*: um monitor de 144 Hz nunca espera
o de 60 Hz, e o estado da animação vem do relógio monotônico
compartilhado enquanto cada output apenas o amostra no seu ritmo.

Ele faz duas coisas: junta rajadas de damage num frame só, e mantém as
animações no ritmo de cada tela. Um output cujo prazo já passou pinta na
hora, então um evento isolado nunca fica esperando. Ainda **não** há
MSC/UST — o período vem da taxa relatada pelo RandR, não de feedback de
apresentação —, então ele ritma e coalesce, mas ainda não trava no
vblank; quando o presenter souber reportar MSC de verdade, só o
`scheduler_tick()` muda.

## O que já está implementado

| Seção do doc | Estado |
|---|---|
| 17/30/45 — capability detection | Composite/Damage/XFixes/Render/RandR detectados em runtime; nada assume XiS |
| 4/18 — output como unidade de apresentação | um pixmap + picture por output, dimensionado ao output, nunca uma superfície global |
| 39 — dirty por output | só o output que o damage tocou é repintado |
| 26 — janela atravessando outputs | recorte `janela ∩ output` por output, um nó de cena em cada |
| 21 — scene graph | `CompScene`/`CompSceneNode` intermediários; efeitos nunca verão X windows |
| 28 — renderer abstrato | `CompRenderer` vtable, backend `xrender` |
| 15/16 — presenter abstrato | `CompPresenter` vtable, backend `copy` (overlay window) |
| 33 — espelho visual | estado vindo só de eventos X; o WM continua sendo a autoridade |
| 38 — leveza | dorme em `poll()`, sem timers, sem polling, sem repintar por precaução |
| — | shape das janelas aplicada como clip (cantos arredondados, clientes com shape própria) |
| — | alpha real: visual de 32 bits do cliente **e** do frame do kiwm, mais `_NET_WM_WINDOW_OPACITY` |
| 22 — transform | matriz 4x4 no nó da cena; o backend XRender consome a parte afim 2D |
| 23/42 — efeitos como módulos | vtable própria; um efeito novo = um arquivo + uma linha |
| 19 — scheduler por output | relógio de frames por output, na taxa de cada um |
| 20/40 — animação por tempo | progresso vem do relógio monotônico; duração é múltiplo de uma unidade global |
| 24.1/24.2/24.3/24.4 — efeitos | fade in/out, scale in/out (origem configurável) e geometry change |
| — | eventos semânticos (open/close/minimize/maximize/shade/focus/...), configuráveis por efeito |
| — | janela retida além do próprio fim (`window_retain`), que é o que permite animar o fechamento |

## O que **não** está implementado (e onde entra)

- **Mais efeitos** (seções 24.1-24.7) — fade, zoom, wobbly, desktop wall,
  cubo. Os dois primeiros cabem no XRender; wobbly (mesh) e blur pedem o
  renderer GL.
- **MSC/UST** (resto da Fase 7, seções 19/49) — o relógio por output já
  existe, mas o período vem do RandR, não de feedback de apresentação.
- **Presenter FLIP do XiS** (Fase 8) — `CompPresentMode` e
  `CompPresenter::get_msc` já existem para isso; `caps.flip_per_crtc`
  está declarado como `false` de propósito, para que nenhum caminho de
  código possa acreditar nele antes da hora.
- **Repaint por região** — o damage hoje decide *quais outputs* repintar,
  não *que parte* deles. É otimização, não mudança de interface.
- **Unredirect de output único** (janela fullscreen) — a decisão é por
  output e cabe no laço de paint, mas ainda não está lá.
- **IPC `kiwm` ⟷ `kicomp`** (seção 32) — deliberadamente ausente na
  primeira versão. Quando existir, ela substitui apenas a *fonte* das
  atualizações; o espelho em `window.c` continua igual.
- **X-Density por output** (seção 56) — o compositor ainda não escala a
  cena por densidade.

## Shape e as camadas do kiwm

**Shape.** Composto, o servidor não recorta mais nada: `NameWindowPixmap`
entrega o retângulo inteiro da janela, então quem tem de aplicar a shape é
o compositor. O `kicomp` lê a região `BOUNDING` da janela
(`XFixesCreateRegionFromWindow`), guarda em cache e usa como clip do
target a cada janela desenhada, invalidando em `ShapeNotify`/resize. É o
que mantém os cantos arredondados do kiwm redondos e uma janela com shape
própria (VirtualBox e afins) com a silhueta certa.

Isso **não** duplica trabalho com o kiwm: o kiwm *calcula* a shape (cantos
arredondados, propagação da shape do cliente para o frame) e continua
tendo de fazê-lo — é o que funciona sem compositor, e a *input* shape
(cliques) é sempre do servidor, composto ou não. O kicomp só *lê* a região
já pronta, uma vez por mudança, e reusa em todo frame.

**Camadas do kiwm.** O kiwm marca suas duas janelas de overlay com
`_KIWM_LAYER` (`"osd"`, `"outline"` — ver `kiwm/PROTOCOL.md`). Com
`--skip-wm-layers` o kicomp simplesmente não as coloca na cena, para
quando ele mesmo for desenhar essas transições como efeito. O kiwm não
sabe de nada disso e não muda de comportamento: quem decide o que mostrar
é o compositor.

(Não dá para "não redirecionar" só essas janelas: `RedirectSubwindows` no
root vale para todos os filhos, e `UnredirectWindow` só desfaz um redirect
*por janela* feito pelo mesmo cliente. Deixá-las fora da cena é o
equivalente prático.)

## Teste

`tests/argb-window.c` é o cliente de aceitação: uma janela
override-redirect de 32 bits preenchida com uma cor meio transparente.

```sh
cc -o tests/argb-window tests/argb-window.c -lxcb -lxcb-render
./tests/argb-window 300 300 380 260 0.5 0x30a0ff
```

Sem compositor o quadrado é opaco; com `kicomp` ele mistura com o que
está atrás.

Verificado num Xephyr 1024x768 com `kiwm` + `xterm` + `xclock`:

- cena idêntica à não-composta (decoração, stacking, posições);
- `xprop -id <frame> -f _NET_WM_WINDOW_OPACITY 32c -set _NET_WM_WINDOW_OPACITY 2147483647`
  deixa a janela 50% translúcida;
- `tests/argb-window` **decorado pelo kiwm** mistura corretamente com o
  xterm branco atrás dele — é o teste do frame ARGB do lado do WM;
- `--override` mistura igual, sem passar pelo WM;
- cantos arredondados do kiwm e do próprio OSD preservados;
- `xrandr --setmonitor` dividindo a tela em dois monitores: dois targets
  independentes, janela atravessando a fronteira sem emenda;
- `--single-drawable` volta para um único target do tamanho da tela;
- `--skip-wm-layers` faz o OSD do alt-tab e o contorno sumirem da cena
  (continuam existindo e funcionando no kiwm);
- sem compositor, a mesma janela ARGB decorada aparece opaca e intacta —
  nenhuma regressão no caminho não-composto;
- matar o `kicomp` devolve a tela ao servidor sem resíduo.

## Estrutura

```
src/
  comp.h              tipos e estado global (KiComp, CompOutput, CompWindow)
  main.c              caps, seleção _NET_WM_CM_Sn, overlay, event loop, paint
  config.c/.h         kicomp.conf
  output.c/.h         outputs RandR, targets e dirty state por output
  window.c/.h         espelho da pilha de janelas (só eventos X)
  scene.c/.h          montagem da cena por output (recorte janela ∩ output)
  transform.c/.h      matriz 4x4 e a inversa afim que o XRender consome
  animation.c/.h      relógio monotônico, easing, unidade global de duração
  scheduler.c/.h      relógio de frames por output
  effect.c/.h         core de efeitos: efeitos rodando + tabela de módulos
  effects/            um arquivo por efeito: geometry, fade-in, fade-out,
                      scale-in, scale-out
  renderer.h          vtable do renderer
  renderer-xrender.c  backend XRender
  presenter.h         vtable do presenter
  presenter-copy.c    backend COPY (overlay window)
tests/
  argb-window.c       cliente de teste ARGB
```
