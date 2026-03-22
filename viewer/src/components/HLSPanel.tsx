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

function getStatusBadge(status: ConnectionStatus): { color: string; label: string } {
  switch (status) {
    case 'connected': return { color: 'bg-green-500', label: '播放中' };
    case 'connecting': return { color: 'bg-yellow-500', label: '加载中' };
    case 'error': return { color: 'bg-red-500', label: '播放错误' };
    case 'stopped': return { color: 'bg-gray-400', label: '已停止' };
    default: return { color: 'bg-gray-400', label: '空闲' };
  }
}

// ===== Component =====

export function HLSPanel({ streamName, credentials, region, preloadedFragments }: HLSPanelProps) {
  const [logs, setLogs] = useState<LogEntry[]>([]);
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

  useEffect(() => {
    logEndRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [logs]);

  // Load fragments when timeRange changes (triggered by date picker)
  const loadedRangeRef = useRef('');
  useEffect(() => {
    const key = `${timeRange.start.getTime()}-${timeRange.end.getTime()}`;
    if (credentials && key !== loadedRangeRef.current) {
      loadedRangeRef.current = key;
      loadFragments(timeRange.start, timeRange.end);
    }
  }, [credentials, loadFragments, timeRange]);

  /** Timeline date changed — update range and reload fragments. */
  const handleRangeChange = useCallback(
    (newStart: Date, newEnd: Date) => {
      setTimeRange({ start: newStart, end: newEnd });
    },
    [],
  );

  /** Click on timeline — play ±30 min window around that time. */
  const handleTimeSelect = useCallback(
    (time: Date) => {
      if (!credentials) return;
      const windowMs = 30 * 60 * 1000;
      start(new Date(time.getTime() - windowMs), new Date(time.getTime() + windowMs));
    },
    [credentials, start],
  );

  const badge = getStatusBadge(status);
  const isIdle = status === 'idle' || status === 'stopped';

  return (
    <div className="flex flex-col gap-4">
      <div className="flex items-center justify-between">
        <h2 className="text-lg font-semibold">录像回放</h2>
        <span className={`inline-flex items-center gap-1.5 rounded-full px-3 py-1 text-xs font-medium text-white ${badge.color}`}>
          <span className="h-2 w-2 rounded-full bg-white/60" />
          {badge.label}
        </span>
      </div>

      <div className="relative w-full overflow-hidden rounded-lg bg-gray-900" style={{ aspectRatio: '16/9' }}>
        <video ref={videoRef} autoPlay playsInline muted className="absolute inset-0 h-full w-full object-contain" />
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
      </div>

      <Timeline
        fragments={fragments}
        startTime={timeRange.start}
        endTime={timeRange.end}
        currentTime={stats.currentTime}
        onTimeSelect={handleTimeSelect}
        onRangeChange={handleRangeChange}
      />

      <div className="flex gap-2">
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

      <HLSStatsDisplay stats={stats} status={status} />

      <div className="flex flex-col gap-1">
        <h3 className="text-sm font-medium text-gray-700">调试日志</h3>
        <div className="h-48 overflow-y-auto rounded-md border border-gray-200 bg-gray-50 p-2 font-mono text-xs">
          {logs.length === 0 && <p className="text-gray-400">暂无日志</p>}
          {logs.map((entry, i) => (
            <div key={i} className="leading-5">
              <span className="text-gray-500">[{formatTimeUTC8(entry.timestamp)}]</span>{' '}
              <span className="text-gray-800">{entry.message}</span>
            </div>
          ))}
          <div ref={logEndRef} />
        </div>
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
    <div className="flex flex-col gap-1">
      <h3 className="text-sm font-medium text-gray-700">统计信息</h3>
      <div className="grid grid-cols-2 gap-x-4 gap-y-1 rounded-md border border-gray-200 bg-gray-50 p-3 text-sm sm:grid-cols-3">
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
