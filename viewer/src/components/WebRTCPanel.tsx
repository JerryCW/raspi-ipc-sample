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

/** Map ConnectionStatus to badge color + label. */
function getStatusBadge(status: ConnectionStatus): { color: string; label: string } {
  switch (status) {
    case 'connected':
      return { color: 'bg-green-500', label: '已连接' };
    case 'connecting':
      return { color: 'bg-yellow-500', label: '连接中' };
    case 'reconnecting':
      return { color: 'bg-yellow-500', label: '重新连接中' };
    case 'error':
      return { color: 'bg-red-500', label: '连接错误' };
    case 'stopped':
      return { color: 'bg-gray-400', label: '已停止' };
    case 'idle':
    default:
      return { color: 'bg-gray-400', label: '空闲' };
  }
}

// ===== Component =====

export function WebRTCPanel({ channelName, credentials, region }: WebRTCPanelProps) {
  const [logs, setLogs] = useState<LogEntry[]>([]);
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
    logEndRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [logs]);

  const badge = getStatusBadge(status);
  const isIdle = status === 'idle' || status === 'stopped';

  return (
    <div className="flex flex-col gap-4">
      {/* Header: title + status badge */}
      <div className="flex items-center justify-between">
        <h2 className="text-lg font-semibold">WebRTC 实时查看</h2>
        <span className={`inline-flex items-center gap-1.5 rounded-full px-3 py-1 text-xs font-medium text-white ${badge.color}`}>
          <span className="h-2 w-2 rounded-full bg-white/60" />
          {badge.label}
        </span>
      </div>

      {/* Video container — 16:9 aspect ratio */}
      <div className="relative w-full overflow-hidden rounded-lg bg-gray-900" style={{ aspectRatio: '16/9' }}>
        <video
          ref={videoRef}
          autoPlay
          playsInline
          muted
          className="absolute inset-0 h-full w-full object-contain"
        />
        {isIdle && (
          <div className="absolute inset-0 flex items-center justify-center text-gray-400">
            <div className="text-center">
              <svg className="mx-auto mb-2 h-12 w-12" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.5} d="M15.75 10.5l4.72-4.72a.75.75 0 011.28.53v11.38a.75.75 0 01-1.28.53l-4.72-4.72M4.5 18.75h9a2.25 2.25 0 002.25-2.25v-9A2.25 2.25 0 0013.5 5.25h-9A2.25 2.25 0 002.25 7.5v9A2.25 2.25 0 004.5 18.75z" />
              </svg>
              <p className="text-sm">点击"开始"查看实时视频</p>
            </div>
          </div>
        )}
      </div>

      {/* Controls: Start / Stop */}
      <div className="flex gap-2">
        <button
          onClick={start}
          disabled={!credentials || !isIdle}
          className="rounded-md bg-blue-600 px-4 py-2 text-sm font-medium text-white hover:bg-blue-700 disabled:cursor-not-allowed disabled:opacity-50"
        >
          开始
        </button>
        <button
          onClick={stop}
          disabled={isIdle}
          className="rounded-md bg-red-600 px-4 py-2 text-sm font-medium text-white hover:bg-red-700 disabled:cursor-not-allowed disabled:opacity-50"
        >
          停止
        </button>
        {!credentials && (
          <span className="self-center text-xs text-yellow-600">请先登录获取凭证</span>
        )}
      </div>

      {/* Stats panel */}
      <StatsDisplay stats={stats} status={status} />

      {/* Debug log */}
      <div className="flex flex-col gap-1">
        <h3 className="text-sm font-medium text-gray-700">调试日志</h3>
        <div className="h-48 overflow-y-auto rounded-md border border-gray-200 bg-gray-50 p-2 font-mono text-xs">
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
    <div className="flex flex-col gap-1">
      <h3 className="text-sm font-medium text-gray-700">统计信息</h3>
      <div className="grid grid-cols-2 gap-x-4 gap-y-1 rounded-md border border-gray-200 bg-gray-50 p-3 text-sm sm:grid-cols-4">
        {items.map((item) => (
          <div key={item.label}>
            <span className="text-gray-500">{item.label}</span>
            <p className="font-medium text-gray-900">{item.value}</p>
          </div>
        ))}
      </div>
    </div>
  );
}
