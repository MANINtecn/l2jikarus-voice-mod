// L2 Samurai Crow — Voice Server
// UDP: recebe áudio dos clientes e roteia para jogadores próximos e party

const dgram = require('dgram');

const VOICE_PORT  = 7779;
const VOICE_RANGE = 200;    // unidades L2 (proximidade geral)
const PKT_AUDIO   = 1;
const STALE_MS    = 10000;
const FRAME_BYTES = 1920;   // 960 samples * 2 bytes (48kHz mono, 20ms)
const NAME_LEN    = 32;

// InPacket (cliente → servidor): type(4)+x(4)+y(4)+z(4)+partyId(4)+name(32)+audio(640) = 692
// OutPacket (servidor → cliente): speakerId(4)+volumeFactor(4)+name(32)+audio(640)      = 680
const PKT_IN_MIN = 52 + FRAME_BYTES; // 692

const server  = dgram.createSocket({ type: 'udp4', reuseAddr: true });
const clients = new Map(); // "ip:port" -> { address, port, x, y, z, partyId, name, lastSeen, id }
let   nextId  = 1;

function dist3(a, b) {
    return Math.sqrt((a.x - b.x) ** 2 + (a.y - b.y) ** 2 + (a.z - b.z) ** 2);
}

function parseName(buf, offset) {
    const slice = buf.slice(offset, offset + NAME_LEN);
    const end   = slice.indexOf(0);
    return slice.slice(0, end >= 0 ? end : NAME_LEN).toString('utf8');
}

server.on('message', (msg, rinfo) => {
    if (msg.length < PKT_IN_MIN) return;
    if (msg.readUInt32LE(0) !== PKT_AUDIO) return;

    const x       = msg.readFloatLE(4);
    const y       = msg.readFloatLE(8);
    const z       = msg.readFloatLE(12);
    const partyId = msg.readUInt32LE(16);
    const name    = parseName(msg, 20);
    const audio   = msg.subarray(52, 52 + FRAME_BYTES);

    const key = `${rinfo.address}:${rinfo.port}`;
    let sender = clients.get(key);
    if (!sender) {
        sender = { address: rinfo.address, port: rinfo.port, x, y, z, partyId, name, lastSeen: Date.now(), id: nextId++ };
        clients.set(key, sender);
        console.log(`[+] ${key}  id=${sender.id}  name=${name}`);
    } else {
        sender.x = x; sender.y = y; sender.z = z;
        sender.partyId  = partyId;
        sender.name     = name;
        sender.lastSeen = Date.now();
    }

    // out: speakerId(4) + volumeFactor(4) + name(32) + audio(640) = 680
    const out = Buffer.allocUnsafe(8 + NAME_LEN + FRAME_BYTES);
    out.writeUInt32LE(sender.id, 0);
    out.fill(0, 8, 8 + NAME_LEN);
    out.write(sender.name.substring(0, NAME_LEN - 1), 8, 'utf8');
    audio.copy(out, 8 + NAME_LEN);

    for (const [otherKey, other] of clients) {
        if (otherKey === key) continue;
        if (Date.now() - other.lastSeen > STALE_MS) continue;

        // Party members ouvem em qualquer distância com volume cheio
        const sameParty = partyId !== 0 && other.partyId !== 0 && partyId === other.partyId;

        let factor;
        if (sameParty) {
            factor = 1.0;
        } else {
            const d = dist3(sender, other);
            if (d > VOICE_RANGE) continue;
            factor = Math.max(0.0, 1.0 - d / VOICE_RANGE);
        }

        out.writeFloatLE(factor, 4);
        server.send(out, 0, out.length, other.port, other.address);
    }
});

server.on('error', (err) => console.error('Erro:', err.message));

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
    console.log(`UDP porta ${VOICE_PORT} | Proximidade ${VOICE_RANGE} u | Party: range ilimitado`);
});