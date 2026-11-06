# kiview — plano

Planejamento de 2026-11-06. Visualizador de imagens e documentos do **kidesktop-base**,
o mais leve possível em RAM e CPU. Visual parecido com o Visualizador de imagens e fax do
Windows XP: a área da imagem ocupa a janela e uma barra de botões fica no rodapé.

A 0.1.0 abre só imagens (JPEG, PNG, WebP, GIF, BMP e o que mais o gdk-pixbuf ler). O motor
é plugável desde o começo, para depois entrarem PDF, Markdown, SVG, OpenRaster,
OpenDocument, EPUB e fontes TTF/OTF sem mexer no frontend.

## Decisões tomadas

| Pergunta | Decisão |
|---|---|
| Modelo de plugin | **Processo separado, como o mpv no kiplay.** O kiview cria uma janela X dentro da área, passa o XID e conversa por socket; o plugin desenha o que quiser nela. |
| Por que não `.so` | Decodificador de formato é o código mais exposto a arquivo malicioso ou quebrado. Em processo separado, um crash do libpoppler/giflib não derruba a janela, o RAM dele some quando o processo sai, e dá para pôr em sandbox depois (seção 7). |
| Um processo por arquivo? | **Não.** Um processo por plugin, mantido vivo enquanto houver arquivo daquele tipo (como o `mpv --idle`). Próxima/anterior não paga `exec` nem inicialização de lib. |
| Quem guarda zoom/posição | **O plugin.** O kiview manda intenções ("zoom ×1.25 ancorado em x,y", "arraste dx,dy", "página +1") e o plugin responde com o estado resultante. Um PDF em rolagem contínua conhece o próprio layout; o frontend não precisa saber. |
| Entrada (mouse/teclado) | **Só o kiview.** Nem ele nem o plugin selecionam botões na janela do plugin, então os eventos chegam à área GTK (o mesmo truque do `videowin.c`). |
| Densidade | **X-DENSITY `_X_DENSITY_SOURCES`, igual ao kiplay.** Com a janela principal densa, a janela do plugin é redirecionada e redimensionada para `w×s, h×s`; o plugin renderiza do arquivo original nessa resolução (seção 4). |
| 100% ("Tamanho real") | 1 pixel da imagem por **pixel lógico**. Fica igual com ou sem compositor; sob a lupa do kicomp, a densidade extra mostra detalhe real do arquivo, sem esticar. "100% = pixel físico" fica como opção futura. |
| Roda do mouse | **Sem Ctrl: anterior/próxima** quando a imagem cabe na janela (como no XP), **rolagem** quando não cabe. **Ctrl+roda: zoom** ancorado no cursor. Para documentos com páginas, sem Ctrl sempre rola. |
| Girar | **Só na visualização** na 0.1.0. Gravar a rotação no arquivo (JPEG sem perda, como o XP fazia) fica para depois, com confirmação. |
| Um processo por janela | Sim. Cada `kiview arquivo` abre uma janela, sem instância única. |

## 1. Arquitetura

```
 kiview (GTK2, gtk2-ng)                          kiview-image (sem GTK)
 ┌───────────────────────────────┐   socket     ┌────────────────────────────┐
 │ janela, rodapé, pasta,        │◄────────────►│ libkvp: protocolo, conexão │
 │ slideshow, tela cheia, input  │  SEQPACKET   │ X própria, XShm, view      │
 │                               │  + fds       │                            │
 │ área (GtkDrawingArea)         │              │ decodificadores, prefetch, │
 │  └─ janela X do plugin ◄──────┼── desenha ───┤ animação                   │
 │     (criada pelo kiview,      │   via XID    └────────────────────────────┘
 │      redirecionada se densa)  │
 └───────────────────────────────┘
```

- **kiview** (frontend, GTK2): janela, barra do rodapé, lista da pasta, slideshow, tela
  cheia, tradução de mouse/teclado em comandos, escolha do plugin pelo tipo do arquivo,
  crash/timeout do plugin, X-DENSITY do lado da janela.
- **libkvp** (biblioteca estática dos plugins, sem GTK): laço de eventos (GLib), protocolo,
  conexão X própria, buffer XShm com superfície cairo, apresentação sem piscar, contas de
  vista (encaixar, ancorar zoom, limitar arraste, girar). Um plugin só implementa
  `open`/`render`/`close` e, se tiver, páginas e animação.
- **kvp_raster** (dentro da libkvp): o pipeline de imagem pronta (pirâmide de reduções,
  escala, xadrez de transparência). Serve ao `kiview-image` e depois ao OpenRaster, e a
  qualquer plugin que produza um bitmap.
- **Plugins**: executáveis `kiview-<nome>` em `$(LIBEXECDIR)/kiview/`, cada um com um
  manifesto `<nome>.kvplugin` em `$(DATADIR)/kiview/plugins/` (e
  `~/.local/share/kiview/plugins/`), mesma ideia dos `.thumbnailer` que o kifiles lê.

### 1.1 Manifesto

```ini
[Plugin]
Name=image
Exec=kiview-image
Protocol=1
MimeTypes=image/jpeg;image/png;image/webp;image/gif;image/bmp;image/x-tga;image/tiff;...
Extensions=jpg;jpeg;png;webp;gif;bmp;tga;tif;tiff;ico;pnm;...
Priority=50
```

- O kiview escolhe o plugin pelo tipo MIME (GIO, por conteúdo); extensão só desempata.
  Maior `Priority` ganha (ex.: um `kiview-pdf` mais rápido pode tomar o lugar do
  `kiview-mupdf` para PDF).
- A união dos `MimeTypes` dos manifestos instalados define quais arquivos da pasta entram
  em anterior/próxima, e o `MimeType=` do `.desktop` é gerado deles no build.
- O manifesto do `kiview-image` lista também os tipos do gdk-pixbuf; o plugin confere em
  tempo de execução os loaders que existem e responde `failed reason=unsupported` para o
  resto.

## 2. Protocolo kiview ↔ plugin

Especificação completa em `kiview/PROTOCOL.md` (a escrever na 0.0.1). Resumo:

- `socketpair(AF_UNIX, SOCK_SEQPACKET)`, a ponta do plugin no fd 3. **Uma mensagem por
  pacote**, texto `verbo chave=valor ...` com valores escapados por `%XX`. Legível em
  depuração (`KIVIEW_TRACE=1` imprime tudo), sem parser de JSON.
- **Arquivos vão como fd** (`SCM_RIGHTS`), não como caminho. O plugin não precisa enxergar
  o sistema de arquivos, e a sandbox (seção 7) fica possível sem mudar o protocolo. Para
  formatos que referenciam outros arquivos (Markdown com imagens relativas), vai também
  um fd do diretório (`dirfd`).
- Versão no `hello`; verbos e chaves desconhecidos são **ignorados**, nunca erro. Assim
  plugins novos funcionam com kiview antigo e vice-versa.
- Toda mudança de superfície tem um `seq`; o plugin responde `frame seq=N` depois de
  desenhar nela. É o "conteúdo primeiro, anúncio depois" do X-DENSITY (seção 4).

| kiview → plugin | Significado |
|---|---|
| `hello version=1` | Abertura. |
| `surface xid= w= h= density=num/den seq=` | Janela onde desenhar, tamanho lógico da área e densidade. A janela já está no tamanho `w×s, h×s`. |
| `style bg=#rrggbb checker=0\|1` | Fundo e xadrez de transparência (tema e tela cheia mudam o fundo). |
| `open id= name= mime= [fd] [dirfd]` | Abre e mostra. `name` só serve para título e dica de formato. |
| `prefetch id= name= mime= [fd]` | Decodifica em segundo plano, sem mostrar (a próxima e a anterior da pasta). |
| `show id=` | Mostra um documento já aberto ou pré-carregado. |
| `close id=` | Libera. |
| `zoom mode=fit\|fit-width\|actual\|set\|by factor= x= y=` | Zoom; `x,y` é a âncora em coordenadas lógicas da área. |
| `pan dx= dy=` | Arraste/rolagem em pixels lógicos. |
| `rotate by=90\|-90` | Gira a vista. |
| `page n=\|rel=` | Troca de página (plugins com páginas). |
| `anim play\|pause` | Animação (GIF/WebP). O plugin avisa `anim_loop` ao fim de cada volta, para a apresentação (seção 3.3). |
| `quit` | Sair. |

| plugin → kiview | Significado |
|---|---|
| `hello version=1 caps=pages,anim,vector,rotate,prefetch` | O que o plugin sabe fazer; o rodapé esconde o que não se aplica. |
| `opened id= w= h= pages= anim= format=` | Documento aberto: tamanho natural, páginas, se anima, descrição ("JPEG 4000×3000"). |
| `failed id= reason=unsupported\|corrupt\|toolarge\|io msg=` | Não abriu; o kiview mostra a mensagem na área. |
| `view id= zoom= fit=0\|1 scroll_x=0\|1 scroll_y=0\|1 page= pages=` | Estado depois de cada comando: % no rodapé, cursor de mão, roda rola ou navega. |
| `frame seq=` | Primeiro quadro desenhado naquela superfície. |
| `anim_loop id=` | A animação completou uma volta. |
| `busy` / `idle` | Renderização demorada (PDF grande): o kiview mostra um indicador depois de ~300 ms. |

Plugin que morre: o kiview mostra "Não foi possível exibir este arquivo" na área, e o
próximo arquivo do mesmo tipo inicia o plugin de novo. Plugin que não responde ao `open`
em 10 s: `SIGKILL` pelo PID (nunca por nome).

## 3. Frontend

### 3.1 Janela

- Área da imagem em cima, ocupando tudo; rodapé com os botões centralizados, ícones
  do tema (fallback embutido), dicas com o atalho.
- Título: `nome.jpg — kiview`. Na barra, à direita, o zoom atual ("37%") e a posição na
  pasta ("12 de 85"); dá para desligar.
- Tamanho inicial: a imagem em 100% se couber em ~80% da área de trabalho, senão
  encaixada. `xis_winstate` lembra tamanho e posição se o usuário redimensionar.
- Arrastar e soltar um arquivo na janela abre ele (a pasta dele vira a pasta atual).

### 3.2 Botões do rodapé (0.1.0)

| Botão | Atalho |
|---|---|
| Anterior / Próxima (mesma pasta) | ←/→, PgUp/PgDn, Backspace/Espaço, roda sem Ctrl quando a imagem cabe |
| Encaixar / Tamanho real | `F` (ou `*` do numérico) / `1` (ou `/`) |
| Iniciar apresentação | F5 |
| Tela cheia | F11, duplo clique |
| Mais zoom / Menos zoom | `+`/`-`, Ctrl+roda (âncora no cursor) |
| Girar à esquerda / à direita | Ctrl+, / Ctrl+. (só a vista) |
| Excluir (lixeira, via GIO) | Delete, com confirmação; segue para a próxima |
| Editar (abre no kipaint, ou no app padrão do tipo) | Ctrl+E |

Home/End: primeira/última da pasta. Arrastar com o botão esquerdo move a imagem quando
ela não cabe (cursor de mão). Esc sai da tela cheia/apresentação.

Botões de página (anterior/próxima página, "3 de 20") só aparecem quando o plugin anuncia
`caps=pages`; não existem na 0.1.0 mas o espaço no rodapé já é previsto.

### 3.3 Pasta e apresentação

- **Pasta** (`kv_folder.c`): lista os irmãos do arquivo aberto via GIO, filtra pelos tipos
  dos manifestos, ordena por nome em ordem natural (`foo2` antes de `foo10`), como o
  kifiles. Navegação **dá a volta** no fim. `GFileMonitor` acompanha arquivos novos e
  apagados sem reler a pasta toda.
- Ao abrir um arquivo, manda `prefetch` da próxima e da anterior. O plugin guarda até 3
  documentos decodificados (atual + vizinhos), com teto de RAM (seção 5.4).
- **Apresentação** (`kv_slideshow.c`): tela cheia, fundo preto, cursor some após 2 s
  parado, rodapé reaparece ao mexer o mouse. Intervalo padrão 5 s (configurável); uma
  imagem animada espera **pelo menos** uma volta da animação. Teclas de navegação
  continuam valendo; Espaço pausa.

### 3.4 Tela cheia

Fundo preto, rodapé escondido que aparece ao encostar o mouse embaixo, mesmos atalhos.
O fundo vai para o plugin por `style`.

### 3.5 Configuração

`~/.config/kiview/kiview.conf` (ini): intervalo da apresentação, fundo, xadrez, mostrar
zoom/posição, roda navega ou rola. Aba no kiconf fica para depois da 0.1.0.

## 4. Densidade (X-DENSITY)

É o mesmo mecanismo do `ui/videowin.c` do kiplay, só que quem renderiza é o plugin:

1. O kiview cria uma janela X simples, filha da janela GDK da área, desconhecida do GDK,
   sem selecionar entrada. O XID vai no `surface`.
2. Lê `_X_DENSITY_SCALE` da janela principal (o gtk2-ng publica) e acompanha por
   `PropertyNotify`. `KIVIEW_DENSITY=S` força uma densidade sem compositor, para teste
   (como `KP_VIDEO_DENSITY`).
3. Densidade > 1: `XCompositeRedirectWindow(Manual)`, redimensiona para `w×s, h×s`, manda
   `surface ... density=s seq=N`. O plugin renderiza **a partir do arquivo** em `s` vezes
   mais pixels (não é um upscale da imagem de 1x).
4. Damage na janela do plugin → o kiview copia uma versão reduzida para a área 1x
   (`XRenderComposite` com transformação), que é o que capturas e gravadores veem.
5. Ao receber `frame seq=N`, publica `[x, y, w, h, janela]` em `_X_DENSITY_SOURCES` na
   janela principal. Apaga quando a densidade volta a 1 ou a área some.
6. Densidade 1 ou sem Composite/Damage: sem redirecionamento, o plugin desenha direto e o
   kiview não faz nada por quadro.

O código do `videowin.c` vira um módulo genérico **`shared/xis_density_source.c`**
(janela filha, redirect, cópia por Damage, anúncio após o quadro, limite de 8×). O kiview
é o primeiro usuário; o kiplay pode adotar depois (repositório separado, cópia ou
submódulo).

O ganho aqui é maior que no vídeo: uma foto de 24 MP encaixada a 20% numa tela com lupa
4× do kicomp mostra pixels reais da foto, e um PDF ou SVG renderiza vetor na densidade.

**Pendências a verificar:** o kicomp só foi testado com fontes em Xvfb (memória
`xdensity-holes`); EGL e o caminho do wobbly com fonte ainda não. O kiview é um bom caso
de teste porque o conteúdo é estático e dá para conferir o mapeamento de texel com o
`kiview-test`.

## 5. Plugin de imagens (`kiview-image`)

### 5.1 Decodificadores

| Formato | Biblioteca | Por quê |
|---|---|---|
| JPEG | **libjpeg-turbo** (API libjpeg) | Redução por DCT (`scale_num/scale_denom`, 1/2 a 1/8): uma foto de 24 MP encaixada numa janela de 1200 px decodifica a 1/4, muito mais rápido e com 1/16 do RAM. A decodificação inteira só acontece quando o zoom pede. |
| PNG | **libpng** | 16 bits → 8, paleta, gama; leitura progressiva por linha (sem carregar o arquivo inteiro). APNG não: o libpng do Ubuntu não tem o patch. |
| WebP | **libwebp + libwebpdemux** | Estático e animado (`WebPAnimDecoder`); o gdk-pixbuf do 22.04 não tem loader de WebP. Estático usa `scaled_width/height` do decoder para a mesma economia do JPEG. |
| GIF | **giflib** | Animação com disposição correta, compondo **um quadro por vez** em vez de guardar todos (o loader do gdk-pixbuf guarda). Sem `.pc` no 22.04: o Makefile testa o header. |
| BMP, ICO, TGA, TIFF, PNM, XPM, ICNS... | **gdk-pixbuf** (sem GTK) | Cauda longa, já instalado com o GTK2. `gdk_pixbuf_new_from_file_at_scale` para a primeira imagem rápida. |
| AVIF, HEIF, JPEG XL | depois da 0.1.0 | Só runtime nesta máquina; entram como decodificador extra se o `-dev` existir no build. |

- **Orientação EXIF**: leitor próprio e pequeno do APP1/IFD0 (só a tag 0x0112), aplicado
  na vista, não nos pixels.
- **Perfil de cor (ICC)** com lcms2: depois da 0.1.0, opcional.
- Decodificação em **thread**; o primeiro quadro sai do nível reduzido e o nível inteiro
  substitui quando fica pronto, sem piscar.
- Arquivo maior que o teto (ex.: > 16k×16k ou > 512 MB decodificado): só os níveis
  reduzidos que o JPEG/WebP conseguem, senão `failed reason=toolarge`.

### 5.2 Renderização (`kvp_raster`)

- Formato interno ARGB32 pré-multiplicado (o do cairo), sem conversões extras.
- **Pirâmide preguiçosa**: níveis 1/2, 1/4... gerados só quando o zoom pede, por média
  de caixa 2×2. Reduzir a partir do nível imediatamente maior dá qualidade boa com
  bilinear, sem o custo de um filtro grande sobre a imagem inteira.
- Desenha só o retângulo visível na superfície (lógico × densidade), em um buffer XShm,
  com `XShmPutImage` (fallback `XPutImage`). Dois buffers para não piscar durante o
  arraste.
- Zoom acima de 100%: vizinho mais próximo a partir de ~400% (ver os pixels), bilinear
  abaixo; configurável depois.
- Xadrez sob transparência, fundo pelo `style`.

### 5.3 Animação

GIF e WebP animados: o plugin tem o próprio temporizador, compõe o próximo quadro e
redesenha. Só o quadro atual e o canvas de composição ficam na memória. Pausa quando a
janela está minimizada ou totalmente coberta (`VisibilityNotify` na janela do plugin).

### 5.4 Memória

- Teto padrão do cache de documentos (atual + prefetch): ~256 MB; acima disso o prefetch
  guarda só o nível reduzido de encaixar.
- `M_MMAP_THRESHOLD` fixo + `malloc_trim` ao fechar documento, pela retenção de arenas
  da glibc com threads de decodificação (memória `glibc-arena-retention`, kifiles 377 →
  80 MB).
- Medir contra eog, ristretto, gpicview e feh (RSS do frontend + plugin, tempo até o
  primeiro quadro, CPU no arraste) com o método da memória `reference_profiling_sem_perf`.

## 6. Plugins futuros

O protocolo já cobre o que eles precisam (páginas, vetor, `dirfd`, `busy`). Ordem
sugerida, do mais barato ao mais caro:

| Plugin | Formatos | Biblioteca | Notas |
|---|---|---|---|
| `kiview-svg` | SVG, SVGZ | librsvg (já instalada, 2.52) | Vetor: rerenderiza no zoom e na densidade, sem pirâmide. `caps=vector`. |
| `kiview-ora` | OpenRaster | zlib + libpng, reaproveita o leitor do `kipaint/kp_ora.c` | Mostra `mergedimage.png` (rápido) com `kvp_raster`; compor as camadas só se o merged faltar. |
| `kiview-font` | TTF, OTF, TTC, WOFF/WOFF2 | freetype + harfbuzz + pango (cairo) | Folha de amostra (nome, estilos, tamanhos, "A ligeira raposa..."), páginas = faces de um TTC. Botão "Instalar" (copiar para `~/.local/share/fonts`) depois. |
| `kiview-mupdf` | PDF, EPUB, CBZ, XPS, FB2 | MuPDF | Uma lib cobre PDF **e** EPUB (com reflow e tamanho de fonte). AGPL-3: em processo separado, compatível com o GPL-3 do projeto. Alternativa só para PDF: poppler-glib (GPL-2+), mais pesada. Precisa de `caps=pages`, rolagem contínua, índice, busca e seleção de texto: verbos a acrescentar ao protocolo quando chegar a hora (ver seção 2). |
| `kiview-markdown` | Markdown | md4c + pango | Layout próprio em rolagem contínua (uma "página" alta), imagens relativas pelo `dirfd`, recarrega quando o arquivo muda. |
| `kiview-odf` | ODT, ODS, ODP | zip + `Thumbnails/thumbnail.png` / LibreOffice | Prévia instantânea pela miniatura do próprio arquivo. Render completo só convertendo (`soffice --headless --convert-to pdf`, com cache) e mostrando pelo `kiview-mupdf`: pesado, opcional, só se o LibreOffice estiver instalado. |

Com páginas, o rodapé ganha página anterior/próxima, "3 de 20" e encaixar largura; uma
barra lateral de miniaturas fica para depois.

## 7. Segurança e sandbox (depois da 0.1.0)

- O plugin já recebe só fds. Próximo passo: **Landlock** (kernel ≥ 5.13; esta máquina tem
  5.15) negando todo o sistema de arquivos exceto o necessário para carregar libs e
  loaders do gdk-pixbuf, e `PR_SET_NO_NEW_PRIVS`. Rede não é usada. Sem libseccomp no
  sistema, seccomp ficaria em BPF escrito à mão: avaliar se vale.
- O plugin é **outro cliente X**: conferir no `xisguard/tests/matrix.py` que desenhar numa
  janela de outro cliente e usar XShm não dispara pedido de permissão (não há grab nem
  leitura de outras janelas).

## 8. Arquivos

```
kiview/
  Makefile               frontend + plugins; plugins com dependência ausente são pulados
  PLAN.md  PROTOCOL.md  README.md
  kiview.desktop.in      MimeType= gerado dos manifestos
  kiview.svg  po/
  kv.h
  kiview.c               main, argumentos, abrir arquivo/pasta
  kv_window.c            janela, layout, tela cheia, rodapé que se esconde
  kv_toolbar.c           botões do rodapé, zoom e posição
  kv_view.c              área: mouse/teclado → comandos, cursor, mensagens de erro
  kv_folder.c            irmãos da pasta, ordem natural, monitor
  kv_slideshow.c         apresentação
  kv_plugin.c            manifestos, spawn, protocolo, crash/timeout
  kv_config.c            kiview.conf
  plugins/
    libkvp/              kvp.h, kvp_main.c (laço, protocolo), kvp_x11.c (janela, XShm),
                         kvp_view.c (encaixar, âncora, limites, rotação), kvp_raster.c
    image/               kiview-image: main.c, dec_jpeg.c, dec_png.c, dec_webp.c,
                         dec_gif.c, dec_pixbuf.c, exif.c, anim.c, cache.c, image.kvplugin
    test/                kiview-test: grade com coordenadas e texels para testar
                         zoom, arraste e densidade; ignora o arquivo
shared/
  xis_density_source.c/.h  janela filha densa (extraído do kiplay videowin.c)
```

Dependências: frontend = gtk+-2.0, Xcomposite, Xdamage, Xrender. `kiview-image` =
glib, gdk-pixbuf-2.0, cairo, Xext (MIT-SHM), libjpeg-turbo, libpng, libwebp(+demux),
giflib. No pacote, as libs dos plugins ficam em `Recommends` do kidesktop-base: faltando
uma, o plugin (ou o decodificador) não é compilado e o tipo simplesmente não aparece.

O `Makefile` raiz já tem `kiview` em `BASE_COMPONENTS`; basta o `kiview/Makefile` existir.
Entrar também em `scripts/lint.sh` (cppcheck, `-Werror`, arrays locais com `= { 0 }`).

## 9. Testes

- **`kiview-test`** com `KIVIEW_DENSITY=2` no Xephyr do fork: confere âncora do zoom,
  limites de arraste, giro e que o texel `(u×s, v×s)` cai no pixel lógico certo.
- Sessão aninhada no Xephyr (`-extension XNOTIFY`) com kicomp para X-DENSITY de verdade;
  limpeza por PID, nunca `pkill` por nome.
- Arquivos quebrados/truncados de cada formato (o plugin tem que responder `failed`, não
  morrer); plugin morto no meio (`kill -SEGV`) para o caminho de recuperação.
- ASan + heaptrack no rig de vazamento; `LANG=pt_BR` (armadilha do `LC_NUMERIC` ao ler
  fatores de zoom do ini).
- a11y: nomes dos botões no AT-SPI, foco por teclado no rodapé.

## 10. Marcos

| Versão | Conteúdo |
|---|---|
| **0.0.1** | Frontend com janela e rodapé, `kv_plugin` + `PROTOCOL.md`, libkvp, `kiview-test`, `kiview-image` só com gdk-pixbuf. Encaixar, 100%, zoom (Ctrl+roda), arraste, anterior/próxima, tela cheia. Sem densidade. |
| **0.0.2** (feito) | Decodificadores próprios: JPEG com DCT, PNG, WebP (com animação), GIF com giflib; EXIF; pirâmide preguiçosa; prefetch dos vizinhos; apresentação. |
| **0.0.3** | `shared/xis_density_source.c` extraído do kiplay e X-DENSITY no kiview; testes com `kiview-test` e kicomp (GLX, XRender, EGL). |
| **0.1.0** | Girar, excluir para a lixeira, editar no kipaint, arrastar e soltar, `kiview.conf`, tradução pt_BR, a11y, `.desktop` com MimeType, lint/CI/deb, medição de RAM e tempo contra eog/ristretto/gpicview. |
| depois | SVG → OpenRaster → fontes → MuPDF (PDF/EPUB) → Markdown → OpenDocument; sandbox Landlock; ICC; AVIF/HEIF/JXL; gravar rotação; aba no kiconf. |
