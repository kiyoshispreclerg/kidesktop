# kicomp

Compositor opcional do `kiwm`, seguindo `kiwm/kiwm-kicomp-projeto.md`.

Este é o **primeiro protótipo (Fase 5)**: ele desenha exatamente a mesma
cena que você veria sem composição — mesma decoração, mesmo stacking,
mesma posição — com uma única diferença visível, que é o critério de
aceitação deste marco:

- janelas de profundidade 32 (ARGB) têm o canal alpha realmente aplicado;
- `_NET_WM_WINDOW_OPACITY` é respeitado.

Nada além disso. Sem efeitos, sem animação, sem OpenGL.

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
| `-v`, `--verbose` | log detalhado (eventos, janelas, camadas) |

Ele sempre imprime no terminal, sem `-v`, o essencial: backend de render e
de apresentação, capabilities detectadas, e quantos drawables existem e
por quê — atualizado a cada mudança de output:

```
kicomp: kicomp 0.1.0 on :0 screen 0 (3840x1080)
kicomp: renderer=xrender presenter=copy
kicomp: capabilities: composite=1 overlay=1 damage=1 xfixes=1 render=1 randr=1 present=0 flip-per-crtc=0
kicomp: 2 drawables (one per output)
kicomp:   [0] DP-1         1920x1080+0+0 @ 143.98 Hz
kicomp:   [1] HDMI-1       1920x1080+1920+0 @ 60.00 Hz
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

## O que **não** está implementado (e onde entra)

- **Efeitos** (`effects/`, seções 23/24) — a vtable de renderer já recebe a
  cena por output, então um efeito entra sem tocar no core.
- **Frame clock / pacing por output** (Fase 7, seções 19/49) — hoje o
  repaint acontece logo depois de drenar os eventos; cada output já é
  independente, mas ainda não tem relógio próprio nem MSC/UST.
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
  output.c/.h         outputs RandR, targets e dirty state por output
  window.c/.h         espelho da pilha de janelas (só eventos X)
  scene.c/.h          montagem da cena por output (recorte janela ∩ output)
  renderer.h          vtable do renderer
  renderer-xrender.c  backend XRender
  presenter.h         vtable do presenter
  presenter-copy.c    backend COPY (overlay window)
tests/
  argb-window.c       cliente de teste ARGB
```
