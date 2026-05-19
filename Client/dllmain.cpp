// L2 Samurai Crow — Native Voice Chat (dinput8.dll proxy)
// Carrega automático com L2.exe. Zero software extra para o jogador.
// Overlay desenhado via hook D3D9 Present — aparece dentro do jogo.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmsystem.h>
#include <d3d9.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

// ── Áudio ─────────────────────────────────────────────────────────────────────
static constexpr int   SAMPLE_RATE   = 16000;
static constexpr int   CHANNELS      = 1;
static constexpr int   BITS          = 16;
static constexpr int   FRAME_SAMPLES = 320;    // 20ms a 16kHz
static constexpr int   FRAME_BYTES   = FRAME_SAMPLES * 2;
static constexpr int   CAPTURE_BUFS  = 4;
static float           g_vadThreshold = 400.0f;

// ── Protocolo ─────────────────────────────────────────────────────────────────
static constexpr uint32_t PKT_AUDIO      = 1;
static constexpr int      VOICE_PORT_UDP = 7779;
static constexpr int      POS_PORT_TCP   = 7778;
static constexpr int      NAME_LEN       = 32;

// OutPacket: type(4)+x(4)+y(4)+z(4)+partyId(4)+name(32)+samples(640) = 692
// InPacket : speakerId(4)+volumeFactor(4)+name(32)+samples(640)       = 680
#pragma pack(push, 1)
struct OutPacket {
    uint32_t type;
    float    x, y, z;
    uint32_t partyId;
    char     name[NAME_LEN];
    int16_t  samples[FRAME_SAMPLES];
};
struct InPacket {
    uint32_t speakerId;
    float    volumeFactor;
    char     name[NAME_LEN];
    int16_t  samples[FRAME_SAMPLES];
};
#pragma pack(pop)

// ── Proxy dinput8.dll ─────────────────────────────────────────────────────────
typedef HRESULT(WINAPI* DI8Create_fn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static HMODULE      g_realDll = nullptr;
static DI8Create_fn g_realFn  = nullptr;

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* p, LPUNKNOWN u) {
    return g_realFn ? g_realFn(h, v, r, p, u) : E_FAIL;
}

// ── Speaker tracking ──────────────────────────────────────────────────────────
struct PartyMember {
    char  name[NAME_LEN];
    DWORD lastTalkMs;   // 0 = nunca falou
    bool  muted;
    float rowY;         // posição Y na tela (atualizado a cada frame)
};
struct NearbyEntry {
    uint32_t id;
    char     name[NAME_LEN];
    DWORD    lastTalkMs;
    bool     muted;
    float    rowY;
};

static std::vector<PartyMember>                  g_party;
static std::mutex                                g_partyMtx;
static std::unordered_map<uint32_t, NearbyEntry> g_nearby;
static std::mutex                                g_nearbyMtx;

// Dados do jogador local (vindos do TCP)
static char     g_selfName[NAME_LEN] = {};
static uint32_t g_selfPartyId = 0;

// ── Globals ───────────────────────────────────────────────────────────────────
static HMODULE           g_dllModule = nullptr;
static std::atomic<bool> g_run       { false };
static std::atomic<bool> g_muted     { false };

static SOCKET     g_udp = INVALID_SOCKET;
static SOCKET     g_tcp = INVALID_SOCKET;
static sockaddr_in g_serverAddr{};

static std::atomic<float> g_posX{0}, g_posY{0}, g_posZ{0};

// Captura
static HWAVEIN  g_waveIn = nullptr;
static WAVEHDR  g_capHdr[CAPTURE_BUFS];
static int16_t  g_capBuf[CAPTURE_BUFS][FRAME_SAMPLES];

// Reprodução: um HWAVEOUT por speakerId
struct Speaker {
    HWAVEOUT hOut = nullptr;
    std::mutex mtx;
    ~Speaker() { if (hOut) { waveOutReset(hOut); waveOutClose(hOut); } }
};
static std::unordered_map<uint32_t, Speaker*> g_speakers;
static std::mutex                              g_spkMtx;

typedef HRESULT(WINAPI* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

// ── Log de debug ──────────────────────────────────────────────────────────────
static std::string g_logPath;
static std::mutex  g_logMtx;

static void dbgLog(const char* fmt, ...) {
    if (g_logPath.empty()) return;
    char msg[512];
    va_list a; va_start(a, fmt); vsnprintf(msg, sizeof(msg), fmt, a); va_end(a);
    std::lock_guard<std::mutex> lk(g_logMtx);
    FILE* f = nullptr;
    fopen_s(&f, g_logPath.c_str(), "a");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
}

// ── voice_config.ini ──────────────────────────────────────────────────────────
static std::string cfgGet(const char* key, const char* def) {
    char path[MAX_PATH], buf[256];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string ini = std::string(path).substr(0, std::string(path).rfind('\\')) + "\\voice_config.ini";
    GetPrivateProfileStringA("VoiceChat", key, def, buf, sizeof(buf), ini.c_str());
    return buf;
}

// ── VAD ───────────────────────────────────────────────────────────────────────
static bool voiceActive(const int16_t* s, int n) {
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)s[i] * s[i];
    return std::sqrt(sum / n) > g_vadThreshold;
}

// ── Reprodução ────────────────────────────────────────────────────────────────
static WAVEFORMATEX makeWfx() {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = CHANNELS;
    wfx.nSamplesPerSec  = SAMPLE_RATE;
    wfx.wBitsPerSample  = BITS;
    wfx.nBlockAlign     = CHANNELS * (BITS / 8);
    wfx.nAvgBytesPerSec = SAMPLE_RATE * wfx.nBlockAlign;
    return wfx;
}

static Speaker* getSpeaker(uint32_t id) {
    std::lock_guard<std::mutex> lk(g_spkMtx);
    auto it = g_speakers.find(id);
    if (it != g_speakers.end()) return it->second;
    auto* s = new Speaker();
    WAVEFORMATEX wfx = makeWfx();
    if (waveOutOpen(&s->hOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        delete s; return nullptr;
    }
    g_speakers[id] = s;
    return s;
}

static void playAudio(uint32_t id, float vol, const int16_t* samples) {
    Speaker* s = getSpeaker(id);
    if (!s || !s->hOut) return;
    auto* buf = new int16_t[FRAME_SAMPLES];
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        float v = samples[i] * vol * 4.0f;
        if (v >  32767.f) v =  32767.f;
        if (v < -32768.f) v = -32768.f;
        buf[i] = (int16_t)v;
    }
    auto* hdr = new WAVEHDR{};
    hdr->lpData = (LPSTR)buf; hdr->dwBufferLength = FRAME_BYTES; hdr->dwUser = (DWORD_PTR)buf;
    waveOutPrepareHeader(s->hOut, hdr, sizeof(WAVEHDR));
    waveOutWrite(s->hOut, hdr, sizeof(WAVEHDR));
    std::thread([s, hdr]() {
        while (!(hdr->dwFlags & WHDR_DONE)) Sleep(1);
        waveOutUnprepareHeader(s->hOut, hdr, sizeof(WAVEHDR));
        delete[] (int16_t*)hdr->dwUser; delete hdr;
    }).detach();
}

// ── recvThread ────────────────────────────────────────────────────────────────
static void recvThread() {
    dbgLog("recvThread iniciado");
    InPacket pkt; sockaddr_in from{}; int fromLen = sizeof(from);
    while (g_run) {
        int n = recvfrom(g_udp, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&from, &fromLen);
        if (n != (int)sizeof(InPacket)) continue;
        pkt.name[NAME_LEN - 1] = '\0';

        bool isMuted  = false;
        bool isParty  = false;

        // Verifica party
        {
            std::lock_guard<std::mutex> lk(g_partyMtx);
            for (auto& pm : g_party) {
                if (strncmp(pm.name, pkt.name, NAME_LEN) == 0) {
                    pm.lastTalkMs = GetTickCount();
                    isMuted = pm.muted;
                    isParty = true;
                    break;
                }
            }
        }

        // Se não é party, rastreia como próximo
        if (!isParty) {
            std::lock_guard<std::mutex> lk(g_nearbyMtx);
            auto it = g_nearby.find(pkt.speakerId);
            if (it == g_nearby.end()) {
                NearbyEntry ne{};
                ne.id = pkt.speakerId;
                strncpy_s(ne.name, pkt.name, NAME_LEN - 1);
                ne.lastTalkMs = GetTickCount();
                g_nearby[pkt.speakerId] = ne;
            } else {
                it->second.lastTalkMs = GetTickCount();
                strncpy_s(it->second.name, pkt.name, NAME_LEN - 1);
                isMuted = it->second.muted;
            }
        }

        if (!isMuted && !g_muted.load())
            playAudio(pkt.speakerId, pkt.volumeFactor, pkt.samples);
    }
}

// ── waveInCb ─────────────────────────────────────────────────────────────────
static void CALLBACK waveInCb(HWAVEIN, UINT msg, DWORD_PTR, DWORD_PTR p1, DWORD_PTR) {
    if (msg != WIM_DATA) return;
    WAVEHDR* hdr = (WAVEHDR*)p1;
    if (!hdr || hdr->dwBytesRecorded == 0) { if (hdr) waveInAddBuffer(g_waveIn, hdr, sizeof(WAVEHDR)); return; }
    if (!g_muted) {
        const int16_t* samples = (const int16_t*)hdr->lpData;
        if (voiceActive(samples, FRAME_SAMPLES) && g_udp != INVALID_SOCKET) {
            OutPacket pkt{};
            pkt.type    = PKT_AUDIO;
            pkt.x       = g_posX; pkt.y = g_posY; pkt.z = g_posZ;
            pkt.partyId = g_selfPartyId;
            strncpy_s(pkt.name, g_selfName, NAME_LEN - 1);
            memcpy(pkt.samples, samples, FRAME_BYTES);
            sendto(g_udp, (const char*)&pkt, sizeof(pkt), 0, (sockaddr*)&g_serverAddr, sizeof(g_serverAddr));
        }
    }
    waveInAddBuffer(g_waveIn, hdr, sizeof(WAVEHDR));
}

// ── posThread ─────────────────────────────────────────────────────────────────
static void posThread() {
    std::string ip = cfgGet("ServerIP", "127.0.0.1");
    dbgLog("posThread: %s:%d", ip.c_str(), POS_PORT_TCP);
    while (g_run) {
        g_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(POS_PORT_TCP);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        if (connect(g_tcp, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(g_tcp); Sleep(3000); continue;
        }
        dbgLog("posThread: conectado");
        std::string buf; char tmp[2048];
        while (g_run) {
            int n = recv(g_tcp, tmp, sizeof(tmp) - 1, 0);
            if (n <= 0) break;
            tmp[n] = '\0'; buf += tmp;
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl); buf = buf.substr(nl + 1);

                // SELF:nome:x,y,z
                if (line.rfind("SELF:", 0) == 0) {
                    const char* p = line.c_str() + 5;
                    const char* colon = strchr(p, ':');
                    if (colon) {
                        size_t nameLen = (size_t)(colon - p);
                        if (nameLen >= NAME_LEN) nameLen = NAME_LEN - 1;
                        strncpy_s(g_selfName, p, nameLen);
                        g_selfName[nameLen] = '\0';
                        float x, y, z;
                        if (sscanf_s(colon + 1, "%f,%f,%f", &x, &y, &z) == 3)
                            g_posX = x, g_posY = y, g_posZ = z;
                    } else {
                        float x, y, z;
                        if (sscanf_s(p, "%f,%f,%f", &x, &y, &z) == 3)
                            g_posX = x, g_posY = y, g_posZ = z;
                    }
                }

                // PARTY:partyId:nome1;nome2;...  ou  PARTY: (sem party)
                else if (line.rfind("PARTY:", 0) == 0) {
                    const char* p = line.c_str() + 6;
                    const char* colon = strchr(p, ':');

                    std::lock_guard<std::mutex> lk(g_partyMtx);
                    if (colon && colon > p) {
                        g_selfPartyId = (uint32_t)atoi(p);
                        const char* nameList = colon + 1;

                        // Reconstrói lista preservando flags de mute
                        std::vector<PartyMember> newParty;
                        char tmp2[512];
                        strncpy_s(tmp2, nameList, sizeof(tmp2) - 1);
                        char* ctx = nullptr;
                        char* tok = strtok_s(tmp2, ";", &ctx);
                        while (tok && tok[0]) {
                            PartyMember pm{};
                            strncpy_s(pm.name, tok, NAME_LEN - 1);
                            // Preserva mute e lastTalk de entrada anterior
                            for (const auto& old : g_party) {
                                if (strncmp(old.name, pm.name, NAME_LEN) == 0) {
                                    pm.muted     = old.muted;
                                    pm.lastTalkMs = old.lastTalkMs;
                                    break;
                                }
                            }
                            newParty.push_back(pm);
                            tok = strtok_s(nullptr, ";", &ctx);
                        }
                        g_party = std::move(newParty);
                    } else {
                        g_selfPartyId = 0;
                        g_party.clear();
                    }
                }
            }
        }
        closesocket(g_tcp); dbgLog("posThread: desconectado");
        if (g_run) Sleep(1000);
    }
}

// ── startCapture ──────────────────────────────────────────────────────────────
static void startCapture() {
    WAVEFORMATEX wfx = makeWfx();
    if (waveInOpen(&g_waveIn, WAVE_MAPPER, &wfx, (DWORD_PTR)waveInCb, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        dbgLog("waveInOpen FALHOU"); return;
    }
    dbgLog("waveInOpen OK");
    for (int i = 0; i < CAPTURE_BUFS; i++) {
        ZeroMemory(&g_capHdr[i], sizeof(WAVEHDR));
        g_capHdr[i].lpData = (LPSTR)g_capBuf[i]; g_capHdr[i].dwBufferLength = FRAME_BYTES;
        waveInPrepareHeader(g_waveIn, &g_capHdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_waveIn, &g_capHdr[i], sizeof(WAVEHDR));
    }
    waveInStart(g_waveIn);
    dbgLog("captura iniciada VAD=%.0f", g_vadThreshold);
}

// ── D3D9 overlay ──────────────────────────────────────────────────────────────
struct PVert { float x, y, z, rhw; DWORD color; };

static void fillRect(IDirect3DDevice9* dev, float x, float y, float w, float h, DWORD col) {
    PVert v[] = {
        {x,   y,   0, 1, col}, {x+w, y,   0, 1, col},
        {x,   y+h, 0, 1, col}, {x+w, y+h, 0, 1, col}
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(PVert));
}

// Ícone de microfone pequeno (14×16 px)
static void drawMic(IDirect3DDevice9* dev, float x, float y, DWORD col) {
    fillRect(dev, x+3,  y+0,  8.0f, 9.0f,  col); // corpo
    fillRect(dev, x+6,  y+9,  2.0f, 4.0f,  col); // haste
    fillRect(dev, x+3,  y+13, 8.0f, 2.0f,  col); // base
}

static constexpr float OX          = 10.0f;
static constexpr float OY_BASE     = 40.0f;  // 30px abaixo do antigo (era 10)
static constexpr float ROW_H       = 20.0f;
static constexpr float ROW_W       = 155.0f;
static constexpr float TEXT_X_OFF  = 20.0f;  // offset x do texto em relação a OX
static constexpr DWORD TALK_MS     = 600;     // ms para considerar "falando"

// Fase 1: retângulos D3D9 (dentro de BeginScene)
static void drawVoiceRects(IDirect3DDevice9* dev) {
    DWORD now = GetTickCount();

    // ── Salva estados ────────────────────────────────────────────────────────
    DWORD sFVF, sZ, sAlpha, sSrc, sDst, sLight, sCull;
    DWORD sCop, sCarg, sAop, sAarg;
    IDirect3DBaseTexture9*  sTex = nullptr;
    IDirect3DVertexShader9* sVS  = nullptr;
    IDirect3DPixelShader9*  sPS  = nullptr;

    dev->GetFVF(&sFVF);
    dev->GetRenderState(D3DRS_ZENABLE,          &sZ);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &sAlpha);
    dev->GetRenderState(D3DRS_SRCBLEND,         &sSrc);
    dev->GetRenderState(D3DRS_DESTBLEND,        &sDst);
    dev->GetRenderState(D3DRS_LIGHTING,         &sLight);
    dev->GetRenderState(D3DRS_CULLMODE,         &sCull);
    dev->GetTextureStageState(0, D3DTSS_COLOROP,   &sCop);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &sCarg);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP,   &sAop);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &sAarg);
    dev->GetTexture(0, &sTex);
    dev->GetVertexShader(&sVS);
    dev->GetPixelShader(&sPS);

    // ── Pipeline 2D ──────────────────────────────────────────────────────────
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetRenderState(D3DRS_ZENABLE,          FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
    dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTexture(0, nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);

    float oy = OY_BASE;

    // Membros de party (sempre visíveis — cinza=idle, verde=falando)
    {
        std::lock_guard<std::mutex> lk(g_partyMtx);
        for (auto& pm : g_party) {
            bool talking = pm.lastTalkMs && (now - pm.lastTalkMs) < TALK_MS;
            pm.rowY = oy;

            DWORD micCol;
            if      (pm.muted)  micCol = D3DCOLOR_ARGB(230, 180, 40,  40);   // vermelho
            else if (talking)   micCol = D3DCOLOR_ARGB(230, 50,  200, 50);   // verde
            else                micCol = D3DCOLOR_ARGB(160, 120, 120, 120);  // cinza idle

            fillRect(dev, OX, oy, ROW_W, ROW_H - 2, D3DCOLOR_ARGB(120, 8, 8, 8));
            drawMic(dev, OX + 2, oy + 1, micCol);
            oy += ROW_H;
        }
    }

    // Jogadores próximos (apenas quando estão falando)
    {
        std::lock_guard<std::mutex> lk(g_nearbyMtx);
        for (auto& [id, ne] : g_nearby) {
            if (!ne.lastTalkMs || (now - ne.lastTalkMs) >= TALK_MS) continue;
            ne.rowY = oy;

            DWORD micCol = ne.muted
                ? D3DCOLOR_ARGB(230, 180, 40,  40)
                : D3DCOLOR_ARGB(230, 50,  200, 50);

            fillRect(dev, OX, oy, ROW_W, ROW_H - 2, D3DCOLOR_ARGB(120, 8, 8, 8));
            drawMic(dev, OX + 2, oy + 1, micCol);
            oy += ROW_H;
        }
    }

    // ── Restaura estados ──────────────────────────────────────────────────────
    dev->SetFVF(sFVF);
    dev->SetRenderState(D3DRS_ZENABLE,          sZ);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, sAlpha);
    dev->SetRenderState(D3DRS_SRCBLEND,         sSrc);
    dev->SetRenderState(D3DRS_DESTBLEND,        sDst);
    dev->SetRenderState(D3DRS_LIGHTING,         sLight);
    dev->SetRenderState(D3DRS_CULLMODE,         sCull);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   sCop);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, sCarg);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   sAop);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, sAarg);
    dev->SetTexture(0, sTex);
    if (sTex) sTex->Release();
    dev->SetVertexShader(sVS);
    if (sVS) sVS->Release();
    dev->SetPixelShader(sPS);
    if (sPS) sPS->Release();
}

// Fase 2: texto GDI no backbuffer (fora de BeginScene) + detecção de clique
static void drawVoiceText(IDirect3DDevice9* dev, HWND hwnd) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;

    HDC hdc = nullptr;
    if (FAILED(bb->GetDC(&hdc))) { bb->Release(); return; }

    // Cache da fonte
    static HFONT s_font = nullptr;
    if (!s_font)
        s_font = CreateFontA(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, "Arial");
    HFONT oldFont = s_font ? (HFONT)SelectObject(hdc, s_font) : nullptr;
    SetBkMode(hdc, TRANSPARENT);

    // Detecção de clique (uma vez por pressão, não por frame)
    static bool s_lastBtn = false;
    bool lBtn    = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool clicked = lBtn && !s_lastBtn;
    s_lastBtn    = lBtn;

    POINT pt{-1, -1};
    if (clicked) {
        GetCursorPos(&pt);
        if (hwnd) ScreenToClient(hwnd, &pt);
    }

    DWORD now = GetTickCount();
    float oy   = OY_BASE;
    float tx   = OX + TEXT_X_OFF;

    // Membros de party
    {
        std::lock_guard<std::mutex> lk(g_partyMtx);
        for (auto& pm : g_party) {
            bool talking = pm.lastTalkMs && (now - pm.lastTalkMs) < TALK_MS;

            COLORREF col;
            if      (pm.muted) col = RGB(220, 60,  60);
            else if (talking)  col = RGB(60,  220, 60);
            else               col = RGB(160, 160, 160);

            SetTextColor(hdc, col);
            TextOutA(hdc, (int)tx, (int)oy + 2, pm.name, (int)strlen(pm.name));

            // Clique no nome → toggle mute
            if (clicked &&
                pt.x >= (int)tx && pt.x < (int)(tx + 130) &&
                pt.y >= (int)oy  && pt.y < (int)(oy + ROW_H))
                pm.muted = !pm.muted;

            oy += ROW_H;
        }
    }

    // Jogadores próximos
    {
        std::lock_guard<std::mutex> lk(g_nearbyMtx);
        for (auto& [id, ne] : g_nearby) {
            if (!ne.lastTalkMs || (now - ne.lastTalkMs) >= TALK_MS) continue;

            COLORREF col = ne.muted ? RGB(220, 60, 60) : RGB(60, 220, 60);

            SetTextColor(hdc, col);
            TextOutA(hdc, (int)tx, (int)oy + 2, ne.name, (int)strlen(ne.name));

            if (clicked &&
                pt.x >= (int)tx && pt.x < (int)(tx + 130) &&
                pt.y >= (int)oy  && pt.y < (int)(oy + ROW_H))
                ne.muted = !ne.muted;

            oy += ROW_H;
        }
    }

    if (oldFont) SelectObject(hdc, oldFont);
    bb->ReleaseDC(hdc);
    bb->Release();

    // Limpa entradas stale do mapa de próximos a cada ~5s
    static DWORD s_cleanupTick = 0;
    if (now - s_cleanupTick > 5000) {
        s_cleanupTick = now;
        std::lock_guard<std::mutex> lk(g_nearbyMtx);
        for (auto it = g_nearby.begin(); it != g_nearby.end();) {
            if (now - it->second.lastTalkMs > 8000) it = g_nearby.erase(it);
            else ++it;
        }
    }
}

// ── Detour inline (restore-call-repatch) ─────────────────────────────────────
struct Detour { BYTE* fnAddr; BYTE saved[5]; };
static Detour g_detourPresent{};

static inline void unpatch(Detour& d) {
    DWORD old;
    VirtualProtect(d.fnAddr, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(d.fnAddr, d.saved, 5);
    FlushInstructionCache(GetCurrentProcess(), d.fnAddr, 5);
    VirtualProtect(d.fnAddr, 5, old, &old);
}
static inline void repatch(Detour& d, void* hook) {
    DWORD old;
    VirtualProtect(d.fnAddr, 5, PAGE_EXECUTE_READWRITE, &old);
    d.fnAddr[0] = 0xE9;
    *(DWORD*)(d.fnAddr + 1) = (DWORD)hook - (DWORD)d.fnAddr - 5;
    FlushInstructionCache(GetCurrentProcess(), d.fnAddr, 5);
    VirtualProtect(d.fnAddr, 5, old, &old);
}

static HRESULT WINAPI hookedPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* rgn) {
    if (g_run && SUCCEEDED(dev->TestCooperativeLevel())) {
        // Fase 1: ícones de microfone via D3D9 (dentro de BeginScene)
        if (SUCCEEDED(dev->BeginScene())) {
            drawVoiceRects(dev);
            dev->EndScene();
        }
        // Fase 2: nomes via GDI no backbuffer (fora de BeginScene)
        D3DDEVICE_CREATION_PARAMETERS cp{};
        HWND hwnd = SUCCEEDED(dev->GetCreationParameters(&cp)) ? cp.hFocusWindow : nullptr;
        drawVoiceText(dev, hwnd);
    }

    unpatch(g_detourPresent);
    HRESULT hr = ((Present_t)g_detourPresent.fnAddr)(dev, src, dst, wnd, rgn);
    repatch(g_detourPresent, (void*)hookedPresent);
    return hr;
}

static bool installDetour(Detour& d, void* fnAddr, void* hook) {
    d.fnAddr = (BYTE*)fnAddr;
    DWORD old;
    if (!VirtualProtect(d.fnAddr, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(d.saved, d.fnAddr, 5);
    d.fnAddr[0] = 0xE9;
    *(DWORD*)(d.fnAddr + 1) = (DWORD)hook - (DWORD)d.fnAddr - 5;
    VirtualProtect(d.fnAddr, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), d.fnAddr, 5);
    dbgLog("installDetour: %p patched -> %p  saved=[%02X %02X %02X %02X %02X]",
           fnAddr, hook, d.saved[0], d.saved[1], d.saved[2], d.saved[3], d.saved[4]);
    return true;
}

static void setupD3DHook() {
    Sleep(500);
    dbgLog("setupD3DHook: iniciando");

    HMODULE hD3D = GetModuleHandleA("d3d9.dll");
    if (!hD3D) hD3D = LoadLibraryA("d3d9.dll");
    if (!hD3D) { dbgLog("setupD3DHook: d3d9.dll nao encontrado"); return; }

    typedef IDirect3D9*(WINAPI* Create9_t)(UINT);
    auto fnCreate = (Create9_t)GetProcAddress(hD3D, "Direct3DCreate9");
    if (!fnCreate) { dbgLog("setupD3DHook: Direct3DCreate9 nao encontrado"); return; }

    IDirect3D9* d3d = fnCreate(D3D_SDK_VERSION);
    if (!d3d) { dbgLog("setupD3DHook: Direct3DCreate9 null"); return; }

    HWND dummy = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 1, 1,
                                 nullptr, nullptr, g_dllModule, nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = dummy;
    pp.Windowed = TRUE; pp.BackBufferFormat = D3DFMT_UNKNOWN;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummy,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr))
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummy,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev) {
        dbgLog("setupD3DHook: CreateDevice falhou hr=0x%08X", (unsigned)hr);
        d3d->Release(); DestroyWindow(dummy); return;
    }

    void** vtbl = *reinterpret_cast<void***>(dev);
    void* addrPresent = vtbl[17];
    dbgLog("Present@%p", addrPresent);

    dev->Release(); d3d->Release(); DestroyWindow(dummy);

    bool ok = installDetour(g_detourPresent, addrPresent, (void*)hookedPresent);
    dbgLog("setupD3DHook: detour Present=%s", ok ? "OK" : "FALHOU");
}

// ── voiceInit ─────────────────────────────────────────────────────────────────
static void voiceInit() {
    {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string dir = std::string(path).substr(0, std::string(path).rfind('\\'));
        g_logPath = dir + "\\voice_debug.log";
        FILE* f = nullptr; fopen_s(&f, g_logPath.c_str(), "w");
        if (f) { fprintf(f, "=== L2 Voice Debug ===\n"); fclose(f); }
    }
    dbgLog("voiceInit: inicio");

    g_vadThreshold = (float)atof(cfgGet("VadThreshold", "400").c_str());
    dbgLog("VAD=%.0f", g_vadThreshold);

    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    std::string ip = cfgGet("ServerIP", "127.0.0.1");
    dbgLog("ServerIP=%s", ip.c_str());

    ZeroMemory(&g_serverAddr, sizeof(g_serverAddr));
    g_serverAddr.sin_family = AF_INET;
    g_serverAddr.sin_port   = htons(VOICE_PORT_UDP);
    inet_pton(AF_INET, ip.c_str(), &g_serverAddr.sin_addr);

    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp != INVALID_SOCKET) {
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = INADDR_ANY;
        bind(g_udp, (sockaddr*)&local, sizeof(local));
        DWORD to = 500; setsockopt(g_udp, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
        dbgLog("UDP OK -> %s:%d", ip.c_str(), VOICE_PORT_UDP);
    }

    std::thread(posThread).detach();
    std::thread(recvThread).detach();
    std::thread(setupD3DHook).detach();
    startCapture();
    dbgLog("voiceInit: completo");
}

// ── DllMain ───────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hMod);
        g_dllModule = hMod;
        {
            char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH);
            g_realDll = LoadLibraryA((std::string(sys) + "\\dinput8.dll").c_str());
            if (g_realDll)
                g_realFn = (DI8Create_fn)GetProcAddress(g_realDll, "DirectInput8Create");
        }
        g_run = true;
        std::thread(voiceInit).detach();
        break;
    case DLL_PROCESS_DETACH:
        g_run = false;
        if (g_waveIn) { waveInStop(g_waveIn); waveInReset(g_waveIn); waveInClose(g_waveIn); }
        if (g_udp != INVALID_SOCKET) closesocket(g_udp);
        if (g_tcp != INVALID_SOCKET) closesocket(g_tcp);
        { std::lock_guard<std::mutex> lk(g_spkMtx); for (auto& [id, s] : g_speakers) delete s; }
        if (g_realDll) FreeLibrary(g_realDll);
        WSACleanup();
        break;
    }
    return TRUE;
}