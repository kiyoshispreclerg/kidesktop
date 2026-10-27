**[English](README.md)** · **Português (Brasil)**

# KiDesktop

Um ambiente de desktop X11 de estilo clássico para Linux, feito de pequenos
programas independentes.

## O que é isto

Eu uso o KDE Plasma 5 no dia a dia, e gosto dele desde que o conheci, na versão
3.5. Isto é uma tentativa de reconstruir as partes desse ambiente que eu
realmente uso -- gerenciador de janelas, painel, lançador, notificações, papel de parede, configurações,
atalhos -- como um punhado de pequenos programas em C que iniciam rápido,
ocupam pouca RAM e não puxam um framework de desktop inteiro só para desenhar
uma barra de tarefas.

Começou como uma única ferramenta: o **xnsguard**, um guardião de permissões
para o [XiS](https://github.com/kiyoshispreclerg/xserver), meu fork do
XLibre/Xorg. Essa ferramenta virou o `xisguard`, e então fui adicionando a peça
que me faltava em seguida, até virar um desktop. O repositório antigo do
xnsguard está arquivado
[aqui](https://github.com/kiyoshispreclerg/xnsguard); tudo o que veio depois
está neste.

O KiDesktop é feito pensando no XiS, mas, como mira o X11 puro, deve rodar em
qualquer servidor X.

## Sobre o objetivo de leveza

Sendo honesto: o desktop em si é leve. A maioria dos componentes usa só
Xlib/XCB + Cairo + Imlib2, sem toolkit, e há peças em GTK2 apenas onde
reimplementar certos widgets em Cairo puro não valia a pena.

Mas no momento em que você abre um navegador, ou qualquer coisa em Electron, ou
um app moderno em GTK3/Qt6, toda essa memória economizada some várias vezes. Ou
seja, a leveza não é *justificável* como um ganho para o sistema inteiro -- é
só a parte que eu consigo controlar, e é bom que o shell fique fora do caminho.

## Sobre IA

Meu outro eu escreve React Native e PHP para viver. Nosso C é básico. Este
projeto inteiro é escrito com muita ajuda de IA. No começo líamos cada linha,
mas depois de muitos testes chegamos à conclusão de que a IA é muito confiável
quando o humano (ou até eu, um "humano artificial") sabe o que quer, e que isso
não é muito diferente de usar um DE como o KDE, que também tem muito código que
não lemos antes de instalar um pacote compilado; pelo menos no nosso código
somos nós que decidimos o que é construído e como se encaixa. As coisas que
usamos, as decisões de arquitetura, os protocolos entre os componentes e todo
"não, faça de outro jeito" são nossos. O código é uma colaboração.

## Status

Longe de estar pronto, mas razoavelmente utilizável -- estou rodando partes
deste repositório no meu desktop enquanto escrevo isto. Espere arestas, funções
faltando e coisas que vão mudar. Tudo está em inglês (código, configs, docs),
embora eu preferisse outros idiomas :)

## Componentes

O KiDesktop é distribuído em dois grupos, compilados e empacotados à parte:

- **core** -- o desktop em si (tudo em *Sessão e janelas*, *Shell*,
  *Configurações*, *Histórico e área de transferência* e *Específico do XiS*
  abaixo). Pacote `kidesktop-core`.
- **base** -- os aplicativos do dia a dia (*Aplicativos* abaixo). Pacote
  `kidesktop-base`, que depende do core. Bibliotecas pesadas são apenas
  recomendadas: cada app roda sem elas, com as funções correspondentes
  desativadas.

O `kidesktop-core` também substitui o antigo pacote único `kidesktop`.
`apt install ./kidesktop-core_*.deb ./kidesktop-base_*.deb` instala os dois.

### Sessão e janelas

**[kisession](kisession/)** -- líder da sessão e supervisor de serviços. Inicia
e reinicia os outros daemons do desktop, depois o autostart XDG.

**[kiwm](kiwm/)** -- o gerenciador de janelas. Stacking, XCB, decoração em
Cairo + Imlib2, multi-monitor com desktops independentes por saída.

**[kicomp](kicomp/)** -- compositor opcional para o `kiwm`. Backends XRender ou
OpenGL (GLX/EGL), transparência, efeitos. O `kiwm` nunca depende dele.

### Shell

**[xispanel](xispanel/)** -- painel/barra de tarefas. Sem toolkit,
vários painéis, qualquer borda, qualquer saída. Lista de tarefas, bandeja, menu
global, pastas, notificações, monitor do sistema e mais widgets.

**[xisserve](xisserve/)** -- lançador de aplicativos e páginas de configurações
diversas (rede, armazenamento, áudio, energia, calendário...), ao estilo
kickoff/krunner. GTK2, iniciado sob demanda pelo `xispanel`.

**[xisback](xisback/)** -- daemon de papel de parede. Uma camada de papel de
parede por (saída, desktop), imagem estática ou apresentação de slides.

### Configurações

**[kiconf](kiconf/)** -- o app de configurações: aparência, atalhos, telas,
entrada, energia e permissões.

**[kiconfd](kiconfd/)** -- daemon de configurações, a outra metade do `kiconf`.
Aplica a configuração central a todos os toolkits que o desktop considera e
recarrega com SIGHUP.

**[kimountd](kimountd/)** -- monta os discos marcados na aba Discos do `kiconf`
quando são conectados ou já estão lá no login, pelo udisks2 (no mesmo lugar
que o KDE e o GNOME usam).

**[kitheme](kitheme/)** -- um único tema de widgets para GTK2, GTK3, GTK4, Qt5 e
Qt6, configurado no `kiconf`. Presets como Oxygen, Windows clássico e plano.

**[xismenu](xismenu/)** -- registrador do menu de aplicativos, para que os menus
globais de Qt/KF5 funcionem sem uma implementação completa de DBusMenu no lado
do gerenciador de janelas.

**[xiskeys](xiskeys/)** -- daemon de atalhos globais para ações sem estado:
executar um comando, teclas de mídia, brilho, captura de tela, bloqueio,
energia.

### Histórico e área de transferência

**[kimemory](kimemory/)** -- histórico da área de transferência. Registra cada
cópia com sua janela de origem, app e documento.

**[kistory](kistory/)** -- histórico de atividade do desktop, em texto simples e
pesquisável: janelas, foco, arquivos, saídas, áudio e eventos do xisguard.
Desativado por padrão.

### Específico do XiS

**[xisguard](xisguard/)** -- a ferramenta original. Conversa com a extensão X
**Xnotify** para arbitrar em tempo real as requisições privilegiadas de
clientes -- permitindo, perguntando, negando ou matando conforme as suas regras.

## Aplicativos (base)

**[kiterm](kiterm/)** -- emulador de terminal com abas (GTK2 + VTE).

**[kifiles](kifiles/)** -- gerenciador de arquivos (GTK2 + GIO): visão
dividida, miniaturas, dispositivos, desfazer.

**[kishot](kishot/)** -- capturas e gravação de tela; grava com som e pode
censurar janelas escolhidas.

**[kipad](kipad/)** -- editor de texto simples (GTK2).

**[kipaint](kipaint/)** -- programa de pintura no estilo do Paint clássico
(GTK2).

**[kitask](kitask/)** -- lista de processos e monitor do sistema; em cima, os gráficos
que você escolher, e uma visualização extra do uso de memória em gráfico de
pizza.

Planejados, ainda sem código:

- **[kiview](kiview/)** -- visualizador de imagens e documentos.
- **[kiman](kiman/)** -- visualizador de manuais e páginas de ajuda.

## Suas configurações e quedas de energia

Todo arquivo de configuração e lista do usuário que o desktop grava (`~/.config/kiwm.conf`, `xispanel.conf`,
`xisguard/perms.conf`, abas e favoritos do kifiles, ...) é gravado de forma atômica: o conteúdo novo vai
para `<arquivo>.tmp`, é sincronizado no disco e só então substitui o antigo. Antes disso, a última cópia
íntegra é guardada como `<arquivo>.backup` ao lado. Se um arquivo estiver ausente, vazio ou zerado após uma
queda, o programa lê o `.backup` e registra um aviso. Um arquivo já corrompido nunca substitui um `.backup`
bom.

Para "resetar" apagando um arquivo, apague também o `.backup`, senão a cópia antiga volta.
Arquivos que você salva num lugar de sua escolha (documentos, exportações) não ganham `.backup`.

## Compilação

Para a suíte completa (core + base), a partir da raiz do repositório:

```sh
./configure   # verifica as dependências compartilhadas (cairo, pango, imlib2, dbus-1, gtk2)
make install
```

O `./configure` também escolhe o que um `make` simples compila e guarda isso
em `config.mk`:

```sh
./configure --core-only   # só o desktop: sem kiterm, kifiles, ...
./configure --with-base   # core + base (o padrão)
```

Cada grupo também pode ser compilado ou instalado sozinho, seja qual for a
escolha do `./configure`: `make core`, `make base`, `make install-core`,
`make install-base`. Apps do base que ainda não têm código são ignorados.

Cada componente também compila sozinho, pulando o `./configure`:

```sh
cd kiwm && make
```

Toda dependência de compilação está listada uma única vez, no `Build-Depends`
do `debian/control`; no Debian/Ubuntu, `sudo ./scripts/install-build-deps.sh`
as instala.

### Verificações e pacotes

```sh
make lint   # todos os componentes com -Werror, cppcheck, verificações de .po/.desktop/scripts
make deb    # dist/: .debs kidesktop-core e kidesktop-base para esta distro
```

O GitHub Actions roda `make lint` no Ubuntu 22.04, Ubuntu 24.04, Debian 12 e Debian 13 a
cada push e pull request (`.github/workflows/ci.yml`).
Os *pacotes de release* (`.github/workflows/release.yml`) geram o `.zip` do
código-fonte e o `.deb` para essas quatro distros: automaticamente para uma tag
ou release estável (`v1.0.0` em diante, sem `-sufixo`), anexados àquela
release; para uma pré-release (`v0.x.y`, `v1.2.0-rc1`) apenas sob demanda --
"Run workflow" com a tag, anexados a uma release marcada como pré-release;
"Run workflow" sem tag mantém os arquivos como artefatos do workflow. O pacote
do Debian 12 também instala no Devuan 5, e o do Debian 13 no Devuan 6 (excalibur).

## Licença

Veja [LICENSE](LICENSE).
