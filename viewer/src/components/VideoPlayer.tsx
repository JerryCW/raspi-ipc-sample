import type { RefObject } from 'react';

interface VideoPlayerProps {
  videoRef: RefObject<HTMLVideoElement>;
  isPlaying: boolean;
  placeholderText?: string;
  muted?: boolean;
}

/**
 * Video player component with 16:9 aspect ratio container.
 * Shows a placeholder when not playing.
 *
 * Validates: Requirements 5.8, 5.9
 */
export function VideoPlayer({
  videoRef,
  isPlaying,
  placeholderText = '点击"开始"查看视频',
  muted = true,
}: VideoPlayerProps) {
  return (
    <div
      className="relative w-full overflow-hidden rounded-lg bg-gray-900"
      style={{ aspectRatio: '16/9' }}
    >
      <video
        ref={videoRef}
        autoPlay
        playsInline
        muted={muted}
        controls
        className="absolute inset-0 h-full w-full object-contain"
      />
      {!isPlaying && (
        <div className="absolute inset-0 flex items-center justify-center text-gray-400">
          <div className="text-center">
            <svg
              className="mx-auto mb-2 h-12 w-12"
              fill="none"
              viewBox="0 0 24 24"
              stroke="currentColor"
            >
              <path
                strokeLinecap="round"
                strokeLinejoin="round"
                strokeWidth={1.5}
                d="M15.75 10.5l4.72-4.72a.75.75 0 011.28.53v11.38a.75.75 0 01-1.28.53l-4.72-4.72M4.5 18.75h9a2.25 2.25 0 002.25-2.25v-9A2.25 2.25 0 0013.5 5.25h-9A2.25 2.25 0 002.25 7.5v9A2.25 2.25 0 004.5 18.75z"
              />
            </svg>
            <p className="text-sm">{placeholderText}</p>
          </div>
        </div>
      )}
    </div>
  );
}
