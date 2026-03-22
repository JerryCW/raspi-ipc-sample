import { describe, it, expect } from 'vitest';
import * as fc from 'fast-check';
import { parseWebRTCStats, parseHLSStats } from '../hooks/useStats';

/**
 * Feature: viewer-rewrite, Property 12: WebRTC 统计解析
 *
 * For any valid RTCStatsReport (containing inbound-rtp, candidate-pair entries),
 * the parse function should extract resolution, fps, bitrate, latency, packetLoss,
 * codec, and relayType — and all numeric fields should be non-negative.
 *
 * **Validates: Requirements 6.1**
 */

// Arbitrary for inbound-rtp video stat entry
const arbInboundRtp = fc.record({
  type: fc.constant('inbound-rtp'),
  kind: fc.constant('video'),
  frameWidth: fc.option(fc.integer({ min: 1, max: 7680 }), { nil: undefined }),
  frameHeight: fc.option(fc.integer({ min: 1, max: 4320 }), { nil: undefined }),
  framesPerSecond: fc.option(fc.double({ min: 0, max: 120, noNaN: true, noDefaultInfinity: true }), { nil: undefined }),
  bytesReceived: fc.option(fc.integer({ min: 0, max: 100_000_000 }), { nil: undefined }),
  packetsLost: fc.option(fc.integer({ min: -10, max: 10000 }), { nil: undefined }),
  codecId: fc.option(fc.constant('codec-0'), { nil: undefined }),
});

// Arbitrary for codec stat entry
const arbCodecStat = fc.record({
  type: fc.constant('codec'),
  mimeType: fc.option(fc.constantFrom('video/H264', 'video/VP8', 'video/VP9', 'video/AV1'), { nil: undefined }),
});

// Arbitrary for candidate-pair stat entry
const arbCandidatePair = fc.record({
  type: fc.constant('candidate-pair'),
  state: fc.constantFrom('succeeded', 'waiting', 'in-progress', 'failed'),
  currentRoundTripTime: fc.option(fc.double({ min: 0, max: 10, noNaN: true, noDefaultInfinity: true }), { nil: undefined }),
  localCandidateId: fc.option(fc.constant('local-0'), { nil: undefined }),
});

// Arbitrary for local candidate stat entry
const arbLocalCandidate = fc.record({
  type: fc.constant('local-candidate'),
  candidateType: fc.constantFrom('host', 'srflx', 'relay'),
});

describe('Property 12: WebRTC 统计解析', () => {
  it('all numeric fields in parsed WebRTC stats are non-negative', () => {
    fc.assert(
      fc.property(
        arbInboundRtp,
        arbCodecStat,
        arbCandidatePair,
        arbLocalCandidate,
        (inboundRtp, codecStat, candidatePair, localCandidate) => {
          const report = new Map<string, Record<string, unknown>>();
          report.set('inbound-rtp-0', inboundRtp as Record<string, unknown>);
          report.set('codec-0', codecStat as Record<string, unknown>);
          report.set('candidate-pair-0', candidatePair as Record<string, unknown>);
          report.set('local-0', localCandidate as Record<string, unknown>);

          const stats = parseWebRTCStats(report);

          expect(stats.fps).toBeGreaterThanOrEqual(0);
          expect(stats.bitrate).toBeGreaterThanOrEqual(0);
          expect(stats.latency).toBeGreaterThanOrEqual(0);
          expect(stats.packetLoss).toBeGreaterThanOrEqual(0);
          expect(stats.duration).toBeGreaterThanOrEqual(0);
        },
      ),
      { numRuns: 100 },
    );
  });

  it('resolution is either "—" or a valid WxH string', () => {
    fc.assert(
      fc.property(
        arbInboundRtp,
        arbCodecStat,
        arbCandidatePair,
        arbLocalCandidate,
        (inboundRtp, codecStat, candidatePair, localCandidate) => {
          const report = new Map<string, Record<string, unknown>>();
          report.set('inbound-rtp-0', inboundRtp as Record<string, unknown>);
          report.set('codec-0', codecStat as Record<string, unknown>);
          report.set('candidate-pair-0', candidatePair as Record<string, unknown>);
          report.set('local-0', localCandidate as Record<string, unknown>);

          const stats = parseWebRTCStats(report);

          // Resolution should be "—" or match "NxN" pattern
          expect(stats.resolution).toMatch(/^(—|\d+×\d+)$/);
        },
      ),
      { numRuns: 100 },
    );
  });

  it('relayType is one of Direct, STUN, TURN, or null', () => {
    fc.assert(
      fc.property(
        arbInboundRtp,
        arbCodecStat,
        arbCandidatePair,
        arbLocalCandidate,
        (inboundRtp, codecStat, candidatePair, localCandidate) => {
          const report = new Map<string, Record<string, unknown>>();
          report.set('inbound-rtp-0', inboundRtp as Record<string, unknown>);
          report.set('codec-0', codecStat as Record<string, unknown>);
          report.set('candidate-pair-0', candidatePair as Record<string, unknown>);
          report.set('local-0', localCandidate as Record<string, unknown>);

          const stats = parseWebRTCStats(report);

          expect([null, 'Direct', 'STUN', 'TURN']).toContain(stats.relayType);
        },
      ),
      { numRuns: 100 },
    );
  });

  it('empty report produces default non-negative stats', () => {
    const report = new Map<string, Record<string, unknown>>();
    const stats = parseWebRTCStats(report);

    expect(stats.fps).toBe(0);
    expect(stats.bitrate).toBe(0);
    expect(stats.latency).toBe(0);
    expect(stats.packetLoss).toBe(0);
    expect(stats.resolution).toBe('—');
    expect(stats.codec).toBe('—');
    expect(stats.relayType).toBeNull();
  });
});


/**
 * Feature: viewer-rewrite, Property 13: HLS 统计解析
 *
 * For any valid video element state (with buffered ranges, videoWidth/Height,
 * playback quality), the parse function should extract resolution, fps, bitrate,
 * bufferLength, and droppedFrames — and all numeric fields should be non-negative.
 *
 * **Validates: Requirements 6.2**
 */

// Arbitrary for a mock video element
const arbVideoElement = fc.record({
  videoWidth: fc.integer({ min: 0, max: 7680 }),
  videoHeight: fc.integer({ min: 0, max: 4320 }),
  currentTime: fc.double({ min: 0, max: 86400, noNaN: true, noDefaultInfinity: true }),
  droppedVideoFrames: fc.integer({ min: 0, max: 10000 }),
  totalVideoFrames: fc.integer({ min: 0, max: 1_000_000 }),
  bufferedEnd: fc.double({ min: 0, max: 86400, noNaN: true, noDefaultInfinity: true }),
  hasBuffered: fc.boolean(),
});

// Arbitrary for optional hls.js instance
const arbHlsInstance = fc.option(
  fc.record({
    currentLevel: fc.integer({ min: -1, max: 10 }),
    levels: fc.array(
      fc.record({
        bitrate: fc.option(fc.integer({ min: 0, max: 50_000_000 }), { nil: undefined }),
      }),
      { minLength: 0, maxLength: 11 },
    ),
  }),
  { nil: null },
);

function buildMockVideo(params: {
  videoWidth: number;
  videoHeight: number;
  currentTime: number;
  droppedVideoFrames: number;
  totalVideoFrames: number;
  bufferedEnd: number;
  hasBuffered: boolean;
}) {
  return {
    videoWidth: params.videoWidth,
    videoHeight: params.videoHeight,
    currentTime: params.currentTime,
    buffered: {
      length: params.hasBuffered ? 1 : 0,
      end: (_index: number) => params.bufferedEnd,
    },
    getVideoPlaybackQuality: () => ({
      droppedVideoFrames: params.droppedVideoFrames,
      totalVideoFrames: params.totalVideoFrames,
    }),
  };
}

describe('Property 13: HLS 统计解析', () => {
  it('all numeric fields in parsed HLS stats are non-negative', () => {
    fc.assert(
      fc.property(arbVideoElement, arbHlsInstance, (videoParams, hls) => {
        const video = buildMockVideo(videoParams);
        const stats = parseHLSStats(video, hls);

        expect(stats.fps).toBeGreaterThanOrEqual(0);
        expect(stats.bitrate).toBeGreaterThanOrEqual(0);
        expect(stats.bufferLength).toBeGreaterThanOrEqual(0);
        expect(stats.droppedFrames).toBeGreaterThanOrEqual(0);
      }),
      { numRuns: 100 },
    );
  });

  it('resolution is either "—" or a valid WxH string', () => {
    fc.assert(
      fc.property(arbVideoElement, arbHlsInstance, (videoParams, hls) => {
        const video = buildMockVideo(videoParams);
        const stats = parseHLSStats(video, hls);

        expect(stats.resolution).toMatch(/^(—|\d+×\d+)$/);
      }),
      { numRuns: 100 },
    );
  });

  it('currentTime is either null or a Date object', () => {
    fc.assert(
      fc.property(arbVideoElement, arbHlsInstance, (videoParams, hls) => {
        const video = buildMockVideo(videoParams);
        const stats = parseHLSStats(video, hls);

        if (stats.currentTime !== null) {
          expect(stats.currentTime).toBeInstanceOf(Date);
        }
      }),
      { numRuns: 100 },
    );
  });

  it('resolution is "—" when videoWidth or videoHeight is 0', () => {
    fc.assert(
      fc.property(
        arbVideoElement.map((v) => ({ ...v, videoWidth: 0 })),
        arbHlsInstance,
        (videoParams, hls) => {
          const video = buildMockVideo(videoParams);
          const stats = parseHLSStats(video, hls);
          expect(stats.resolution).toBe('—');
        },
      ),
      { numRuns: 100 },
    );
  });
});
