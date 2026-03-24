import { useState, useCallback, useRef, useEffect } from 'react';
import type { AWSCredentials, ConnectionStatus, WebRTCStats, LogEntry } from '../types';
import { useWebRTC } from '../hooks/useWebRTC';

// ===== Props =====

export interface WebRTCPanelProps {
  channelName: string;
  credentials: AWSCredentials | null;
  region: string;
}

// ===== Helpers =====

/** Format a Date to HH:MM:SS in UTC+8 for debug log timestamps. */
function formatTime(date: Date): string {
  const utc8 = new Date(date.getTime() + 8 * 60 * 60 * 1000);
  return utc8.toISOString().slice(11, 19);
}

/** Format duration in ms to MM:SS. */
function formatDuration(ms: number): string {
  const totalSec = Math.floor(ms / 1000);
  const m = Math.floor(totalSec / 60);
  const s = totalSec % 60;
  return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
}

/** Format bitrate to human-readable string. */
function formatBitrate(bps: number): string {
  if (bps === 0) return '—';
  if (bps >= 1_000_000) return `${(bps / 1_000_000).toFixed(1)} Mbps`;
  if (bps >= 1_000) return `${(bps / 1_000).toFixed(0)} kbps`;
  return `${bps} bps`;
}

/** Map ConnectionStatus to indicator dot color. */
function getIndicatorColor(status: ConnectionStatus): string {
  switch (status) {
    case 'connected':
      return 'bg-green-500';
    case 'connecting':
    case 'reconnecting':
      return 'bg-yellow-500 animate-pulse';
    case 'error':
      return 'bg-red-500';
    case 'stopped':
    case 'idle':
    default:
      return 'bg-gray-400';
  }
}

// ===== Component =====

export function WebRTCPanel({ channelName, credentials, region }: WebRTCPanelProps) {
  const [logs, setLogs] = useState<LogEntry[]>([]);
  const [showDebugLog, setShowDebugLog] = useState(false);
  const [showStats, setShowStats] = useState(false);
  const logEndRef = useRef<HTMLDivElement>(null);

  const addLog = useCallback((message: string) => {
    setLogs((prev) => [...prev, { timestamp: new Date(), message }]);
  }, []);

  const { status, videoRef, stats, start, stop } = useWebRTC({
    channelName,
    credentials,
    region,
    onLog: addLog,
  });

  // Auto-scroll debug log to bottom
  useEffect(() => {
    if (showDebugLog) {
      logEndRef.current?.scrollIntoView({ behavior: 'smooth' });
    }
  }, [logs, showDebugLog]);

  const isIdle = status === 'idle' || status === 'stopped';
  const isConnecting = status === 'connecting' || status === 'reconnecting';
  const isConnected = status === 'connected';

  const handleToggle = () => {
    if (isIdle) {
      start();
    } else {
      stop();
    }
  };

  return (
    <div className="flex flex-col gap-4">
      {/* Video card with connection indicator */}
      <div className="relative w-full overflow-hidden rounded-2xl shadow-lg bg-gray-900 aspect-video">
        {/* Connection indicator dot */}
        <span className={`absolute top-3 right-3 z-10 h-2 w-2 rounded-full ${getIndicatorColor(status)}`} />

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
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.5} d="M15.75 10.5l4.72-4.72a.75.75 0 011.28.53v11.38a.75.75 0 01-1.28.53l-4.72-4.72M4.5 18.75h9a2.25 2.25 0 002.25-2.25v-9A2.25 2.25 0 0013.5 5.25h-9A2.25 2.25 0 002.25 7.5v9A2.25 2.25 0 004.5 18.75z" />
              </svg>
              <p className="text-sm">点击下方按钮查看实时视频</p>
            </div>
          </div>
        )}
        {isConnecting && (
          <div className="absolute inset-0 flex items-center justify-center bg-black/40">
            <div className="flex flex-col items-center gap-2">
              <div className="h-8 w-8 animate-spin rounded-full border-3 border-brand-400 border-t-transparent" />
              <p className="text-sm text-white/80">连接中...</p>
            </div>
          </div>
        )}
      </div>

      {/* Control area — toggle button centered below video */}
      <div className="flex flex-col items-center gap-2">
        <button
          onClick={handleToggle}
          disabled={!credentials && isIdle}
          className={`flex items-center justify-center rounded-full w-14 h-14 md:w-12 md:h-12 text-white transition-colors disabled:cursor-not-allowed disabled:opacity-50 ${
            isConnecting
              ? 'bg-yellow-500'
              : isConnected
                ? 'bg-red-500 hover:bg-red-600'
                : 'bg-brand-500 hover:bg-brand-600'
          }`}
        >
          {isConnecting ? (
            /* Spinner */
            <svg className="h-6 w-6 animate-spin" fill="none" viewBox="0 0 24 24">
              <circle className="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="4" />
              <path className="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z" />
            </svg>
          ) : isIdle ? (
            /* Play icon ▶ */
            <svg className="h-6 w-6 ml-0.5" fill="currentColor" viewBox="0 0 24 24">
              <path d="M8 5v14l11-7z" />
            </svg>
          ) : (
            /* Stop icon ■ */
            <svg className="h-5 w-5" fill="currentColor" viewBox="0 0 24 24">
              <rect x="6" y="6" width="12" height="12" rx="1" />
            </svg>
          )}
        </button>
        {!credentials && isIdle && (
          <span className="text-xs text-yellow-600">请先登录获取凭证</span>
        )}
      </div>

      {/* Collapsible stats */}
      <div>
        <button
          onClick={() => setShowStats((v) => !v)}
          className="text-xs text-gray-500 hover:text-gray-700 transition-colors"
        >
          统计信息 {showStats ? '▲' : '▼'}
        </button>
        {showStats && <StatsDisplay stats={stats} status={status} />}
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
            {logs.length === 0 && (
              <p className="text-gray-400">暂无日志</p>
            )}
            {logs.map((entry, i) => (
              <div key={i} className="leading-5">
                <span className="text-gray-500">[{formatTime(entry.timestamp)}]</span>{' '}
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


// ===== Stats sub-component =====

function StatsDisplay({ stats, status }: { stats: WebRTCStats; status: ConnectionStatus }) {
  const active = status === 'connected' || status === 'connecting' || status === 'reconnecting';

  const items: { label: string; value: string }[] = [
    { label: '分辨率', value: active ? stats.resolution : '—' },
    { label: '帧率', value: active && stats.fps > 0 ? `${stats.fps} fps` : '—' },
    { label: '码率', value: active ? formatBitrate(stats.bitrate) : '—' },
    { label: '延迟', value: active && stats.latency > 0 ? `${stats.latency.toFixed(0)} ms` : '—' },
    { label: '丢包', value: active ? String(stats.packetLoss) : '—' },
    { label: '编解码器', value: active ? stats.codec : '—' },
    { label: '中继类型', value: active && stats.relayType ? stats.relayType : '—' },
    { label: '播放时长', value: active && stats.duration > 0 ? formatDuration(stats.duration) : '—' },
  ];

  return (
    <div className="mt-1 grid grid-cols-2 gap-x-4 gap-y-1 rounded-md border border-gray-200 bg-gray-50 p-3 text-sm sm:grid-cols-4">
      {items.map((item) => (
        <div key={item.label}>
          <span className="text-gray-500">{item.label}</span>
          <p className="font-medium text-gray-900">{item.value}</p>
        </div>
      ))}
    </div>
  );
}
