import { useState, useRef, useCallback, useEffect } from 'react';
import type { AWSCredentials, ConnectionStatus, WebRTCStats } from '../types';
import { getSignalingChannelConfig, getIceServerConfig } from '../services/kvs';
import { createPresignedUrl } from '../services/sigv4';

// ===== Pure helper functions (exported for testability) =====

/**
 * Map RTCIceConnectionState to our ConnectionStatus type.
 *
 * - 'connected' | 'completed' → 'connected'
 * - 'disconnected' → 'reconnecting'
 * - 'failed' → 'error'
 * - 'new' | 'checking' → 'connecting'
 * - 'closed' → 'stopped'
 *
 * Validates: Requirements 3.10
 */
export function mapIceState(state: RTCIceConnectionState): ConnectionStatus {
  switch (state) {
    case 'connected':
    case 'completed':
      return 'connected';
    case 'disconnected':
      return 'reconnecting';
    case 'failed':
      return 'error';
    case 'new':
    case 'checking':
      return 'connecting';
    case 'closed':
      return 'stopped';
    default:
      return 'connecting';
  }
}

/**
 * ICE candidate buffer for managing candidates that arrive before the
 * remote description (SDP answer) is set.
 *
 * - Before answer: candidates are buffered via `add()`
 * - After answer: `flush()` returns all buffered candidates and clears the buffer
 * - Subsequent `add()` calls after flush still work (buffer is reusable)
 *
 * Validates: Requirements 3.6
 */
export function createIceCandidateBuffer() {
  let candidates: RTCIceCandidateInit[] = [];

  return {
    /** Add a candidate to the buffer. */
    add(candidate: RTCIceCandidateInit) {
      candidates.push(candidate);
    },
    /** Flush all buffered candidates and clear the buffer. */
    flush(): RTCIceCandidateInit[] {
      const flushed = [...candidates];
      candidates = [];
      return flushed;
    },
    /** Current buffer contents (read-only snapshot). */
    get buffer(): readonly RTCIceCandidateInit[] {
      return [...candidates];
    },
  };
}

/**
 * Retry tracker for connection attempts.
 *
 * Validates: Requirements 3.7, 3.8
 */
export function createRetryTracker(maxRetries: number) {
  let attemptCount = 0;

  return {
    /** Whether another retry is allowed. */
    canRetry(): boolean {
      return attemptCount < maxRetries;
    },
    /** Record a retry attempt. Returns the new attempt count. */
    recordAttempt(): number {
      attemptCount += 1;
      return attemptCount;
    },
    /** Reset the tracker. */
    reset() {
      attemptCount = 0;
    },
    /** Current attempt count. */
    get attempts(): number {
      return attemptCount;
    },
  };
}


// ===== Constants =====

const CONNECTION_TIMEOUT_MS = 15_000;
const MAX_RETRIES = 3;
const TOTAL_SESSION_TIMEOUT_MS = 60_000; // 60s total timeout — force error if still not connected
const CLIENT_ID = `viewer-${Date.now()}`;

const INITIAL_STATS: WebRTCStats = {
  resolution: '—',
  fps: 0,
  bitrate: 0,
  latency: 0,
  packetLoss: 0,
  codec: '—',
  relayType: null,
  duration: 0,
};

// ===== Hook =====

/**
 * WebRTC connection lifecycle hook.
 *
 * Manages the full lifecycle:
 * 1. KVS API → get signaling channel config + ICE servers
 * 2. SigV4 sign the WSS endpoint
 * 3. Create RTCPeerConnection with STUN/TURN
 * 4. Add video + audio recvonly transceivers
 * 5. Open WebSocket, send SDP offer, handle SDP answer
 * 6. Buffer ICE candidates before answer, flush after
 * 7. 15s timeout → auto-retry up to 3 times
 * 8. stop() cleans up everything
 *
 * Validates: Requirements 3.1–3.10
 */
export function useWebRTC(config: {
  channelName: string;
  credentials: AWSCredentials | null;
  region: string;
  onLog: (msg: string) => void;
}): {
  status: ConnectionStatus;
  videoRef: React.RefObject<HTMLVideoElement>;
  stats: WebRTCStats;
  start: () => Promise<void>;
  stop: () => void;
} {
  const { channelName, credentials, region, onLog } = config;

  const [status, setStatus] = useState<ConnectionStatus>('idle');
  const [stats, setStats] = useState<WebRTCStats>(INITIAL_STATS);
  const videoRef = useRef<HTMLVideoElement>(null!);

  // Mutable refs for resources that need cleanup
  const wsRef = useRef<WebSocket | null>(null);
  const pcRef = useRef<RTCPeerConnection | null>(null);
  const timeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const sessionTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const statsIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const retryRef = useRef(createRetryTracker(MAX_RETRIES));
  const startTimeRef = useRef<number>(0);
  const isStoppedRef = useRef(false);

  /** Clean up all resources. */
  const cleanup = useCallback(() => {
    if (timeoutRef.current) {
      clearTimeout(timeoutRef.current);
      timeoutRef.current = null;
    }
    if (sessionTimeoutRef.current) {
      clearTimeout(sessionTimeoutRef.current);
      sessionTimeoutRef.current = null;
    }
    if (statsIntervalRef.current) {
      clearInterval(statsIntervalRef.current);
      statsIntervalRef.current = null;
    }
    if (wsRef.current) {
      try {
        wsRef.current.close();
      } catch {
        // ignore
      }
      wsRef.current = null;
    }
    if (pcRef.current) {
      // Stop all media tracks
      const receivers = pcRef.current.getReceivers();
      for (const receiver of receivers) {
        if (receiver.track) {
          receiver.track.stop();
        }
      }
      try {
        pcRef.current.close();
      } catch {
        // ignore
      }
      pcRef.current = null;
    }
    // Clear video element srcObject
    if (videoRef.current) {
      videoRef.current.srcObject = null;
    }
  }, []);

  /** Stop the connection and reset state. */
  const stop = useCallback(() => {
    isStoppedRef.current = true;
    cleanup();
    retryRef.current.reset();
    setStatus('stopped');
    setStats(INITIAL_STATS);
    onLog('WebRTC connection stopped');
  }, [cleanup, onLog]);

  /** Start stats collection interval. */
  const startStatsCollection = useCallback(() => {
    if (statsIntervalRef.current) {
      clearInterval(statsIntervalRef.current);
    }
    startTimeRef.current = Date.now();

    statsIntervalRef.current = setInterval(async () => {
      const pc = pcRef.current;
      if (!pc) return;

      try {
        const report = await pc.getStats();
        let resolution = '—';
        let fps = 0;
        let bitrate = 0;
        let latency = 0;
        let packetLoss = 0;
        let codec = '—';
        let relayType: WebRTCStats['relayType'] = null;

        report.forEach((stat) => {
          if (stat.type === 'inbound-rtp' && stat.kind === 'video') {
            if (stat.frameWidth && stat.frameHeight) {
              resolution = `${stat.frameWidth}×${stat.frameHeight}`;
            }
            fps = stat.framesPerSecond ?? 0;
            bitrate = stat.bytesReceived ? stat.bytesReceived * 8 : 0;
            packetLoss = stat.packetsLost ?? 0;
            if (stat.codecId) {
              const codecStat = report.get(stat.codecId);
              if (codecStat) {
                codec = codecStat.mimeType ?? '—';
              }
            }
          }
          if (stat.type === 'candidate-pair' && stat.state === 'succeeded') {
            latency = stat.currentRoundTripTime
              ? stat.currentRoundTripTime * 1000
              : 0;
            // Determine relay type from local candidate
            if (stat.localCandidateId) {
              const localCandidate = report.get(stat.localCandidateId);
              if (localCandidate) {
                const candidateType = localCandidate.candidateType;
                if (candidateType === 'relay') {
                  relayType = 'TURN';
                } else if (candidateType === 'srflx') {
                  relayType = 'STUN';
                } else if (candidateType === 'host') {
                  relayType = 'Direct';
                }
              }
            }
          }
        });

        const duration = Date.now() - startTimeRef.current;

        setStats({
          resolution,
          fps,
          bitrate,
          latency,
          packetLoss,
          codec,
          relayType,
          duration,
        });
      } catch {
        // Stats collection failure is non-fatal
      }
    }, 1000);
  }, []);

  /** Core connection logic. Separated for retry support. */
  const connect = useCallback(async () => {
    if (!credentials) {
      onLog('Error: No AWS credentials available');
      setStatus('error');
      return;
    }

    isStoppedRef.current = false;
    setStatus('connecting');
    onLog('Starting WebRTC connection...');

    try {
      // Step 1: Get signaling channel config
      onLog('Getting signaling channel config...');
      const signalingConfig = await getSignalingChannelConfig(
        channelName,
        credentials,
        region
      );
      onLog(
        `Signaling channel ARN: ${signalingConfig.channelArn}`
      );

      if (isStoppedRef.current) return;

      // Step 2: Get ICE server config
      onLog('Getting ICE server config...');
      const iceServers = await getIceServerConfig(
        signalingConfig.channelArn,
        signalingConfig.httpsEndpoint,
        credentials,
        region
      );
      onLog(`Got ${iceServers.length} ICE server(s)`);

      if (isStoppedRef.current) return;

      // Step 3: Create presigned URL
      onLog('Signing WSS endpoint...');
      const presignedUrl = await createPresignedUrl(
        signalingConfig.wssEndpoint,
        signalingConfig.channelArn,
        CLIENT_ID,
        credentials,
        region
      );
      onLog('WSS endpoint signed');

      if (isStoppedRef.current) return;

      // Step 4: Create RTCPeerConnection
      const pc = new RTCPeerConnection({ iceServers });
      pcRef.current = pc;
      onLog('RTCPeerConnection created');

      // Step 5: Add recvonly transceivers
      pc.addTransceiver('video', { direction: 'recvonly' });
      pc.addTransceiver('audio', { direction: 'recvonly' });
      onLog('Added video + audio recvonly transceivers');

      // ICE candidate buffer
      const candidateBuffer = createIceCandidateBuffer();
      let remoteDescriptionSet = false;

      // Handle ICE connection state changes
      pc.oniceconnectionstatechange = () => {
        const iceState = pc.iceConnectionState;
        onLog(`ICE connection state: ${iceState}`);
        const mappedStatus = mapIceState(iceState);
        setStatus(mappedStatus);

        if (iceState === 'connected' || iceState === 'completed') {
          // Connection established — clear timeouts, reset retries
          if (timeoutRef.current) {
            clearTimeout(timeoutRef.current);
            timeoutRef.current = null;
          }
          if (sessionTimeoutRef.current) {
            clearTimeout(sessionTimeoutRef.current);
            sessionTimeoutRef.current = null;
          }
          retryRef.current.reset();
          startStatsCollection();
          onLog('WebRTC connected successfully');
        }
      };

      // Handle incoming tracks
      pc.ontrack = (event) => {
        onLog(`Received ${event.track.kind} track`);
        if (event.track.kind === 'video' && videoRef.current) {
          const stream =
            event.streams[0] ?? new MediaStream([event.track]);
          videoRef.current.srcObject = stream;
          videoRef.current.play().catch(() => {
            // Autoplay may be blocked
            onLog('Autoplay blocked — user interaction required');
          });
        }
      };

      if (isStoppedRef.current) {
        pc.close();
        return;
      }

      // Step 6: Open WebSocket
      onLog('Opening WebSocket connection...');
      const ws = new WebSocket(presignedUrl);
      wsRef.current = ws;

      // Send local ICE candidates to MASTER via WebSocket
      pc.onicecandidate = (event) => {
        if (event.candidate && ws.readyState === WebSocket.OPEN) {
          const message = JSON.stringify({
            action: 'ICE_CANDIDATE',
            messagePayload: btoa(JSON.stringify(event.candidate)),
            recipientClientId: 'MASTER',
          });
          ws.send(message);
          onLog(`Sent local ICE candidate: ${event.candidate.candidate.slice(0, 50)}...`);
        }
      };

      // Set connection timeout
      timeoutRef.current = setTimeout(() => {
        onLog('Connection timeout (15s)');
        cleanup();

        if (retryRef.current.canRetry()) {
          const attempt = retryRef.current.recordAttempt();
          onLog(
            `Retrying connection (attempt ${attempt}/${MAX_RETRIES})...`
          );
          setStatus('reconnecting');
          connect();
        } else {
          onLog(
            'Max retries reached — connection failed'
          );
          setStatus('error');
        }
      }, CONNECTION_TIMEOUT_MS);

      ws.onopen = async () => {
        onLog('WebSocket connected');

        if (isStoppedRef.current) return;

        try {
          // Create and send SDP offer
          const offer = await pc.createOffer();
          await pc.setLocalDescription(offer);
          onLog('SDP offer created and set as local description');

          const offerMessage = JSON.stringify({
            action: 'SDP_OFFER',
            messagePayload: btoa(JSON.stringify(offer)),
            recipientClientId: 'MASTER',
          });
          ws.send(offerMessage);
          onLog('SDP offer sent to MASTER');
        } catch (err) {
          onLog(
            `Error creating/sending offer: ${err instanceof Error ? err.message : String(err)}`
          );
          cleanup();
          setStatus('error');
        }
      };

      ws.onmessage = async (event) => {
        try {
          const data = event.data as string;
          if (!data || data.trim() === '') return; // Skip empty/heartbeat frames
          const message = JSON.parse(data);
          const { messageType, messagePayload } = message;

          if (messageType === 'SDP_ANSWER') {
            onLog('Received SDP answer');
            const answer = JSON.parse(atob(messagePayload));
            await pc.setRemoteDescription(answer);
            remoteDescriptionSet = true;
            onLog('Remote description set');

            // Flush buffered ICE candidates
            const buffered = candidateBuffer.flush();
            if (buffered.length > 0) {
              onLog(
                `Flushing ${buffered.length} buffered ICE candidate(s)`
              );
              for (const candidate of buffered) {
                await pc.addIceCandidate(candidate);
              }
            }
          } else if (messageType === 'ICE_CANDIDATE') {
            const candidate = JSON.parse(atob(messagePayload));
            if (remoteDescriptionSet) {
              await pc.addIceCandidate(candidate);
              onLog('ICE candidate added directly');
            } else {
              candidateBuffer.add(candidate);
              onLog('ICE candidate buffered (waiting for answer)');
            }
          }
        } catch (err) {
          onLog(
            `Error processing WebSocket message: ${err instanceof Error ? err.message : String(err)}`
          );
        }
      };

      ws.onerror = (event) => {
        onLog(`WebSocket error: ${String(event)}`);
      };

      ws.onclose = () => {
        onLog('WebSocket closed');
      };
    } catch (err) {
      const msg =
        err instanceof Error ? err.message : String(err);
      onLog(`Connection error: ${msg}`);
      cleanup();

      if (!isStoppedRef.current && retryRef.current.canRetry()) {
        const attempt = retryRef.current.recordAttempt();
        onLog(
          `Retrying connection (attempt ${attempt}/${MAX_RETRIES})...`
        );
        setStatus('reconnecting');
        connect();
      } else if (!isStoppedRef.current) {
        onLog('Max retries reached — connection failed');
        setStatus('error');
      }
    }
  }, [
    channelName,
    credentials,
    region,
    onLog,
    cleanup,
    startStatsCollection,
  ]);

  /** Public start method — resets retry tracker and begins connection. */
  const start = useCallback(async () => {
    cleanup();
    retryRef.current.reset();
    isStoppedRef.current = false;

    // Total session timeout: if not connected within 60s (across all retries),
    // force status to 'error' so the user can retry manually
    sessionTimeoutRef.current = setTimeout(() => {
      if (isStoppedRef.current) return;
      onLog('Total session timeout (60s) — connection failed');
      cleanup();
      setStatus('error');
    }, TOTAL_SESSION_TIMEOUT_MS);

    await connect();
  }, [cleanup, connect, onLog]);

  // Cleanup on unmount
  useEffect(() => {
    return () => {
      cleanup();
      if (statsIntervalRef.current) {
        clearInterval(statsIntervalRef.current);
      }
    };
  }, [cleanup]);

  return { status, videoRef, stats, start, stop };
}
