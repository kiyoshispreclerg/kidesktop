#!/bin/sh
# Aplica um dos estilos (mac | win11 | kde) sobrescrevendo as configs em ~/.config.
# Faz backup do que existir em ~/.config/kidesktop-styles-backup-<data>/.
# Uso: ./apply.sh mac
set -e
style=$1
here=$(cd "$(dirname "$0")" && pwd)
[ -d "$here/$style" ] || { echo "uso: $0 mac|win11|kde"; exit 1; }
cfg=${XDG_CONFIG_HOME:-$HOME/.config}
bak=$cfg/kidesktop-styles-backup-$(date +%Y%m%d-%H%M%S)
mkdir -p "$bak"
for f in xispanel.conf kiwm.conf kicomp.conf xispanel-pinned-dock.list; do
	[ -f "$cfg/$f" ] && cp "$cfg/$f" "$bak/"
	cp "$here/$style/$f" "$cfg/$f"
done
echo "backup em $bak"
xispanel --reload
echo "xispanel recarregado; kiwm.conf/kicomp.conf valem no proximo login (ou: kiwm --replace &, kicomp --replace &)"
