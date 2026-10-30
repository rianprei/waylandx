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

| Do X11 (agora também para apps Wayland) | Como |
|---|---|
| App escolhe a própria posição | protocolo `waylandx-unstable-v1` (Wayland) e configure requests (X11) |
| Always-on-top / below / sticky | `_NET_WM_STATE` (X11), protocolo próprio (Wayland), `waylandxctl above` |
| Captura de tela sem diálogo | `waylandxctl screenshot`, screencopy, ext-image-copy-capture, portal sem chooser |
| Automação de input (xdotool) | `waylandxctl type/key/click/mousemove`, virtual-keyboard/pointer abertos, shim `xdotool` |
| Introspecção e controle (wmctrl/xprop) | `waylandxctl list/info/move/close…`, shims `wmctrl`, foreign-toplevel |
| Atalhos globais para qualquer programa | `waylandxctl bind COMBO CMD` |
| Clipboard acessível de qualquer lugar | data-control (wl-clipboard) + shims `xclip`/`xsel` |
| Rodar app remoto com janela local (ssh -X) | `waylandx-remote host app` (waypipe), `ssh -X` via Xwayland |
| WM tradicional: flutuante, decorações, arrastar com Mod | decorações desenhadas pelo servidor, Mod+arrastar, snap; tiling opcional |
| Política aberta | qualquer cliente local pode capturar e injetar; só o socket IPC é restrito ao seu usuário (0600) |

## Compilar e testar

Dependências (Arch/CachyOS): `wlroots0.20 wayland wayland-protocols libxkbcommon libinput pixman libxcb xcb-util-wm cairo pango xorg-xwayland meson ninja`.
Para os testes e extras: `xclip wl-clipboard waypipe file`, e opcionalmente `swaylock xdotool wmctrl xdg-desktop-portal-wlr`.

```sh
meson setup build --prefix="$HOME/.local"
ninja -C build
meson test -C build
```

O `meson test` roda os testes unitários e duas suítes de ponta a ponta sem tela (`tests/run-headless.sh` e
`tests/run-multihead.sh`). Elas usam clientes Wayland e X11 de verdade para testar injeção de input, IME,
screenshots, regras, shims, clipboard, workspaces em dois monitores, tiling, painel, waypipe, lock e reload
de config. O CI roda tudo normalmente e com ASan/UBSan.

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
| Mod+Tab | alterna janelas |
| Mod+←/→, Mod+↑/↓ | meia tela, maximizar/restaurar |
| Mod+1..9, Mod+Shift+1..9 | trocar de workspace neste monitor / mover janela |
| Mod+H / Mod+L | diminuir/aumentar a área master |
| Print / Mod+Shift+S | screenshot da tela / da janela |
| Mod+Shift+R / Mod+Shift+E | recarregar config / sair |
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
src/lock.c        ext-session-lock
panel/            waylandx-panel (uma barra por monitor + launcher)
ctl/              waylandxctl
shims/            xdotool, wmctrl, xclip, xsel
session/          waylandx-session, waylandx-remote, .desktop, configs de portal
tests/            unitários, probes Wayland/X11/IME, suítes e2e sem tela
```

## Limites conhecidos

- **Apps X11 em monitores com escala > 1** são ampliados (ficam borrados). O wlroots 0.20 não tem API de escala para o Xwayland.
- **Clipboard X11 sem foco:** o wlroots só deixa um cliente X11 ler ou escrever o clipboard enquanto alguma janela X11 tem foco. Apps X11 focados funcionam normalmente e scripts usam os shims; um gerenciador de clipboard X11 em segundo plano exigiria um patch no wlroots.
- **Botões da própria mesa digitalizadora** (pad: botões, anéis, faixas) e o sensor de tampa do notebook ainda não são tratados.
- **Ainda não testado em hardware real:** sessão TTY/DRM, VRR e tearing em monitor físico, multi-monitor físico, touchscreen e mesa digitalizadora. As suítes rodam sem tela e aninhadas; o código de touch e de mesa compila, mas não foi exercitado por um dispositivo real.

## Licença

MIT — veja [LICENSE](LICENSE).
