const path = require('path');
const fs = require('fs');
const express = require('express');
const http = require('http');
const WebSocket = require('ws');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

const staticDir = path.join(__dirname, 'public');
console.log('Serving static from:', staticDir);

app.use(express.static(staticDir));

app.get('/', (req, res) => {
  res.sendFile(path.join(staticDir, 'index.html'));
});

// ---- State files ----
const STATE_FILE = path.join(__dirname, 'strokes.json');
const FRAME_FILE = path.join(__dirname, 'frame.raw');

let backgroundColor = '#000000';
let strokes = [];
let saveScheduled = false;

// Load strokes/background like before (unchanged idea)
function loadState() {
  try {
    if (!fs.existsSync(STATE_FILE)) {
      console.log('No state file, starting with empty canvas');
      return;
    }
    const raw = fs.readFileSync(STATE_FILE, 'utf8');
    const data = JSON.parse(raw);
    if (Array.isArray(data)) {
      // legacy
      strokes = data;
      backgroundColor = '#000000';
      console.log(`Loaded legacy state: ${strokes.length} strokes`);
    } else if (data && typeof data === 'object') {
      backgroundColor = data.backgroundColor || '#000000';
      strokes = Array.isArray(data.strokes) ? data.strokes : [];
      console.log(`Loaded state: bg=${backgroundColor}, strokes=${strokes.length}`);
    } else {
      console.warn('State file has unexpected format, starting fresh');
    }
  } catch (err) {
    console.error('Error loading state file:', err);
  }
}

function scheduleSaveState() {
  if (saveScheduled) return;
  saveScheduled = true;
  setTimeout(() => {
    saveScheduled = false;
    const data = { backgroundColor, strokes };
    try {
      fs.writeFileSync(STATE_FILE, JSON.stringify(data), 'utf8');
    } catch (err) {
      console.error('Error saving state file:', err);
    }
  }, 1000);
}

loadState();

// ---- /frame.raw handling ----

// Only this route uses raw body parser
app.post('/frame.raw',
  express.raw({ type: 'application/octet-stream', limit: '500kb' }),
  (req, res) => {
    if (!req.body || !req.body.length) {
      return res.sendStatus(400);
    }
    fs.writeFile(FRAME_FILE, req.body, (err) => {
      if (err) {
        console.error('Error saving frame.raw:', err);
        return res.sendStatus(500);
      }
      // console.log('frame.raw updated, size', req.body.length);
      res.sendStatus(200);
    });
  }
);

// Serve the latest frame to devices
app.get('/frame.raw', (req, res) => {
  if (!fs.existsSync(FRAME_FILE)) {
    console.log('[FRAME] GET /frame.raw 404 (no file)');
    return res.sendStatus(404);
  }

  const stats = fs.statSync(FRAME_FILE);
  console.log('[FRAME] GET /frame.raw, size:', stats.size);

  res.setHeader('Content-Type', 'application/octet-stream');
  res.setHeader('Content-Length', stats.size);

  const stream = fs.createReadStream(FRAME_FILE);
  stream.pipe(res);
});

// ---- WebSocket handling ----

const clients = new Set();

function sendFullState(ws) {
  if (ws.readyState !== WebSocket.OPEN) return;

  const total = strokes.length;
  const chunkSize = 200;
  const delayMs = 10;

  console.log(`Sending full state to browser: bg=${backgroundColor}, strokes=${total}`);

  ws.send(JSON.stringify({ type: 'fill', color: backgroundColor }));

  if (total === 0) return;

  let index = 0;
  function sendChunk() {
    if (ws.readyState !== WebSocket.OPEN) return;
    const end = Math.min(index + chunkSize, total);
    for (let i = index; i < end; i++) {
      ws.send(JSON.stringify(strokes[i]));
    }
    index = end;
    if (index < total) {
      setTimeout(sendChunk, delayMs);
    } else {
      console.log('Full-state replay to browser complete');
    }
  }
  sendChunk();
}

wss.on('connection', (ws, req) => {
  ws.role = 'unknown'; // 'browser' or 'device'
  clients.add(ws);

  console.log('New client connected');

  ws.on('message', (message) => {
    let data;
    try {
      data = JSON.parse(message.toString());
    } catch (e) {
      console.error('Invalid JSON from client:', e);
      return;
    }

    if (data.type === 'hello' && data.role) {
      ws.role = data.role;
      console.log(`Client identified as: ${ws.role}`);

      // Only browsers need full stroke history; devices will HTTP GET frame.raw
      if (ws.role === 'browser') {
        sendFullState(ws);
      }
      return;
    }

    if (data.type === 'stroke') {
      const stroke = {
        type: 'stroke',
        from: data.from,
        to: data.to,
        color: data.color || '#ffffff',
        width: data.width || 2,      // ← add this
      };
      strokes.push(stroke);
      scheduleSaveState();

      for (const client of clients) {
        if (client !== ws && client.readyState === WebSocket.OPEN) {
          client.send(JSON.stringify(stroke));
        }
      }
    } else if (data.type === 'fill') {
      const color = data.color || '#000000';
      backgroundColor = color;
      strokes = [];
      scheduleSaveState();

      const fillMsg = JSON.stringify({ type: 'fill', color: backgroundColor });
      for (const client of clients) {
        if (client.readyState === WebSocket.OPEN) {
          client.send(fillMsg);
        }
      }
    }
  });

  ws.on('close', () => {
    clients.delete(ws);
    console.log('Client disconnected');
    scheduleSaveState();
  });
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log(`TeleDraw server listening on http://localhost:${PORT}`);
});
