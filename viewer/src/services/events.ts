import type { ActivityEvent } from '../types';

/**
 * 获取指定日期的活动事件列表。
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
 * 获取事件缩略图的 presigned URL。
 */
export async function fetchThumbnailUrl(
  eventId: string,
  token: string,
): Promise<string> {
  const res = await fetch(
    `/api/events/${encodeURIComponent(eventId)}/thumbnail`,
    { headers: { Authorization: `Bearer ${token}` } },
  );

  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error((body as { error?: string }).error ?? `Request failed: ${res.status}`);
  }

  const data = (await res.json()) as { url: string };
  return data.url;
}

/**
 * 下载事件视频片段。
 */
export async function exportVideoClip(
  eventId: string,
  token: string,
): Promise<{ blob: Blob; filename: string }> {
  const res = await fetch(
    `/api/events/${encodeURIComponent(eventId)}/clip`,
    {
      headers: { Authorization: `Bearer ${token}` },
    },
  );

  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error(
      (body as { error?: string }).error ?? `Export failed: ${res.status}`,
    );
  }

  const blob = await res.blob();
  const disposition = res.headers.get('Content-Disposition') || '';
  const filenameMatch = disposition.match(/filename="?([^"]+)"?/);
  const filename = filenameMatch?.[1] || `event_${eventId}.mp4`;

  return { blob, filename };
}
