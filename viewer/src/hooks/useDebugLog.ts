import { useState, useCallback } from 'react';
import type { LogEntry } from '../types';

/**
 * Format a Date to HH:MM:SS string.
 * Exported as a pure function for Property 14 testing.
 *
 * Validates: Requirements 7.1
 */
export function formatTimestamp(date: Date): string {
  const hh = String(date.getHours()).padStart(2, '0');
  const mm = String(date.getMinutes()).padStart(2, '0');
  const ss = String(date.getSeconds()).padStart(2, '0');
  return `${hh}:${mm}:${ss}`;
}

/**
 * Debug log management hook.
 *
 * Manages a list of log entries with timestamps and provides
 * an addLog callback for appending new entries.
 *
 * Validates: Requirements 7.1, 7.2, 7.3, 7.4
 */
export function useDebugLog(): {
  logs: LogEntry[];
  addLog: (message: string) => void;
  clearLogs: () => void;
} {
  const [logs, setLogs] = useState<LogEntry[]>([]);

  const addLog = useCallback((message: string) => {
    setLogs((prev) => [...prev, { timestamp: new Date(), message }]);
  }, []);

  const clearLogs = useCallback(() => {
    setLogs([]);
  }, []);

  return { logs, addLog, clearLogs };
}
