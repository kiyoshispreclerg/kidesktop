# Estilos de layout (modelos)

Cada pasta e um conjunto completo de configs usado nos prints de `../estilos/`.
`./apply.sh <estilo>` sobrescreve as do `~/.config` (com backup) para trocar o
layout inteiro de uma vez.

| estilo | painel | kiwm | prints |
|---|---|---|---|
| `mac` | barra no topo com globalmenu aberto + dock flutuante so de icones embaixo (`pct=54`, `thickness=72`) | `window_gap=10` | `mac-*.png` |
| `win11` | barra de 48px embaixo, tasklist so de icones centralizada entre dois `spacer` | `window_gap=0` | `win11-*.png` |
| `kde` | painel flutuante embaixo (`follow_wm_gap=1`), tasklist larga + pager | `window_gap=12` | `kde-*.png` |

Arquivos por estilo: `xispanel.conf`, `xispanel-pinned-dock.list` (fixados, por
`WM_CLASS` com inicial maiuscula), `kiwm.conf`, `kicomp.conf` (copia da sua, com
sombras e `live_windows=all`), `kisession.conf` (so para o teste: sem polkit/audio).

- `greenkiyo-glass/colors`: o seu tema `themes/greenkiyo` com o verde translucido
  (`#1c931ed0` na titlebar/borda). O painel usa `bg=#1c931ee6` no `THEME`.
  `theme=` nos `kiwm.conf`/`xispanel.conf` aponta para o caminho absoluto desta pasta.
- `xisback.conf`: os 4 wallpapers de `../wallpapers/`, um por desktop.
- Mudancas de `kiwm.conf`/`kicomp.conf` so valem depois de reiniciar o kiwm/kicomp.
