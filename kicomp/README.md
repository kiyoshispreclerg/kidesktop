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
./kicomp -v          # --replace para substituir outro compositor
```

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

## Limitação conhecida: transparência de janelas decoradas

`kiwm` cria o frame com a profundidade/visual do root (`client.c`,
`xcb_create_window(..., wm.screen->root_depth, ..., wm.screen->root_visual, ...)`).
Um cliente ARGB reparentado para dentro de um frame de 24 bits perde o
alpha ao desenhar no pixmap do frame — o `kicomp` recebe o frame já
achatado e não tem como recuperar o canal.

Ou seja, hoje a transparência real aparece em:

- janelas override-redirect (menus, tooltips, popups do `xispanel`);
- janelas não gerenciadas/não reparentadas;
- qualquer janela via `_NET_WM_WINDOW_OPACITY` (que é aplicado pelo
  compositor sobre o frame inteiro, e portanto funciona sempre).

Para aplicações ARGB decoradas funcionarem, `kiwm` precisa criar o frame
com o visual/depth do cliente quando ele for de 32 bits (colormap próprio
+ `CWBorderPixel`), e `decoration.c` precisa pintar no visual do frame em
vez de `wm.visual`. É uma mudança do lado do WM, fora do escopo deste
protótipo.

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
- `tests/argb-window` mistura corretamente sobre o fundo e sobre a janela
  translúcida abaixo dele;
- `xrandr --setmonitor` dividindo a tela em dois monitores: dois targets
  independentes, janela atravessando a fronteira sem emenda;
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
