# kipaint — plano

Planejamento de 2026-10-23. Programa de pintura do **kidesktop-base**, GTK2, o mais leve
possível em RAM e CPU. Parecido com o Paint clássico (Windows XP/7), com zoom por
Ctrl+scroll, camadas com transparência e camadas tratadas como objetos (alças para mover,
redimensionar e girar, como a ferramenta Transformar do GIMP ou um editor vetorial, mas
com bitmaps).

## Decisões tomadas

| Pergunta | Decisão |
|---|---|
| Formato nativo | **OpenRaster (.ora)** a partir da 0.0.2, para quem edita com camadas. |
| Salvar imagem plana | **Ctrl+S mantém o formato** de PNG/JPG/BMP/TIFF abertos. Só vira .ora quando o usuário passa a usar camadas (seção 1.1). |
| Rotação | **Alça de rotação sempre visível**. O ciclo de cliques do Corel (escala → rotação/inclinação) fica como opção. |
| Painel de camadas | **Visível por padrão**, com opção de esconder (aí parece o Paint clássico). |
| Interface | Estilo **Paint do XP** (caixa de ferramentas + paleta no rodapé), sem ribbon. |

## 1. Formato de arquivo

PNG e JPG não guardam camadas. O formato nativo é o **OpenRaster (.ora)**:

- É um ZIP com `mimetype`, `stack.xml` (camadas, ordem, x/y, opacidade, visibilidade),
  um PNG por camada, `mergedimage.png` e `Thumbnails/thumbnail.png`. **GIMP, Krita e
  MyPaint abrem .ora**, então o arquivo não fica preso ao kipaint.
- Não precisa de dependência nova. Pra gravar, montamos o ZIP sem compressão (os PNGs já
  vêm comprimidos), com crc32 do zlib, que o gdk-pixbuf já puxa. Pra abrir .ora de
  outros programas, o zlib faz o inflate.
- O .ora só guarda x/y. Rotação e escala: o PNG da camada é gravado já transformado (é o
  que GIMP e Krita veem). O bitmap original e a matriz vão num arquivo extra dentro do ZIP
  (`kipaint/…`, referenciado por atributo com namespace no `stack.xml`), e com isso o
  kipaint reabre a camada ainda editável, sem perder qualidade. Texto vivo segue a mesma
  ideia.
- **PNG/JPG/BMP/TIFF continuam sendo formatos de salvar de verdade**, não só de
  exportar: abrir uma imagem e dar Ctrl+S regrava o mesmo arquivo, no mesmo formato,
  como na 0.0.1. O achatamento é feito via gdk-pixbuf.
- Na 0.0.1 só existia uma camada, então "Salvar" gravava direto pela extensão.

### 1.1 Modo do documento: "imagem plana" ou "projeto em camadas"

O documento está sempre em um de dois modos, e o modo decide o que Ctrl+S faz.

| | **Imagem plana** | **Projeto em camadas** |
|---|---|---|
| Como se chega | Abrir PNG/JPG/BMP/TIFF, ou Novo | Abrir .ora, ou "Salvar como..." em .ora |
| Ctrl+S | Regrava o arquivo, no mesmo formato | Regrava o .ora |
| Salvar como... | Qualquer formato, inclusive .ora | Qualquer formato (os planos achatam) |
| PNG/JPG/... aparecem como | Salvar normal | **Exportar...** (o título e o Ctrl+S continuam no .ora) |

- Enquanto o documento tem **uma camada só**, nada muda em relação à 0.0.1: o usuário
  abriu uma imagem, edita e dá Ctrl+S, sem aviso nenhum. Camadas são um extra.
- **Aviso ao achatar:** se o documento está em modo imagem plana, tem **mais de uma
  camada** e o usuário dá Ctrl+S (ou Salvar), o kipaint pergunta antes de gravar:

  > *Salvar em <formato> junta as camadas em uma só imagem. As camadas não serão
  > guardadas nesse arquivo.*
  > **[Continuar]** **[Salvar como...]** **[Cancelar]**

  - **Continuar:** grava o arquivo achatado, como o formato pede. As camadas continuam
    no editor, e o documento segue em modo imagem plana. A pergunta volta no próximo
    Ctrl+S (com a opção "Não perguntar de novo nesta sessão" no próprio aviso).
  - **Salvar como...:** abre o diálogo normal, já com o filtro de .ora sugerido. Se o
    usuário gravar em .ora, o documento **passa a ser um projeto em camadas**, como se
    tivesse sido aberto de um .ora: o caminho do documento vira o .ora, o título muda, e
    o PNG/JPG de origem deixa de ser o destino do Ctrl+S e passa a ser **Exportar**.
    Se gravar em outro formato plano, segue em modo imagem plana (com o aviso no
    próximo Ctrl+S).
  - **Cancelar:** não grava e não muda nada.
- O aviso vale para qualquer caminho que grave um formato plano a partir de um
  documento com camadas: Ctrl+S, "Salvar" e fechar a janela com "Salvar" no diálogo de
  alterações pendentes.
- **Achatar nunca altera o documento.** O que muda é só o arquivo gravado; as camadas e
  o histórico de desfazer continuam intactos.
- **Menu Arquivo** por modo:
  - Imagem plana: *Salvar*, *Salvar como...*
  - Projeto em camadas: *Salvar* (.ora), *Salvar como...*, **Exportar...** (diálogo com
    PNG/JPG/BMP/TIFF, lembra a última pasta e o último formato; não muda o caminho do
    documento nem o estado de "modificado").
- **"Modificado":** em projeto em camadas, exportar não limpa o asterisco do título,
  porque o .ora continua sem as últimas edições. Em imagem plana, salvar limpa, como hoje.
- **Formatos que o kipaint abre mas não grava** (GIF, WebP, ...): Ctrl+S cai em
  "Salvar como..." com o nome sugerido em .png, como já é na 0.0.1.
- **JPG/BMP com camadas:** além do aviso de achatar, o achatamento é feito sobre branco,
  como já é hoje para o alfa, e o aviso de alfa perdido é o mesmo da 0.0.1.

## 2. Arquitetura

- **Só cairo e gdk-pixbuf**, sem GEGL e sem threads. Camada é uma `cairo_image_surface`
  ARGB32 (pré-multiplicada) com matriz (translação, escala, rotação), opacidade,
  visibilidade e nome.
- **Cache de 3 superfícies** (0.0.2): "abaixo da ativa" (já composta), "ativa" e "acima"
  (já composta). Enquanto você pinta, só a ativa muda, e cada movimento custa 3 blits no
  retângulo sujo, não importa quantas camadas existam.
- **Canvas próprio:** GtkDrawingArea + GtkAdjustments + scrollbars próprias (nada de
  GtkViewport, porque uma imagem grande a 32× passaria do limite de 32767 px de uma janela
  X). Desenha só a área exposta.
  - Zoom com Ctrl+scroll centrado no cursor, em passos de 1/8× a 32×.
  - Filtro NEAREST acima de 100% e GOOD abaixo.
  - Grade de pixels a partir de 800%.
- **Undo por tiles de 64×64:** uma operação guarda o conteúdo original só dos tiles que
  toca. Desfazer e refazer são o mesmo "troca", que permuta o tile guardado com o da
  superfície. Transformações (0.0.3) guardam só a matriz. Limite de RAM de 64 MB, e as
  operações mais antigas são descartadas ao passar disso. `malloc_trim` ao fechar a
  imagem.
- Referência de custo: uma camada 1920×1080 ocupa cerca de 8 MB.

## 3. Interface

```
┌ Menu: Arquivo Editar Exibir Imagem Camadas Ajuda ───────────┐
│┌──┐ ┌──────────────────────────────────────┐ ┌───────────┐ │
││▭✎│ │                                      │ │ Camadas   │ │
││⌫◈│ │              canvas                  │ │ 👁 Texto  │ │
││💧🔍│ │                                      │ │ 👁 Foto   │ │
││A ╱│ │                                      │ │ 👁 Fundo  │ │
││▢◯│ └──────────────────────────────────────┘ │ [+][−][⧉] │ │
││opç│                                          └───────────┘ │
│└──┘                                                         │
│ [■1/■2]  ▪▪▪▪▪▪▪▪▪▪▪▪▪▪  (paleta de 28 cores)                │
│ status: x,y · tamanho da seleção · zoom 100% · W×H          │
└─────────────────────────────────────────────────────────────┘
```

- **Caixa de ferramentas à esquerda**, em 2 colunas, com ícones desenhados em cairo (não
  dependem do tema de ícones), e opções da ferramenta embaixo (espessura,
  preenchimento/contorno, estilo).
- **Cores:**
  - Botão esquerdo pinta com a cor 1 e o direito com a cor 2.
  - Duplo-clique numa cor da paleta abre o seletor de cor.
  - O conta-gotas pega a cor 1 com o botão esquerdo e a cor 2 com o direito, e depois
    volta pra ferramenta anterior (como no Paint).
- **Painel de camadas** à direita (0.0.2), ocultável.

## 4. Camadas como objetos (0.0.3)

- Com a ferramenta **Mover/Transformar**, um clique seleciona a camada sob o cursor
  (pixel não transparente). Aparecem 8 alças de redimensionar e uma alça de rotação.
- Opção estilo Corel: clicar de novo na mesma camada troca as alças de escala pelas de
  rotação e inclinação.
- Shift mantém a proporção, Ctrl prende a rotação em passos de 15°, e as setas movem
  1 px por vez.
- A edição não é destrutiva: o bitmap original é preservado e só é reamostrado ao
  "Aplicar transformação" ou ao exportar.
- **Paste, arrastar imagem do kifiles e seleção recortada viram uma camada nova.** É o
  "flutuante" do Paint, só que persistente.
- Pintar numa camada transformada funciona pela matriz inversa: o traço vai para o
  espaço da camada.

## 5. Ferramentas

Seleção (retângulo e forma livre), borracha, balde, conta-gotas, lupa, lápis (1 px sem
antialias), pincel, spray, texto, linha, curva, retângulo, polígono, elipse e retângulo
arredondado.

- **Texto** com Pango, como camada de texto re-editável até ser rasterizada.
- **Borracha:** como no Paint, pinta com a cor 2. A opção "Transparente" apaga pra alfa.
- Menu **Imagem**: girar/espelhar, redimensionar/inclinar, tamanho da tela, cortar para a
  seleção e inverter cores.

## 6. Arquivos

Um arquivo por responsabilidade. Cada ferramenta tem o próprio arquivo (com ícone e
eventos) desde o início.

```
kipaint/
  kipaint.c          janela, menus, atalhos, abrir/salvar, status
  kp.h               tipos e estado compartilhados
  kp_doc.c           documento, lista de camadas, conta-gotas composto
  kp_layer.c         superfície + propriedades
  kp_canvas.c        view: zoom, scroll, grade, contorno do pincel, despacho de eventos
  kp_undo.c          undo por tiles
  kp_paint.c         primitivas de pixel (traço quadrado, cor pré-multiplicada)
  kp_palette.c       rodapé + cores 1/2
  kp_toolbox.c       caixa + painel de opções
  kp_io.c            abrir/gravar via gdk-pixbuf
  kp_config.c        ~/.config/kipaint.conf (paleta, cores, tamanhos, grade)
  tools/kp_tool_pencil.c, _brush.c, _eraser.c, _fill.c, _picker.c, ...
  (0.0.2+) kp_layers_panel.c  kp_ora.c  tools/kp_tool_transform.c ...
  po/  kipaint.desktop.in  kipaint.svg  README.md
```

## 7. Marcos

| Versão | Escopo |
|---|---|
| **0.0.1** | Esqueleto: janela, canvas com zoom Ctrl+scroll, scroll e arrasto com o botão do meio; uma camada; lápis, pincel, borracha, balde, conta-gotas; paleta com cores 1/2; Novo/Abrir/Salvar/Salvar como em PNG/JPG/BMP/TIFF; undo/redo por tiles; grade de pixels; i18n pt_BR; .desktop; integração com o Makefile principal, o lint e o `make deb` |
| **0.0.2** (feito, exceto o cache de 3 superfícies, adiado: a composição atual só repinta a área exposta) | Camadas: painel, adicionar/remover/duplicar/reordenar, opacidade, visibilidade, cache de 3 superfícies; salvar/abrir .ora; **modo imagem plana x projeto em camadas** (seção 1.1): aviso de achatar com Continuar / Salvar como... / Cancelar, Exportar... no modo projeto |
| 0.0.3 | Transformar: alças, mover, escalar, girar, ciclo de cliques estilo Corel; paste e DnD como camada nova |
| 0.0.4 | Seleções (retângulo/livre), recortar/copiar/colar, menu Imagem |
| 0.0.5 | Formas, linha/curva, spray, texto (camada de texto viva) |
| depois | Lupa, réguas, modos de mistura, cores com alfa nos traços, X-DENSITY, KiTheme |

## Notas da 0.0.2

- "Salvar como..." com extensão plana num projeto em camadas escreve o arquivo plano,
  sem o aviso (a escolha já foi explícita) e o documento volta ao modo imagem plana
  com esse arquivo como destino. Quem quer manter o .ora usa Exportar.
- Desfazer cobre as operações de camada (adicionar, excluir, duplicar, mover,
  renomear, visibilidade, opacidade); a opacidade ao arrastar o controle vira um
  passo só.
- Não se pinta numa camada oculta (exceto o conta-gotas).
- .ora: grupos aninhados viram lista plana; x/y de camadas de outros programas são
  aplicados ao abrir (a camada vira do tamanho do documento).

## Limitações conhecidas da 0.0.1

- As cores dos traços são opacas. O conta-gotas descarta o alfa. Com cor translúcida,
  segmentos sobrepostos de um mesmo traço escureceriam; isso volta junto com uma máscara
  de traço.
- Uma camada só, sem seleção, sem clipboard.
