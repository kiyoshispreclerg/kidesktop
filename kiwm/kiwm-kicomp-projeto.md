# XiS Window Manager + Compositor
## Projeto arquitetural — `kiwm` + `kicomp`

> Documento de arquitetura e plano de implementação.
> Objetivo: fornecer ao agente de programação uma especificação clara para construir um stacking window manager leve, bonito e extensível para XiS/X11, mantendo compatibilidade com servidores X11 convencionais.

---

## 1. Visão geral

O projeto consiste em dois processos/componentes independentes:

- **`kiwm`** — stacking window manager.
- **`kicomp`** — compositor opcional.

A regra fundamental é:

> **O WM deve ser completamente funcional sem composição. O compositor nunca deve ser necessário para que o desktop seja utilizável.**

Arquitetura:

```text
                 kisession
                    |
          +---------+---------+
          |                   |
        kiwm               xispanel
          |
          +-------------------+
          |                   |
       X11/XiS             kicomp
                              |
                         Composite/Render
                              |
                         X11/XiS server
```

Em uma sessão sem composição:

```text
applications
     |
   kiwm
     |
    X11
     |
   outputs
```

Com composição:

```text
applications
     |
   kiwm
     |
   scene/state
     |
  kicomp
     |
  Composite
     |
  renderer
     |
  per-output presentation
```

---

# 2. Objetivos

## 2.1 Objetivos principais

### `kiwm`

- stacking WM simples e leve;
- código C pequeno e legível;
- XCB como interface principal com X11;
- funcionamento correto em X11 convencional;
- funcionamento correto em XiS;
- decoração própria baseada em PNG/Cairo;
- foco, stacking, move, resize, maximize, minimize;
- múltiplos outputs;
- desktops virtuais independentes por output;
- suporte suficiente a ICCCM/EWMH para aplicações comuns, sem tentar implementar toda a especificação;
- regras/configuração simples;
- comportamento previsível;
- ausência total de dependência do compositor.

### `kicomp`

- compositor opcional;
- arquitetura orientada a output;
- um render target/drawable por output;
- pacing independente por output;
- compatível com taxas de atualização diferentes;
- aproveitar FLIP por CRTC quando disponível no XiS;
- funcionar em X servers que só suportam apresentação tradicional/COPY;
- efeitos implementados como módulos independentes;
- efeitos não devem modificar o estado lógico do WM;
- permitir efeitos por output;
- permitir que uma janela atravesse vários outputs;
- renderização baseada em tempo, não em número de frames.

---

# 3. Não objetivos

Não tentar inicialmente:

- substituir KWin/Mutter em compatibilidade;
- implementar 100% de EWMH;
- implementar todas as extensões exóticas de X11;
- implementar tiling WM;
- implementar Wayland;
- criar um toolkit gráfico;
- colocar OpenGL dentro do `kiwm`;
- fazer do compositor uma dependência obrigatória;
- suportar efeitos complexos antes de o WM básico estar sólido.

Compatibilidade ampla é desejável, mas **simplicidade é prioridade**.

---

# 4. Princípio arquitetural mais importante

## Output é a unidade de apresentação do compositor

Não criar uma única grande surface global para todos os monitores.

O compositor deve possuir uma cena por output:

```text
KiComp
 |
 +-- XisOutput 0
 |     |
 |     +-- scene
 |     +-- drawable
 |     +-- timing
 |     +-- presentation
 |
 +-- XisOutput 1
 |     |
 |     +-- scene
 |     +-- drawable
 |     +-- timing
 |     +-- presentation
 |
 +-- XisOutput 2
       |
       +-- scene
       +-- drawable
       +-- timing
       +-- presentation
```

Isso permite:

```text
Output 0: 60 Hz
Output 1: 144 Hz
Output 2: 75 Hz
```

sem forçar todos os outputs a compartilhar o mesmo pacing.

A animação é definida em função do tempo:

```c
state = effect_update(state, monotonic_time);
```

Cada output amostra esse estado no seu próprio ritmo.

---

# 5. WM: modelo de dados

## 5.1 `KiWM`

Responsável pelo estado global e pelo event loop.

Conceito:

```c
typedef struct KiWM {
    xcb_connection_t *conn;
    xcb_screen_t *screen;

    XisAtoms atoms;

    XisOutput *outputs;
    size_t output_count;

    XisClient *clients;
    size_t client_count;

    XisConfig config;

    bool running;
} KiWM;
```

---

## 5.2 `XisOutput`

Output é uma entidade do WM e também do compositor.

```c
typedef struct XisOutput {
    int id;

    int x;
    int y;
    int width;
    int height;

    int refresh_rate;

    /* density/scale reportado pelo servidor (XiS X-Density) ou
     * calculado a partir de RandR/EDID em X11 convencional */
    double density;        /* ex.: 1.0, 1.5, 2.0 */
    double dpi;             /* opcional, quando o backend fornecer */

    XisWorkspace *workspace;

    /* compositor-owned data must be optional */
    void *comp_data;
} XisOutput;
```

Importante:

- o WM não deve depender do compositor;
- `comp_data` pode ser `NULL`;
- output detection deve funcionar com RandR;
- não assumir que existe XiS;
- não assumir que existe FLIP;
- não assumir que existe X-Density/X-Input-Scale — default `density = 1.0` quando o
  backend não informar nada.

---

# 6. Desktops virtuais por output

Cada output possui seu próprio workspace ativo.

Exemplo:

```text
Output A
    workspace = 3

Output B
    workspace = 1
```

Isso permite:

```text
+----------------------+----------------------+
|      Output A        |      Output B        |
|                      |                      |
|      desktop 3       |      desktop 1       |
|                      |                      |
+----------------------+----------------------+
```

Não implementar inicialmente um modelo de workspace global.

API conceitual:

```c
XisWorkspace *xis_output_workspace(XisOutput *);
void xis_output_set_workspace(XisOutput *, int index);
```

Um client pode pertencer a um workspace e ser visível em um ou mais outputs conforme regras futuras.

---

# 7. Client

```c
typedef struct XisClient {
    xcb_window_t window;

    int x;
    int y;
    int width;
    int height;

    bool mapped;
    bool focused;
    bool maximized;
    bool minimized;
    bool fullscreen;

    XisOutput *output;
    XisWorkspace *workspace;

    xcb_window_t frame;

    XisDecoration *decoration;

    /* compositor state must be separate */
    void *comp_data;
} XisClient;
```

O estado lógico da janela pertence ao WM.

O compositor não deve alterar diretamente:

```text
x/y/width/height
workspace
focus
stacking
mapped
```

Ele pode possuir estado visual separado:

```text
transform
opacity
animation
mesh
texture
```

---

# 8. Janelas atravessando outputs

Uma janela não deve ser conceitualmente "duplicada" no WM.

Ela possui uma geometria global:

```text
Client
  geometry = global
```

O compositor determina em quais outputs a janela é visível.

Conceito:

```c
typedef struct XisClientOutput {
    XisOutput *output;

    XisRect visible_rect;

    XisTransform transform;

    float opacity;
} XisClientOutput;
```

Assim:

```text
Client
 |
 +-- Output 0
 |     visible portion
 |
 +-- Output 1
       visible portion
```

Isso é obrigatório para suportar corretamente:

- janelas atravessando monitores;
- efeitos;
- zoom;
- wobbly;
- rotação;
- desktop wall;
- cubo;
- diferentes refresh rates.

---

# 9. Decoração

O WM deve suportar decoração própria sem compositor.

Usar:

- Cairo;
- PNG;
- XCB/X11 pixmaps ou surfaces apropriadas.

O client continua sendo uma janela X11 normal.

Estrutura:

```text
+------------------------------+
|        decoration            |
|  title             buttons   |
+------------------------------+
|                              |
|                              |
|          client              |
|                              |
|                              |
+------------------------------+
```

Não rasterizar o conteúdo da aplicação através do Cairo.

Cairo é utilizado apenas para a decoração.

---

# 10. Tema

Formato inicial sugerido:

```text
theme/
    frame.png
    title-active.png
    title-inactive.png

    close.png
    maximize.png
    minimize.png

    theme.conf
```

`theme.conf` define geometria:

```ini
[frame]
left=6
right=6
top=32
bottom=6

[title]
x=12
y=8

[button.close]
x=...
y=...
width=16
height=16
```

A implementação deve permitir cache das surfaces Cairo.

Não recarregar PNG em cada repaint.

---

# 11. Event loop do WM

Basear o WM em XCB.

Eventos essenciais:

```text
MapRequest
UnmapNotify
DestroyNotify
ConfigureRequest
ConfigureNotify
PropertyNotify
ClientMessage
ButtonPress
ButtonRelease
MotionNotify
EnterNotify
LeaveNotify
KeyPress
FocusIn
FocusOut
```

Event loop conceitual:

```c
while (wm->running) {
    xcb_generic_event_t *event = xcb_wait_for_event(wm->conn);

    if (!event)
        break;

    kiwm_handle_event(wm, event);

    free(event);
}
```

Não implementar polling periódico.

Evitar timers frequentes quando não há trabalho.

---

# 12. Política de EWMH/ICCCM

Implementar apenas o subconjunto realmente necessário.

Prioridade:

1. ICCCM básico;
2. `_NET_SUPPORTED`;
3. `_NET_ACTIVE_WINDOW`;
4. `_NET_CLIENT_LIST`;
5. `_NET_NUMBER_OF_DESKTOPS`;
6. `_NET_CURRENT_DESKTOP`;
7. `_NET_WM_DESKTOP`;
8. `_NET_WM_STATE`;
9. fullscreen;
10. maximized;
11. minimized;
12. close request;
13. `_NET_WORKAREA`;
14. basic `_NET_DESKTOP_*`.

Não perseguir conformidade completa.

A arquitetura deve permitir adicionar atoms posteriormente sem reescrever o core.

---

# 13. Gerenciamento de stacking

O WM deve manter uma ordem lógica:

```text
bottom
  |
  +-- desktop windows
  +-- normal windows
  +-- always-on-top
  +-- transient/dialog
  +-- override/floating special cases
  |
top
```

A ordem deve ser independente do compositor.

O compositor recebe essa ordem e a transforma em scene order.

---

# 14. Compositor

`kicomp` deve ser um processo separado.

Ele deve:

1. conectar ao X;
2. assumir Composite;
3. descobrir outputs;
4. descobrir janelas;
5. acompanhar mudanças;
6. construir cenas;
7. renderizar cada output;
8. apresentar cada output independentemente.

O WM e o compositor não devem depender de uma API proprietária obrigatória entre si na primeira versão.

Inicialmente, sincronização pode usar:

- propriedades X11;
- X events;
- configuração compartilhada simples;
- ou uma pequena IPC Unix socket posteriormente.

Preferência futura: **IPC explícita e simples**, para não acoplar o compositor ao código do WM.

---

# 15. Backend de apresentação

Criar uma abstração desde o início:

```c
typedef enum {
    XIS_PRESENT_COPY,
    XIS_PRESENT_FLIP
} XisPresentMode;
```

E:

```c
typedef struct XisPresenter {
    bool (*init)(XisOutput *);
    void (*destroy)(XisOutput *);

    bool (*begin_frame)(XisOutput *);
    bool (*present)(XisOutput *, XisPresentMode mode);

    uint64_t (*get_msc)(XisOutput *);
} XisPresenter;
```

O compositor não deve chamar diretamente uma implementação XiS-specific.

---

# 16. Backend X11 convencional

Em servidores X comuns:

```text
scene
  |
render target
  |
COPY/present tradicional
```

Sem FLIP por CRTC, o compositor deve continuar funcional.

Pode usar:

- XRender;
- GLX;
- OpenGL;
- Composite;
- mecanismos disponíveis no servidor.

Não exigir XiS.

---

# 17. Backend XiS

Quando o servidor suportar as extensões necessárias:

```text
scene
  |
drawable/output target
  |
XiS per-CRTC FLIP
  |
CRTC
```

A implementação deve detectar capability em runtime.

Nunca fazer:

```c
if (XiS)
    assume_flip_everywhere();
```

Fazer capability detection:

```c
XisCapabilities caps;

caps.composite = ...;
caps.present = ...;
caps.flip_per_crtc = ...;
caps.gl = ...;
caps.density = ...;       /* X-Density disponível por output */
caps.input_scale = ...;   /* X-Input-Scale disponível por output */
```

---

# 18. Um drawable por output

Esta é uma regra arquitetural forte:

> Cada output possui seu próprio drawable/render target.

Exemplo:

```text
Output 0
    drawable 0
    scene 0
    timing 0

Output 1
    drawable 1
    scene 1
    timing 1
```

Não criar uma única surface:

```text
[ monitor0 | monitor1 | monitor2 ]
```

para apresentação normal.

Uma surface global pode ser utilizada apenas internamente em efeitos que realmente necessitem dela, nunca como requisito do pipeline de apresentação.

---

# 19. Scheduler

Cada output possui seu próprio scheduler.

Conceito:

```c
typedef struct XisFrameClock {
    uint64_t frame;
    uint64_t last_msc;
    uint64_t last_ust;

    double refresh_period;

    bool pending;
} XisFrameClock;
```

Fluxo:

```text
output frame requested
        |
        v
compute animation state
        |
        v
render output
        |
        v
present
        |
        v
wait/receive completion
        |
        v
next output frame
```

Não bloquear o output de 60 Hz esperando o output de 144 Hz.

---

# 20. Renderização baseada em tempo

Efeitos nunca devem fazer:

```c
progress += 0.05;
```

porque isso depende do FPS.

Fazer:

```c
progress = elapsed / duration;
```

Exemplo:

```c
double effect_progress(double now,
                       double start,
                       double duration)
{
    double p = (now - start) / duration;

    if (p < 0.0)
        return 0.0;

    if (p > 1.0)
        return 1.0;

    return p;
}
```

Assim o mesmo efeito funciona em:

```text
60 Hz
75 Hz
120 Hz
144 Hz
240 Hz
```

---

# 21. Scene graph

O compositor deve possuir uma representação intermediária da cena.

Não deixar efeitos manipularem diretamente X windows.

Conceito:

```c
typedef struct XisSceneNode {
    XisClient *client;

    XisRect geometry;

    XisTransform transform;

    float opacity;

    int z;

    uint32_t flags;
} XisSceneNode;
```

Uma cena:

```text
XisScene
 |
 +-- background
 +-- client A
 +-- client B
 +-- client C
 +-- panel
```

---

# 22. Transform

Usar matriz 2D/3D genérica desde cedo, mesmo que o primeiro renderer só precise de 2D.

```c
typedef struct XisTransform {
    float m[4][4];
} XisTransform;
```

Operações:

```c
xis_transform_identity()
xis_transform_translate()
xis_transform_scale()
xis_transform_rotate_x()
xis_transform_rotate_y()
xis_transform_rotate_z()
xis_transform_multiply()
```

Isso prepara o compositor para:

- zoom;
- rotação;
- cubo;
- wobbly;
- desktop wall;
- geometry change.

---

# 23. Efeitos como módulos

O core não deve conter código específico de cada efeito.

Interface sugerida:

```c
typedef struct XisEffect XisEffect;

typedef struct {
    void (*start)(XisEffect *, XisScene *);
    void (*update)(XisEffect *, XisScene *, double time);
    void (*apply)(XisEffect *, XisScene *, XisOutput *);
    bool (*finished)(XisEffect *, double time);
    void (*destroy)(XisEffect *);
} XisEffectOps;

struct XisEffect {
    const XisEffectOps *ops;

    double start_time;
    double duration;

    void *data;
};
```

O core faz:

```c
effect->ops->update(...);
effect->ops->apply(...);
```

e não sabe se é:

```text
fade
zoom
cube
wobbly
blur
```

---

# 24. Efeitos desejados

Implementar progressivamente.

## 24.1 Fade

Entrada:

```text
opacity: 0 -> 1
```

Saída:

```text
opacity: 1 -> 0
```

---

## 24.2 Zoom de abertura

Origem:

```text
pointer position
```

Transform:

```text
T(pointer)
S(scale)
T(-pointer)
```

Scale:

```text
0 -> 1
```

---

## 24.3 Zoom + fade ao fechar

```text
scale   1 -> 0
opacity 1 -> 0
```

---

## 24.4 Geometry Change

Interpolar:

```text
x
y
width
height
```

entre geometria antiga e nova.

---

## 24.5 Wobbly Windows

Não modificar a geometria lógica.

Criar mesh visual:

```text
logical rectangle
       |
       v
subdivided mesh
       |
       v
spring/damper simulation
       |
       v
GPU rendering
```

Idealmente o efeito deve operar por output após clipping da janela.

---

## 24.6 Desktop Wall

Representar workspaces como surfaces/scene groups:

```text
workspace 0
workspace 1
workspace 2
workspace 3
```

Transformar os grupos horizontalmente.

---

## 24.7 Cube

Usar cada workspace como uma face.

```text
             +-------+
            /       /|
           /   1   / |
          +-------+  |
          |       |  |
          |   0   | /
          |       |/
          +-------+
```

As faces devem ser renderizadas por output.

Não alterar o workspace lógico do WM até a transição terminar.

---

# 25. Efeitos por output

Um efeito pode estar ativo em apenas um output:

```text
Output 0:
    cube effect

Output 1:
    normal scene
```

Ou a mesma transição pode estar ativa em outputs diferentes com estados independentes:

```text
Output 0:
    workspace 2 -> 3

Output 1:
    workspace 0 -> 0
```

A API de efeitos deve receber `XisOutput *`.

---

# 26. Janela atravessando outputs durante efeitos

Nunca assumir:

```c
client->output
```

como única fonte de renderização.

O WM pode possuir um output principal para decisões de gerenciamento, mas o compositor deve calcular:

```text
client ∩ output
```

para cada output.

Exemplo:

```text
          Client
    +------------------+
    |                  |
    |                  |
----+------------------+----
    |                  |
    |                  |
    +------------------+

 Output A | Output B
```

O compositor renderiza:

```text
Output A:
    clipped client portion

Output B:
    clipped client portion
```

Cada output pode aplicar seu próprio transform/animation state.

---

# 27. Estado lógico vs estado visual

Esta separação é obrigatória.

Exemplo:

```text
WM state:

window:
    x=100
    y=100
    width=800
    height=600
```

Durante abertura:

```text
visual state:

scale=0.4
opacity=0.6
```

Durante geometry change:

```text
logical:
    target = 1200x800

visual:
    current = interpolate(800x600, 1200x800)
```

O efeito nunca deve "enganar" o WM sobre a geometria real.

---

# 28. Renderer

Abstrair o renderer:

```c
typedef struct XisRenderer {
    bool (*init)(XisOutput *);
    void (*destroy)(XisOutput *);

    void (*begin)(XisOutput *);
    void (*draw_scene)(XisOutput *, XisScene *);
    void (*end)(XisOutput *);
} XisRenderer;
```

Backends possíveis:

```text
renderer-xrender
renderer-opengl
```

Primeiro objetivo:

- fazer um renderer simples;
- depois OpenGL.

Não misturar lógica de efeitos com OpenGL diretamente.

---

# 29. OpenGL

OpenGL é necessário principalmente para:

- transforms 3D;
- cubo;
- wobbly;
- blur;
- efeitos avançados.

O compositor deve continuar tendo uma separação clara:

```text
Effect
   |
Scene abstraction
   |
Renderer abstraction
   |
OpenGL / XRender
```

Um efeito não deve fazer chamadas OpenGL espalhadas pelo core.

---

# 30. Compatibilidade sem XiS

O compositor deve detectar:

```text
Composite
Render
GLX
Present
XiS
```

e escolher o melhor caminho disponível.

Exemplo:

```text
XiS + per-CRTC FLIP
    -> FLIP backend

XiS sem FLIP
    -> normal presentation

Xorg convencional + Composite
    -> COPY/normal presentation

sem Composite
    -> kiwm continua funcionando sem kicomp
```

---

# 31. Falha do compositor

Se `kicomp` morrer:

```text
applications
     |
    X11
     |
   kiwm
```

deve continuar possível voltar ao modo não-composited.

O WM não deve depender do compositor para:

- focus;
- mapping;
- movement;
- resizing;
- stacking;
- decorations.

---

# 32. IPC futura

Uma pequena IPC Unix socket pode permitir:

```text
kiwm <-> kicomp
```

Mensagens conceituais:

```text
OUTPUT_ADD
OUTPUT_REMOVE
OUTPUT_CONFIGURE

CLIENT_ADD
CLIENT_REMOVE
CLIENT_GEOMETRY
CLIENT_MAP
CLIENT_UNMAP
CLIENT_STACK

WORKSPACE_CHANGE

FRAME_REQUEST
```

Não criar uma API gigantesca.

O objetivo é transportar estado, não transformar o compositor em um segundo WM.

---

# 33. Comunicação de estado

O compositor precisa saber:

```text
client created
client destroyed
client mapped
client unmapped
geometry changed
stack changed
workspace changed
output changed
```

O WM é a autoridade.

O compositor mantém um espelho visual.

---

# 34. Estrutura de diretórios

Proposta:

```text
kiwm/
├── meson.build
├── README.md
├── LICENSE
├── src/
│   ├── main.c
│   ├── wm.c
│   ├── wm.h
│   ├── client.c
│   ├── client.h
│   ├── output.c
│   ├── output.h
│   ├── workspace.c
│   ├── workspace.h
│   ├── events.c
│   ├── events.h
│   ├── atoms.c
│   ├── atoms.h
│   ├── ewmh.c
│   ├── ewmh.h
│   ├── decoration.c
│   ├── decoration.h
│   ├── theme.c
│   ├── theme.h
│   ├── config.c
│   └── config.h
│
└── data/
    └── themes/
```

Compositor:

```text
kicomp/
├── meson.build
├── README.md
├── LICENSE
├── src/
│   ├── main.c
│   ├── comp.c
│   ├── comp.h
│   ├── output.c
│   ├── output.h
│   ├── scene.c
│   ├── scene.h
│   ├── node.c
│   ├── node.h
│   ├── effect.c
│   ├── effect.h
│   ├── animation.c
│   ├── animation.h
│   ├── transform.c
│   ├── transform.h
│   ├── scheduler.c
│   ├── scheduler.h
│   ├── presenter.c
│   ├── presenter.h
│   ├── presenter-copy.c
│   ├── presenter-flip.c
│   ├── renderer.c
│   ├── renderer.h
│   ├── renderer-xrender.c
│   ├── renderer-gl.c
│   ├── ipc.c
│   └── ipc.h
│
└── effects/
    ├── fade.c
    ├── zoom.c
    ├── geometry.c
    ├── wobbly.c
    ├── desktop-wall.c
    └── cube.c
```

---

# 35. Dependências

## `kiwm`

Preferencialmente:

- XCB;
- XCB-RandR;
- XCB-ICCCM;
- XCB-EWMH, se útil;
- Cairo;
- libpng ou Cairo PNG support.

Evitar frameworks pesados.

---

## `kicomp`

Base:

- XCB;
- Composite;
- Render;
- RandR;
- Present, quando disponível.

Opcional:

- GLX/OpenGL.

Não tornar GTK/Qt dependências.

---

# 36. Build

Preferir Meson.

Exemplo conceitual:

```text
meson setup build
meson compile -C build
```

Targets:

```text
kiwm
kicomp
```

O usuário deve poder instalar somente:

```text
kiwm
```

sem instalar o compositor.

---

# 37. Fases de implementação

## Fase 1 — WM mínimo

Implementar:

- conexão XCB;
- root ownership;
- MapRequest;
- ConfigureRequest;
- DestroyNotify;
- focus;
- stacking;
- move;
- resize;
- close;
- basic RandR outputs.

Sem compositor.

---

## Fase 2 — Decoração

Implementar:

- frame windows;
- titlebar;
- PNG;
- Cairo;
- botões;
- focus visual;
- temas;
- cache de surfaces.

---

## Fase 3 — Workspaces por output

Implementar:

- criação de workspaces;
- workspace ativo por output;
- mover client entre workspaces;
- atalhos;
- mostrar/esconder clients.

---

## Fase 4 — EWMH mínimo

Adicionar somente o necessário para aplicações comuns.

---

## Fase 5 — Compositor básico

Implementar:

- Composite;
- tracking de clients;
- scene graph;
- um render target por output;
- renderer básico;
- COPY/normal presentation.

Sem efeitos inicialmente.

---

## Fase 6 — OpenGL renderer

Implementar:

- GLX;
- texture import;
- transforms;
- alpha;
- clipping;
- render por output.

---

## Fase 7 — scheduler/pacing

Implementar:

- clock por output;
- MSC/UST quando disponível;
- apresentação independente;
- capability detection.

---

## Fase 8 — XiS FLIP

Adicionar backend específico:

```text
XisFlipPresenter
```

Somente ativado quando a capability existir.

Nunca tornar XiS obrigatório.

---

## Fase 9 — efeitos simples

Implementar:

1. fade;
2. zoom open;
3. zoom close;
4. geometry change.

Esses efeitos validam a arquitetura.

---

## Fase 10 — efeitos avançados

Depois:

1. wobbly;
2. desktop wall;
3. cube;
4. blur;
5. outros.

---

# 38. Critérios de qualidade

## Leveza

`kiwm` não deve:

- possuir loop de renderização;
- fazer polling;
- manter compositor interno;
- executar timers constantes;
- redesenhar decoração sem necessidade.

Em idle, deve dormir esperando X events.

---

## Compositor

Quando nenhuma animação estiver ativa, o compositor deve:

- não renderizar continuamente;
- dormir aguardando eventos/frame requests;
- acordar somente quando necessário.

Durante animações:

```text
animation active
    -> request frames
```

Ao terminar:

```text
animation inactive
    -> stop rendering
```

---

# 39. Regra para invalidação

Nunca renderizar por precaução.

Usar dirty state:

```c
typedef struct {
    bool scene_dirty;
    bool geometry_dirty;
    bool decoration_dirty;
    bool effect_dirty;
} XisDirtyState;
```

Cada output pode possuir dirty state próprio.

Exemplo:

```text
Output 0:
    dirty = true

Output 1:
    dirty = false
```

Renderizar somente Output 0.

---

# 40. Animações

Não criar uma thread por animação.

Um scheduler central do compositor pode manter:

```c
XisAnimation *animations;
```

e atualizar todas as animações no frame daquele output.

Uma animação deve ser algo como:

```c
typedef struct {
    double start;
    double duration;

    float (*easing)(float progress);

    void (*apply)(void *state, float progress);

    bool finished;
} XisAnimation;
```

Easing inicial:

- linear;
- ease-in;
- ease-out;
- ease-in-out.

---

# 41. Threading

Primeira implementação deve ser predominantemente single-threaded.

`kiwm`:

```text
single thread
    |
    +-- X events
    +-- WM state
```

`kicomp`:

```text
single main/render thread
    |
    +-- X events
    +-- scene
    +-- scheduler
    +-- renderer
```

Não introduzir threads apenas para parecer mais rápido.

Threads podem ser adicionadas posteriormente se profiling justificar.

---

# 42. Regra de ouro para efeitos

Um novo efeito deve exigir, idealmente:

```text
1 arquivo .c
```

e nenhuma alteração no core.

Exemplo:

```text
effects/my-effect.c
```

registrado:

```c
xis_effect_register(&my_effect);
```

O core só conhece:

```c
XisEffect
XisEffectOps
```

Isso é um requisito arquitetural importante.

---

# 43. API interna recomendada

Manter APIs pequenas.

### WM

```c
kiwm_manage_window()
kiwm_unmanage_window()
kiwm_focus()
kiwm_move()
kiwm_resize()
kiwm_map()
kiwm_unmap()
kiwm_restack()
```

### Output

```c
xis_output_update()
xis_output_workspace()
xis_output_set_workspace()
xis_output_contains()
xis_output_intersection()
```

### Scene

```c
xis_scene_build()
xis_scene_add()
xis_scene_remove()
xis_scene_update()
xis_scene_render()
```

### Effect

```c
xis_effect_start()
xis_effect_update()
xis_effect_apply()
xis_effect_stop()
```

### Transform

```c
xis_transform_identity()
xis_transform_translate()
xis_transform_scale()
xis_transform_rotate()
xis_transform_multiply()
```

---

# 44. Segurança contra complexidade

Evitar criar abstrações genéricas demais.

O projeto deve permanecer pequeno.

Preferir:

```c
struct XisClient
struct XisOutput
struct XisWorkspace
struct XisScene
struct XisEffect
```

a uma hierarquia extensa de objetos.

Evitar C++ apenas por organização.

C é suficiente.

---

# 45. Compatibilidade com servidores X tradicionais

O código deve ser compilável e utilizável em Xorg convencional.

Não assumir:

- XiS;
- FLIP;
- DRM leasing;
- VRR;
- per-CRTC presentation;
- OpenGL.

As capabilities devem ser detectadas.

XiS deve ser uma otimização/backend, não uma dependência arquitetural.

---

# 46. VRR / refresh futuro

A arquitetura deve deixar espaço para:

```text
Output timing
    |
    +-- fixed refresh
    +-- variable refresh
    +-- MSC
    +-- UST
```

Não codificar `refresh_rate` como constante para toda a sessão.

Cada output tem seu próprio clock.

---

# 47. Performance

Prioridades:

1. não renderizar quando idle;
2. não copiar imagens desnecessariamente;
3. cachear PNG/Cairo surfaces;
4. cachear texturas;
5. manter scene nodes simples;
6. renderizar somente outputs afetados;
7. evitar reconstrução completa da scene quando apenas uma janela mudou;
8. usar FLIP quando XiS permitir;
9. evitar sincronização global entre outputs.

---

# 48. Testes essenciais

## WM

Testar:

- abrir/fechar;
- mover;
- resize;
- maximize;
- fullscreen;
- dialog/transient;
- override-redirect;
- focus;
- stacking;
- workspace;
- monitor hotplug.

## Compositor

Testar:

- compositor desligado;
- compositor ligado;
- output único;
- dois outputs;
- refresh diferente;
- janela atravessando outputs;
- output hotplug;
- janela fullscreen;
- animação;
- compositor reiniciado;
- servidor X sem XiS;
- XiS sem FLIP.

---

# 49. Teste crítico de pacing

Setup:

```text
Output 0 = 60 Hz
Output 1 = 144 Hz
```

Uma janela deve atravessar ambos.

Executar animação de 2 segundos.

Esperado:

```text
Output 0:
~120 frames

Output 1:
~288 frames
```

sem obrigar o output de 144 Hz a esperar o de 60 Hz.

O estado visual deve ser baseado no mesmo relógio monotônico, mas cada output deve apresentar no seu próprio ritmo.

---

# 50. Teste crítico de FLIP

No XiS:

```text
Output 0 -> FLIP
Output 1 -> FLIP
```

com pacing independente.

Em Xorg convencional:

```text
Output 0 -> COPY
Output 1 -> COPY
```

A API de compositor deve ser a mesma.

Somente o backend de presentation muda.

---

# 51. Teste crítico de fallback

Executar:

```text
kisession
```

sem `kicomp`.

Resultado esperado:

```text
kiwm
xisback
xispanel
applications
```

funcionando normalmente.

Depois:

```text
kicomp &
```

Resultado:

```text
kiwm
kicomp
xisback
xispanel
applications
```

com composição e efeitos.

---

# 52. Design final desejado

A arquitetura final deve ser:

```text
                           XiDesktop
                              |
                 +------------+------------+
                 |                         |
               kiwm                    kicomp
                 |                         |
          logical state               scene state
                 |                         |
        +--------+--------+        +-------+-------+
        |        |        |        |       |       |
      client  workspace output   output  output  output
                                      |       |       |
                                    scene   scene   scene
                                      |       |       |
                                  renderer renderer renderer
                                      |       |       |
                                    FLIP/COPY per output
                                      |       |       |
                                    CRTC    CRTC    CRTC
```

---

# 53. Filosofia do projeto

`kiwm` deve ser:

> **leve como um WM minimalista, mas confortável como um desktop completo.**

`kicomp` deve ser:

> **um compositor moderno orientado a outputs, sem transformar o WM em um monstro.**

O projeto deve privilegiar:

- simplicidade;
- baixo consumo;
- ausência de polling;
- separação clara de responsabilidades;
- renderização por output;
- pacing independente;
- capability detection;
- efeitos desacoplados;
- compatibilidade X11;
- suporte otimizado a XiS.

---

# 54. Primeira implementação recomendada

Não começar pelo compositor.

Implementar nesta ordem:

```text
1. KiWM
2. clients
3. frames
4. Cairo/PNG decorations
5. outputs/RandR
6. workspaces por output
7. basic EWMH
8. IPC/state model
9. kicomp skeleton
10. scene graph
11. renderer
12. per-output scheduling
13. COPY presenter
14. XiS FLIP presenter
15. fade
16. zoom
17. geometry change
18. wobbly
19. desktop wall
20. cube
```

O primeiro marco de sucesso deve ser:

```text
kiwm
```

funcionando perfeitamente sozinho.

O segundo:

```text
kiwm + kicomp
```

sem efeitos.

O terceiro:

```text
kiwm + kicomp
       |
       +-- 60 Hz output
       +-- 144 Hz output
```

com pacing independente.

Somente depois implementar efeitos complexos.

---

# 55. Decisão arquitetural resumida

### Obrigatório

- XCB;
- stacking;
- Cairo/PNG decorations;
- outputs;
- workspace por output;
- state lógico separado do state visual;
- compositor opcional;
- scene por output;
- renderer abstrato;
- presenter abstrato;
- scheduler por output;
- efeitos como módulos.

### XiS-specific

- capability detection;
- per-CRTC FLIP;
- MSC/UST quando disponível;
- backend específico de presentation.

### Nunca obrigatório

- XiS;
- FLIP;
- OpenGL;
- compositor;
- EWMH completo.

---

# 56. Compatibilidade com X-Density e X-Input-Scale (XiS)

XiS pode expor, por output, densidade lógica (`X-Density`) e um fator de escala aplicado à
entrada (`X-Input-Scale`). Isso é distinto de RandR tradicional: permite mistura de monitores
com DPI diferentes numa mesma sessão sem depender de um único `Xft.dpi` global.

Regras:

- `kiwm` lê `density` por output na criação/hotplug e guarda em `XisOutput::density`;
- decoração (frame, título, botões) deve ser desenhada considerando a densidade do output
  onde a janela está posicionada — não existe uma densidade única de sessão;
- se a janela atravessa dois outputs com densidades diferentes, a decoração usa a densidade
  do output onde está o maior percentual visível (ou o output "principal" do client);
- `kicomp` deve escalar a cena por output usando o mesmo fator, não um fator global aplicado
  uma vez à saída final;
- `X-Input-Scale` afeta o mapeamento de coordenadas de ponteiro entre espaço físico e lógico;
  o WM não deve fazer sua própria conversão — deve consumir as coordenadas já mapeadas pelo
  servidor quando a extensão existir, e só assumir 1:1 quando não existir;
- em X11 convencional (sem XiS), `density` vem de um cálculo best-effort via RandR/EDID
  (`(width_px * 25.4) / width_mm`) com fallback para `1.0` se a informação não existir;
- nunca aplicar a densidade uma segunda vez — se o servidor já escalou a entrada
  (`X-Input-Scale`), o WM não deve reescalar por conta própria.

Capability detection (seção 12) deve reportar se `X-Density`/`X-Input-Scale` existem; ausência
de qualquer uma delas apenas remove a otimização, nunca quebra o funcionamento básico.

Teste crítico: dois outputs, um com `density=1.0` outro com `density=2.0`. Mover uma janela
lentamente do primeiro para o segundo deve manter a decoração legível em ambos, sem borrão
ou tamanho incorreto, e o ponteiro deve permanecer sob o cursor visual do usuário durante a
travessia.

---

# 57. Resultado esperado

O usuário deve poder iniciar:

```sh
kiwm
```

e ter um desktop pequeno, rápido e bonito com:

- decoração PNG;
- Cairo;
- múltiplos monitores;
- desktops independentes por output;
- gerenciamento completo de janelas básico;
- baixo uso de CPU em idle.

E opcionalmente:

```sh
kicomp &
```

para obter:

- transparência;
- fade;
- zoom;
- geometry animation;
- wobbly windows;
- desktop wall;
- cube;
- blur futuro;
- pacing independente por output;
- FLIP por CRTC no XiS.

O compositor deve ser uma **camada de apresentação**, não uma segunda implementação do window manager.

---

## 58. Regra final para o agente de implementação

Ao tomar decisões não especificadas neste documento:

1. escolher a solução mais simples;
2. manter `kiwm` independente de composição;
3. manter `kicomp` independente do WM tanto quanto possível;
4. tratar output como unidade de apresentação;
5. nunca introduzir sincronização global entre outputs sem necessidade;
6. nunca colocar lógica de efeito no core;
7. nunca assumir XiS;
8. nunca assumir FLIP;
9. nunca usar frame count como relógio de animação;
10. nunca assumir X-Density/X-Input-Scale — sempre ter fallback `density=1.0`;
11. preferir código C pequeno, explícito e fácil de depurar.

**A arquitetura deve permitir que um novo efeito seja adicionado sem modificar o event loop do WM, o gerenciamento de clients ou o scheduler central.**
