<div align="center">

# L2 Jikarus — Proximity Voice Chat

**Sistema de voz por proximidade nativo para Lineage 2**

![C++](https://img.shields.io/badge/C++-00599C?style=for-the-badge&logo=c%2B%2B&logoColor=white)
![Node.js](https://img.shields.io/badge/Node.js-339933?style=for-the-badge&logo=nodedotjs&logoColor=white)
![Win32](https://img.shields.io/badge/Win32_API-0078D4?style=for-the-badge&logo=windows&logoColor=white)
![DirectX 9](https://img.shields.io/badge/DirectX_9-FF0000?style=for-the-badge&logo=microsoftedge&logoColor=white)
![Java](https://img.shields.io/badge/Java-ED8B00?style=for-the-badge&logo=openjdk&logoColor=white)

</div>

---

## Demo

> 🎥 *GIF em breve — gravando demonstração in-game*

O indicador aparece no canto superior esquerdo do jogo em tempo real:

| Estado | Cor | Quando aparece |
|---|---|---|
| Inativo | ⬜ Cinza | Microfone aberto, sem fala |
| Transmitindo | 🟩 Verde | VAD detectou voz |
| Mutado | 🟥 Vermelho | Jogador silenciado |
| Recebendo | 🔵🔵🔵 Azul | Outro jogador falando próximo |

---

## Como funciona

```
┌─────────────────────────────────────────────────────────┐
│                     JOGADOR A                           │
│  Microfone → waveIn (16kHz PCM) → VAD → UDP:7779       │
└──────────────────────────┬──────────────────────────────┘
                           │ UDP
                           ▼
┌─────────────────────────────────────────────────────────┐
│              VOICE SERVER (Node.js)                     │
│  Recebe posição 3D + áudio → calcula distância →        │
│  volumeFactor = 1 - (dist / 2000) → roteia para         │
│  jogadores dentro do range                              │
└──────────────────────────┬──────────────────────────────┘
                           │ UDP
                           ▼
┌─────────────────────────────────────────────────────────┐
│                     JOGADOR B                           │
│  waveOut → playback com volume proporcional à dist      │
└─────────────────────────────────────────────────────────┘

GameServer Java → TCP:7778 → broadcast posições a cada 500ms
```

---

## Destaques Técnicos

### dinput8.dll — Proxy + Hook D3D9

Carrega automaticamente junto com `L2.exe` como proxy do DirectInput. Sem instalador, sem software extra para o jogador — basta colocar a DLL na pasta `system/`.

**Hook D3D9 via detour inline (restore-call-repatch):**
```cpp
// Salva 5 bytes originais → escreve JMP → no hook: restaura → chama original → repatcha
// Solução para Unreal Engine que cacheia o ponteiro Present na criação do device
unpatch(g_detourPresent);
HRESULT hr = originalPresent(dev, src, dst, wnd, rgn);
repatch(g_detourPresent, hookedPresent);
```

### Roteamento UDP por Proximidade

```javascript
const factor = Math.max(0.0, 1.0 - d / VOICE_RANGE); // volume cai linearmente
server.send(outPacket, other.port, other.address);
```

### VAD (Voice Activity Detection)

```cpp
// RMS sobre o frame PCM — só transmite se passar o threshold
float rms = sqrt(sumSq / FRAME_SAMPLES);
if (rms > g_vadThreshold) sendAudio();
```

---

## Arquitetura de Componentes

```
Mods/VoiceChat/
├── Client/
│   ├── dllmain.cpp          # DLL proxy + captura + overlay D3D9
│   ├── dinput8.def          # Export sem decoração stdcall
│   └── voice_config.ini     # ServerIP e VadThreshold
├── VoiceServer/
│   └── server.js            # Roteador UDP Node.js
└── Server/
    ├── VoiceChatManager.java # TCP broadcast de posições
    └── VoiceCommand.java     # Comando .voice no chat
```

---

## Build

**Requisitos:** MSYS2 + MinGW-w32 (g++ 32-bit), Node.js 18+

```bash
# Client (DLL)
g++ -m32 -shared -O2 -std=c++17 -o dinput8.dll dllmain.cpp dinput8.def \
    -ld3d9 -ldxguid -lws2_32 -lwinmm -lgdi32 -luser32

# Voice Server
node VoiceServer/server.js
```

## Instalação

```
1. Copiar dinput8.dll  →  L2/system/dinput8.dll
2. Editar voice_config.ini com o IP do servidor
3. Iniciar node server.js no servidor
4. Abrir o jogo — overlay aparece automaticamente
```

---

## Desenvolvido por

**TECX SOFTHOUSE** — [github.com/MANINtecn](https://github.com/MANINtecn)