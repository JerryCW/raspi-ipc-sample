import { useState, useCallback, useRef, useEffect } from 'react';
import type { AWSCredentials, ConnectionStatus, HLSStats, LogEntry } from '../types';
import { useHLS } from '../hooks/useHLS';
import Timeline from './Timeline';

// ===== Props =====

export interface HLSPanelProps {
  streamName: string;
  credentials: AWSCredentials | null;
  region: string;
  preloadedFragments?: import('../types').Fragment[];
}

// ===== Helpers =====

/** Format time in UTC+8 24h format. */
function formatTimeUTC8(date: Date): string {
  const utc8 = new Date(date.getTime() + 8 * 60 * 60 * 1000);
  return utc8.toISOString().slice(11, 19);
}

/** Build default time range: today 00:00-23:59 in UTC+8. */
export function getDefaultTimeRange(): { start: Date; end: Date } {
  const now = new Date();
  const utc8Ms = now.getTime() + 8 * 60 * 60 * 1000;
  const dayStartUtc8 = Math.floor(utc8Ms / (24 * 60 * 60 * 1000)) * (24 * 60 * 60 * 1000);
  const start = new Date(dayStartUtc8 - 8 * 60 * 60 * 1000);
  const end = new Date(start.getTime() + 24 * 60 * 60 * 1000);
  return { start, end };
}

function formatBitrate(bps: number): string {
  if (bps === 0) return '—';
  if (bps >= 1_000_000) return `${(bps / 1_000_000).toFixed(1)} Mbps`;
  if (bps >= 1_000) return `${(bps / 1_000).toFixed(0)} kbps`;
  return `${bps} bps`;
}

// ===== Component =====

export function HLSPanel({ streamName, credentials, region, preloadedFragments }: HLSPanelProps) {
  const [logs, setLogs] = useState<LogEntry[]>([]);
  const [showDebugLog, setShowDebugLog] = useState(false);
  const [showStats, setShowStats] = useState(false);
  const logEndRef = useRef<HTMLDivElement>(null);
  const [timeRange, setTimeRange] = useState(getDefaultTimeRange);

  const addLog = useCallback((message: string) => {
    setLogs((prev) => [...prev, { timestamp: new Date(), message }]);
  }, []);

  // Use preloaded fragments if available
  const { status, videoRef, stats, fragments: hookFragments, start, stop, loadFragments } = useHLS({
    streamName, credentials, region, onLog: addLog,
  });

  // Merge: use preloaded fragments until hook loads its own
  const fragments = hookFragments.length > 0 ? hookFragments : (preloadedFragments ?? []);

  // Auto-scroll debug log to bottom
  useEffect(() => {
    if (showDebugLog) {
      logEndRef.current?.scrollIntoView({ behavior: 'smooth' });
    }
  }, [logs, showDebugLog]);

  // Load fragments when timeRange changes (triggered by date picker)
  const loadedRangeRef = useRef('');
  useEffect(() => {
    const key = `${timeRange.start.getTime()}-${timeRange.end.getTime()}`;
    if (credentials && key !== loadedRangeRef.current) {
      loadedRangeRef.current = key;
      loadFragments(timeRange.start, timeRange.end);
    }
  }, [credentials, loadFragments, timeRange]);

  // Auto-refresh fragments every 60s
  useEffect(() => {
    const id = setInterval(() => {
      if (credentials) {
        loadFragments(timeRange.start, timeRange.end);
      }
    }, 60_000);
    return () => clearInterval(id);
  }, [credentials, loadFragments, timeRange]);

  /** Timeline date changed — update range and reload fragments. */
  const handleRangeChange = useCallback(
    (newStart: Date, newEnd: Date) => {
      setTimeRange({ start: newStart, end: newEnd });
    },
    [],
  );

  /** Click on timeline — play from clicked time, window = clicked time → +60 min. */
  const handleTimeSelect = useCallback(
    (time: Date) => {
      if (!credentials) return;
      const windowMs = 60 * 60 * 1000;
      start(time, new Date(time.getTime() + windowMs));
    },
    [credentials, start],
  );

  const isIdle = status === 'idle' || status === 'stopped';

  /** Skip forward/backward by delta seconds. If the target is outside the
   *  current HLS buffer, restart playback from the new time point. */
  const handleSkip = useCallback((deltaSec: number) => {
    const video = videoRef.current;
    if (!video || !credentials || !stats.currentTime) return;

    const targetTime = new Date(stats.currentTime.getTime() + deltaSec * 1000);

    // Check if target is within buffered range
    const buffered = video.buffered;
    let inRange = false;
    for (let i = 0; i < buffered.length; i++) {
      const newTime = video.currentTime + deltaSec;
      if (newTime >= buffered.start(i) && newTime <= buffered.end(i)) {
        inRange = true;
        break;
      }
    }

    if (inRange) {
      video.currentTime += deltaSec;
    } else {
      // Outside buffer — restart HLS from the target time
      const windowMs = 60 * 60 * 1000;
      start(targetTime, new Date(targetTime.getTime() + windowMs));
    }
  }, [videoRef, credentials, stats.currentTime, start]);

  return (
    <div className="flex flex-col gap-4">
      {/* Video card — matches WebRTCPanel style */}
      <div className="relative w-full overflow-hidden rounded-2xl bg-gray-900 aspect-video ring-2 ring-brand-500/40" style={{boxShadow: '0 4px 30px rgba(134,188,37,0.8)'}}>
        <video
          ref={videoRef}
          autoPlay
          playsInline
          muted
          className={`absolute inset-0 h-full w-full object-contain ${isIdle ? 'hidden' : ''}`}
        />
        {isIdle && (
          <div className="absolute inset-0 flex items-center justify-center text-gray-400">
            <div className="text-center">
              <svg className="mx-auto mb-2 h-12 w-12" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.5} d="M21 12a9 9 0 11-18 0 9 9 0 0118 0z" />
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.5} d="M15.91 11.672a.375.375 0 010 .656l-5.603 3.113a.375.375 0 01-.557-.328V8.887c0-.286.307-.466.557-.327l5.603 3.112z" />
              </svg>
              <p className="text-sm">在时间轴上点击选择回放时间点</p>
            </div>
          </div>
        )}
        {status === 'connecting' && (
          <div className="absolute inset-0 flex items-center justify-center bg-black/40">
            <div className="flex flex-col items-center gap-2">
              <div className="h-8 w-8 animate-spin rounded-full border-3 border-brand-400 border-t-transparent" />
              <p className="text-sm text-white/80">加载录像中...</p>
            </div>
          </div>
        )}

        {/* Playback time overlay */}
        {!isIdle && stats.currentTime && (
          <span className="absolute top-3 left-3 z-10 rounded-lg bg-black/50 px-2 py-1 text-xs text-white/90 font-mono">
            {new Date(stats.currentTime.getTime() + 8 * 60 * 60 * 1000).toISOString().replace('T', ' ').slice(0, 19)}
          </span>
        )}
      </div>

      {/* Control row: skip buttons (left) + date picker (right) */}
      <div className="flex items-center justify-between">
        <div className="flex items-center w-1/2">
          <button
            onClick={() => handleSkip(-60)}
            disabled={isIdle}
            className="flex-1 flex items-center justify-center py-1.5 text-gray-500 hover:text-gray-800 transition-colors disabled:cursor-not-allowed disabled:opacity-30"
            title="-60s"
          >
            <svg className="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
              <path strokeLinecap="round" strokeLinejoin="round" d="M18.5 19l-7-7 7-7M11.5 19l-7-7 7-7" />
            </svg>
          </button>
          <button
            onClick={() => handleSkip(-15)}
            disabled={isIdle}
            className="flex-1 flex items-center justify-center py-1.5 text-gray-500 hover:text-gray-800 transition-colors disabled:cursor-not-allowed disabled:opacity-30"
            title="-15s"
          >
            <svg className="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
              <path strokeLinecap="round" strokeLinejoin="round" d="M15 19l-7-7 7-7" />
            </svg>
          </button>
          <button
            onClick={stop}
            disabled={isIdle}
            className="flex-1 flex items-center justify-center py-1.5 text-red-500 hover:text-red-600 transition-colors disabled:cursor-not-allowed disabled:opacity-30"
            title="停止"
          >
            <svg className="h-5 w-5" fill="currentColor" viewBox="0 0 24 24">
              <rect x="6" y="6" width="12" height="12" rx="2" />
            </svg>
          </button>
          <button
            onClick={() => handleSkip(15)}
            disabled={isIdle}
            className="flex-1 flex items-center justify-center py-1.5 text-gray-500 hover:text-gray-800 transition-colors disabled:cursor-not-allowed disabled:opacity-30"
            title="+15s"
          >
            <svg className="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
              <path strokeLinecap="round" strokeLinejoin="round" d="M9 5l7 7-7 7" />
            </svg>
          </button>
          <button
            onClick={() => handleSkip(60)}
            disabled={isIdle}
            className="flex-1 flex items-center justify-center py-1.5 text-gray-500 hover:text-gray-800 transition-colors disabled:cursor-not-allowed disabled:opacity-30"
            title="+60s"
          >
            <svg className="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
              <path strokeLinecap="round" strokeLinejoin="round" d="M5.5 5l7 7-7 7M12.5 5l7 7-7 7" />
            </svg>
          </button>
        </div>

        {/* Date picker (right) */}
        <input
          type="date"
          value={(() => {
            const utc8 = new Date(timeRange.start.getTime() + 8 * 60 * 60 * 1000);
            return utc8.toISOString().slice(0, 10);
          })()}
          max={(() => {
            const utc8 = new Date(Date.now() + 8 * 60 * 60 * 1000);
            return utc8.toISOString().slice(0, 10);
          })()}
          onChange={(e) => {
            if (!e.target.value) return;
            const [y, m, d] = e.target.value.split('-').map(Number);
            const utc8Midnight = Date.UTC(y, m - 1, d, 0, 0, 0);
            const start = new Date(utc8Midnight - 8 * 60 * 60 * 1000);
            const end = new Date(start.getTime() + 24 * 60 * 60 * 1000);
            setTimeRange({ start, end });
          }}
          className="w-40 rounded-md border border-gray-300 px-3 py-1.5 text-sm focus:border-brand-500 focus:outline-none focus:ring-1 focus:ring-brand-500"
        />
      </div>

      <Timeline
        fragments={fragments}
        startTime={timeRange.start}
        endTime={timeRange.end}
        currentTime={stats.currentTime}
        onTimeSelect={handleTimeSelect}
        onRangeChange={handleRangeChange}
        hideDatePicker
      />

      {!credentials && isIdle && (
        <div className="text-center">
          <span className="text-xs text-yellow-600">请先登录获取凭证</span>
        </div>
      )}

      {/* Collapsible stats */}
      <div>
        <button
          onClick={() => setShowStats((v) => !v)}
          className="text-xs text-gray-500 hover:text-gray-700 transition-colors"
        >
          统计信息 {showStats ? '▲' : '▼'}
        </button>
        {showStats && <HLSStatsDisplay stats={stats} status={status} />}
      </div>

      {/* Collapsible debug log */}
      <div>
        <button
          onClick={() => setShowDebugLog((v) => !v)}
          className="text-xs text-gray-500 hover:text-gray-700 transition-colors"
        >
          调试日志 {showDebugLog ? '▲' : '▼'}
        </button>
        {showDebugLog && (
          <div className="mt-1 h-48 overflow-y-auto rounded-md border border-gray-200 bg-gray-50 p-2 font-mono text-xs">
            {logs.length === 0 && <p className="text-gray-400">暂无日志</p>}
            {logs.map((entry, i) => (
              <div key={i} className="leading-5">
                <span className="text-gray-500">[{formatTimeUTC8(entry.timestamp)}]</span>{' '}
                <span className="text-gray-800">{entry.message}</span>
              </div>
            ))}
            <div ref={logEndRef} />
          </div>
        )}
      </div>
    </div>
  );
}

function HLSStatsDisplay({ stats, status }: { stats: HLSStats; status: ConnectionStatus }) {
  const active = status === 'connected' || status === 'connecting';
  const items = [
    { label: '分辨率', value: active ? stats.resolution : '—' },
    { label: '帧率', value: active && stats.fps > 0 ? `${stats.fps} fps` : '—' },
    { label: '码率', value: active ? formatBitrate(stats.bitrate) : '—' },
    { label: '缓冲时长', value: active && stats.bufferLength > 0 ? `${stats.bufferLength.toFixed(1)} s` : '—' },
    { label: '丢帧数', value: active ? String(stats.droppedFrames) : '—' },
    { label: '当前时间', value: active && stats.currentTime ? formatTimeUTC8(stats.currentTime) : '—' },
  ];
  return (
    <div className="mt-1 grid grid-cols-2 gap-x-4 gap-y-1 rounded-md border border-gray-200 bg-gray-50 p-3 text-sm sm:grid-cols-3">
      {items.map((item) => (
        <div key={item.label}>
          <span className="text-gray-500">{item.label}</span>
          <p className="font-medium text-gray-900">{item.value}</p>
        </div>
      ))}
    </div>
  );
}
