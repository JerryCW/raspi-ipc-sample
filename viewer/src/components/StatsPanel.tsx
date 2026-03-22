import type { WebRTCStats, HLSStats, ConnectionStatus } from '../types';
import { getStatColor } from '../hooks/useStats';

// Re-export for convenience
export { getStatColor };

// ===== Helpers =====

const COLOR_MAP = {
  good: 'text-green-600',
  warn: 'text-yellow-600',
  bad: 'text-red-600',
} as const;

function formatBitrate(bps: number): string {
  if (bps === 0) return '—';
  if (bps >= 1_000_000) return `${(bps / 1_000_000).toFixed(1)} Mbps`;
  if (bps >= 1_000) return `${(bps / 1_000).toFixed(0)} kbps`;
  return `${bps} bps`;
}

function formatDuration(ms: number): string {
  const totalSec = Math.floor(ms / 1000);
  const m = Math.floor(totalSec / 60);
  const s = totalSec % 60;
  return `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
}

interface StatItem {
  label: string;
  value: string;
  colorClass?: string;
}

// ===== WebRTC Stats Panel =====

interface WebRTCStatsPanelProps {
  stats: WebRTCStats;
  status: ConnectionStatus;
}

export function WebRTCStatsPanel({ stats, status }: WebRTCStatsPanelProps) {
  const active = status === 'connected' || status === 'connecting' || status === 'reconnecting';

  const items: StatItem[] = [
    { label: '分辨率', value: active ? stats.resolution : '—' },
    {
      label: '帧率',
      value: active && stats.fps > 0 ? `${stats.fps} fps` : '—',
      colorClass: active && stats.fps > 0 ? COLOR_MAP[getStatColor('fps', stats.fps)] : undefined,
    },
    {
      label: '码率',
      value: active ? formatBitrate(stats.bitrate) : '—',
      colorClass: active && stats.bitrate > 0 ? COLOR_MAP[getStatColor('bitrate', stats.bitrate)] : undefined,
    },
    {
      label: '延迟',
      value: active && stats.latency > 0 ? `${stats.latency.toFixed(0)} ms` : '—',
      colorClass: active && stats.latency > 0 ? COLOR_MAP[getStatColor('latency', stats.latency)] : undefined,
    },
    {
      label: '丢包',
      value: active ? String(stats.packetLoss) : '—',
      colorClass: active ? COLOR_MAP[getStatColor('packetLoss', stats.packetLoss)] : undefined,
    },
    { label: '编解码器', value: active ? stats.codec : '—' },
    { label: '中继类型', value: active && stats.relayType ? stats.relayType : '—' },
    { label: '播放时长', value: active && stats.duration > 0 ? formatDuration(stats.duration) : '—' },
  ];

  return <StatGrid items={items} columns="sm:grid-cols-4" />;
}

// ===== HLS Stats Panel =====

interface HLSStatsPanelProps {
  stats: HLSStats;
  status: ConnectionStatus;
}

export function HLSStatsPanel({ stats, status }: HLSStatsPanelProps) {
  const active = status === 'connected' || status === 'connecting';

  const items: StatItem[] = [
    { label: '分辨率', value: active ? stats.resolution : '—' },
    {
      label: '帧率',
      value: active && stats.fps > 0 ? `${stats.fps} fps` : '—',
      colorClass: active && stats.fps > 0 ? COLOR_MAP[getStatColor('fps', stats.fps)] : undefined,
    },
    {
      label: '码率',
      value: active ? formatBitrate(stats.bitrate) : '—',
      colorClass: active && stats.bitrate > 0 ? COLOR_MAP[getStatColor('bitrate', stats.bitrate)] : undefined,
    },
    {
      label: '缓冲时长',
      value: active && stats.bufferLength > 0 ? `${stats.bufferLength.toFixed(1)} s` : '—',
      colorClass: active && stats.bufferLength > 0 ? COLOR_MAP[getStatColor('bufferLength', stats.bufferLength)] : undefined,
    },
    {
      label: '丢帧数',
      value: active ? String(stats.droppedFrames) : '—',
      colorClass: active ? COLOR_MAP[getStatColor('droppedFrames', stats.droppedFrames)] : undefined,
    },
    {
      label: '当前时间',
      value: active && stats.currentTime ? stats.currentTime.toLocaleTimeString() : '—',
    },
  ];

  return <StatGrid items={items} columns="sm:grid-cols-3" />;
}

// ===== Shared grid =====

function StatGrid({ items, columns }: { items: StatItem[]; columns: string }) {
  return (
    <div className="flex flex-col gap-1">
      <h3 className="text-sm font-medium text-gray-700">统计信息</h3>
      <div className={`grid grid-cols-2 gap-x-4 gap-y-1 rounded-md border border-gray-200 bg-gray-50 p-3 text-sm ${columns}`}>
        {items.map((item) => (
          <div key={item.label}>
            <span className="text-gray-500">{item.label}</span>
            <p className={`font-medium ${item.colorClass ?? 'text-gray-900'}`}>{item.value}</p>
          </div>
        ))}
      </div>
    </div>
  );
}
