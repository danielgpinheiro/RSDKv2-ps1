# FMV playback lento — análise (CD seek + double-buffer + conversão)

Data: 2026-09-22. Objetivo: responder "é problema de playback ou de decode do MDEC?"
e auditar a conversão .rsv → .mdec/.str quanto ao framerate.

## Medição (PCSX-Redux, `getFrameCount()` = contador de VBlank ~60 Hz — o único relógio
confiável; o COP0 Count lê 0 sob o interpretador e o RC0 envolve rápido demais)

```
[video] f=316 read=0 vlc=0 mdec=0            (por 16 frames, video.cpp)
[video] f=336 decode=0 upload=0 (64 ticks)   (por 64 ticks, main.cpp)
[video] f=652 ...  -> delta f ~336..352 por 64 ticks
```

- **Frame loop ≈ 11 fps** (64 ticks = ~336–352 VBlanks ≈ 88–92 ms/tick), não 60 fps.
- **Vídeo ≈ 2,8 fps** (16 frames = ~5,6–5,9 s), não os 15 fps do arquivo.
- `decode=0` e `upload=0`: o `PS1VideoNextFrame()` (VLC+MDEC) e o `uploadToVRAM`
  (16 slices) juntos cabem em <1 VBlank — **não são o gargalo**.
- `read=0`: o `getFrameCount()` NÃO avança durante o `fread` — o
  `readSectorsBlocking` desliga as IRQs durante a leitura do CD. O custo real do
  `fread` (seek + leitura dos 10 setores) é o que estoura o frame loop.

## Veredito

**É problema de PLAYBACK/STREAMING, não de decode do MDEC.**

O decode VLC (software) + MDEC (hardware `DecDCTin/out`) é rápido (<1 VBlank).
O gargalo é o `fread` síncrono em `ps1/cdrom_fs.cpp:168`:

```
g_ps1CDRom->readSectorsBlocking(sector, sectors, s_batch, *g_ps1GPU)
```

- **CD seek**: o `readSectorsBlocking` faz `SETLOC` (seek) + `READ` a cada frame.
  Mesmo com o arquivo contíguo (frames sequenciais ⇒ seek ~0 entre frames), a
  leitura bloqueante de 10 setores (20480 B) a ~2x CD ≈ 68 ms + latência do
  controlador ≈ 2 ms ⇒ **~70–250 ms por frame** no emulador (que modela a
  latência), ~3–4× o orçamento de 66,7 ms/frame a 15 fps.
- **Double-buffer**: não há. O fluxo é serial — `fread` (bloqueante) → VLC → MDEC
  → upload → flip. A leitura do frame N+1 não se sobrepõe à exibição do frame N,
  e o `fread` bloqueante paralisa o `frame()` a cada 4 ticks.

## Conversão .rsv → .mdec/.str (framerate)

- `.rsv` = **936 frames** GIF, 256×160, tocados a **~15 fps** (o script
  `NexusIntro/Intro.txt` case 2 chama `NextVideoFrame()` a cada 4 ticks de 60 Hz;
  `Object.Value1==1008` é só o disparador do fade, o vídeo congela após o frame 936).
- `.mdec` (`convert_fmv.py`) = **936 frames** (correto). É RLE cru, sem framerate —
  a cadência é do player.
- `.str` (psxavenc `-r 15`) = **934 frames** (19 128 320 B / 20 480 = 934,0) — **2
  frames a menos** que o .rsv. Desvio de ~0,13 s, desprezível; **o framerate 15 fps
  está correto** e bate com a cadência do script.
- **Conclusão**: a conversão não tem problema de framerate. O "lento" vem do
  streaming síncrono, não da codificação.

## Correção recomendada (próxima etapa)

1. **Leitura assíncrona + double-buffer**: `readSectors` (callback) para ler o frame
   N+1 em ping-pong (`s_frame[2]`) enquanto o MDEC decodifica/exibe o frame N.
2. **MDEC async**: não sincronizar o `DecDCToutSync` imediatamente; sobrepor com a
   leitura seguinte.
3. **Upload só quando o frame muda** (hoje reenvia as 16 slices a cada tick).
