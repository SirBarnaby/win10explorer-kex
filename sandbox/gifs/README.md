# Recording the README GIFs

The GIFs in `assets/` are screen captures from Windows Sandbox. Nothing is recorded on the working desktop.

1. Start the lab Sandbox. Its logon script waits for requests in the mapped results folder.
2. Copy `Gif-Lib.ps1`, `Gif-Main.ps1` and `Record-Gifs.ps1` into the lab payload folder, next to `proof\` and `final\`.
3. Write the scenes to record into `gif-run.json` in the payload folder, for example `{"scenes":["tabs-overview","tab-reorder"]}`.
4. Write `{"action":"script","file":"Record-Gifs.ps1"}` to `request.json` in the results folder. Frames arrive in `results\gifs\<scene>\`, with `scene.json` and `status.json`.
5. On the host, run `python Fill-Frames.py <scene> <results\gifs>` and encode with ffmpeg:

```
ffmpeg -framerate 25 -i seq/<scene>/%04d.png -vf "split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=none:diff_mode=rectangle" -loop 0 <scene>.gif
```

Full-screen scenes (`tab-tear-out`, `touchpad-snap`) are captured at 20 fps and scaled with `scale=880:-1:flags=lanczos`. `options` is cropped to the Explorer window with `crop=866:633:127:50`.

Things to know:

- The Sandbox reports build 19041, so every window gets its tabs through the lab's `HeaderLab.exe --attach-only`, including the window a torn-out tab opens. The tab strip itself is the shipped `ExplorerTabs.dll`.
- The pointer and keys are real input sent with `SendInput`. Tab positions come from the strip's accessibility interface.
- A remote session has "Show animations in Windows" off. The recorder turns it on in the guest, as it is on a desktop PC.
- Pictures, Music and Videos add Explorer's own contextual title-bar tab, which moves the strip on every switch. The scenes use plain folders.
- The Sandbox has no touchpad. `touchpad-snap` sends Ctrl+Alt+Win+Arrow, the shortcut the three-finger swipes are bound to.
- A torn-out tab opens its window near (287, 110). The tear-out scene keeps the source window at the top right so neither strip covers the other.
