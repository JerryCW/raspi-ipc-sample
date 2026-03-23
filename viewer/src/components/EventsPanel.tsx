import { useState, useCallback, useEffect } from 'react';
import type { ActivityEvent } from '../types';
import { useEvents } from '../hooks/useEvents';
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
  onJumpToPlayback: (startMs: number, endMs: number) => void;
}

// ===== Helpers =====

/** Format ms timestamp to UTC+8 HH:MM:SS. */
function formatTimestamp(ms: number): string {
  const d = new Date(ms + 8 * 60 * 60 * 1000);
  return d.toISOString().slice(11, 19);
}

/** Format duration in seconds to human-readable string. */
function formatDuration(seconds: number): string {
  if (seconds < 60) return `${seconds}秒`;
  const m = Math.floor(seconds / 60);
  const s = seconds % 60;
  return s > 0 ? `${m}分${s}秒` : `${m}分钟`;
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
 * Activity events panel — date picker + event list.
 *
 * Validates: Requirements 5.3, 5.5, 5.7
 */
export function EventsPanel({ idToken, onJumpToPlayback }: EventsPanelProps) {
  const [selectedDate, setSelectedDate] = useState(todayUTC8);
  const { events, isLoading, error, retry } = useEvents(selectedDate, idToken);

  const handleDateChange = useCallback((e: React.ChangeEvent<HTMLInputElement>) => {
    const val = e.target.value; // YYYY-MM-DD
    if (!val) return;
    // Parse as UTC+8 start-of-day
    const d = new Date(`${val}T00:00:00+08:00`);
    setSelectedDate(d);
  }, []);

  return (
    <div className="flex flex-col gap-4">
      <div className="flex items-center justify-between">
        <h2 className="text-lg font-semibold">活动事件</h2>
        <input
          type="date"
          value={toInputDate(selectedDate)}
          onChange={handleDateChange}
          className="rounded-md border border-gray-300 px-3 py-1.5 text-sm focus:border-blue-500 focus:outline-none focus:ring-1 focus:ring-blue-500"
        />
      </div>

      {/* Loading state */}
      {isLoading && (
        <div className="flex items-center justify-center py-12">
          <div className="h-6 w-6 animate-spin rounded-full border-2 border-blue-600 border-t-transparent" />
          <span className="ml-2 text-sm text-gray-500">加载中...</span>
        </div>
      )}

      {/* Error state */}
      {!isLoading && error && (
        <div className="rounded-md bg-red-50 p-4 text-center">
          <p className="text-sm text-red-600">{error}</p>
          <button
            onClick={retry}
            className="mt-2 rounded-md bg-red-600 px-3 py-1.5 text-xs font-medium text-white hover:bg-red-700"
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
        <div className="flex flex-col gap-2">
          {events.map((event) => (
            <EventCard
              key={event.sessionId}
              event={event}
              idToken={idToken}
              onJump={onJumpToPlayback}
            />
          ))}
        </div>
      )}
    </div>
  );
}


// ===== EventCard =====

interface EventCardProps {
  event: ActivityEvent;
  idToken: string | null;
  onJump: (startMs: number, endMs: number) => void;
}

function EventCard({ event, idToken, onJump }: EventCardProps) {
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
    onJump(event.kvsStartTimestamp - 5000, event.kvsEndTimestamp + 5000);
  }, [event.kvsStartTimestamp, event.kvsEndTimestamp, onJump]);

  const icon = CLASS_ICONS[event.detectedClass] ?? '❓';

  return (
    <button
      onClick={handleClick}
      className="flex items-center gap-3 rounded-lg border border-gray-200 bg-white p-3 text-left transition-colors hover:border-blue-300 hover:bg-blue-50"
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

      {/* Arrow */}
      <svg className="h-4 w-4 shrink-0 text-gray-300" fill="none" viewBox="0 0 24 24" stroke="currentColor">
        <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M9 5l7 7-7 7" />
      </svg>
    </button>
  );
}
