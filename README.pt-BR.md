# WaylandX

[![CI](https://github.com/rianprei/waylandx/actions/workflows/ci.yml/badge.svg)](https://github.com/rianprei/waylandx/actions/workflows/ci.yml)

*[Read in English](README.md)*

Um servidor de display único que fala **Wayland e X11 ao mesmo tempo** e junta o melhor dos dois:
o pipeline moderno e sem tearing do Wayland com as liberdades que o X11 sempre deu
(posicionar janelas, capturar a tela, automatizar input, ferramentas tipo xdotool/wmctrl, rede).

Construído em C11 sobre wlroots 0.20. Apps X11 rodam via Xwayland rootless, gerenciado pelo próprio WaylandX.

## O que vem de cada lado

| Do Wayland (agora também para apps X11) | Como |
|---|---|
| Sem tearing, KMS atômico, damage tracking | wlroots scene graph |
| VRR por monitor | `[output:NOME] vrr = on` |
| Tearing permitido em jogos fullscreen | protocolo tearing-control ou regra `allow_tearing = true` |
| Escala fracionada por monitor | `scale = 1.25` + fractional-scale-v1 |
| dmabuf, explicit sync (drm-syncobj), color-management, presentation-time | habilitados quando a GPU suporta |
| Ponteiro relativo e travado (jogos), gestos de touchpad | relative-pointer, pointer-constraints, pointer-gestures |
| Mesa digitalizadora, touchscreen, métodos de entrada (IME) | tablet-v2, wl_touch, text-input-v3 + input-method-v2 |
| Lock de tela seguro | ext-session-lock (swaylock etc.); locker que trava mantém a tela bloqueada |
| Apps X11 nítidos em HiDPI | o X11 renderiza na maior escala dos monitores, com `Xft.dpi` publicado, como no KWin |

| Do X11 (agora também para apps Wayland) | Como |
|---|---|
| App escolhe a própria posição | protocolo `waylandx-unstable-v1` (Wayland) e configure requests (X11) |
| Always-on-top / below / sticky | `_NET_WM_STATE` (X11), protocolo próprio (Wayland), `waylandxctl above` |
| Captura de tela sem diálogo | `waylandxctl screenshot`, screencopy, ext-image-copy-capture, portal sem chooser |
| Automação de input (xdotool) | `waylandxctl type/key/click/mousemove`, virtual-keyboard/pointer abertos, shim `xdotool` |
| Introspecção e controle (wmctrl/xprop) | `waylandxctl list/info/move/close…`, shims `wmctrl`, foreign-toplevel |
| Atalhos globais para qualquer programa | `waylandxctl bind COMBO CMD` |
| Clipboard acessível de qualquer lugar | apps X11 em segundo plano leem **e definem** o clipboard (o conteúdo sobrevive ao app); data-control (wl-clipboard); shims `xclip`/`xsel` |
| Rodar app remoto com janela local (ssh -X) | `waylandx-remote host app` (waypipe), `ssh -X` via Xwayland |
| WM tradicional: flutuante, decorações, arrastar com Mod | decorações desenhadas pelo servidor, Mod+arrastar, snap; tiling opcional |
| Tampa do notebook / dock | fechar a tampa desliga a tela interna quando há monitor externo |
| Política aberta | qualquer cliente local pode capturar e injetar; só o socket IPC é restrito ao seu usuário (0600) |
| Teclas injetadas acionam atalhos do WM (XTest) | `waylandxctl keydown alt; key Tab` faz Alt+Tab, como o `xdotool` no X11 |

| O que um desktop completo (KDE/GNOME) oferece | Como |
|---|---|
| Notificações | o `waylandx-panel` é o servidor de notificações freedesktop: ícones, markup, ações; as críticas ficam na tela |
| Bandeja do sistema | StatusNotifierItem + menus DBusMenu (Discord, Steam, KDE Connect, nm-applet…) |
| Volume, rede e bateria na barra | PipeWire (`wpctl`), NetworkManager, sysfs; clique ou role para mudar |
| OSD de volume e brilho, teclas de mídia | funcionam sem configurar, inclusive na tela de bloqueio; brilho via logind (sem root) |
| Seletor Alt+Tab | segure Alt (ou Mod), navegue com Tab na ordem de uso, solte para trocar |
| Bloqueio e tela desligada por inatividade | embutido; players de vídeo e jogos que inibem o idle pausam |
| Bloquear antes de suspender, `loginctl lock-session` | inibidor do logind, liberado quando o locker aparece |
| Luz noturna | `night_light = 20:00-07:00`, transição de 30 minutos, gamma do hardware |
| Ícones dos apps | na lista de tarefas, notificações, bandeja e Alt+Tab, do seu tema de ícones |

## Compilar e testar

Dependências (Arch/CachyOS): `wlroots0.20 wayland wayland-protocols libxkbcommon libinput pixman libxcb xcb-util-wm cairo pango xorg-xwayland meson ninja`.
Para os testes e extras: `xclip wl-clipboard waypipe file`, e opcionalmente `swaylock xdotool wmctrl xdg-desktop-portal-wlr`.

```sh
meson setup build --prefix="$HOME/.local"
ninja -C build
meson test -C build
```

O `meson test` roda os testes unitários e cinco suítes de ponta a ponta sem tela, com clientes Wayland e
X11 de verdade:

- `run-headless.sh`: injeção de input, IME, screenshots, regras, shims, clipboard nos dois sentidos,
  tiling, painel, waypipe, idle e lock, reload de config;
- `run-multihead.sh`: workspaces por monitor, hotplug, tampa do notebook;
- `run-hidpi.sh`: X11 nítido em monitor escala 2, mapeamento de input, screenshots em resolução nativa;
- `run-session.sh`: bloquear antes de suspender, lock-session, brilho e MPRIS contra um logind falso;
- `run-desktop.sh`: notificações, OSD, Alt+Tab, itens da bandeja e seus menus num D-Bus privado.

O CI roda tudo normalmente e com ASan/UBSan. Nada mexe na sua sessão real: as suítes de D-Bus usam um
barramento privado, um `wpctl` falso e um backlight falso.

## Rodar

**Dentro do seu desktop atual** (abre uma janela; o modificador vira Alt automaticamente):

```sh
./build/waylandx
```

**Como sessão própria** (TTY ou tela de login):

```sh
meson install -C build                                   # instala em ~/.local
sudo cp session/waylandx.desktop /usr/share/wayland-sessions/
sudo sed -i "s|^Exec=.*|Exec=$HOME/.local/bin/waylandx-session|" /usr/share/wayland-sessions/waylandx.desktop
mkdir -p ~/.config/waylandx ~/.config/xdg-desktop-portal ~/.config/xdg-desktop-portal-wlr
cp config/default.ini ~/.config/waylandx/config.ini
cp session/WaylandX-portals.conf ~/.config/xdg-desktop-portal/
cp session/xdg-desktop-portal-wlr.conf ~/.config/xdg-desktop-portal-wlr/config
```

Depois escolha "WaylandX" no SDDM, ou rode `waylandx-session` a partir de um TTY.

## Atalhos padrão

`Mod` = Super (Alt quando aninhado). Tudo é configurável em `[keybind]`.

| Atalho | Ação |
|---|---|
| Mod+Enter | terminal |
| Mod+D | launcher do painel |
| Mod+Q / Mod+Shift+Q | fechar / matar |
| Mod+F, Mod+M, Mod+N | fullscreen, maximizar, minimizar |
| Mod+Espaço | flutuante ⇄ tiling para a janela |
| Mod+T | liga/desliga tiling no workspace |
| Mod+A / Mod+S | sempre no topo / em todos os workspaces |
| Alt+Tab / Mod+Tab (+Shift) | seletor de janelas: segure, navegue com Tab, solte |
| Mod+←/→, Mod+↑/↓ | meia tela, maximizar/restaurar |
| Mod+1..9, Mod+Shift+1..9 | trocar de workspace neste monitor / mover janela |
| Mod+H / Mod+L | diminuir/aumentar a área master |
| Print / Mod+Shift+S | screenshot da tela / da janela |
| Mod+Shift+R / Mod+Shift+E | recarregar config / sair |
| Ctrl+Alt+L | bloquear a tela |
| Teclas de volume, brilho e play/pause/próxima/anterior | embutidas, com OSD |
| Mod+arrastar (esq./dir.) | mover / redimensionar de qualquer ponto da janela |

## waylandxctl

```sh
waylandxctl list [--plain]                 # todas as janelas (Wayland e X11)
waylandxctl info -w 3                      # id, app_id/class, pid, x11_window, geometria, estado
waylandxctl move -w 3 100 200              # e resize, geometry, close, kill, focus
waylandxctl above on -w focused            # maximize|fullscreen|minimize|above|below|sticky|floating
waylandxctl workspace 2 -o DP-1            # workspaces são por monitor
waylandxctl type "olá mundo"               # qualquer texto Unicode, independente do layout
waylandxctl key ctrl+shift+t Return        # keydown/keyup também
waylandxctl mousemove 500 300; waylandxctl click 1 2   # botões no padrão X11 (4/5 = scroll)
waylandxctl screenshot ~/tela.png [-o DP-1 | -w 3 | -r 0,0,800,600]
waylandxctl bind super+F1 notify-send oi   # atalho global que dispara um comando
waylandxctl subscribe                      # stream de eventos JSON
waylandxctl lock; waylandxctl screens off  # bloquear agora / telas desligadas até o próximo input
waylandxctl brightness +5%                 # volume up|down|mute, media play-pause|next…
waylandxctl nightlight on                  # on|off|auto (auto segue night_light)
waylandxctl switch next                    # Alt+Tab a partir de scripts
waylandxctl help
```

O protocolo do socket é texto: um comando por linha, resposta JSON terminada por uma linha em branco.

## Shims de ferramentas X11

Dentro da sessão, `share/waylandx/shims` entra na frente do `PATH`:

- `xdotool`: type, key, keydown/up, mousemove(_relative), click, search (com encadeamento), getactivewindow,
  windowactivate/move/size/close/kill/minimize, getwindowname/pid/geometry, desktops.
- `wmctrl`: `-l [-p -G -x]`, `-a`, `-c`, `-r … -e g,x,y,w,h`, `-r … -b add|remove|toggle,above|sticky|…`, `-s`, `-t`, `-d`.
- `xclip`/`xsel`: leem e escrevem o clipboard de qualquer lugar.

Tudo isso funciona com janelas Wayland **e** X11. Subcomandos não cobertos caem no binário real.

## Configuração

Veja `config/default.ini`. Seções: `[general]`, `[input]`, `[output:NOME]`, `[keybind]`, `[rule:NOME]`, `[autostart]`.
Reload ao vivo com `waylandxctl reload`, Mod+Shift+R ou SIGHUP. Um arquivo com erro é rejeitado inteiro e a config anterior continua valendo.

## Estrutura

```
src/main.c        setup, sinais, spawn, reload
src/output.c      monitores, modo/escala/VRR, tearing, output-management, fundo
src/input.c       teclado, ponteiro, touch, mesa digitalizadora, atalhos, mover/redimensionar/snap, gestos
src/ime.c         ponte de métodos de entrada (text-input-v3 <-> input-method-v2)
src/seat.c        foco, clipboard, drag-and-drop, cursor
src/view.c        janela unificada (xdg + X11): estados, camadas, posicionamento
src/xdg.c         xdg-shell, popups, negociação de decoração
src/xwayland.c    Xwayland: posição pedida, override-redirect, EWMH
src/deco.c        decorações do servidor (cairo/pango)
src/layer.c       layer-shell (painéis, wallpapers), área útil
src/layout.c      tiling master/stack
src/workspace.c   workspaces por monitor + ext-workspace
src/rules.c       regras por app_id/class/título
src/config.c      parser INI
src/ipc*.c        socket IPC e comandos
src/inject.c      injeção de teclado/mouse (keymap gerado na hora)
src/capture.c     screenshots
src/wxproto.c     protocolo waylandx-unstable-v1
src/protocols.c   demais protocolos Wayland
src/x11clip.c     clipboard X11 nos dois sentidos, sem foco
src/xwlscale.c    X11 nítido em HiDPI (tela X escalada, xdg-output só para o Xwayland, Xft.dpi)
src/lock.c        ext-session-lock
src/idle.c        bloqueio e telas desligadas por inatividade
src/logind.c      bloquear antes de suspender, lock-session, brilho, MPRIS
src/nightlight.c  luz noturna
src/switcher.c    Alt+Tab
panel/            waylandx-panel: barras, launcher, notificações, bandeja, status, OSD, Alt+Tab
ctl/              waylandxctl
shims/            xdotool, wmctrl, xclip, xsel
session/          waylandx-session, waylandx-remote, .desktop, configs de portal
tests/            unitários, probes Wayland/X11/IME, suítes e2e sem tela
```

## Limites conhecidos

- **Apps X11 que ignoram o `Xft.dpi`** (Xlib/Motif antigos) ficam pequenos em HiDPI com o padrão `xwayland_scale = auto`; use `xwayland_scale = 1` para esticá-los. O cursor dos apps X11 sai no tamanho certo, mas não em resolução cheia.
- **A luz noturna** precisa da tabela de gamma do hardware (qualquer monitor real via DRM) ou do renderizador Vulkan; sessões aninhadas com GLES2 ficam sem o tom.
- **Os menus da bandeja** são só de mouse (sem navegação por teclado ainda); submenus abrem no lugar do menu pai.
- **As notificações** não têm painel de histórico nem imagens dentro do texto.
- **Botões da mesa digitalizadora** se associam à última mesa conectada (funciona com uma mesa; várias mesas ao mesmo tempo não são diferenciadas).
- **Ainda não testado em hardware real:** sessão TTY/DRM, VRR e tearing em monitor físico, multi-monitor físico, touchscreen e mesa digitalizadora. As suítes rodam sem tela e aninhadas; o código de touch e de mesa compila, mas não foi exercitado por um dispositivo real.

## Licença

MIT — veja [LICENSE](LICENSE).
