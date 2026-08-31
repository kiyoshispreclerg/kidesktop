# kicomp

Compositor opcional do `kiwm`, seguindo `kiwm/kiwm-kicomp-projeto.md`.

Começou como o protótipo da Fase 5 — a mesma cena que você veria sem
composição, com transparência real (alpha de janelas de 32 bits e
`_NET_WM_WINDOW_OPACITY`) como única diferença. Hoje já tem também:

- shape aplicada na composição (cantos arredondados, clientes com shape);
- interface de efeitos com o primeiro deles, *geometry change*;
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

# ---- um efeito ----
[effect:geometry]
enabled  = 1
duration = 1.0   # múltiplo de animation_duration, não milissegundos
```

**A unidade de animação.** Nenhum efeito tem tempo próprio em
milissegundos: cada um pede um *múltiplo* de `animation_duration` — `0.5`
para algo que deve parecer instantâneo, `1.0` para uma transição comum,
`2.0` para uma transição grande. Assim uma única chave acelera ou
desacelera o desktop inteiro de forma coerente, em vez de deixar um
punhado de animações reguladas independentemente. `animation_duration=0`
mantém os efeitos ligados mas termina todos imediatamente.

Cada `[effect:<nome>]` aceita as mesmas duas chaves — `enabled` e
`duration` — e serão sempre essas duas, para qualquer efeito futuro.

## Efeitos

A interface está em `src/effect.h` e é deliberadamente pequena: o core
conhece um efeito rodando (`CompEffect`/`CompEffectOps`: `update`,
`apply`, `finished`, `destroy`) e um módulo que decide quando começar um
(`CompEffectModule`, com callbacks de configure/map/unmap). Ele nunca
sabe *o que* o efeito é.

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
| 24.4 — geometry change | primeiro efeito |

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
  effects/geometry.c  geometry change
  renderer.h          vtable do renderer
  renderer-xrender.c  backend XRender
  presenter.h         vtable do presenter
  presenter-copy.c    backend COPY (overlay window)
tests/
  argb-window.c       cliente de teste ARGB
```
