import { describe, it, expect } from 'vitest';
import * as fc from 'fast-check';
import request from 'supertest';
import { app } from '../index.js';

// Feature: viewer-rewrite, Property 15: 健康检查端点响应结构
// **Validates: Requirements 8.2**
describe('Property 15: 健康检查端点响应结构', () => {
  it('should return JSON with status "healthy" and a valid ISO 8601 timestamp', async () => {
    await fc.assert(
      fc.asyncProperty(fc.integer({ min: 0, max: 99 }), async () => {
        const res = await request(app).get('/health').expect(200);

        // Response should be JSON
        expect(res.headers['content-type']).toMatch(/application\/json/);

        // Must have status field with value 'healthy'
        expect(res.body).toHaveProperty('status', 'healthy');

        // Must have timestamp field
        expect(res.body).toHaveProperty('timestamp');
        expect(typeof res.body.timestamp).toBe('string');

        // Timestamp must be a valid ISO 8601 string
        const parsed = new Date(res.body.timestamp);
        expect(parsed.toString()).not.toBe('Invalid Date');
        // ISO 8601 format check: must match pattern like 2024-01-01T00:00:00.000Z
        expect(res.body.timestamp).toMatch(
          /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z$/
        );
      }),
      { numRuns: 100 }
    );
  });
});

// Unit tests for removed endpoints
// **Validates: Requirements 8.4, 8.5**
describe('Removed endpoints return 404', () => {
  it('GET /api/credentials should return 404', async () => {
    const res = await request(app).get('/api/credentials');
    expect(res.status).toBe(404);
  });

  it('GET /api/webrtc-config should return 404', async () => {
    const res = await request(app).get('/api/webrtc-config');
    expect(res.status).toBe(404);
  });
});
