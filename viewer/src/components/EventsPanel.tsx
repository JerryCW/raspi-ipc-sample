import { useState, useCallback, useEffect } from 'react';
import type { ActivityEvent, AWSCredentials } from '../types';
import { useEvents } from '../hooks/useEvents';
import { useHLS } from '../hooks/useHLS';
import { fetchThumbnailUrl } from '../services/events';

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
      <div className="relative w-full overflow-hidden rounded-2xl shadow-lg bg-gray-900">
        {isIdle ? (
          <div className="aspect-video flex items-center justify-center text-gray-400">
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
            className="rounded-md border border-gray-300 px-3 py-1.5 text-sm focus:border-brand-500 focus:outline-none focus:ring-1 focus:ring-brand-500"
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

  const icon = CLASS_ICONS[event.detectedClass] ?? '❓';

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
          <span className="text-base">{icon}</span>
          <span className="text-sm font-medium text-gray-900">
            {event.detectedClass}
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
    </button>
  );
}
