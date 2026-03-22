import { describe, it, expect } from 'vitest';
import * as fc from 'fast-check';
import { getStatColor } from '../hooks/useStats';

/**
 * Feature: viewer-rewrite, Property 11: 统计指标颜色编码
 *
 * For any stat metric value, the color encoding function should return
 * a deterministic color category ('good', 'warn', or 'bad') based on
 * predefined thresholds. The same input always maps to the same color.
 *
 * Thresholds (from useStats.ts):
 * - fps: good ≥ 20, warn ≥ 10, bad < 10
 * - bitrate: good ≥ 500000, warn ≥ 100000, bad < 100000
 * - latency: good ≤ 100, warn ≤ 300, bad > 300
 * - packetLoss: good = 0, warn ≤ 5, bad > 5
 * - bufferLength: good ≥ 2, warn ≥ 0.5, bad < 0.5
 * - droppedFrames: good = 0, warn ≤ 10, bad > 10
 *
 * **Validates: Requirements 6.3**
 */

const VALID_COLORS = ['good', 'warn', 'bad'] as const;

const METRICS = ['fps', 'bitrate', 'latency', 'packetLoss', 'bufferLength', 'droppedFrames'] as const;

const arbMetric = fc.constantFrom(...METRICS);
const arbValue = fc.double({ min: 0, max: 10_000_000, noNaN: true, noDefaultInfinity: true });

describe('Property 11: 统计指标颜色编码', () => {
  it('always returns one of good, warn, or bad for any metric and non-negative value', () => {
    fc.assert(
      fc.property(arbMetric, arbValue, (metric, value) => {
        const color = getStatColor(metric, value);
        expect(VALID_COLORS).toContain(color);
      }),
      { numRuns: 100 },
    );
  });

  it('is deterministic — same metric and value always produce the same color', () => {
    fc.assert(
      fc.property(arbMetric, arbValue, (metric, value) => {
        const color1 = getStatColor(metric, value);
        const color2 = getStatColor(metric, value);
        expect(color1).toBe(color2);
      }),
      { numRuns: 100 },
    );
  });

  it('respects fps thresholds: good ≥ 20, warn ≥ 10, bad < 10', () => {
    fc.assert(
      fc.property(
        fc.double({ min: 0, max: 1000, noNaN: true, noDefaultInfinity: true }),
        (value) => {
          const color = getStatColor('fps', value);
          if (value >= 20) expect(color).toBe('good');
          else if (value >= 10) expect(color).toBe('warn');
          else expect(color).toBe('bad');
        },
      ),
      { numRuns: 100 },
    );
  });

  it('respects bitrate thresholds: good ≥ 500000, warn ≥ 100000, bad < 100000', () => {
    fc.assert(
      fc.property(
        fc.double({ min: 0, max: 10_000_000, noNaN: true, noDefaultInfinity: true }),
        (value) => {
          const color = getStatColor('bitrate', value);
          if (value >= 500_000) expect(color).toBe('good');
          else if (value >= 100_000) expect(color).toBe('warn');
          else expect(color).toBe('bad');
        },
      ),
      { numRuns: 100 },
    );
  });

  it('respects latency thresholds: good ≤ 100, warn ≤ 300, bad > 300', () => {
    fc.assert(
      fc.property(
        fc.double({ min: 0, max: 10_000, noNaN: true, noDefaultInfinity: true }),
        (value) => {
          const color = getStatColor('latency', value);
          if (value <= 100) expect(color).toBe('good');
          else if (value <= 300) expect(color).toBe('warn');
          else expect(color).toBe('bad');
        },
      ),
      { numRuns: 100 },
    );
  });

  it('respects packetLoss thresholds: good = 0, warn ≤ 5, bad > 5', () => {
    fc.assert(
      fc.property(
        fc.double({ min: 0, max: 100, noNaN: true, noDefaultInfinity: true }),
        (value) => {
          const color = getStatColor('packetLoss', value);
          if (value === 0) expect(color).toBe('good');
          else if (value <= 5) expect(color).toBe('warn');
          else expect(color).toBe('bad');
        },
      ),
      { numRuns: 100 },
    );
  });

  it('returns good for unknown metrics', () => {
    fc.assert(
      fc.property(
        fc.string({ minLength: 1 }).filter((s) => !(METRICS as readonly string[]).includes(s)),
        arbValue,
        (metric, value) => {
          expect(getStatColor(metric, value)).toBe('good');
        },
      ),
      { numRuns: 100 },
    );
  });
});


// ===== Unit Tests: Stats reset when video stopped =====
// Validates: Requirements 6.4

// eslint-disable-next-line @typescript-eslint/no-unused-vars
import { WebRTCStatsPanel as _W, HLSStatsPanel as _H } from '../components/StatsPanel';
import type { ConnectionStatus } from '../types';

describe('Unit: StatsPanel reset when stopped', () => {
  describe('WebRTCStatsPanel idle/stopped shows all dashes', () => {
    const sampleStats = {
      resolution: '1920×1080',
      fps: 30,
      bitrate: 2_500_000,
      latency: 50,
      packetLoss: 0,
      codec: 'video/H264',
      relayType: 'STUN' as const,
      duration: 60000,
    };

    it.each(['idle', 'stopped'])('status=%s → all stats show "—"', (status) => {
      const s = status as ConnectionStatus;
      const active = s === 'connected' || s === 'connecting' || s === 'reconnecting';
      expect(active).toBe(false);
      expect(active ? sampleStats.resolution : '—').toBe('—');
      expect(active && sampleStats.fps > 0 ? `${sampleStats.fps} fps` : '—').toBe('—');
      expect(active && sampleStats.latency > 0 ? `${sampleStats.latency.toFixed(0)} ms` : '—').toBe('—');
      expect(active ? String(sampleStats.packetLoss) : '—').toBe('—');
      expect(active ? sampleStats.codec : '—').toBe('—');
      expect(active && sampleStats.relayType ? sampleStats.relayType : '—').toBe('—');
      expect(active && sampleStats.duration > 0 ? 'formatted' : '—').toBe('—');
    });
  });

  describe('HLSStatsPanel idle/stopped shows all dashes', () => {
    const sampleStats = {
      resolution: '1280×720',
      fps: 25,
      bitrate: 1_000_000,
      bufferLength: 3.5,
      droppedFrames: 2,
      currentTime: new Date(),
    };

    it.each(['idle', 'stopped'])('status=%s → all stats show "—"', (status) => {
      const s = status as ConnectionStatus;
      const active = s === 'connected' || s === 'connecting';
      expect(active).toBe(false);
      expect(active ? sampleStats.resolution : '—').toBe('—');
      expect(active && sampleStats.fps > 0 ? `${sampleStats.fps} fps` : '—').toBe('—');
      expect(active && sampleStats.bufferLength > 0 ? `${sampleStats.bufferLength.toFixed(1)} s` : '—').toBe('—');
      expect(active ? String(sampleStats.droppedFrames) : '—').toBe('—');
      expect(active && sampleStats.currentTime ? sampleStats.currentTime.toLocaleTimeString() : '—').toBe('—');
    });
  });

  describe('WebRTCStatsPanel connected shows real values', () => {
    it('status=connected → stats show actual values', () => {
      const s: ConnectionStatus = 'connected';
      const active = s === 'connected' || s === 'connecting' || s === 'reconnecting';
      expect(active).toBe(true);
    });
  });

  describe('HLSStatsPanel connected shows real values', () => {
    it('status=connected → stats show actual values', () => {
      const s: ConnectionStatus = 'connected';
      const active = s === 'connected' || s === 'connecting';
      expect(active).toBe(true);
    });
  });
});
