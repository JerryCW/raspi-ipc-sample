import type { ActivityEvent } from '../types';

/**
 * Fetch activity events for a given date.
 *
 * Validates: Requirements 5.2, 5.5
 */
export async function fetchEvents(
  date: string,
  token: string,
): Promise<ActivityEvent[]> {
  const res = await fetch(`/api/events?date=${encodeURIComponent(date)}`, {
    headers: { Authorization: `Bearer ${token}` },
  });

  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error((body as { error?: string }).error ?? `Request failed: ${res.status}`);
  }

  const data = (await res.json()) as { events: ActivityEvent[] };
  return data.events;
}

/**
 * Fetch a pre-signed thumbnail URL for an event session.
 *
 * Validates: Requirements 5.5
 */
export async function fetchThumbnailUrl(
  sessionId: string,
  type: 'start' | 'end',
  token: string,
): Promise<string> {
  const res = await fetch(
    `/api/events/${encodeURIComponent(sessionId)}/thumbnail?type=${type}`,
    { headers: { Authorization: `Bearer ${token}` } },
  );

  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error((body as { error?: string }).error ?? `Request failed: ${res.status}`);
  }

  const data = (await res.json()) as { url: string };
  return data.url;
}
