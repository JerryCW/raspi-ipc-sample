import express from 'express';
import path from 'path';
import { fileURLToPath } from 'url';
import { verifyJwt, getEvents, getThumbnail } from './events.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const app = express();
const port = process.env.PORT || 3000;

// Serve Vite build output
app.use(express.static(path.join(__dirname, '../dist')));

// Health check endpoint
app.get('/health', (_req, res) => {
  res.json({ status: 'healthy', timestamp: new Date().toISOString() });
});

// Events API — protected by JWT
app.get('/api/events', verifyJwt, getEvents);
app.get('/api/events/:sessionId/thumbnail', verifyJwt, getThumbnail);

// Catch-all for unknown API routes — return 404
app.all('/api/{*splat}', (_req, res) => {
  res.status(404).json({ error: 'Not Found' });
});

// SPA fallback — serve index.html for all non-file routes
app.get('/{*splat}', (_req, res) => {
  res.sendFile(path.join(__dirname, '../dist/index.html'));
});

app.listen(port, '0.0.0.0', () => {
  console.log(`KVS Camera Viewer running on port ${port}`);
});

export { app };
