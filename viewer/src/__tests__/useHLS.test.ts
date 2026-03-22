/**
 * Unit Tests: HLS 默认时间范围、Safari 降级、错误处理、停止清理
 * Validates: Requirements 4.1, 4.5, 4.9, 4.10
 */
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { renderHook, act } from '@testing-library/react';
import type { AWSCredentials } from '../types';

// ===== Mocks =====

// Track hls.js event handlers and mock methods per instance
const mockHlsDestroy = vi.fn();
const mockHlsLoadSource = vi.fn();
const mockHlsAttachMedia = vi.fn();
let hlsEventHandlers: Record<string, (...args: unknown[]) => void> = {};

vi.mock('hls.js', () => {
  const HlsMock = vi.fn(function (this: Record<string, unknown>) {
    this.destroy = mockHlsDestroy;
    this.loadSource = mockHlsLoadSource;
    this.attachMedia = mockHlsAttachMedia;
    this.currentLevel = -1;
    this.levels = [];
    this.on = vi.fn((event: string, handler: (...args: unknown[]) => void) => {
      hlsEventHandlers[event] = handler;
    });
  }) as unknown as { new(): Record<string, unknown>; isSupported: () => boolean; Events: Record<string, string> };
  HlsMock.isSupported = vi.fn(() => true) as unknown as () => boolean;
  HlsMock.Events = {
    MANIFEST_PARSED: 'hlsManifestParsed',
    ERROR: 'hlsError',
  };
  return { default: HlsMock };
});

// Mock KVS service
vi.mock('../services/kvs', () => ({
  listFragments: vi.fn().mockResolvedValue([]),
  getHlsStreamingUrl: vi.fn().mockResolvedValue('https://example.com/hls/stream.m3u8'),
}));

// ===== Helpers =====

const mockCredentials: AWSCredentials = {
  accessKeyId: 'AKIAIOSFODNN7EXAMPLE',
  secretAccessKey: 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY',
  sessionToken: 'FwoGZXIvYXdzEBYaDH...',
  expiration: new Date(Date.now() + 3600_000),
};

function createMockVideoElement(overrides: Partial<HTMLVideoElement> = {}): HTMLVideoElement {
  return {
    canPlayType: vi.fn(() => ''),
    play: vi.fn().mockResolvedValue(undefined),
    pause: vi.fn(),
    load: vi.fn(),
    removeAttribute: vi.fn(),
    src: '',
    videoWidth: 0,
    videoHeight: 0,
    currentTime: 0,
    buffered: { length: 0, start: vi.fn(), end: vi.fn() },
    error: null,
    onloadedmetadata: null,
    onerror: null,
    getVideoPlaybackQuality: vi.fn(() => ({
      droppedVideoFrames: 0,
      totalVideoFrames: 0,
      creationTime: 0,
      corruptedVideoFrames: 0,
    })),
    ...overrides,
  } as unknown as HTMLVideoElement;
}


// ===== Test: Default time range (Requirement 4.1) =====

describe('Unit: 默认时间范围 — 最近 24 小时 (Requirement 4.1)', () => {
  it('getDefaultTimeRange returns a range spanning exactly 24 hours', async () => {
    const { getDefaultTimeRange } = await import('../components/HLSPanel');
    const before = Date.now();
    const { start, end } = getDefaultTimeRange();
    const after = Date.now();

    // End time should be approximately "now"
    expect(end.getTime()).toBeGreaterThanOrEqual(before);
    expect(end.getTime()).toBeLessThanOrEqual(after);

    // Start time should be exactly 24 hours before end
    const diffMs = end.getTime() - start.getTime();
    expect(diffMs).toBe(24 * 60 * 60 * 1000);
  });

  it('start is always before end', async () => {
    const { getDefaultTimeRange } = await import('../components/HLSPanel');
    const { start, end } = getDefaultTimeRange();
    expect(start.getTime()).toBeLessThan(end.getTime());
  });

  it('returns Date objects', async () => {
    const { getDefaultTimeRange } = await import('../components/HLSPanel');
    const { start, end } = getDefaultTimeRange();
    expect(start).toBeInstanceOf(Date);
    expect(end).toBeInstanceOf(Date);
  });
});

// ===== Test: Safari native HLS fallback (Requirement 4.5) =====

describe('Unit: Safari 原生 HLS 降级逻辑 (Requirement 4.5)', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    hlsEventHandlers = {};
  });

  it('should use native HLS when canPlayType returns a truthy value (Safari)', async () => {
    const { useHLS } = await import('../hooks/useHLS');
    const logs: string[] = [];
    const mockVideo = createMockVideoElement({
      canPlayType: vi.fn((type: string) =>
        type === 'application/vnd.apple.mpegurl' ? 'maybe' : '',
      ) as HTMLVideoElement['canPlayType'],
    });

    const { result } = renderHook(() =>
      useHLS({
        streamName: 'test-stream',
        credentials: mockCredentials,
        region: 'ap-southeast-1',
        onLog: (msg) => logs.push(msg),
      }),
    );

    // Inject mock video element into the ref
    (result.current.videoRef as { current: HTMLVideoElement }).current = mockVideo;

    await act(async () => {
      await result.current.start(new Date(), new Date());
    });

    // Should log that native HLS is being used
    expect(logs.some((l) => l.includes('native HLS') || l.includes('Safari'))).toBe(true);
    // Should set src directly on the video element
    expect(mockVideo.src).toBe('https://example.com/hls/stream.m3u8');
    // hls.js loadSource should NOT have been called
    expect(mockHlsLoadSource).not.toHaveBeenCalled();
  });

  it('should use hls.js when canPlayType returns empty string (non-Safari)', async () => {
    const { useHLS } = await import('../hooks/useHLS');
    const logs: string[] = [];
    const mockVideo = createMockVideoElement();

    const { result } = renderHook(() =>
      useHLS({
        streamName: 'test-stream',
        credentials: mockCredentials,
        region: 'ap-southeast-1',
        onLog: (msg) => logs.push(msg),
      }),
    );

    (result.current.videoRef as { current: HTMLVideoElement }).current = mockVideo;

    await act(async () => {
      await result.current.start(new Date(), new Date());
    });

    // Should log that hls.js is being used
    expect(logs.some((l) => l.includes('hls.js'))).toBe(true);
    // hls.js should have been used
    expect(mockHlsLoadSource).toHaveBeenCalledWith('https://example.com/hls/stream.m3u8');
    expect(mockHlsAttachMedia).toHaveBeenCalledWith(mockVideo);
  });
});


// ===== Test: Fatal error handling (Requirement 4.9) =====

describe('Unit: 致命错误时的状态栏提示 (Requirement 4.9)', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    hlsEventHandlers = {};
  });

  it('should set status to error and log fatal error message on hls.js fatal error', async () => {
    const { useHLS } = await import('../hooks/useHLS');
    const logs: string[] = [];
    const mockVideo = createMockVideoElement();

    const { result } = renderHook(() =>
      useHLS({
        streamName: 'test-stream',
        credentials: mockCredentials,
        region: 'ap-southeast-1',
        onLog: (msg) => logs.push(msg),
      }),
    );

    (result.current.videoRef as { current: HTMLVideoElement }).current = mockVideo;

    // Start playback (will use hls.js since canPlayType returns '')
    await act(async () => {
      await result.current.start(new Date(), new Date());
    });

    // Simulate a fatal error from hls.js
    const errorHandler = hlsEventHandlers['hlsError'];
    expect(errorHandler).toBeDefined();

    act(() => {
      errorHandler('hlsError', {
        fatal: true,
        type: 'networkError',
        details: 'manifestLoadError',
      });
    });

    // Status should be 'error'
    expect(result.current.status).toBe('error');
    // Should have logged the fatal error
    expect(logs.some((l) => l.includes('fatal error'))).toBe(true);
    expect(logs.some((l) => l.includes('networkError'))).toBe(true);
    // hls.js destroy should have been called (cleanup)
    expect(mockHlsDestroy).toHaveBeenCalled();
  });

  it('should only log non-fatal errors without changing status to error', async () => {
    const { useHLS } = await import('../hooks/useHLS');
    const logs: string[] = [];
    const mockVideo = createMockVideoElement();

    const { result } = renderHook(() =>
      useHLS({
        streamName: 'test-stream',
        credentials: mockCredentials,
        region: 'ap-southeast-1',
        onLog: (msg) => logs.push(msg),
      }),
    );

    (result.current.videoRef as { current: HTMLVideoElement }).current = mockVideo;

    await act(async () => {
      await result.current.start(new Date(), new Date());
    });

    // Simulate MANIFEST_PARSED to set status to 'connected'
    const manifestHandler = hlsEventHandlers['hlsManifestParsed'];
    if (manifestHandler) {
      act(() => {
        manifestHandler();
      });
    }

    // Simulate a non-fatal error
    const errorHandler = hlsEventHandlers['hlsError'];
    act(() => {
      errorHandler('hlsError', {
        fatal: false,
        type: 'mediaError',
        details: 'bufferStalledError',
      });
    });

    // Status should NOT be error for non-fatal
    expect(result.current.status).not.toBe('error');
    // Should still log the non-fatal error
    expect(logs.some((l) => l.includes('non-fatal'))).toBe(true);
    // hls.js destroy should NOT have been called for non-fatal
    expect(mockHlsDestroy).not.toHaveBeenCalled();
  });

  it('should set status to error when getHlsStreamingUrl throws', async () => {
    const { getHlsStreamingUrl } = await import('../services/kvs');
    (getHlsStreamingUrl as ReturnType<typeof vi.fn>).mockRejectedValueOnce(
      new Error('KVS API timeout'),
    );

    const { useHLS } = await import('../hooks/useHLS');
    const logs: string[] = [];
    const mockVideo = createMockVideoElement();

    const { result } = renderHook(() =>
      useHLS({
        streamName: 'test-stream',
        credentials: mockCredentials,
        region: 'ap-southeast-1',
        onLog: (msg) => logs.push(msg),
      }),
    );

    (result.current.videoRef as { current: HTMLVideoElement }).current = mockVideo;

    await act(async () => {
      await result.current.start(new Date(), new Date());
    });

    expect(result.current.status).toBe('error');
    expect(logs.some((l) => l.includes('KVS API timeout'))).toBe(true);
  });
});


// ===== Test: Stop cleanup (Requirement 4.10) =====

describe('Unit: stop 时 hls.js 实例销毁和状态重置 (Requirement 4.10)', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    hlsEventHandlers = {};
  });

  it('should destroy hls.js instance when stop is called', async () => {
    const { useHLS } = await import('../hooks/useHLS');
    const logs: string[] = [];
    const mockVideo = createMockVideoElement();

    const { result } = renderHook(() =>
      useHLS({
        streamName: 'test-stream',
        credentials: mockCredentials,
        region: 'ap-southeast-1',
        onLog: (msg) => logs.push(msg),
      }),
    );

    (result.current.videoRef as { current: HTMLVideoElement }).current = mockVideo;

    // Start playback
    await act(async () => {
      await result.current.start(new Date(), new Date());
    });

    // Verify hls.js was created and used
    expect(mockHlsLoadSource).toHaveBeenCalled();

    // Stop playback
    act(() => {
      result.current.stop();
    });

    // hls.js destroy should have been called
    expect(mockHlsDestroy).toHaveBeenCalled();
  });

  it('should reset status to stopped after stop', async () => {
    const { useHLS } = await import('../hooks/useHLS');
    const logs: string[] = [];
    const mockVideo = createMockVideoElement();

    const { result } = renderHook(() =>
      useHLS({
        streamName: 'test-stream',
        credentials: mockCredentials,
        region: 'ap-southeast-1',
        onLog: (msg) => logs.push(msg),
      }),
    );

    (result.current.videoRef as { current: HTMLVideoElement }).current = mockVideo;

    await act(async () => {
      await result.current.start(new Date(), new Date());
    });

    expect(result.current.status).not.toBe('idle');

    act(() => {
      result.current.stop();
    });

    expect(result.current.status).toBe('stopped');
  });

  it('should reset stats to initial values after stop', async () => {
    const { useHLS, INITIAL_STATS } = await import('../hooks/useHLS');
    const logs: string[] = [];
    const mockVideo = createMockVideoElement();

    const { result } = renderHook(() =>
      useHLS({
        streamName: 'test-stream',
        credentials: mockCredentials,
        region: 'ap-southeast-1',
        onLog: (msg) => logs.push(msg),
      }),
    );

    (result.current.videoRef as { current: HTMLVideoElement }).current = mockVideo;

    await act(async () => {
      await result.current.start(new Date(), new Date());
    });

    act(() => {
      result.current.stop();
    });

    expect(result.current.stats).toEqual(INITIAL_STATS);
  });

  it('should log stop message', async () => {
    const { useHLS } = await import('../hooks/useHLS');
    const logs: string[] = [];
    const mockVideo = createMockVideoElement();

    const { result } = renderHook(() =>
      useHLS({
        streamName: 'test-stream',
        credentials: mockCredentials,
        region: 'ap-southeast-1',
        onLog: (msg) => logs.push(msg),
      }),
    );

    (result.current.videoRef as { current: HTMLVideoElement }).current = mockVideo;

    await act(async () => {
      await result.current.start(new Date(), new Date());
    });

    act(() => {
      result.current.stop();
    });

    expect(logs.some((l) => l.includes('stopped'))).toBe(true);
  });

  it('should pause video and remove src on stop', async () => {
    const { useHLS } = await import('../hooks/useHLS');
    const logs: string[] = [];
    const mockVideo = createMockVideoElement();

    const { result } = renderHook(() =>
      useHLS({
        streamName: 'test-stream',
        credentials: mockCredentials,
        region: 'ap-southeast-1',
        onLog: (msg) => logs.push(msg),
      }),
    );

    (result.current.videoRef as { current: HTMLVideoElement }).current = mockVideo;

    await act(async () => {
      await result.current.start(new Date(), new Date());
    });

    act(() => {
      result.current.stop();
    });

    expect(mockVideo.pause).toHaveBeenCalled();
    expect(mockVideo.removeAttribute).toHaveBeenCalledWith('src');
    expect(mockVideo.load).toHaveBeenCalled();
  });
});
