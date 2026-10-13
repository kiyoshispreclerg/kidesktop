**[English](README.md)** · **Português (Brasil)**

# KiDesktop

Um ambiente de desktop X11 completo para Linux, feito de pequenos programas
independentes.

## O que é isto

Eu uso o Plasma 5 no dia a dia, e gosto dele. Isto é uma tentativa de
reconstruir as partes desse ambiente que eu realmente uso -- gerenciador de
janelas, painel, lançador, notificações, papel de parede, configurações,
atalhos -- como um punhado de pequenos programas em C que iniciam rápido,
ocupam pouca RAM e não puxam um framework de desktop inteiro só para desenhar
uma barra de tarefas.

Começou como uma única ferramenta: o **xnsguard**, um guardião de permissões
para o [XiS](https://github.com/kiyoshispreclerg/xserver), meu fork leve do
XLibre/Xorg. Essa ferramenta virou o `xisguard`, e então fui adicionando a peça
que me faltava em seguida, até virar um desktop. O repositório antigo do
xnsguard está arquivado
[aqui](https://github.com/kiyoshispreclerg/xnsguard); tudo o que veio depois
está neste.

O KiDesktop é feito pensando no XiS, mas não é exclusivo dele. Só o `xisguard`
precisa especificamente do XiS. Todo o resto mira o X11 puro e roda em qualquer
servidor X.

## Sobre o objetivo de leveza

Sendo honesto: o desktop em si é leve. A maioria dos componentes usa só
Xlib/XCB + Cairo + Imlib2, sem toolkit. A única peça em GTK2 (`xisserve`) existe
apenas onde reimplementar certos widgets em Cairo puro não valia a pena.

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

### Sessão e janelas

**[kisession](kisession/)** -- líder da sessão e supervisor de serviços. Inicia
e reinicia os outros daemons do desktop, depois o autostart XDG.

**[kiwm](kiwm/)** -- o gerenciador de janelas. Stacking, XCB, decoração em
Cairo + Imlib2, multi-monitor com desktops por saída. O WM em que este desktop
roda no dia a dia.

**[kicomp](kicomp/)** -- compositor opcional para o `kiwm`. Backends XRender ou
OpenGL (GLX/EGL), transparência real, cantos com forma, efeitos. O `kiwm` nunca
depende dele.

### Shell

**[xispanel](xispanel/)** -- daemon de painel/barra de tarefas. Sem toolkit,
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

**[xismenu](xismenu/)** -- registrador do menu de aplicativos, para que os menus
globais de Qt/KF5 funcionem sem uma implementação completa de DBusMenu no lado
do gerenciador de janelas.

**[xiskeys](xiskeys/)** -- daemon de atalhos globais para ações sem estado:
executar um comando, teclas de mídia, brilho, captura de tela, bloqueio,
energia.

### Histórico e área de transferência

**[kimemory](kimemory/)** -- histórico da área de transferência. Registra cada
cópia com sua janela de origem, app e documento, servido de volta pelo
`xisserve --clipboard` ou pela CLI `kimemory`.

**[kistory](kistory/)** -- histórico de atividade do desktop, em texto simples e
pesquisável: janelas, foco, arquivos, saídas, áudio e eventos do xisguard. Sem
capturas de tela, sem conteúdo de janelas. CLI `kistory`, ou pesquise pelo
lançador do `xisserve`.

### Específico do XiS

**[xisguard](xisguard/)** -- a ferramenta original, e o único componente que
exige o XiS. Conversa com a extensão X **Xnotify** para arbitrar em tempo real
as requisições privilegiadas de clientes -- permitindo, perguntando, negando ou
matando conforme as suas regras.

## Compilação

Para a suíte completa, a partir da raiz do repositório:

```sh
./configure   # verifica as dependências compartilhadas (cairo, pango, imlib2, dbus-1, gtk2)
make install
```

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
make deb    # dist/kidesktop_<versão>~<distro><release>_<arch>.deb para esta distro
```

O GitHub Actions roda `make lint` no Ubuntu 22.04, Ubuntu 24.04 e Debian 12 a
cada push e pull request (`.github/workflows/ci.yml`).
Os *pacotes de release* (`.github/workflows/release.yml`) geram o `.zip` do
código-fonte e o `.deb` para essas três distros: automaticamente para uma tag
ou release estável (`v1.0.0` em diante, sem `-sufixo`), anexados àquela
release; para uma pré-release (`v0.x.y`, `v1.2.0-rc1`) apenas sob demanda --
"Run workflow" com a tag, anexados a uma release marcada como pré-release;
"Run workflow" sem tag mantém os arquivos como artefatos do workflow. O pacote
do Debian 12 também instala no Devuan 5.

## Licença

Veja [LICENSE](LICENSE).
