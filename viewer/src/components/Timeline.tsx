import React, { useCallback, useMemo, useRef, useState, useEffect } from 'react';
import type { Fragment } from '../types';

// ===== Constants =====

export const MIN_RANGE_MS = 60 * 1000;
export const MAX_RANGE_MS = 7 * 24 * 60 * 60 * 1000;
const TIMELINE_HEIGHT = 60;
const AXIS_HEIGHT = 22;
const SVG_HEIGHT = TIMELINE_HEIGHT + AXIS_HEIGHT;
const ONE_DAY_MS = 24 * 60 * 60 * 1000;
const UTC8_OFFSET_MS = 8 * 60 * 60 * 1000;

// ===== Exported pure helpers =====

export interface TimeSegment {
  start: number;
  end: number;
  hasRecording: boolean;
}

export function clampTimeRange(
  startMs: number,
  endMs: number
): { start: number; end: number } {
  const duration = endMs - startMs;
  const mid = (startMs + endMs) / 2;
  if (duration < MIN_RANGE_MS) {
    return { start: Math.round(mid - MIN_RANGE_MS / 2), end: Math.round(mid + MIN_RANGE_MS / 2) };
  }
  if (duration > MAX_RANGE_MS) {
    return { start: Math.round(mid - MAX_RANGE_MS / 2), end: Math.round(mid + MAX_RANGE_MS / 2) };
  }
  return { start: startMs, end: endMs };
}

export function mapFragmentsToSegments(
  fragments: Fragment[],
  rangeStart: number,
  rangeEnd: number
): TimeSegment[] {
  if (rangeStart >= rangeEnd) return [];
  if (fragments.length === 0) {
    return [{ start: rangeStart, end: rangeEnd, hasRecording: false }];
  }
  const intervals: { start: number; end: number }[] = [];
  for (const f of fragments) {
    const fStart = f.producerTimestamp.getTime();
    const fEnd = fStart + f.fragmentLengthMillis;
    const clippedStart = Math.max(fStart, rangeStart);
    const clippedEnd = Math.min(fEnd, rangeEnd);
    if (clippedStart < clippedEnd) {
      intervals.push({ start: clippedStart, end: clippedEnd });
    }
  }
  if (intervals.length === 0) {
    return [{ start: rangeStart, end: rangeEnd, hasRecording: false }];
  }
  intervals.sort((a, b) => a.start - b.start);
  const merged: { start: number; end: number }[] = [intervals[0]];
  for (let i = 1; i < intervals.length; i++) {
    const last = merged[merged.length - 1];
    if (intervals[i].start <= last.end) {
      last.end = Math.max(last.end, intervals[i].end);
    } else {
      merged.push({ ...intervals[i] });
    }
  }
  const segments: TimeSegment[] = [];
  let cursor = rangeStart;
  for (const interval of merged) {
    if (cursor < interval.start) {
      segments.push({ start: cursor, end: interval.start, hasRecording: false });
    }
    segments.push({ start: interval.start, end: interval.end, hasRecording: true });
    cursor = interval.end;
  }
  if (cursor < rangeEnd) {
    segments.push({ start: cursor, end: rangeEnd, hasRecording: false });
  }
  return segments;
}

// ===== UTC+8 helpers =====

/** Get the start of a day (00:00:00) in UTC+8 for a given Date. */
export function getDayStartUTC8(date: Date): Date {
  const utc8Ms = date.getTime() + UTC8_OFFSET_MS;
  const dayStartUtc8 = Math.floor(utc8Ms / ONE_DAY_MS) * ONE_DAY_MS;
  return new Date(dayStartUtc8 - UTC8_OFFSET_MS);
}

/** Format a date as YYYY-MM-DD in UTC+8. */
function formatDateUTC8(date: Date): string {
  const d = new Date(date.getTime() + UTC8_OFFSET_MS);
  return d.toISOString().slice(0, 10);
}

/** Format hour label HH:00 from hour number. */
function formatHourLabel(hour: number): string {
  return `${String(hour).padStart(2, '0')}:00`;
}

// ===== Component =====

interface TimelineProps {
  fragments: Fragment[];
  startTime: Date;
  endTime: Date;
  currentTime: Date | null;
  onTimeSelect: (time: Date) => void;
  onRangeChange: (start: Date, end: Date) => void;
}

const Timeline: React.FC<TimelineProps> = ({
  fragments,
  startTime: _startTime,
  endTime: _endTime,
  currentTime,
  onTimeSelect,
  onRangeChange,
}) => {
  const svgRef = useRef<SVGSVGElement>(null);

  // Hover tooltip state
  const [hoverInfo, setHoverInfo] = useState<{ x: number; time: string } | null>(null);

  // Selected date state — defaults to today in UTC+8
  const [selectedDate, setSelectedDate] = useState(() => formatDateUTC8(new Date()));

  // "Now" indicator — update every 30s
  const [nowMs, setNowMs] = useState(() => Date.now());
  useEffect(() => {
    const id = setInterval(() => setNowMs(Date.now()), 30_000);
    return () => clearInterval(id);
  }, []);

  // Fixed range: selected day 00:00 - 23:59:59 in UTC+8
  const dayStart = useMemo(() => {
    const [y, m, d] = selectedDate.split('-').map(Number);
    // Build UTC+8 midnight, convert to UTC
    const utc8Midnight = Date.UTC(y, m - 1, d, 0, 0, 0);
    return utc8Midnight - UTC8_OFFSET_MS;
  }, [selectedDate]);
  const dayEnd = dayStart + ONE_DAY_MS;
  const rangeMs = ONE_DAY_MS;

  // Notify parent when date changes
  useEffect(() => {
    onRangeChange(new Date(dayStart), new Date(dayEnd));
  }, [dayStart, dayEnd, onRangeChange]);

  const segments = useMemo(
    () => mapFragmentsToSegments(fragments, dayStart, dayEnd),
    [fragments, dayStart, dayEnd]
  );

  // Hour tick marks: 0, 1, 2, ... 24
  const hourTicks = useMemo(() => {
    const ticks: number[] = [];
    for (let h = 0; h <= 24; h += 2) {
      ticks.push(dayStart + h * 3600_000);
    }
    return ticks;
  }, [dayStart]);

  const handleClick = useCallback(
    (e: React.MouseEvent<SVGSVGElement>) => {
      const svg = svgRef.current;
      if (!svg) return;
      const rect = svg.getBoundingClientRect();
      const x = e.clientX - rect.left;
      const ms = dayStart + (x / rect.width) * rangeMs;
      const clamped = Math.max(dayStart, Math.min(dayEnd, ms));
      onTimeSelect(new Date(clamped));
    },
    [dayStart, dayEnd, rangeMs, onTimeSelect]
  );

  /** Convert a clientX position to a formatted HH:MM:SS time string. */
  const xToTime = useCallback(
    (clientX: number): { x: number; time: string } | null => {
      const svg = svgRef.current;
      if (!svg) return null;
      const rect = svg.getBoundingClientRect();
      const x = clientX - rect.left;
      const ms = dayStart + (x / rect.width) * rangeMs;
      const clamped = Math.max(dayStart, Math.min(dayEnd, ms));
      const d = new Date(clamped + UTC8_OFFSET_MS);
      const time = d.toISOString().slice(11, 19); // HH:MM:SS
      return { x, time };
    },
    [dayStart, dayEnd, rangeMs],
  );

  const handleMouseMove = useCallback(
    (e: React.MouseEvent<SVGSVGElement>) => {
      setHoverInfo(xToTime(e.clientX));
    },
    [xToTime],
  );

  const handleMouseLeave = useCallback(() => {
    setHoverInfo(null);
  }, []);

  const handleTouchMove = useCallback(
    (e: React.TouchEvent<SVGSVGElement>) => {
      if (e.touches.length > 0) {
        setHoverInfo(xToTime(e.touches[0].clientX));
      }
    },
    [xToTime],
  );

  const handleTouchEnd = useCallback(() => {
    setHoverInfo(null);
  }, []);

  // "Now" indicator position (only show if today)
  const isToday = formatDateUTC8(new Date(nowMs)) === selectedDate;
  const nowPct = isToday ? ((nowMs - dayStart) / rangeMs) * 100 : -1;

  // Playback indicator position
  const playPct = currentTime
    ? ((currentTime.getTime() - dayStart) / rangeMs) * 100
    : -1;

  // Date navigation
  const goToPrevDay = useCallback(() => {
    const [y, m, d] = selectedDate.split('-').map(Number);
    const prev = new Date(Date.UTC(y, m - 1, d - 1));
    setSelectedDate(prev.toISOString().slice(0, 10));
  }, [selectedDate]);

  const goToNextDay = useCallback(() => {
    const [y, m, d] = selectedDate.split('-').map(Number);
    const next = new Date(Date.UTC(y, m - 1, d + 1));
    const nextStr = next.toISOString().slice(0, 10);
    const todayStr = formatDateUTC8(new Date());
    if (nextStr <= todayStr) {
      setSelectedDate(nextStr);
    }
  }, [selectedDate]);

  const goToToday = useCallback(() => {
    setSelectedDate(formatDateUTC8(new Date()));
  }, []);

  const todayStr = formatDateUTC8(new Date());
  const isSelectedToday = selectedDate === todayStr;

  return (
    <div className="flex flex-col gap-2">
      {/* Date picker bar */}
      <div className="flex items-center gap-2 text-sm">
        <button
          onClick={goToPrevDay}
          className="rounded border border-gray-300 px-2 py-1 text-gray-600 hover:bg-gray-100"
        >
          ◀
        </button>
        <input
          type="date"
          value={selectedDate}
          max={todayStr}
          onChange={(e) => setSelectedDate(e.target.value)}
          className="rounded border border-gray-300 px-2 py-1 text-gray-700"
        />
        <button
          onClick={goToNextDay}
          disabled={isSelectedToday}
          className="rounded border border-gray-300 px-2 py-1 text-gray-600 hover:bg-gray-100 disabled:opacity-30"
        >
          ▶
        </button>
        {!isSelectedToday && (
          <button
            onClick={goToToday}
            className="rounded bg-brand-100 px-2 py-1 text-brand-700 hover:bg-brand-200"
          >
            今天
          </button>
        )}
      </div>

      {/* SVG Timeline */}
      <div className="relative w-full select-none">
        {/* Hover tooltip */}
        {hoverInfo && (
          <div
            className="pointer-events-none absolute z-10 -translate-x-1/2"
            style={{ left: hoverInfo.x, top: -36 }}
          >
            <div className="rounded-lg bg-gray-900 px-2 py-1 text-xs text-white shadow-lg">
              {hoverInfo.time}
            </div>
            {/* Triangle arrow pointing down */}
            <div className="flex justify-center">
              <div className="h-0 w-0 border-x-4 border-t-4 border-x-transparent border-t-gray-900" />
            </div>
          </div>
        )}
        <svg
          ref={svgRef}
          width="100%"
          height={SVG_HEIGHT}
          className="cursor-crosshair"
          onClick={handleClick}
          onMouseMove={handleMouseMove}
          onMouseLeave={handleMouseLeave}
          onTouchMove={handleTouchMove}
          onTouchEnd={handleTouchEnd}
        >
          {/* Gray background */}
          <rect x="0" y="0" width="100%" height={TIMELINE_HEIGHT} fill="#e5e7eb" rx={4} />

          {/* Green recording segments */}
          {segments
            .filter((s) => s.hasRecording)
            .map((s, i) => {
              const xPct = ((s.start - dayStart) / rangeMs) * 100;
              const wPct = ((s.end - s.start) / rangeMs) * 100;
              return (
                <rect key={i} x={`${xPct}%`} y="0" width={`${wPct}%`} height={TIMELINE_HEIGHT} fill="#86BC25" opacity={0.8} rx={2} />
              );
            })}

          {/* "Now" indicator — blue triangle + dashed line */}
          {isToday && nowPct >= 0 && nowPct <= 100 && (
            <g>
              <line
                x1={`${nowPct}%`} y1="0"
                x2={`${nowPct}%`} y2={TIMELINE_HEIGHT}
                stroke="#86BC25" strokeWidth={1.5} strokeDasharray="4 2"
              />
              <polygon
                points={`${nowPct}%,-1 ${nowPct - 0.6}%,8 ${nowPct + 0.6}%,8`}
                fill="#86BC25"
                transform={`translate(0, 0)`}
              />
              {/* Use absolute pixel positioning for triangle via a foreignObject workaround */}
            </g>
          )}

          {/* Playback position — red triangle + solid line */}
          {playPct >= 0 && playPct <= 100 && (
            <g>
              <line
                x1={`${playPct}%`} y1="0"
                x2={`${playPct}%`} y2={TIMELINE_HEIGHT}
                stroke="#ef4444" strokeWidth={2}
              />
              <circle
                cx={`${playPct}%`} cy={TIMELINE_HEIGHT / 2}
                r={5} fill="#ef4444" stroke="white" strokeWidth={1.5}
              />
            </g>
          )}

          {/* Bottom axis */}
          <line x1="0" y1={TIMELINE_HEIGHT} x2="100%" y2={TIMELINE_HEIGHT} stroke="#9ca3af" strokeWidth={1} />

          {/* Hour tick marks */}
          {hourTicks.map((t) => {
            const xPct = ((t - dayStart) / rangeMs) * 100;
            const hour = Math.round((t - dayStart) / 3600_000);
            return (
              <g key={t}>
                <line
                  x1={`${xPct}%`} y1={TIMELINE_HEIGHT}
                  x2={`${xPct}%`} y2={TIMELINE_HEIGHT + 5}
                  stroke="#6b7280" strokeWidth={1}
                />
                <text
                  x={`${xPct}%`} y={TIMELINE_HEIGHT + 17}
                  textAnchor="middle" fontSize={10} fill="#6b7280"
                >
                  {formatHourLabel(hour)}
                </text>
              </g>
            );
          })}
        </svg>
      </div>

      {/* Legend */}
      <div className="flex items-center gap-4 text-xs text-gray-500">
        <span className="flex items-center gap-1">
          <span className="inline-block h-2.5 w-2.5 rounded-sm bg-brand-500 opacity-80" /> 有录像
        </span>
        <span className="flex items-center gap-1">
          <span className="inline-block h-2.5 w-2.5 rounded-sm bg-gray-300" /> 无录像
        </span>
        {isToday && (
          <span className="flex items-center gap-1">
            <span className="inline-block h-2.5 w-0.5 bg-brand-500" style={{ borderLeft: '2px dashed #86BC25' }} /> 当前时间
          </span>
        )}
        <span className="flex items-center gap-1">
          <span className="inline-block h-2.5 w-2.5 rounded-full bg-red-500" /> 播放位置
        </span>
      </div>
    </div>
  );
};

export default Timeline;
