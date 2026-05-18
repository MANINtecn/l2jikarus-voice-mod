# L2 Jikarus — Proximity Voice Chat Mod

Sistema de voz por proximidade para servidor Lineage 2 privado, desenvolvido do zero sem uso de software externo para o jogador.

## Como funciona

```
Microfone do jogador
    │
    ▼
dinput8.dll (proxy + captura de áudio)
    │  UDP 7779
    ▼
Voice Server (Node.js) — roteia por proximidade
    │  UDP 7779
    ▼
dinput8.dll (playback) → Alto-falante do jogador próximo
```

O servidor Java envia posições dos jogadores via TCP 7778 a cada 500ms.  
O áudio só chega a jogadores dentro de 2000 unidades L2. O volume cai linearmente com a distância.

## Componentes

| Pasta | Descrição |
|---|---|
| `Client/` | DLL proxy + captura/playback de áudio + overlay D3D9 |
| `VoiceServer/` | Roteador UDP em Node.js |

## Tecnologias

- **C++ (Win32)** — dinput8.dll proxy, `waveIn/waveOut` (16kHz PCM), hook D3D9 Present via detour inline
- **Node.js** — roteador UDP stateless com cálculo de distância 3D
- **Java** — integração com GameServer L2J para broadcast de posições via TCP

## Overlay in-game

Indicador visual desenhado diretamente no DirectX 9 (sem janela separada):

| Cor | Estado |
|---|---|
| Cinza | Microfone inativo |
| Verde | Transmitindo (VAD ativo) |
| Vermelho | Mutado |
| Azul (3 pontos) | Recebendo áudio de jogador próximo |

## Build

```bash
# Requer MSYS2 mingw32 (g++ 32-bit)
g++ -m32 -shared -O2 -std=c++17 -o dinput8.dll dllmain.cpp dinput8.def \
    -ld3d9 -ldxguid -lws2_32 -lwinmm -lgdi32 -luser32
```

## Configuração (`voice_config.ini`)

```ini
ServerIP=127.0.0.1
VadThreshold=400
```

## Desenvolvido por

**TECX SOFTHOUSE** — [github.com/MANINtecn](https://github.com/MANINtecn)