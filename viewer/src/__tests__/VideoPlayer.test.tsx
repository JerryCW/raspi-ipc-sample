import { describe, it, expect } from 'vitest';
import { render, screen } from '@testing-library/react';
import { createRef } from 'react';
import { VideoPlayer } from '../components/VideoPlayer';

/**
 * Unit tests for VideoPlayer placeholder behavior.
 *
 * Validates: Requirements 5.9
 * - WHILE 视频流未开始播放, THE Viewer SHALL 在视频容器中显示占位符提示
 */

describe('Unit: VideoPlayer placeholder', () => {
  it('shows placeholder text when isPlaying is false', () => {
    const videoRef = createRef<HTMLVideoElement>();
    render(<VideoPlayer videoRef={videoRef} isPlaying={false} />);

    expect(screen.getByText('点击"开始"查看视频')).toBeDefined();
  });

  it('shows custom placeholder text when provided', () => {
    const videoRef = createRef<HTMLVideoElement>();
    render(
      <VideoPlayer
        videoRef={videoRef}
        isPlaying={false}
        placeholderText="自定义占位符"
      />,
    );

    expect(screen.getByText('自定义占位符')).toBeDefined();
  });

  it('hides placeholder when isPlaying is true', () => {
    const videoRef = createRef<HTMLVideoElement>();
    render(<VideoPlayer videoRef={videoRef} isPlaying={true} />);

    expect(screen.queryByText('点击"开始"查看视频')).toBeNull();
  });
});
