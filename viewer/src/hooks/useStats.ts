import { useState, useEffect, useRef } from 'react';
import type { WebRTCStats, HLSStats } from '../types';

// ===== Initial values =====

const INITIAL_WEBRTC_STATS: WebRTCStats = {
  resolution: '—',
  fps: 0,
  bitrate: 0,
  latency: 0,
  packetLoss: 0,
  codec: '—',
  relayType: null,
  duration: 0,
};

const INITIAL_HLS_STATS: HLSStats = {
  resolution: '—',
  fps: 0,
  bitrate: 0,
  bufferLength: 0,
  droppedFrames: 0,
  currentTime: null,
};

// ===== Exported pure functions for property testing =====

/**
 * Determine color category for a stat metric value.
 * Exported for Property 11 testing.
 *
 * Thresholds:
 * - fps: good ≥ 20, warn ≥ 10, bad < 10
 * - bitrate: good ≥ 500000, warn ≥ 100000, bad < 100000
 * - latency: good ≤ 100, warn ≤ 300, bad > 300
 * - packetLoss: good = 0, warn ≤ 5, bad > 5
 * - bufferLength: good ≥ 2, warn ≥ 0.5, bad < 0.5
 * - droppedFrames: good = 0, warn ≤ 10, bad > 10
 *
 * Validates: Requirements 6.3
 */
export function getStatColor(
  metric: string,
  value: number,
): 'good' | 'warn' | 'bad' {
  switch (metric) {
    case 'fps':
      if (value >= 20) return 'good';
      if (value >= 10) return 'warn';
      return 'bad';
    case 'bitrate':
      if (value >= 500_000) return 'good';
      if (value >= 100_000) return 'warn';
      return 'bad';
    case 'latency':
      if (value <= 100) return 'good';
      if (value <= 300) return 'warn';
      return 'bad';
    case 'packetLoss':
      if (value === 0) return 'good';
      if (value <= 5) return 'warn';
      return 'bad';
    case 'bufferLength':
      if (value >= 2) return 'good';
      if (value >= 0.5) return 'warn';
      return 'bad';
    case 'droppedFrames':
      if (value === 0) return 'good';
      if (value <= 10) return 'warn';
      return 'bad';
    default:
      return 'good';
  }
}

/**
 * Parse an RTCStatsReport (as a Map) into our WebRTCStats shape.
 * Exported for Property 12 testing.
 *
 * Validates: Requirements 6.1
 */
export function parseWebRTCStats(
  report: Map<string, Record<string, unknown>>,
): WebRTCStats {
  let resolution = '—';
  let fps = 0;
  let bitrate = 0;
  let latency = 0;
  let packetLoss = 0;
  let codec = '—';
  let relayType: WebRTCStats['relayType'] = null;

  report.forEach((stat) => {
    if (stat.type === 'inbound-rtp' && stat.kind === 'video') {
      const w = stat.frameWidth as number | undefined;
      const h = stat.frameHeight as number | undefined;
      if (w && h) {
        resolution = `${w}×${h}`;
      }
      fps = Math.max(0, (stat.framesPerSecond as number) ?? 0);
      bitrate = Math.max(0, stat.bytesReceived ? (stat.bytesReceived as number) * 8 : 0);
      packetLoss = Math.max(0, (stat.packetsLost as number) ?? 0);
      if (stat.codecId) {
        const codecStat = report.get(stat.codecId as string);
        if (codecStat) {
          codec = (codecStat.mimeType as string) ?? '—';
        }
      }
    }
    if (stat.type === 'candidate-pair' && stat.state === 'succeeded') {
      latency = Math.max(
        0,
        stat.currentRoundTripTime ? (stat.currentRoundTripTime as number) * 1000 : 0,
      );
      if (stat.localCandidateId) {
        const localCandidate = report.get(stat.localCandidateId as string);
        if (localCandidate) {
          const candidateType = localCandidate.candidateType as string;
          if (candidateType === 'relay') relayType = 'TURN';
          else if (candidateType === 'srflx') relayType = 'STUN';
          else if (candidateType === 'host') relayType = 'Direct';
        }
      }
    }
  });

  return { resolution, fps, bitrate, latency, packetLoss, codec, relayType, duration: 0 };
}


/**
 * Parse HLS stats from a video element and optional hls.js instance.
 * Exported for Property 13 testing.
 *
 * Validates: Requirements 6.2
 */
export function parseHLSStats(
  video: {
    videoWidth: number;
    videoHeight: number;
    currentTime: number;
    buffered: { length: number; end: (index: number) => number };
    getVideoPlaybackQuality?: () => {
      droppedVideoFrames: number;
      totalVideoFrames: number;
    };
  },
  hls: { currentLevel: number; levels: Array<{ bitrate?: number }> } | null,
): HLSStats {
  const resolution =
    video.videoWidth && video.videoHeight
      ? `${video.videoWidth}×${video.videoHeight}`
      : '—';

  const quality = video.getVideoPlaybackQuality?.();
  const droppedFrames = Math.max(0, quality?.droppedVideoFrames ?? 0);
  const totalFrames = Math.max(0, quality?.totalVideoFrames ?? 0);

  const fps =
    video.currentTime > 0 ? Math.max(0, Math.round(totalFrames / video.currentTime)) : 0;

  let bufferLength = 0;
  if (video.buffered.length > 0) {
    const bufferedEnd = video.buffered.end(video.buffered.length - 1);
    bufferLength = Math.max(0, bufferedEnd - video.currentTime);
  }

  let bitrate = 0;
  if (hls && hls.currentLevel >= 0 && hls.levels[hls.currentLevel]) {
    bitrate = Math.max(0, hls.levels[hls.currentLevel].bitrate ?? 0);
  }

  const currentTime = video.currentTime > 0 ? new Date() : null;

  return {
    resolution,
    fps,
    bitrate,
    bufferLength: Math.max(0, Math.round(bufferLength * 100) / 100),
    droppedFrames,
    currentTime,
  };
}

// ===== Hooks =====

/**
 * WebRTC stats collection hook. Updates every second.
 * Validates: Requirements 6.1
 */
export function useWebRTCStats(
  peerConnection: RTCPeerConnection | null,
): WebRTCStats {
  const [stats, setStats] = useState<WebRTCStats>(INITIAL_WEBRTC_STATS);
  const startTimeRef = useRef<number>(0);

  useEffect(() => {
    if (!peerConnection) {
      setStats(INITIAL_WEBRTC_STATS);
      return;
    }

    startTimeRef.current = Date.now();

    const interval = setInterval(async () => {
      try {
        const report = await peerConnection.getStats();
        const mapped = new Map<string, Record<string, unknown>>();
        report.forEach((value, key) => {
          mapped.set(key, value as Record<string, unknown>);
        });
        const parsed = parseWebRTCStats(mapped);
        parsed.duration = Date.now() - startTimeRef.current;
        setStats(parsed);
      } catch {
        // Stats collection failure is non-fatal
      }
    }, 1000);

    return () => clearInterval(interval);
  }, [peerConnection]);

  return stats;
}

/**
 * HLS stats collection hook. Updates every second.
 * Validates: Requirements 6.2
 */
export function useHLSStats(
  videoElement: HTMLVideoElement | null,
  hlsInstance: unknown,
): HLSStats {
  const [stats, setStats] = useState<HLSStats>(INITIAL_HLS_STATS);

  useEffect(() => {
    if (!videoElement) {
      setStats(INITIAL_HLS_STATS);
      return;
    }

    const interval = setInterval(() => {
      try {
        const hls = hlsInstance as {
          currentLevel: number;
          levels: Array<{ bitrate?: number }>;
        } | null;
        const parsed = parseHLSStats(videoElement, hls);
        setStats(parsed);
      } catch {
        // Stats collection failure is non-fatal
      }
    }, 1000);

    return () => clearInterval(interval);
  }, [videoElement, hlsInstance]);

  return stats;
}
