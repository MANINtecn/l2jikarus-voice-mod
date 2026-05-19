// L2 Samurai Crow — Native Voice Chat (dinput8.dll proxy)
// Carrega automático com L2.exe. Zero software extra para o jogador.

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
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

// ── Áudio ─────────────────────────────────────────────────────────────────────
// 48kHz nativo evita resample do Windows (maioria dos devices roda 48kHz)
static constexpr int   SAMPLE_RATE   = 48000;
static constexpr int   CHANNELS      = 1;
static constexpr int   BITS          = 16;
static constexpr int   FRAME_SAMPLES = 960;     // 20ms a 48kHz
static constexpr int   FRAME_BYTES   = FRAME_SAMPLES * 2; // 1920
static constexpr int   CAPTURE_BUFS  = 4;
static float           g_vadThreshold = 400.0f;

// ── Protocolo ─────────────────────────────────────────────────────────────────
static constexpr uint32_t PKT_AUDIO      = 1;
static constexpr int      VOICE_PORT_UDP = 7779;
static constexpr int      POS_PORT_TCP   = 7778;
static constexpr int      NAME_LEN       = 32;

// OutPacket: type(4)+x(4)+y(4)+z(4)+partyId(4)+name(32)+samples(1920) = 1972
// InPacket : speakerId(4)+volumeFactor(4)+name(32)+samples(1920)       = 1960
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
    DWORD lastTalkMs;
    bool  muted;
    float rowY;
};
struct NearbyEntry {
    uint32_t speakerId;      // 0 se ainda não falou (presente só por TCP)
    DWORD    lastTalkMs;     // último áudio UDP recebido
    DWORD    lastSeenTcpMs;  // última vez no NEARBY: do servidor
    bool     muted;
    float    rowY;
};

static std::vector<PartyMember>                     g_party;
static std::mutex                                   g_partyMtx;
static std::unordered_map<std::string, NearbyEntry> g_nearby; // chave = nome
static std::mutex                                   g_nearbyMtx;

static char     g_selfName[NAME_LEN] = {};
static uint32_t g_selfPartyId = 0;

// ── Globals ───────────────────────────────────────────────────────────────────
static HMODULE           g_dllModule = nullptr;
static std::atomic<bool> g_run       { false };
static std::atomic<bool> g_muted     { false };
static std::atomic<DWORD> g_txTick   { 0 };

static SOCKET     g_udp = INVALID_SOCKET;
static SOCKET     g_tcp = INVALID_SOCKET;
static sockaddr_in g_serverAddr{};

static std::atomic<float> g_posX{0}, g_posY{0}, g_posZ{0};

static HWAVEIN  g_waveIn = nullptr;
static WAVEHDR  g_capHdr[CAPTURE_BUFS];
static int16_t  g_capBuf[CAPTURE_BUFS][FRAME_SAMPLES];

struct Speaker {
    HWAVEOUT hOut = nullptr;
    std::mutex mtx;
    ~Speaker() { if (hOut) { waveOutReset(hOut); waveOutClose(hOut); } }
};
static std::unordered_map<uint32_t, Speaker*> g_speakers;
static std::mutex                              g_spkMtx;

typedef HRESULT(WINAPI* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

// ── Mouse hook (impede click de vazar para o jogo) ── globals ─────────────────
static HWND  g_gameHwnd  = nullptr;
static HHOOK g_mouseHook = nullptr;

// ── Log ───────────────────────────────────────────────────────────────────────
static std::string g_logPath;
static std::mutex  g_logMtx;

static void dbgLog(const char* fmt, ...) {
    if (g_logPath.empty()) return;
    char msg[512];
    va_list a; va_start(a, fmt); vsnprintf(msg, sizeof(msg), fmt, a); va_end(a);
    std::lock_guard<std::mutex> lk(g_logMtx);
    FILE* f = nullptr; fopen_s(&f, g_logPath.c_str(), "a");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
}

// ── Configuração ──────────────────────────────────────────────────────────────
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
        float v = samples[i] * vol * 2.0f; // boost 2x (era 4x — reduziu distorção)
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

        // Ignora o próprio áudio (evita echo em teste com 2 contas)
        if (g_selfName[0] && strncmp(pkt.name, g_selfName, NAME_LEN) == 0) continue;

        bool isMuted = false, isParty = false;

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
        if (!isParty) {
            std::lock_guard<std::mutex> lk(g_nearbyMtx);
            std::string key(pkt.name);
            auto& ne = g_nearby[key];
            ne.speakerId  = pkt.speakerId;
            ne.lastTalkMs = GetTickCount();
            isMuted       = ne.muted;
        }
        if (!isMuted && !g_muted.load()) {
            // Dedup: ignora pacote se já tocamos áudio deste speakerId nos últimos 15ms
            // (evita double-play quando 2 contas no mesmo PC capturam o mesmo mic)
            static std::unordered_map<uint32_t, DWORD> s_lastPlay;
            static std::mutex s_lpMtx;
            DWORD nowMs = GetTickCount();
            bool skip = false;
            {
                std::lock_guard<std::mutex> lk(s_lpMtx);
                auto& last = s_lastPlay[pkt.speakerId];
                if (nowMs - last < 15) skip = true;
                else last = nowMs;
            }
            if (!skip)
                playAudio(pkt.speakerId, pkt.volumeFactor, pkt.samples);
        }
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
            g_txTick = GetTickCount();
        }
    }
    waveInAddBuffer(g_waveIn, hdr, sizeof(WAVEHDR));
}

// ── posThread ─────────────────────────────────────────────────────────────────
static void posThread() {
    std::string ip = cfgGet("ServerIP", "127.0.0.1");
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
                if (line.rfind("SELF:", 0) == 0) {
                    const char* p = line.c_str() + 5;
                    const char* colon = strchr(p, ':');
                    if (colon) {
                        size_t len = (size_t)(colon - p); if (len >= NAME_LEN) len = NAME_LEN - 1;
                        strncpy_s(g_selfName, p, len); g_selfName[len] = '\0';
                        float x, y, z;
                        if (sscanf_s(colon + 1, "%f,%f,%f", &x, &y, &z) == 3) g_posX=x,g_posY=y,g_posZ=z;
                    } else {
                        float x, y, z;
                        if (sscanf_s(p, "%f,%f,%f", &x, &y, &z) == 3) g_posX=x,g_posY=y,g_posZ=z;
                    }
                    // Remove próprio nome de g_nearby (pode ter entrado antes do SELF: chegar)
                    if (g_selfName[0]) {
                        std::lock_guard<std::mutex> lkn(g_nearbyMtx);
                        g_nearby.erase(std::string(g_selfName));
                    }
                } else if (line.rfind("NEARBY:", 0) == 0) {
                    const char* p = line.c_str() + 7;
                    std::lock_guard<std::mutex> lk(g_nearbyMtx);
                    DWORD tcpNow = GetTickCount();
                    if (*p && *p != '\r' && *p != '\n') {
                        char tmp2[1024]; strncpy_s(tmp2, p, sizeof(tmp2) - 1);
                        char* ctx2 = nullptr; char* tok = strtok_s(tmp2, ";", &ctx2);
                        while (tok && tok[0]) {
                            char* colon = strchr(tok, ':');
                            if (colon) *colon = '\0'; // extrai só o nome
                            // nunca adiciona o próprio personagem
                            if (tok[0] && strncmp(tok, g_selfName, NAME_LEN) != 0) {
                                auto& ne = g_nearby[std::string(tok)];
                                ne.lastSeenTcpMs = tcpNow;
                            }
                            tok = strtok_s(nullptr, ";", &ctx2);
                        }
                    }
                } else if (line.rfind("PARTY:", 0) == 0) {
                    const char* p = line.c_str() + 6;
                    const char* colon = strchr(p, ':');
                    std::lock_guard<std::mutex> lk(g_partyMtx);
                    if (colon && colon > p) {
                        g_selfPartyId = (uint32_t)atoi(p);
                        std::vector<PartyMember> newParty;
                        char tmp2[512]; strncpy_s(tmp2, colon + 1, sizeof(tmp2) - 1);
                        char* ctx = nullptr; char* tok = strtok_s(tmp2, ";", &ctx);
                        while (tok && tok[0]) {
                            PartyMember pm{}; strncpy_s(pm.name, tok, NAME_LEN - 1);
                            for (const auto& old : g_party)
                                if (strncmp(old.name, pm.name, NAME_LEN) == 0) { pm.muted = old.muted; pm.lastTalkMs = old.lastTalkMs; break; }
                            newParty.push_back(pm);
                            tok = strtok_s(nullptr, ";", &ctx);
                        }
                        g_party = std::move(newParty);
                    } else { g_selfPartyId = 0; g_party.clear(); }
                }
            }
        }
        closesocket(g_tcp); if (g_run) Sleep(1000);
    }
}

// ── Captura ───────────────────────────────────────────────────────────────────
static void startCapture() {
    WAVEFORMATEX wfx = makeWfx();
    if (waveInOpen(&g_waveIn, WAVE_MAPPER, &wfx, (DWORD_PTR)waveInCb, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        dbgLog("waveInOpen FALHOU"); return;
    }
    dbgLog("waveInOpen OK 48kHz");
    for (int i = 0; i < CAPTURE_BUFS; i++) {
        ZeroMemory(&g_capHdr[i], sizeof(WAVEHDR));
        g_capHdr[i].lpData = (LPSTR)g_capBuf[i]; g_capHdr[i].dwBufferLength = FRAME_BYTES;
        waveInPrepareHeader(g_waveIn, &g_capHdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_waveIn, &g_capHdr[i], sizeof(WAVEHDR));
    }
    waveInStart(g_waveIn);
    dbgLog("captura iniciada VAD=%.0f", g_vadThreshold);
}

// ── Overlay D3D9 com fonte pixel ──────────────────────────────────────────────
struct PVert { float x, y, z, rhw; DWORD color; };

static void fillRect(IDirect3DDevice9* dev, float x, float y, float w, float h, DWORD col) {
    PVert v[] = {
        {x,   y,   0, 1, col}, {x+w, y,   0, 1, col},
        {x,   y+h, 0, 1, col}, {x+w, y+h, 0, 1, col}
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(PVert));
}

// ── Texto GDI → textura D3D9 (fonte ClearType real, sem pixel-art) ──────────
struct GlyphTex { IDirect3DTexture9* tex = nullptr; int w = 0, h = 0; };
static std::unordered_map<std::string, GlyphTex> g_glyphCache;
static std::mutex g_glyphMtx;

static GlyphTex makeTextTex(IDirect3DDevice9* dev, const char* text, DWORD argb) {
    GlyphTex out{};
    int len = (int)strlen(text);
    if (!len) return out;
    const int FS = 16; // tamanho da fonte px
    // Mede largura
    HDC hdcTmp = CreateCompatibleDC(nullptr);
    HFONT hfTmp = CreateFontA(FS, 0,0,0, FW_BOLD,0,0,0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        VARIABLE_PITCH|FF_SWISS, "Tahoma");
    SelectObject(hdcTmp, hfTmp);
    SIZE sz{}; GetTextExtentPoint32A(hdcTmp, text, len, &sz);
    DeleteObject(hfTmp); DeleteDC(hdcTmp);
    int W = sz.cx + 6, H = sz.cy + 4;
    // Cria DIB 32bpp
    BITMAPINFO bmi{}; auto& bh = bmi.bmiHeader;
    bh.biSize=sizeof(bh); bh.biWidth=W; bh.biHeight=-H;
    bh.biPlanes=1; bh.biBitCount=32; bh.biCompression=BI_RGB;
    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) { DeleteDC(hdc); return out; }
    SelectObject(hdc, hbm);
    memset(bits, 0, W * H * 4);
    HFONT hFont = CreateFontA(FS, 0,0,0, FW_BOLD,0,0,0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        VARIABLE_PITCH|FF_SWISS, "Tahoma");
    HGDIOBJ hOld = SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0,0,0));   TextOutA(hdc, 2, 2, text, len); // sombra
    SetTextColor(hdc, RGB((argb>>16)&0xFF,(argb>>8)&0xFF,argb&0xFF));
    TextOutA(hdc, 1, 1, text, len);  // texto principal
    GdiFlush();
    SelectObject(hdc, hOld); DeleteObject(hFont); DeleteDC(hdc);
    // DIB → textura D3D9
    IDirect3DTexture9* tex = nullptr;
    if (SUCCEEDED(dev->CreateTexture(W,H,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&tex,nullptr))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(tex->LockRect(0,&lr,nullptr,0))) {
            BYTE sa = (argb>>24)&0xFF;
            auto* src = (DWORD*)bits;
            for (int y=0;y<H;y++) {
                auto* dst = (DWORD*)((BYTE*)lr.pBits + y*lr.Pitch);
                for (int x=0;x<W;x++) {
                    DWORD px=src[y*W+x];
                    BYTE r=(px>>16)&0xFF, g=(px>>8)&0xFF, b=px&0xFF;
                    BYTE bright=(BYTE)(((int)r+g+b)/3);
                    dst[x]=D3DCOLOR_ARGB((bright*sa)/255, r, g, b);
                }
            }
            tex->UnlockRect(0);
        }
        out.tex=tex; out.w=W; out.h=H;
    }
    DeleteObject(hbm);
    return out;
}

static void drawText(IDirect3DDevice9* dev, float x, float y, const char* text, DWORD argb) {
    char key[48]; snprintf(key, sizeof(key), "%.31s_%08X", text, argb);
    GlyphTex* gt = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_glyphMtx);
        auto it = g_glyphCache.find(key);
        if (it == g_glyphCache.end()) {
            GlyphTex ng = makeTextTex(dev, text, argb);
            if (!ng.tex) return;
            g_glyphCache[key] = ng;
        }
        gt = &g_glyphCache[key];
    }
    if (!gt || !gt->tex) return;
    // Renderiza quad texturizado
    dev->SetTexture(0, gt->tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    float w=(float)gt->w, h=(float)gt->h;
    struct PVT { float x,y,z,rhw; DWORD c; float u,v; };
    PVT v[]={{x,y,0,1,0xFFFFFFFF,0,0},{x+w,y,0,1,0xFFFFFFFF,1,0},
             {x,y+h,0,1,0xFFFFFFFF,0,1},{x+w,y+h,0,1,0xFFFFFFFF,1,1}};
    dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,v,sizeof(PVT));
    // Restaura estado sem textura
    dev->SetTexture(0, nullptr);
    dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
}

// Ícone de microfone com forma arredondada (~16×21 px)
static void drawMic(IDirect3DDevice9* dev, float x, float y, DWORD col) {
    // Corpo arredondado
    fillRect(dev, x+4, y+0,  8, 1, col);
    fillRect(dev, x+2, y+1,  12,1, col);
    fillRect(dev, x+1, y+2,  14,9, col);
    fillRect(dev, x+2, y+11, 12,1, col);
    fillRect(dev, x+4, y+12, 8, 1, col);
    // Haste
    fillRect(dev, x+7, y+13, 2, 4, col);
    // Arco U: esq + dir + base
    fillRect(dev, x+1, y+13, 5, 2, col);
    fillRect(dev, x+10,y+13, 5, 2, col);
    fillRect(dev, x+1, y+18, 14,2, col);
}

static constexpr float OX       = 10.0f;
static constexpr float OY_SELF  = 90.0f;  // ícone próprio
static constexpr float OY_LIST  = 116.0f; // lista de jogadores
static constexpr float ROW_H    = 32.0f;
static constexpr float ROW_W    = 240.0f;
static constexpr DWORD TALK_MS  = 600;

// ── WH_MOUSE hook (intercepta WM_LBUTTONDOWN antes de chegar ao jogo) ─────────
static LRESULT CALLBACK mouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN && g_gameHwnd && g_run) {
        MOUSEHOOKSTRUCT* ms = (MOUSEHOOKSTRUCT*)lParam;
        POINT pt = ms->pt;
        ScreenToClient(g_gameHwnd, &pt);
        if (pt.x >= (int)OX && pt.x < (int)(OX + ROW_W) && pt.y >= (int)OY_LIST) {
            int rows = 0;
            { std::lock_guard<std::mutex> l(g_partyMtx);  rows += (int)g_party.size(); }
            { std::lock_guard<std::mutex> l(g_nearbyMtx); rows += (int)g_nearby.size(); }
            if (pt.y < (int)(OY_LIST + rows * ROW_H))
                return 1; // descarta — não passa para o jogo
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static void drawVoiceOverlay(IDirect3DDevice9* dev) {
    if (FAILED(dev->TestCooperativeLevel())) return;
    DWORD now = GetTickCount();

    // ── Obtém HWND e instala subclass na primeira vez ─────────────────────────
    D3DDEVICE_CREATION_PARAMETERS cp{};
    HWND hwnd = SUCCEEDED(dev->GetCreationParameters(&cp)) ? cp.hFocusWindow : nullptr;
    if (hwnd && !g_gameHwnd) {
        g_gameHwnd  = hwnd;
        DWORD tid   = GetWindowThreadProcessId(hwnd, nullptr);
        g_mouseHook = SetWindowsHookExA(WH_MOUSE, mouseHookProc, nullptr, tid);
        dbgLog("mouseHook=%p tid=%u", (void*)g_mouseHook, (unsigned)tid);
    }

    // Click detection (dispara apenas na borda de descida)
    static bool s_lastBtn = false;
    bool lBtn    = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool clicked = lBtn && !s_lastBtn;
    s_lastBtn    = lBtn;
    POINT pt{-1, -1};
    if (clicked) { GetCursorPos(&pt); if (hwnd) ScreenToClient(hwnd, &pt); }

    // ── Salva estados D3D ─────────────────────────────────────────────────────
    DWORD sFVF, sZ, sAlpha, sSrc, sDst, sLight, sCull, sCop, sCarg, sAop, sAarg;
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

    // ── Pipeline 2D ───────────────────────────────────────────────────────────
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

    // ── Indicador próprio (só ícone, sem linha) ────────────────────────────────
    bool selfTalking = (now - g_txTick.load()) < 500;
    bool selfMuted   = g_muted.load();
    DWORD selfCol = selfMuted  ? D3DCOLOR_ARGB(220, 180, 40, 40)
                  : selfTalking? D3DCOLOR_ARGB(220, 50, 200, 50)
                  :              D3DCOLOR_ARGB(140, 110, 110, 110);
    drawMic(dev, OX, OY_SELF, selfCol);

    // ── Lista de jogadores ────────────────────────────────────────────────────
    float oy = OY_LIST;

    auto drawRow = [&](float rowY, const char* name, bool talking, bool muted, bool& clickOut) {
        // Cores estilo L2: idle=dourado, falando=verde vivo, mutado=vermelho
        DWORD micCol  = muted   ? D3DCOLOR_ARGB(230, 220, 55, 55)
                      : talking ? D3DCOLOR_ARGB(255, 70, 230, 70)
                      :           D3DCOLOR_ARGB(160, 160, 140, 90);
        DWORD txtCol  = muted   ? D3DCOLOR_ARGB(255, 230, 70, 70)
                      : talking ? D3DCOLOR_ARGB(255, 90, 240, 90)
                      :           D3DCOLOR_ARGB(230, 220, 210, 170);

        // Fundo semitransparente
        fillRect(dev, OX, rowY, ROW_W, ROW_H - 2, D3DCOLOR_ARGB(150, 5, 5, 5));
        // Microfone
        drawMic(dev, OX + 2, rowY + 4, micCol);
        // Nome (fonte ClearType via GDI)
        drawText(dev, OX + 26, rowY + 7, name, txtCol);

        // Click em qualquer parte da linha → toggle mute
        if (clicked &&
            pt.x >= (int)OX && pt.x < (int)(OX + ROW_W) &&
            pt.y >= (int)rowY && pt.y < (int)(rowY + ROW_H))
            clickOut = true;
    };

    // Party (sempre visíveis)
    {
        std::lock_guard<std::mutex> lk(g_partyMtx);
        for (auto& pm : g_party) {
            bool talking = pm.lastTalkMs && (now - pm.lastTalkMs) < TALK_MS;
            pm.rowY = oy;
            bool click = false;
            drawRow(oy, pm.name, talking, pm.muted, click);
            if (click) pm.muted = !pm.muted;
            oy += ROW_H;
        }
    }

    // Próximos (cinza = no range mas calado, verde = falando)
    {
        std::lock_guard<std::mutex> lk(g_nearbyMtx);
        // Limpa entradas sem sinal recente de TCP nem UDP
        static DWORD s_clean = 0;
        if (now - s_clean > 5000) {
            s_clean = now;
            for (auto it = g_nearby.begin(); it != g_nearby.end();) {
                bool tcpFresh  = it->second.lastSeenTcpMs && (now - it->second.lastSeenTcpMs < 3000);
                bool talkFresh = it->second.lastTalkMs    && (now - it->second.lastTalkMs    < 8000);
                it = (!tcpFresh && !talkFresh) ? g_nearby.erase(it) : ++it;
            }
        }
        for (auto& [name, ne] : g_nearby) {
            bool present = ne.lastSeenTcpMs && (now - ne.lastSeenTcpMs < 2000);
            bool talking = ne.lastTalkMs    && (now - ne.lastTalkMs    < TALK_MS);
            if (!present && !talking) continue;
            ne.rowY = oy;
            bool click = false;
            drawRow(oy, name.c_str(), talking, ne.muted, click);
            if (click) ne.muted = !ne.muted;
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
    dev->SetVertexShader(sVS); if (sVS) sVS->Release();
    dev->SetPixelShader(sPS);  if (sPS) sPS->Release();
}

// ── Detour inline ─────────────────────────────────────────────────────────────
struct Detour { BYTE* fnAddr; BYTE saved[5]; };
static Detour g_detourPresent{};

static inline void unpatch(Detour& d) {
    DWORD old; VirtualProtect(d.fnAddr, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(d.fnAddr, d.saved, 5); FlushInstructionCache(GetCurrentProcess(), d.fnAddr, 5);
    VirtualProtect(d.fnAddr, 5, old, &old);
}
static inline void repatch(Detour& d, void* hook) {
    DWORD old; VirtualProtect(d.fnAddr, 5, PAGE_EXECUTE_READWRITE, &old);
    d.fnAddr[0] = 0xE9; *(DWORD*)(d.fnAddr + 1) = (DWORD)hook - (DWORD)d.fnAddr - 5;
    FlushInstructionCache(GetCurrentProcess(), d.fnAddr, 5); VirtualProtect(d.fnAddr, 5, old, &old);
}

static HRESULT WINAPI hookedPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* rgn) {
    if (g_run && SUCCEEDED(dev->BeginScene())) {
        drawVoiceOverlay(dev);
        dev->EndScene();
    }
    unpatch(g_detourPresent);
    HRESULT hr = ((Present_t)g_detourPresent.fnAddr)(dev, src, dst, wnd, rgn);
    repatch(g_detourPresent, (void*)hookedPresent);
    return hr;
}

static bool installDetour(Detour& d, void* fnAddr, void* hook) {
    d.fnAddr = (BYTE*)fnAddr; DWORD old;
    if (!VirtualProtect(d.fnAddr, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(d.saved, d.fnAddr, 5);
    d.fnAddr[0] = 0xE9; *(DWORD*)(d.fnAddr + 1) = (DWORD)hook - (DWORD)d.fnAddr - 5;
    VirtualProtect(d.fnAddr, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), d.fnAddr, 5);
    dbgLog("installDetour: %p -> %p  saved=[%02X %02X %02X %02X %02X]",
           fnAddr, hook, d.saved[0], d.saved[1], d.saved[2], d.saved[3], d.saved[4]);
    return true;
}

static void setupD3DHook() {
    Sleep(500); dbgLog("setupD3DHook: iniciando");
    HMODULE hD3D = GetModuleHandleA("d3d9.dll");
    if (!hD3D) hD3D = LoadLibraryA("d3d9.dll");
    if (!hD3D) { dbgLog("d3d9.dll nao encontrado"); return; }
    typedef IDirect3D9*(WINAPI* Create9_t)(UINT);
    auto fn = (Create9_t)GetProcAddress(hD3D, "Direct3DCreate9");
    if (!fn) { dbgLog("Direct3DCreate9 nao encontrado"); return; }
    IDirect3D9* d3d = fn(D3D_SDK_VERSION);
    if (!d3d) { dbgLog("Direct3DCreate9 null"); return; }
    HWND dummy = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_dllModule, nullptr);
    D3DPRESENT_PARAMETERS pp{}; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = dummy; pp.Windowed = TRUE; pp.BackBufferFormat = D3DFMT_UNKNOWN;
    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummy, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummy, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr) || !dev) { dbgLog("CreateDevice falhou hr=0x%08X", (unsigned)hr); d3d->Release(); DestroyWindow(dummy); return; }
    void** vtbl = *reinterpret_cast<void***>(dev);
    void* addrPresent = vtbl[17];
    dbgLog("Present@%p", addrPresent);
    dev->Release(); d3d->Release(); DestroyWindow(dummy);
    bool ok = installDetour(g_detourPresent, addrPresent, (void*)hookedPresent);
    dbgLog("setupD3DHook: %s", ok ? "OK" : "FALHOU");
}

// ── voiceInit ─────────────────────────────────────────────────────────────────
static void voiceInit() {
    {
        char path[MAX_PATH]; GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string dir = std::string(path).substr(0, std::string(path).rfind('\\'));
        g_logPath = dir + "\\voice_debug.log";
        FILE* f = nullptr; fopen_s(&f, g_logPath.c_str(), "w");
        if (f) { fprintf(f, "=== L2 Voice Debug ===\n"); fclose(f); }
    }
    dbgLog("voiceInit: inicio");
    g_vadThreshold = (float)atof(cfgGet("VadThreshold", "400").c_str());
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    std::string ip = cfgGet("ServerIP", "127.0.0.1");
    dbgLog("ServerIP=%s  VAD=%.0f  48kHz", ip.c_str(), g_vadThreshold);
    ZeroMemory(&g_serverAddr, sizeof(g_serverAddr));
    g_serverAddr.sin_family = AF_INET; g_serverAddr.sin_port = htons(VOICE_PORT_UDP);
    inet_pton(AF_INET, ip.c_str(), &g_serverAddr.sin_addr);
    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp != INVALID_SOCKET) {
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = INADDR_ANY;
        bind(g_udp, (sockaddr*)&local, sizeof(local));
        DWORD to = 500; setsockopt(g_udp, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));
        dbgLog("UDP OK -> %s:7779", ip.c_str());
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
        DisableThreadLibraryCalls(hMod); g_dllModule = hMod;
        { char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH);
          g_realDll = LoadLibraryA((std::string(sys) + "\\dinput8.dll").c_str());
          if (g_realDll) g_realFn = (DI8Create_fn)GetProcAddress(g_realDll, "DirectInput8Create"); }
        g_run = true; std::thread(voiceInit).detach(); break;
    case DLL_PROCESS_DETACH:
        g_run = false;
        if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
        { std::lock_guard<std::mutex> lk(g_glyphMtx); for (auto& [k,g] : g_glyphCache) if (g.tex) g.tex->Release(); g_glyphCache.clear(); }
        if (g_waveIn) { waveInStop(g_waveIn); waveInReset(g_waveIn); waveInClose(g_waveIn); }
        if (g_udp != INVALID_SOCKET) closesocket(g_udp);
        if (g_tcp != INVALID_SOCKET) closesocket(g_tcp);
        { std::lock_guard<std::mutex> lk(g_spkMtx); for (auto& [id, s] : g_speakers) delete s; }
        if (g_realDll) FreeLibrary(g_realDll); WSACleanup(); break;
    }
    return TRUE;
}
