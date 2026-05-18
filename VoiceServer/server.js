// L2 Samurai Crow — Voice Server
// UDP: recebe áudio dos clientes e roteia para jogadores próximos
// node server.js

const dgram = require('dgram');

const VOICE_PORT   = 7779;
const VOICE_RANGE  = 2000;   // unidades L2
const PKT_AUDIO    = 1;
const STALE_MS     = 10000;  // remove cliente sem pacote por 10s
const FRAME_BYTES  = 640;    // 320 samples * 2 bytes (16-bit PCM 16kHz mono)
const PACKET_MIN   = 16 + FRAME_BYTES; // type(4)+x(4)+y(4)+z(4)+audio(640)

const server  = dgram.createSocket({ type: 'udp4', reuseAddr: true });
const clients = new Map(); // "ip:port" -> { address, port, x, y, z, lastSeen, id }
let   nextId  = 1;

function dist3(a, b) {
    return Math.sqrt((a.x-b.x)**2 + (a.y-b.y)**2 + (a.z-b.z)**2);
}

server.on('message', (msg, rinfo) => {
    if (msg.length < PACKET_MIN) return;
    if (msg.readUInt32LE(0) !== PKT_AUDIO) return;

    const x     = msg.readFloatLE(4);
    const y     = msg.readFloatLE(8);
    const z     = msg.readFloatLE(12);
    const audio = msg.slice(16, 16 + FRAME_BYTES);

    const key = `${rinfo.address}:${rinfo.port}`;
    let sender = clients.get(key);
    if (!sender) {
        sender = { address: rinfo.address, port: rinfo.port, x, y, z, lastSeen: Date.now(), id: nextId++ };
        clients.set(key, sender);
        console.log(`[+] ${key} (id ${sender.id})`);
    } else {
        sender.x = x; sender.y = y; sender.z = z;
        sender.lastSeen = Date.now();
    }

    // Monta header do pacote de saída: speakerId(4) + volumeFactor(4) + audio
    const out = Buffer.allocUnsafe(8 + FRAME_BYTES);
    out.writeUInt32LE(sender.id, 0);

    for (const [otherKey, other] of clients) {
        if (otherKey === key) continue;
        if (Date.now() - other.lastSeen > STALE_MS) continue;

        const d = dist3(sender, other);
        if (d > VOICE_RANGE) continue;

        // Volume: 1.0 junto, 0.0 no limite do range
        const factor = Math.max(0.0, 1.0 - d / VOICE_RANGE);
        out.writeFloatLE(factor, 4);
        audio.copy(out, 8);

        server.send(out, 0, out.length, other.port, other.address);
    }
});

server.on('error', (err) => console.error('Erro:', err.message));

// Remove clientes inativos
setInterval(() => {
    const now = Date.now();
    for (const [key, c] of clients) {
        if (now - c.lastSeen > STALE_MS) {
            clients.delete(key);
            console.log(`[-] ${key}`);
        }
    }
}, 3000);

server.bind(VOICE_PORT, '0.0.0.0', () => {
    console.log(`=== L2 Samurai Crow — Voice Server ===`);
    console.log(`UDP porta ${VOICE_PORT} | Range ${VOICE_RANGE} unidades L2`);
});