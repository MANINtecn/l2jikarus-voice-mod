// L2 Samurai Crow — VoiceProxy.exe
// Executa junto com o L2. Conecta ao GameServer (porta 7778) e alimenta o Mumble com posicoes.
// Compilar: g++ -o VoiceProxy.exe VoiceProxy.cpp -lws2_32 -static

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <iostream>
#include <atomic>
#include <thread>
#include <csignal>

#pragma comment(lib, "ws2_32.lib")

struct LinkedMem
{
	UINT32  uiVersion;
	UINT32  uiTick;
	float   fAvatarPosition[3];
	float   fAvatarFront[3];
	float   fAvatarTop[3];
	wchar_t name[256];
	float   fCameraPosition[3];
	float   fCameraFront[3];
	float   fCameraTop[3];
	wchar_t identity[256];
	UINT32  context_len;
	BYTE    context[256];
	wchar_t description[2048];
};

static std::atomic<bool> g_running { true };
static LinkedMem* g_lm   = nullptr;
static HANDLE     g_file = nullptr;

static constexpr float L2_SCALE = 0.025f;

static void onSignal(int) { g_running = false; }

static std::string readConfig(const std::string& path, const char* key, const char* def)
{
	char buf[256];
	GetPrivateProfileStringA("VoiceChat", key, def, buf, sizeof(buf), path.c_str());
	return buf;
}

static bool initMumbleLink()
{
	g_file = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LinkedMem), L"MumbleLink");
	if (!g_file) { std::cerr << "Erro: nao foi possivel criar MumbleLink\n"; return false; }

	g_lm = reinterpret_cast<LinkedMem*>(MapViewOfFile(g_file, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LinkedMem)));
	if (!g_lm) { std::cerr << "Erro: nao foi possivel mapear MumbleLink\n"; return false; }

	ZeroMemory(g_lm, sizeof(LinkedMem));
	g_lm->uiVersion = 2;
	wcscpy_s(g_lm->name,        L"L2SamuraiCrow");
	wcscpy_s(g_lm->description, L"Lineage 2 Samurai Crow proximity voice");
	g_lm->fAvatarFront[2] = 1.f;
	g_lm->fAvatarTop[1]   = 1.f;
	g_lm->fCameraFront[2] = 1.f;
	g_lm->fCameraTop[1]   = 1.f;
	return true;
}

static void parseLine(const std::string& line)
{
	if (line.rfind("SELF:", 0) != 0 || !g_lm) return;
	float x = 0, y = 0, z = 0;
	if (sscanf_s(line.c_str() + 5, "%f,%f,%f", &x, &y, &z) == 3)
	{
		g_lm->fAvatarPosition[0] = x * L2_SCALE;
		g_lm->fAvatarPosition[1] = z * L2_SCALE;
		g_lm->fAvatarPosition[2] = y * L2_SCALE;
		g_lm->fCameraPosition[0] = g_lm->fAvatarPosition[0];
		g_lm->fCameraPosition[1] = g_lm->fAvatarPosition[1];
		g_lm->fCameraPosition[2] = g_lm->fAvatarPosition[2];
		g_lm->uiTick++;
	}
}

int main()
{
	std::signal(SIGINT,  onSignal);
	std::signal(SIGTERM, onSignal);

	char exePath[MAX_PATH];
	GetModuleFileNameA(nullptr, exePath, MAX_PATH);
	std::string dir(exePath);
	dir = dir.substr(0, dir.rfind('\\'));
	std::string cfgPath = dir + "\\voice_config.ini";

	std::string serverIp = readConfig(cfgPath, "ServerIP", "127.0.0.1");

	std::cout << "=== L2 Samurai Crow — VoiceProxy ===\n";
	std::cout << "Servidor: " << serverIp << ":7778\n";
	std::cout << "Pressione Ctrl+C para sair.\n\n";

	if (!initMumbleLink()) return 1;

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	while (g_running)
	{
		SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port   = htons(7778);
		inet_pton(AF_INET, serverIp.c_str(), &addr.sin_addr);

		std::cout << "Conectando ao servidor...\n";
		if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
		{
			std::cout << "Falha na conexao. Tentando novamente em 3s...\n";
			closesocket(sock);
			Sleep(3000);
			continue;
		}

		std::cout << "Conectado! Chat de voz por aproximacao ativo.\n";
		std::string buffer;
		char tmp[4096];

		while (g_running)
		{
			int n = recv(sock, tmp, sizeof(tmp) - 1, 0);
			if (n <= 0) break;
			tmp[n] = '\0';
			buffer += tmp;

			size_t nl;
			while ((nl = buffer.find('\n')) != std::string::npos)
			{
				std::string line = buffer.substr(0, nl);
				buffer = buffer.substr(nl + 1);
				if (!line.empty() && line.back() == '\r') line.pop_back();
				parseLine(line);
			}
		}

		closesocket(sock);
		if (g_running)
		{
			std::cout << "Conexao perdida. Reconectando em 2s...\n";
			Sleep(2000);
		}
	}

	if (g_lm)   UnmapViewOfFile(g_lm);
	if (g_file) CloseHandle(g_file);
	WSACleanup();
	std::cout << "VoiceProxy encerrado.\n";
	return 0;
}