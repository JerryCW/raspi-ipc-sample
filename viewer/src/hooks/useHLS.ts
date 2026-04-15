import { useState, useRef, useCallback, useEffect } from 'react';
import Hls from 'hls.js';
import type { AWSCredentials, ConnectionStatus, HLSStats, Fragment } from '../types';
import { listFragments, getHlsStreamingUrl } from '../services/kvs';

// ===== Constants =====

export const INITIAL_STATS: HLSStats = {
  resolution: '—',
  fps: 0,
  bitrate: 0,
  bufferLength: 0,
  droppedFrames: 0,
  currentTime: null,
};

// ===== Hook =====

/**
 * HLS playback lifecycle hook.
 *
 * Manages the full lifecycle:
 * 1. loadFragments() — query available fragments via ListFragments API
 * 2. start(startTime, endTime) — get HLS URL via GetHLSStreamingSessionURL (ON_DEMAND), play via hls.js or native
 * 3. Safari detection — use native HLS when video.canPlayType('application/vnd.apple.mpegurl') is truthy
 * 4. Fatal hls.js errors — stop playback, set error status, log error
 * 5. Non-fatal hls.js errors — log only, let hls.js auto-recover
 * 6. stop() — destroy hls.js instance, reset video src, reset stats
 * 7. Stats collection — 1s interval reading video element properties
 *
 * Validates: Requirements 4.3, 4.4, 4.5, 4.9, 4.10
 */
export function useHLS(config: {
  streamName: string;
  credentials: AWSCredentials | null;
  region: string;
  onLog: (msg: string) => void;
}): {
  status: ConnectionStatus;
  videoRef: React.RefObject<HTMLVideoElement>;
  stats: HLSStats;
  fragments: Fragment[];
  start: (startTime: Date, endTime: Date) => Promise<void>;
  stop: () => void;
  loadFragments: (startTime: Date, endTime: Date) => Promise<void>;
} {
  const { streamName, credentials, region, onLog } = config;

  const [status, setStatus] = useState<ConnectionStatus>('idle');
  const [stats, setStats] = useState<HLSStats>(INITIAL_STATS);
  const [fragments, setFragments] = useState<Fragment[]>([]);
  const videoRef = useRef<HTMLVideoElement>(null!);

  // Track the playback start timestamp for mapping video.currentTime to real time
  const playbackStartRef = useRef<Date | null>(null);

  // Mutable refs for resources that need cleanup
  const hlsRef = useRef<Hls | null>(null);
  const statsIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const isStoppedRef = useRef(false);

  /** Clean up all resources. */
  const cleanup = useCallback(() => {
    if (statsIntervalRef.current) {
      clearInterval(statsIntervalRef.current);
      statsIntervalRef.current = null;
    }
    if (hlsRef.current) {
      try {
        hlsRef.current.destroy();
      } catch {
        // ignore
      }
      hlsRef.current = null;
    }
    // Reset video element
    if (videoRef.current) {
      videoRef.current.pause();
      videoRef.current.removeAttribute('src');
      videoRef.current.load();
    }
  }, []);

  /** Start stats collection interval (1s). */
  const startStatsCollection = useCallback(() => {
    if (statsIntervalRef.current) {
      clearInterval(statsIntervalRef.current);
    }

    statsIntervalRef.current = setInterval(() => {
      const video = videoRef.current;
      if (!video) return;

      try {
        // Resolution
        const resolution =
          video.videoWidth && video.videoHeight
            ? `${video.videoWidth}×${video.videoHeight}`
            : '—';

        // FPS — use getVideoPlaybackQuality if available
        const quality = video.getVideoPlaybackQuality?.();
        const droppedFrames = quality?.droppedVideoFrames ?? 0;
        const totalFrames = quality?.totalVideoFrames ?? 0;

        // Estimate FPS from total frames and current time
        const fps =
          video.currentTime > 0
            ? Math.round(totalFrames / video.currentTime)
            : 0;

        // Buffer length — time ahead of current position
        let bufferLength = 0;
        if (video.buffered.length > 0) {
          const bufferedEnd = video.buffered.end(video.buffered.length - 1);
          bufferLength = Math.max(0, bufferedEnd - video.currentTime);
        }

        // Bitrate — estimate from hls.js level info if available
        let bitrate = 0;
        const hls = hlsRef.current;
        if (hls && hls.currentLevel >= 0 && hls.levels[hls.currentLevel]) {
          bitrate = hls.levels[hls.currentLevel].bitrate ?? 0;
        }

        // Current playback time as Date — map video.currentTime to real timestamp
        const playStart = playbackStartRef.current;
        const currentTime = video.currentTime > 0 && playStart
          ? new Date(playStart.getTime() + video.currentTime * 1000)
          : null;

        setStats({
          resolution,
          fps,
          bitrate,
          bufferLength: Math.round(bufferLength * 100) / 100,
          droppedFrames,
          currentTime,
        });
      } catch {
        // Stats collection failure is non-fatal
      }
    }, 1000);
  }, []);

  /** Stop playback and reset state. Validates: Requirements 4.10 */
  const stop = useCallback(() => {
    isStoppedRef.current = true;
    cleanup();
    setStatus('stopped');
    setStats(INITIAL_STATS);
    onLog('HLS playback stopped');
  }, [cleanup, onLog]);

  /**
   * Load available fragments for a time range.
   * Calls ListFragments API and stores results in state.
   */
  const loadFragments = useCallback(
    async (startTime: Date, endTime: Date) => {
      if (!credentials) {
        onLog('Error: No AWS credentials available');
        return;
      }

      onLog(`Loading fragments: ${startTime.toISOString()} → ${endTime.toISOString()}`);

      try {
        const result = await listFragments(streamName, startTime, endTime, credentials, region);
        setFragments(result);
        onLog(`Loaded ${result.length} fragment(s)`);
      } catch (err) {
        const msg = err instanceof Error ? err.message : String(err);
        onLog(`Error loading fragments: ${msg}`);
        setFragments([]);
      }
    },
    [streamName, credentials, region, onLog],
  );

  /**
   * Start HLS playback for a given time range.
   *
   * 1. Get HLS streaming session URL (ON_DEMAND mode)
   * 2. Detect Safari native HLS support
   * 3. Play via hls.js or native HLS
   *
   * Validates: Requirements 4.3, 4.4, 4.5, 4.9
   */
  const start = useCallback(
    async (startTime: Date, endTime: Date) => {
      if (!credentials) {
        onLog('Error: No AWS credentials available');
        setStatus('error');
        return;
      }

      // Clean up any previous session
      cleanup();
      isStoppedRef.current = false;
      playbackStartRef.current = startTime;
      setStatus('connecting');
      onLog('Starting HLS playback...');

      try {
        // Step 1: Get HLS streaming session URL (ON_DEMAND)
        onLog(`Requesting HLS URL: ${startTime.toISOString()} → ${endTime.toISOString()}`);
        const hlsUrl = await getHlsStreamingUrl(
          streamName,
          startTime,
          endTime,
          credentials,
          region,
        );
        onLog('HLS streaming session URL obtained');

        if (isStoppedRef.current) return;

        const video = videoRef.current;
        if (!video) {
          onLog('Error: Video element not available');
          setStatus('error');
          return;
        }

        // Step 2: Check for native HLS support (Safari only)
        // Chrome on macOS may report canPlayType truthy but fail to play KVS HLS.
        // Force hls.js for non-Safari browsers.
        // Validates: Requirements 4.5
        const isSafari = /^((?!chrome|android).)*safari/i.test(navigator.userAgent);
        const canPlayNativeHLS = isSafari && video.canPlayType('application/vnd.apple.mpegurl');

        if (canPlayNativeHLS) {
          // Safari native HLS — set src directly
          onLog('Using native HLS playback (Safari)');
          video.src = hlsUrl;

          video.onloadedmetadata = () => {
            if (!isStoppedRef.current) {
              video.play().catch(() => {
                onLog('Autoplay blocked — user interaction required');
              });
              setStatus('connected');
              onLog('HLS playback started (native)');
              startStatsCollection();
            }
          };

          video.onerror = () => {
            if (!isStoppedRef.current) {
              const errorMsg = video.error?.message ?? 'Unknown playback error';
              onLog(`Native HLS error: ${errorMsg}`);
              setStatus('error');
            }
          };
        } else if (Hls.isSupported()) {
          // Step 3: Use hls.js for non-Safari browsers
          // Validates: Requirements 4.4
          onLog('Using hls.js for playback');
          const hls = new Hls({
            enableWorker: true,
            lowLatencyMode: false,
          });
          hlsRef.current = hls;

          hls.loadSource(hlsUrl);
          hls.attachMedia(video);

          hls.on(Hls.Events.MANIFEST_PARSED, () => {
            if (!isStoppedRef.current) {
              video.play().catch(() => {
                onLog('Autoplay blocked — user interaction required');
              });
              setStatus('connected');
              onLog('HLS playback started (hls.js)');
              startStatsCollection();
            }
          });

          // Fatal error handling — Validates: Requirements 4.9
          hls.on(Hls.Events.ERROR, (_event, data) => {
            if (data.fatal) {
              onLog(`HLS fatal error: ${data.type} — ${data.details}`);
              cleanup();
              setStatus('error');
            } else {
              // Non-fatal: log only, hls.js auto-recovers
              onLog(`HLS non-fatal error: ${data.type} — ${data.details}`);
            }
          });
        } else {
          // Browser supports neither hls.js nor native HLS
          onLog('Error: Browser does not support HLS playback');
          setStatus('error');
        }
      } catch (err) {
        const msg = err instanceof Error ? err.message : String(err);
        onLog(`HLS connection error: ${msg}`);
        cleanup();
        setStatus('error');
      }
    },
    [streamName, credentials, region, onLog, cleanup, startStatsCollection],
  );

  // Cleanup on unmount
  useEffect(() => {
    return () => {
      cleanup();
    };
  }, [cleanup]);

  return { status, videoRef, stats, fragments, start, stop, loadFragments };
}
