# VoiceChat — Instalação

## Arquitetura

```
GameServer (Java) ──porta 7778──▶ DLL no cliente ──MumbleLink──▶ Mumble
```

O GameServer transmite posições dos jogadores próximos.
A DLL lê essa posição e atualiza o Mumble que ajusta o volume por distância.
Range: 2000 unidades L2 (~50m no jogo, um pouco maior que range de party).

## Parte 1 — Servidor (Java)

1. Copie `VoiceChatManager.java` para:
   `source/java/net/sf/l2jdev/gameserver/model/voicechat/`

2. Copie `VoiceCommand.java` para:
   `source/java/net/sf/l2jdev/gameserver/handler/voicedcommandhandlers/`

3. Em `GameServer.java` (startup), adicione:
   ```java
   VoiceChatManager.getInstance().start();
   ```

4. Registre o comando no HandlerManager:
   ```java
   VoicedCommandHandler.getInstance().registerHandler(new VoiceCommand());
   ```

5. Abra a porta 7778 TCP no firewall se necessário.

## Parte 2 — DLL Cliente

### Compilar (Visual Studio 2019+)
```
mkdir build && cd build
cmake -A Win32 ..        # IMPORTANTE: 32-bit porque L2.exe é 32-bit
cmake --build . --config Release
```
Gera: `build/Release/dinput8.dll`

### Instalar no cliente
1. Copie `dinput8.dll` para a pasta raiz do cliente L2 (onde está `l2.exe`)
2. Copie `voice_config.ini` para a mesma pasta
3. Se jogar por rede, edite `voice_config.ini` e coloque o IP do servidor

## Parte 3 — Mumble

1. Baixe e instale o Mumble: https://www.mumble.info
2. Configure um servidor Murmur (porta padrão 64738)
3. No Mumble: Configure → Settings → Plugins → habilite "Link to Game and Transmit Position"
4. Conecte todos os jogadores no mesmo servidor Mumble

## Usar no jogo

- `.voice` — liga/desliga o chat de voz (aparece mensagem na tela)
- Sem comandos adicionais — funciona automaticamente por aproximação