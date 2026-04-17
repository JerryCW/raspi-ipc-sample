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

  useEffect(() => {
    if (showDebugLog) {
      logEndRef.current?.scrollIntoView({ behavior: 'smooth' });
    }
  }, [logs, showDebugLog]);

  const isIdle = status === 'idle' || status === 'stopped' || status === 'error';
  const isConnecting = status === 'connecting' || status === 'reconnecting';
  const isConnected = status === 'connected';

  // Real-time clock for overlay (UTC+8)
  const [clockStr, setClockStr] = useState('');
  useEffect(() => {
    const tick = () => {
      const now = new Date();
      const utc8 = new Date(now.getTime() + 8 * 60 * 60 * 1000);
      setClockStr(utc8.toISOString().replace('T', ' ').slice(0, 19));
    };
    tick();
    const id = setInterval(tick, 1000);
    return () => clearInterval(id);
  }, []);

  const [showStopBtn, setShowStopBtn] = useState(false);
  const hoverTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const handleMouseEnter = useCallback(() => {
    if (!isIdle && !isConnecting) {
      hoverTimerRef.current = setTimeout(() => setShowStopBtn(true), 1000);
    }
  }, [isIdle, isConnecting]);

  const handleMouseLeave = useCallback(() => {
    if (hoverTimerRef.current) {
      clearTimeout(hoverTimerRef.current);
      hoverTimerRef.current = null;
    }
    setShowStopBtn(false);
  }, []);

  // Reset showStopBtn when status changes
  useEffect(() => {
    if (isIdle) setShowStopBtn(false);
  }, [isIdle]);

  const handleToggle = () => {
    if (isIdle) {
      start();
    } else {
      stop();
    }
  };

  return (
    <div className="flex flex-col gap-4">
      {/* Video card — toggle button inside, hover-to-show when playing */}
      <div className="group relative w-full overflow-hidden rounded-2xl bg-gray-900 aspect-video cursor-pointer ring-2 ring-brand-500/40" style={{boxShadow: '0 4px 30px rgba(134,188,37,0.8)'}}
        onMouseEnter={handleMouseEnter}
        onMouseLeave={handleMouseLeave}
      >
        {/* Connection indicator dot */}
        <span className={`absolute top-3 right-3 z-10 h-2 w-2 rounded-full ${getIndicatorColor(status)}`} />

        {/* Date/time overlay */}
        {!isIdle && (
          <span className="absolute top-3 left-3 z-10 rounded-lg bg-black/50 px-2 py-1 text-xs text-white/90 font-mono">
            {clockStr}
          </span>
        )}

        <video
          ref={videoRef}
          autoPlay
          playsInline
          muted
          controls
          className={`w-full ${isIdle ? 'hidden' : ''}`}
        />

        {/* Idle placeholder — just the play button, no text */}

        {/* Connecting spinner — clickable to cancel */}
        {isConnecting && (
          <div className="absolute inset-0 flex items-center justify-center bg-black/40 z-20 cursor-pointer" onClick={handleToggle}>
            <div className="flex flex-col items-center gap-2">
              <div className="h-8 w-8 animate-spin rounded-full border-3 border-brand-400 border-t-transparent" />
              <p className="text-sm text-white/80">连接中... 点击取消</p>
            </div>
          </div>
        )}

        {/* Center toggle button overlay */}
        {!isConnecting && (
          <div className={`absolute inset-0 z-20 flex items-center justify-center transition-opacity duration-300 ${
            isIdle ? 'opacity-100' : showStopBtn ? 'opacity-100' : 'opacity-0'
          }`}>
            <button
              onClick={handleToggle}
              disabled={!credentials && isIdle}
              className={`flex items-center justify-center rounded-full w-16 h-16 text-white shadow-lg transition-all disabled:cursor-not-allowed disabled:opacity-50 ${
                isConnected
                  ? 'bg-red-500/80 hover:bg-red-600'
                  : 'bg-brand-500/90 hover:bg-brand-600'
              }`}
            >
              {isIdle ? (
                <svg className="h-7 w-7 ml-0.5" fill="currentColor" viewBox="0 0 24 24">
                  <path d="M8 5v14l11-7z" />
                </svg>
              ) : (
                <svg className="h-6 w-6" fill="currentColor" viewBox="0 0 24 24">
                  <rect x="6" y="6" width="12" height="12" rx="1" />
                </svg>
              )}
            </button>
          </div>
        )}
      </div>

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
