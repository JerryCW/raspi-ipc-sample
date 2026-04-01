import React, { useCallback, useMemo, useRef, useState, useEffect } from 'react';
import type { Fragment } from '../types';

// ===== Constants =====

export const MIN_RANGE_MS = 60 * 1000;
export const MAX_RANGE_MS = 7 * 24 * 60 * 60 * 1000;
const TIMELINE_HEIGHT = 36;
const ONE_DAY_MS = 24 * 60 * 60 * 1000;
const UTC8_OFFSET_MS = 8 * 60 * 60 * 1000;

// ===== Exported pure helpers =====

export interface TimeSegment { start: number; end: number; hasRecording: boolean; }

export function clampTimeRange(startMs: number, endMs: number): { start: number; end: number } {
  const duration = endMs - startMs;
  const mid = (startMs + endMs) / 2;
  if (duration < MIN_RANGE_MS) return { start: Math.round(mid - MIN_RANGE_MS / 2), end: Math.round(mid + MIN_RANGE_MS / 2) };
  if (duration > MAX_RANGE_MS) return { start: Math.round(mid - MAX_RANGE_MS / 2), end: Math.round(mid + MAX_RANGE_MS / 2) };
  return { start: startMs, end: endMs };
}

export function mapFragmentsToSegments(fragments: Fragment[], rangeStart: number, rangeEnd: number): TimeSegment[] {
  if (rangeStart >= rangeEnd) return [];
  if (fragments.length === 0) return [{ start: rangeStart, end: rangeEnd, hasRecording: false }];
  const intervals: { start: number; end: number }[] = [];
  for (const f of fragments) {
    const fStart = f.producerTimestamp.getTime();
    const fEnd = fStart + f.fragmentLengthMillis;
    const clippedStart = Math.max(fStart, rangeStart);
    const clippedEnd = Math.min(fEnd, rangeEnd);
    if (clippedStart < clippedEnd) intervals.push({ start: clippedStart, end: clippedEnd });
  }
  if (intervals.length === 0) return [{ start: rangeStart, end: rangeEnd, hasRecording: false }];
  intervals.sort((a, b) => a.start - b.start);
  const merged: { start: number; end: number }[] = [intervals[0]];
  for (let i = 1; i < intervals.length; i++) {
    const last = merged[merged.length - 1];
    if (intervals[i].start <= last.end) last.end = Math.max(last.end, intervals[i].end);
    else merged.push({ ...intervals[i] });
  }
  const segments: TimeSegment[] = [];
  let cursor = rangeStart;
  for (const interval of merged) {
    if (cursor < interval.start) segments.push({ start: cursor, end: interval.start, hasRecording: false });
    segments.push({ start: interval.start, end: interval.end, hasRecording: true });
    cursor = interval.end;
  }
  if (cursor < rangeEnd) segments.push({ start: cursor, end: rangeEnd, hasRecording: false });
  return segments;
}


// ===== UTC+8 helpers =====

export function getDayStartUTC8(date: Date): Date {
  const utc8Ms = date.getTime() + UTC8_OFFSET_MS;
  const dayStartUtc8 = Math.floor(utc8Ms / ONE_DAY_MS) * ONE_DAY_MS;
  return new Date(dayStartUtc8 - UTC8_OFFSET_MS);
}

function formatDateUTC8(date: Date): string {
  const d = new Date(date.getTime() + UTC8_OFFSET_MS);
  return d.toISOString().slice(0, 10);
}

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
  hideDatePicker?: boolean;
}

const Timeline: React.FC<TimelineProps> = ({
  fragments, startTime: _startTime, endTime: _endTime, currentTime,
  onTimeSelect, onRangeChange, hideDatePicker = false,
}) => {
  const barRef = useRef<HTMLDivElement>(null);
  const [hoverInfo, setHoverInfo] = useState<{ x: number; time: string } | null>(null);
  const [selectedDate, setSelectedDate] = useState(() => formatDateUTC8(new Date()));

  useEffect(() => {
    if (hideDatePicker) setSelectedDate(formatDateUTC8(_startTime));
  }, [hideDatePicker, _startTime]);

  const [nowMs, setNowMs] = useState(() => Date.now());
  useEffect(() => { const id = setInterval(() => setNowMs(Date.now()), 30_000); return () => clearInterval(id); }, []);

  const dayStart = useMemo(() => {
    const [y, m, d] = selectedDate.split('-').map(Number);
    return Date.UTC(y, m - 1, d, 0, 0, 0) - UTC8_OFFSET_MS;
  }, [selectedDate]);
  const dayEnd = dayStart + ONE_DAY_MS;
  const rangeMs = ONE_DAY_MS;

  useEffect(() => { onRangeChange(new Date(dayStart), new Date(dayEnd)); }, [dayStart, dayEnd, onRangeChange]);

  const segments = useMemo(() => mapFragmentsToSegments(fragments, dayStart, dayEnd), [fragments, dayStart, dayEnd]);

  const hourTicks = useMemo(() => {
    const ticks: number[] = [];
    for (let h = 0; h <= 24; h += 2) ticks.push(h);
    return ticks;
  }, []);

  const clientXToMs = useCallback((clientX: number): number | null => {
    const el = barRef.current;
    if (!el) return null;
    const rect = el.getBoundingClientRect();
    const x = clientX - rect.left;
    const ms = dayStart + (x / rect.width) * rangeMs;
    return Math.max(dayStart, Math.min(dayEnd, ms));
  }, [dayStart, dayEnd, rangeMs]);

  const clientXToInfo = useCallback((clientX: number): { x: number; time: string } | null => {
    const el = barRef.current;
    if (!el) return null;
    const rect = el.getBoundingClientRect();
    const x = clientX - rect.left;
    const ms = dayStart + (x / rect.width) * rangeMs;
    const clamped = Math.max(dayStart, Math.min(dayEnd, ms));
    const d = new Date(clamped + UTC8_OFFSET_MS);
    return { x, time: d.toISOString().slice(11, 19) };
  }, [dayStart, dayEnd, rangeMs]);

  const handleClick = useCallback((e: React.MouseEvent<HTMLDivElement>) => {
    const ms = clientXToMs(e.clientX);
    if (ms !== null) onTimeSelect(new Date(ms));
  }, [clientXToMs, onTimeSelect]);

  const handleMouseMove = useCallback((e: React.MouseEvent<HTMLDivElement>) => { setHoverInfo(clientXToInfo(e.clientX)); }, [clientXToInfo]);
  const handleMouseLeave = useCallback(() => { setHoverInfo(null); }, []);
  const handleTouchMove = useCallback((e: React.TouchEvent<HTMLDivElement>) => {
    if (e.touches.length > 0) setHoverInfo(clientXToInfo(e.touches[0].clientX));
  }, [clientXToInfo]);
  const handleTouchEnd = useCallback(() => { setHoverInfo(null); }, []);

  const isToday = formatDateUTC8(new Date(nowMs)) === selectedDate;
  const nowPct = isToday ? ((nowMs - dayStart) / rangeMs) * 100 : -1;
  const playPct = currentTime ? ((currentTime.getTime() - dayStart) / rangeMs) * 100 : -1;

  const goToPrevDay = useCallback(() => {
    const [y, m, d] = selectedDate.split('-').map(Number);
    setSelectedDate(new Date(Date.UTC(y, m - 1, d - 1)).toISOString().slice(0, 10));
  }, [selectedDate]);
  const goToNextDay = useCallback(() => {
    const [y, m, d] = selectedDate.split('-').map(Number);
    const nextStr = new Date(Date.UTC(y, m - 1, d + 1)).toISOString().slice(0, 10);
    if (nextStr <= formatDateUTC8(new Date())) setSelectedDate(nextStr);
  }, [selectedDate]);
  const goToToday = useCallback(() => { setSelectedDate(formatDateUTC8(new Date())); }, []);

  const todayStr = formatDateUTC8(new Date());
  const isSelectedToday = selectedDate === todayStr;


  return (
    <div className="flex flex-col gap-2">
      {/* Date picker bar */}
      {!hideDatePicker && (
      <div className="flex items-center gap-2 text-sm">
        <button onClick={goToPrevDay} className="rounded border border-gray-300 px-2 py-1 text-gray-600 hover:bg-gray-100">◀</button>
        <input type="date" value={selectedDate} max={todayStr} onChange={(e) => setSelectedDate(e.target.value)} className="rounded border border-gray-300 px-2 py-1 text-gray-700" />
        <button onClick={goToNextDay} disabled={isSelectedToday} className="rounded border border-gray-300 px-2 py-1 text-gray-600 hover:bg-gray-100 disabled:opacity-30">▶</button>
        {!isSelectedToday && (<button onClick={goToToday} className="rounded bg-brand-100 px-2 py-1 text-brand-700 hover:bg-brand-200">今天</button>)}
      </div>
      )}

      {/* Timeline bar (pure HTML divs) */}
      <div className="relative w-full select-none">
        {/* Hover tooltip */}
        {hoverInfo && (
          <div className="pointer-events-none absolute z-10 -translate-x-1/2" style={{ left: hoverInfo.x, top: -36 }}>
            <div className="rounded-lg bg-gray-900 px-2 py-1 text-xs text-white shadow-lg">{hoverInfo.time}</div>
            <div className="flex justify-center"><div className="h-0 w-0 border-x-4 border-t-4 border-x-transparent border-t-gray-900" /></div>
          </div>
        )}

        {/* Timeline track */}
        <div
          ref={barRef}
          className="relative w-full cursor-crosshair overflow-hidden rounded"
          style={{ height: TIMELINE_HEIGHT, backgroundColor: '#e5e7eb' }}
          onClick={handleClick}
          onMouseMove={handleMouseMove}
          onMouseLeave={handleMouseLeave}
          onTouchMove={handleTouchMove}
          onTouchEnd={handleTouchEnd}
        >
          {/* Green recording segments */}
          {segments.filter((s) => s.hasRecording).map((s, i) => {
            const leftPct = ((s.start - dayStart) / rangeMs) * 100;
            const widthPct = ((s.end - s.start) / rangeMs) * 100;
            return (
              <div key={i} className="absolute top-0 bottom-0" style={{ left: `${leftPct}%`, width: `${widthPct}%`, backgroundColor: '#86BC25', opacity: 0.8, borderRadius: 2 }} />
            );
          })}

          {/* "Now" indicator — dashed green line */}
          {isToday && nowPct >= 0 && nowPct <= 100 && (
            <div className="absolute top-0 bottom-0" style={{ left: `${nowPct}%`, width: 2, borderLeft: '2px dashed #86BC25' }} />
          )}

          {/* Playback position — red line + dot */}
          {playPct >= 0 && playPct <= 100 && (
            <>
              <div className="absolute top-0 bottom-0" style={{ left: `${playPct}%`, width: 2, backgroundColor: '#ef4444' }} />
              <div className="absolute" style={{ left: `${playPct}%`, top: TIMELINE_HEIGHT / 2 - 5, width: 10, height: 10, borderRadius: '50%', backgroundColor: '#ef4444', border: '1.5px solid white', transform: 'translateX(-50%)' }} />
            </>
          )}
        </div>

        {/* Hour tick labels (below the bar) */}
        <div className="relative w-full" style={{ height: 22 }}>
          {hourTicks.map((h) => {
            const pct = (h / 24) * 100;
            return (
              <span key={h} className="absolute text-gray-500" style={{ left: `${pct}%`, top: 5, transform: 'translateX(-50%)', fontSize: 10 }}>
                {formatHourLabel(h)}
              </span>
            );
          })}
        </div>
      </div>

      {/* Legend */}
      <div className="flex items-center gap-4 text-xs text-gray-500">
        <span className="flex items-center gap-1"><span className="inline-block h-2.5 w-2.5 rounded-sm bg-brand-500 opacity-80" /> 有录像</span>
        <span className="flex items-center gap-1"><span className="inline-block h-2.5 w-2.5 rounded-sm bg-gray-300" /> 无录像</span>
        {isToday && (<span className="flex items-center gap-1"><span className="inline-block h-2.5 w-0.5" style={{ borderLeft: '2px dashed #86BC25' }} /> 当前时间</span>)}
        <span className="flex items-center gap-1"><span className="inline-block h-2.5 w-2.5 rounded-full bg-red-500" /> 播放位置</span>
      </div>
    </div>
  );
};

export default Timeline;