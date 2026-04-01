import { useState, useCallback, useEffect } from 'react';
import type { ActivityEvent, AWSCredentials } from '../types';
import { useEvents } from '../hooks/useEvents';
import { useHLS } from '../hooks/useHLS';
import { fetchThumbnailUrl, exportVideoClip } from '../services/events';

// ===== Constants =====

const CLASS_ICONS: Record<ActivityEvent['detectedClass'], string> = {
  person: '🧑',
  cat: '🐱',
  dog: '🐕',
  bird: '🐦',
};

// ===== Props =====

export interface EventsPanelProps {
  idToken: string | null;
  streamName: string;
  credentials: AWSCredentials | null;
  region: string;
}

// ===== Helpers =====

/** Format ms timestamp to UTC+8 HH:MM:SS. */
function formatTimestamp(ms: number): string {
  const d = new Date(ms + 8 * 60 * 60 * 1000);
  return d.toISOString().slice(11, 19);
}

/** Format duration in seconds to human-readable string. */
function formatDuration(seconds: number): string {
  const s = Math.round(seconds);
  if (s < 60) return `${s}秒`;
  const m = Math.floor(s / 60);
  const r = s % 60;
  return r > 0 ? `${m}分${r}秒` : `${m}分钟`;
}

/** Get today in UTC+8 as a Date (start of day). */
function todayUTC8(): Date {
  const now = new Date();
  const utc8Ms = now.getTime() + 8 * 60 * 60 * 1000;
  const dayStart = Math.floor(utc8Ms / 86400000) * 86400000;
  return new Date(dayStart - 8 * 60 * 60 * 1000);
}

/** Format Date to YYYY-MM-DD for input[type=date]. */
function toInputDate(d: Date): string {
  const utc8 = new Date(d.getTime() + 8 * 60 * 60 * 1000);
  return utc8.toISOString().slice(0, 10);
}

// ===== Component =====

/**
 * Activity events panel — top-bottom layout.
 * Top: full-width HLS video player. Bottom: date picker + event list.
 * Clicking an event plays its recording in the embedded player.
 *
 * Validates: Requirements 5.3, 5.5, 5.7
 */
export function EventsPanel({ idToken, streamName, credentials, region }: EventsPanelProps) {
  const [selectedDate, setSelectedDate] = useState(todayUTC8);
  const [activeSessionId, setActiveSessionId] = useState<string | null>(null);
  const { events, isLoading, error, retry } = useEvents(selectedDate, idToken);

  const { status, videoRef, start, stop } = useHLS({
    streamName,
    credentials,
    region,
    onLog: (msg) => console.log('[EventsHLS]', msg),
  });

  const handleDateChange = useCallback((e: React.ChangeEvent<HTMLInputElement>) => {
    const val = e.target.value;
    if (!val) return;
    const d = new Date(`${val}T00:00:00+08:00`);
    setSelectedDate(d);
    // Stop current playback when date changes
    stop();
    setActiveSessionId(null);
  }, [stop]);

  const handleEventClick = useCallback(
    (event: ActivityEvent) => {
      setActiveSessionId(event.sessionId);
      start(
        new Date(event.kvsStartTimestamp - 5000),
        new Date(event.kvsEndTimestamp + 5000),
      );
    },
    [start],
  );

  const isIdle = status === 'idle' || status === 'stopped';

  return (
    <div className="flex flex-col gap-4">
      {/* Top: Video player (full width) */}
      <div className="relative w-full overflow-hidden rounded-2xl bg-gray-900 aspect-video ring-2 ring-brand-500/40" style={{boxShadow: '0 4px 30px rgba(134,188,37,0.8)'}}>
        {isIdle ? (
          <div className="absolute inset-0 flex items-center justify-center text-gray-400">
            <div className="text-center">
              <svg className="mx-auto mb-2 h-12 w-12" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.5} d="M21 12a9 9 0 11-18 0 9 9 0 0118 0z" />
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.5} d="M15.91 11.672a.375.375 0 010 .656l-5.603 3.113a.375.375 0 01-.557-.328V8.887c0-.286.307-.466.557-.327l5.603 3.112z" />
              </svg>
              <p className="text-sm">点击下方事件查看回放</p>
            </div>
          </div>
        ) : (
          <video
            ref={videoRef}
            autoPlay
            playsInline
            muted
            className="w-full object-contain"
          />
        )}
        {status === 'connecting' && (
          <div className="absolute inset-0 flex items-center justify-center bg-black/30">
            <div className="h-8 w-8 animate-spin rounded-full border-2 border-white border-t-transparent" />
          </div>
        )}
      </div>

      {/* Bottom: Date picker + event list */}
      <div className="flex flex-col gap-4">
        <div className="flex items-center justify-between">
          <h2 className="text-lg font-semibold">活动事件</h2>
          <input
            type="date"
            value={toInputDate(selectedDate)}
            onChange={handleDateChange}
            className="w-40 rounded-md border border-gray-300 px-3 py-1.5 text-sm focus:border-brand-500 focus:outline-none focus:ring-1 focus:ring-brand-500"
          />
        </div>

        {/* Loading state */}
        {isLoading && (
          <div className="flex items-center justify-center py-12">
            <div className="h-6 w-6 animate-spin rounded-full border-2 border-brand-500 border-t-transparent" />
            <span className="ml-2 text-sm text-gray-500">加载中...</span>
          </div>
        )}

        {/* Error state */}
        {!isLoading && error && (
          <div className="rounded-md bg-red-50 p-4 text-center">
            <p className="text-sm text-red-600">{error}</p>
            <button
              onClick={retry}
              className="mt-2 rounded-xl bg-red-600 px-3 py-1.5 text-xs font-medium text-white hover:bg-red-700"
            >
              重试
            </button>
          </div>
        )}

        {/* Empty state */}
        {!isLoading && !error && events.length === 0 && (
          <div className="py-12 text-center text-sm text-gray-400">
            当天没有检测到活动事件
          </div>
        )}

        {/* Event list */}
        {!isLoading && !error && events.length > 0 && (
          <div className="flex flex-col gap-2 overflow-y-auto max-h-[50vh]">
            {events.map((event) => (
              <EventCard
                key={event.sessionId}
                event={event}
                idToken={idToken}
                isActive={event.sessionId === activeSessionId}
                onClick={handleEventClick}
              />
            ))}
          </div>
        )}
      </div>
    </div>
  );
}


// ===== EventCard =====

interface EventCardProps {
  event: ActivityEvent;
  idToken: string | null;
  isActive: boolean;
  onClick: (event: ActivityEvent) => void;
}

function EventCard({ event, idToken, isActive, onClick }: EventCardProps) {
  const [thumbUrl, setThumbUrl] = useState<string | null>(null);
  const [thumbError, setThumbError] = useState(false);
  const [downloadState, setDownloadState] = useState<'idle' | 'loading' | 'error'>('idle');
  const [downloadError, setDownloadError] = useState<string | null>(null);

  // Load thumbnail on mount
  useEffect(() => {
    if (!idToken) return;
    fetchThumbnailUrl(event.sessionId, 'start', idToken)
      .then(setThumbUrl)
      .catch(() => setThumbError(true));
  }, [event.sessionId, idToken]);

  const handleClick = useCallback(() => {
    onClick(event);
  }, [event, onClick]);

  const handleDownload = useCallback(
    async (e: React.MouseEvent) => {
      e.stopPropagation(); // Prevent triggering event playback
      if (!idToken || downloadState === 'loading') return;

      setDownloadState('loading');
      setDownloadError(null);

      try {
        const { blob, filename } = await exportVideoClip(event.sessionId, idToken);
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = filename;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
        setDownloadState('idle');
      } catch (err) {
        const msg = err instanceof Error ? err.message : '下载失败';
        setDownloadError(msg);
        setDownloadState('error');
        setTimeout(() => {
          setDownloadState('idle');
          setDownloadError(null);
        }, 3000);
      }
    },
    [event.sessionId, idToken, downloadState],
  );

  const icon = CLASS_ICONS[event.detectedClass] ?? '❓';
  const isBirdWithSpecies = event.primaryClass === 'bird' && !!event.birdSpecies;

  return (
    <button
      onClick={handleClick}
      className={`flex items-center gap-3 rounded-lg border backdrop-blur-sm bg-white/80 p-3 text-left transition-colors hover:bg-brand-50 ${
        isActive
          ? 'border-brand-500 ring-1 ring-brand-500'
          : 'border-gray-200 hover:border-brand-300'
      }`}
    >
      {/* Thumbnail or placeholder */}
      <div className="flex h-14 w-14 shrink-0 items-center justify-center overflow-hidden rounded-md bg-gray-100">
        {thumbUrl && !thumbError ? (
          <img
            src={thumbUrl}
            alt={event.detectedClass}
            className="h-full w-full object-cover"
            onError={() => setThumbError(true)}
          />
        ) : (
          <span className="text-2xl">{icon}</span>
        )}
      </div>

      {/* Info */}
      <div className="flex min-w-0 flex-1 flex-col gap-0.5">
        <div className="flex items-center gap-1.5">
          {event.detectedClasses && event.detectedClasses.length > 0 ? (
            <span className="text-base">
              {event.detectedClasses.map((cls) => CLASS_ICONS[cls as ActivityEvent['detectedClass']] ?? '❓').join('')}
            </span>
          ) : (
            <span className="text-base">{icon}</span>
          )}
          <span className="text-sm font-medium text-gray-900">
            {isBirdWithSpecies ? event.birdSpecies : event.detectedClass}
          </span>
          <span className="ml-auto text-xs text-gray-400">
            {event.detectionCount}次检测
          </span>
        </div>
        <div className="text-xs text-gray-500">
          {formatTimestamp(event.kvsStartTimestamp)} – {formatTimestamp(event.kvsEndTimestamp)}
          <span className="ml-2 text-gray-400">
            {formatDuration(event.durationSeconds)}
          </span>
        </div>
        <div className="text-xs text-gray-400">
          置信度 {(event.maxConfidence * 100).toFixed(0)}%
        </div>
      </div>

      {/* Download button */}
      <div className="flex shrink-0 flex-col items-center gap-1">
        <button
          onClick={handleDownload}
          disabled={downloadState === 'loading'}
          className="rounded-md p-1.5 text-gray-400 transition-colors hover:bg-gray-100 hover:text-gray-600 disabled:opacity-50"
          title="下载视频"
        >
          {downloadState === 'loading' ? (
            <div className="h-5 w-5 animate-spin rounded-full border-2 border-gray-400 border-t-transparent" />
          ) : (
            <svg className="h-5 w-5" fill="none" viewBox="0 0 24 24" stroke="currentColor">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.5} d="M3 16.5v2.25A2.25 2.25 0 005.25 21h13.5A2.25 2.25 0 0021 18.75V16.5M16.5 12L12 16.5m0 0L7.5 12m4.5 4.5V3" />
            </svg>
          )}
        </button>
        {downloadState === 'error' && downloadError && (
          <span className="max-w-[80px] truncate text-[10px] text-red-500" title={downloadError}>
            {downloadError}
          </span>
        )}
      </div>
    </button>
  );
}
