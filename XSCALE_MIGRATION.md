# X-INPUT-SCALE → X-SCALE: o que muda nos programas do xnsguard

O servidor XiS trocou a extensão experimental **X-INPUT-SCALE** pela
**X-SCALE**. No servidor, isso está na branch `x-scale`; a documentação fica
em `doc/x-scale.md` daquele repositório. Este documento lista os programas
daqui que falavam com a extensão antiga e diz o que cada um precisa, se
precisar de algo.

## O que mudou no modelo

A X-INPUT-SCALE mudava o modelo de três formas:

- a CRTC escalada **não** tinha transform: o compositor desenhava um
  desktop lógico menor no canto da área física e ampliava esse desenho;
- o ponteiro ficava confinado a essa caixa lógica;
- o kicomp publicava as caixas em `_XIS_CONFINED_AREA`, e cada programa
  encolhia as próprias saídas a partir delas.

Isso deixava uma faixa morta entre a tela escalada e a vizinha, e por isso
foi abandonado.

Na **X-SCALE**, a saída é escalada do jeito normal do RandR, com um
**transform de CRTC**: `xrandr --scale-from`, ou o kicomp aplicando a
propriedade `DPI` (`apply_scale`). A consequência para os programas daqui é
direta:

- **a geometria do RandR já é a lógica.** `XRRGetCrtcInfo` e
  `XRRGetMonitors` devolvem a área ocupada no root, já transformada;
- `_NET_WORKAREA` e o root também já estão em coordenadas lógicas;
- ninguém precisa mais corrigir geometria a partir de uma extensão;
- `_XIS_CONFINED_AREA` não existe mais, e o kicomp apaga um resto dela ao
  iniciar.

A extensão nova só existe para o **compositor** entregar a imagem nítida
(física) de uma saída escalada ao servidor. Nenhum outro programa daqui
precisa falar com ela.

**Comportamento no servidor novo:** a extensão `X-INPUT-SCALE` simplesmente
não existe. Os programas que a procuram por nome (`QueryExtension`) caem no
caminho "extensão ausente". Esse caminho já existia nos stubs e nos testes
de presença, e agora ele é o comportamento correto.

## Resumo

| Programa | Funciona sem mudar? | O que fazer |
|---|---|---|
| `kicomp` | já migrado | nada (branch `x-scale`) |
| `kiwm` | sim | remover código morto |
| `xispanel` | sim | remover código morto |
| `xisback` | sim | remover código morto |
| `kiconf` | **não totalmente** | corrigir a prévia de DPI da aba Telas |
| `kishot` | **não** | geometria e captura da tela escalada |
| `shared/xis_inputscale.*` | — | remover depois do `kishot` |

## kicomp — já migrado

Branch `x-scale` deste repositório:

- **`1dd0861`:** escala por transform, saída "com fonte" (*sourced*)
  entregue ao servidor, `apply_scale` aplicando `DPI` → transform.
- **`383123e`:** fala X-SCALE, com o módulo renomeado de
  `inputscale.c/.h` para `xscale.c/.h`.

Num servidor sem X-SCALE, o kicomp desenha as saídas escaladas em tamanho
lógico e o servidor amplia. O resultado fica borrado, mas correto.

## kiwm — funciona, só tem código morto

- `kiwm/output.c` (`apply_confined_areas()`) lê `_XIS_CONFINED_AREA` e
  encolhe cada saída para a caixa confinada.
- `kiwm/atoms.c` interna o átomo.
- `kiwm/events.c` relê a propriedade quando ela muda.
- `kiwm/PROTOCOL.md` documenta a propriedade em "`_XIS_CONFINED_AREA`:
  kiwm as a consumer".

A propriedade nunca mais é publicada, então `apply_confined_areas()` não
faz nada. A geometria que o kiwm lê do RandR já é a caixa lógica, e
maximizar, encaixar janelas e `_NET_WORKAREA` saem certos.

**A fazer (opcional, limpeza):**

- remover `apply_confined_areas()`, o átomo e a releitura em `events.c`;
- trocar a seção do `PROTOCOL.md` por uma nota de que a escala por saída
  agora vem do transform do RandR.

## xispanel — funciona, só tem código morto

`xispanel/inputscale.c` (com `inputscale_stub.c` e o bloco
`INPUTSCALE_SRCS` do `Makefile`) faz três coisas:

- pergunta `XISGetCrtcConfine` em `resolve_output_geometry()`
  (`xispanel.c`), para dimensionar o painel pela caixa confinada;
- abre uma segunda conexão xcb só para receber `XISConfineNotify`;
- põe o fd dessa conexão no `select()` do loop principal.

No servidor novo, a extensão está ausente. `inputscale_get_confine()` não
altera nada, e o painel usa a largura e a altura de `XRRGetCrtcInfo`, que
agora já são as lógicas. Quando a escala muda, a mudança chega como um
evento normal de RandR (o transform muda a CRTC), que o painel já trata.

**A fazer (opcional, limpeza):**

- apagar `inputscale.c` e `inputscale_stub.c`;
- remover as chamadas `inputscale_init()`, `inputscale_get_confine()`,
  `inputscale_fd()` e `inputscale_poll_change()` de `xispanel.c`;
- remover o bloco do `Makefile`.

Não há nada a trocar por X-SCALE: o painel não precisa da extensão.

## xisback — funciona, só tem código morto

`xisback/xisback_inputscale.c` (com `xisback_inputscale_stub.c` e
`INPUTSCALE_SRC` no `Makefile`) é uma cópia adaptada do `inputscale.c` do
xispanel. O `xisback.c` chama `xis_get_confine()` para dimensionar o papel
de parede de cada saída.

A situação é a mesma do xispanel: a extensão está ausente, a geometria do
RandR já é a lógica, e o papel de parede sai certo.

**A fazer (opcional, limpeza):**

- apagar os dois arquivos;
- remover `xis_init()`, `xis_get_confine()`, `xis_fd()` e
  `xis_poll_change()` de `xisback.c`;
- remover o bloco do `Makefile`.

## kiconf — a prévia de DPI da aba Telas precisa mudar

Em `kiconf/tabs/screens.c`, dois trechos dependem do modelo antigo:

- **`screens_density_available()`** só considera a prévia "com densidade"
  disponível se `XQueryExtension(dpy, "X-INPUT-SCALE", ...)` responder e
  houver um dono de `_X_DENSITY_MANAGER_S<n>`. No servidor novo o primeiro
  teste sempre falha, então a prévia com DPI nunca liga.
- **`output_visual_size()`** calcula o tamanho da saída na prévia como
  `modo × --scale ÷ (DPI/96)`. O `÷ DPI/96` representava a caixa confinada
  do modelo antigo. Agora o kicomp com `apply_scale` transforma o DPI
  num transform, e o fator `--scale` já representa isso. Se o teste de
  presença simplesmente trocasse para `X-SCALE`, a prévia **dividiria duas
  vezes** e mostraria a saída menor do que ela é.

**A fazer:**

1. Em `output_visual_size()`, usar só `modo × scale` do transform, que é
   exatamente a área que a saída ocupa no root. Tirar a divisão por DPI.
2. Repensar `screens_density_available()`. A prévia não depende mais de
   extensão nenhuma, porque o transform já está no RandR. O que ainda faz
   sentido saber é se a imagem vai sair **nítida**. Isso exige um dono de
   `_X_DENSITY_MANAGER_S<n>` (os clientes redesenham denso) e, se quiser
   mostrar isso, que a CRTC aceite fonte (X-SCALE `GetCrtcScanout`, flag
   `XScaleScanoutCapable`). Para a geometria, nada disso importa.
3. Na hora de aplicar, o kiconf pode continuar mudando só a propriedade
   `DPI` (`xrandr --set DPI`). O kicomp com `apply_scale = 1` transforma em
   transform e reposiciona as vizinhas. Se o kiconf também aplicar
   `--scale-from` por conta própria, os dois vão brigar; escolher um só.

## kishot — precisa de mudança

O kishot usa `shared/xis_inputscale.*` (`XisScaleMap`) em três lugares:

- **`ks_rec.c`** (`pointer_on_frame()`): converte a posição do ponteiro do
  root lógico para o framebuffer físico gravado e aumenta o cursor pelo
  fator físico/lógico. Ele se atualiza com `XISConfineNotify`.
- **`ks_grab.c`**: aumenta o cursor colado na captura de tela.
- **`ks_censor.c`** (`ks_censor_to_physical()`): leva os retângulos de
  censura do root lógico para o framebuffer físico.

No modelo antigo, a área da tela escalada no root **era** o framebuffer
físico, com a imagem ampliada desenhada pelo kicomp. Por isso a captura
pelo root saía física, e tudo precisava dessa conversão.

### O que acontece agora

1. **A conversão vira identidade.** Sem a extensão, `xis_scalemap_load()`
   não encontra nada confinado, e ponteiro e censura não são mais
   convertidos. Para capturas do root, isso está certo: o root agora é
   lógico.
2. **O problema real: a captura da tela com fonte.** Numa saída que o
   kicomp entrega ao servidor como fonte X-SCALE, o kicomp desenha **só**
   na fonte física, não no overlay. Então a área dessa saída no root fica
   com uma imagem velha. O kishot copia a tela pelo root
   (`GetImage`/SHM) sempre que não há nada censurado, e por isso gravaria
   uma imagem congelada dessa saída. O mesmo vale para qualquer gravador
   comum (OBS, `ffmpeg -f x11grab`).

### Caminhos para o item 2

- **No servidor (recomendado, vale para todos os gravadores):** a X-SCALE
  passaria a costurar a fonte no `GetImage` do root. O XiS já faz isso para
  os flips por CRTC (`present_flip_getimage`, em
  `doc/present-per-crtc-flip.md`): nas regiões de uma CRTC com fonte, o
  servidor lê a fonte reduzida para o tamanho lógico. O custo só existe
  durante a captura. Com isso, o kishot captura certo em resolução lógica
  e só precisa de limpeza: apagar o `XisScaleMap` e usar a identidade.
- **No kishot (para gravar nítido, em resolução física):** usar os slots
  do protocolo de captura do kicomp (`kicomp/CAPTURE-PROTOCOL.md`) também
  para saídas com fonte, e não só quando há censura. Cada alvo de
  `_KICOMP_CAPTURE_TARGETS` já descreve a caixa lógica (`r[1..4]`) e o
  tamanho físico (`r[5..6]`). A conversão de ponteiro, cursor e censura
  sai daí: `físico = (lógico − origem) × físico/lógico`, sem extensão
  nenhuma. Isso substitui o `XisScaleMap`.

As duas coisas podem conviver: o servidor resolve o caso geral, e o kishot
usa os slots quando quiser a gravação em resolução física.

**A fazer:**

- decidir o caminho (o do servidor depende de um commit na branch `x-scale`
  do xserver);
- reescrever `pointer_on_frame()`, o trecho do cursor em `ks_grab.c` e
  `ks_censor_to_physical()` em cima disso;
- remover `XISConfineNotify` do loop de `ks_rec.c`;
- atualizar o trecho do `kishot/README.md` sobre X-INPUT-SCALE.

## shared/xis_inputscale.c/.h — remover

Esses arquivos são o cliente da X-INPUT-SCALE 1.0: presença, confinamento,
eventos e o `XisScaleMap`. Só o kishot os usa (está no `SRCS` e no `HDRS`
do `kishot/Makefile`). Devem ser apagados quando o kishot for migrado.
Nenhum programa daqui precisa de um cliente X-SCALE compartilhado: só o
compositor fala com a extensão, e o kicomp tem o próprio `src/xscale.c`.
