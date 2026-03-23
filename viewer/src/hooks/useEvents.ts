import { useState, useEffect, useCallback, useRef } from 'react';
import type { ActivityEvent } from '../types';
import { fetchEvents } from '../services/events';

interface UseEventsReturn {
  events: ActivityEvent[];
  isLoading: boolean;
  error: string | null;
  retry: () => void;
}

/**
 * Format a Date to YYYY-MM-DD in UTC+8.
 */
function toDateStringUTC8(d: Date): string {
  const utc8 = new Date(d.getTime() + 8 * 60 * 60 * 1000);
  return utc8.toISOString().slice(0, 10);
}

/**
 * Hook to fetch and manage activity events for a given date.
 *
 * Validates: Requirements 5.2, 5.6, 5.7
 */
export function useEvents(date: Date, idToken: string | null): UseEventsReturn {
  const [events, setEvents] = useState<ActivityEvent[]>([]);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const retryRef = useRef(0);

  const dateStr = toDateStringUTC8(date);

  const load = useCallback(async () => {
    if (!idToken) return;
    setIsLoading(true);
    setError(null);
    try {
      const data = await fetchEvents(dateStr, idToken);
      setEvents(data);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to load events');
      setEvents([]);
    } finally {
      setIsLoading(false);
    }
  }, [dateStr, idToken]);

  useEffect(() => {
    void load();
  }, [load]);

  const retry = useCallback(() => {
    retryRef.current++;
    void load();
  }, [load]);

  return { events, isLoading, error, retry };
}
