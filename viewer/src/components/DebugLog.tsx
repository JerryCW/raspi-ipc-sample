import { useEffect, useRef } from 'react';
import type { LogEntry } from '../types';
import { formatTimestamp } from '../hooks/useDebugLog';

interface DebugLogProps {
  logs: LogEntry[];
}

/**
 * Debug log display component.
 *
 * - Monospace font
 * - HH:MM:SS timestamps
 * - Auto-scrolls to latest entry
 *
 * Validates: Requirements 7.1, 7.2, 7.3, 7.4
 */
export function DebugLog({ logs }: DebugLogProps) {
  const endRef = useRef<HTMLDivElement>(null);

  // Auto-scroll to bottom when new logs arrive
  useEffect(() => {
    endRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [logs]);

  return (
    <div className="flex flex-col gap-1">
      <h3 className="text-sm font-medium text-gray-700">调试日志</h3>
      <div className="h-48 overflow-y-auto rounded-md border border-gray-200 bg-gray-50 p-2 font-mono text-xs">
        {logs.length === 0 && (
          <p className="text-gray-400">暂无日志</p>
        )}
        {logs.map((entry, i) => (
          <div key={i} className="leading-5">
            <span className="text-gray-500">[{formatTimestamp(entry.timestamp)}]</span>{' '}
            <span className="text-gray-800">{entry.message}</span>
          </div>
        ))}
        <div ref={endRef} />
      </div>
    </div>
  );
}
