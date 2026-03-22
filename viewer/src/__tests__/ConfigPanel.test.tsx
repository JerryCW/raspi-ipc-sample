import { describe, it, expect, vi } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { ConfigPanel } from '../components/ConfigPanel';

/**
 * Unit tests for ConfigPanel component.
 *
 * Validates: Requirements 5.6, 5.7
 * - 5.6: Viewer displays Config_Panel with Region, WebRTC Channel Name, HLS Stream Name
 * - 5.7: Config_Panel allows editing Region, Channel Name, Stream Name values
 */

describe('Unit: ConfigPanel', () => {
  const defaultProps = {
    region: 'ap-southeast-1',
    channelName: 'test_channel',
    streamName: 'test_stream',
    onRegionChange: vi.fn(),
    onChannelNameChange: vi.fn(),
    onStreamNameChange: vi.fn(),
  };

  it('renders all 3 config fields: Region, Channel Name, Stream Name', () => {
    render(<ConfigPanel {...defaultProps} />);

    expect(screen.getByLabelText('Region')).toBeDefined();
    expect(screen.getByLabelText('WebRTC Channel Name')).toBeDefined();
    expect(screen.getByLabelText('HLS Stream Name')).toBeDefined();
  });

  it('displays current values in the input fields', () => {
    render(<ConfigPanel {...defaultProps} />);

    const regionInput = screen.getByLabelText('Region') as HTMLInputElement;
    const channelInput = screen.getByLabelText('WebRTC Channel Name') as HTMLInputElement;
    const streamInput = screen.getByLabelText('HLS Stream Name') as HTMLInputElement;

    expect(regionInput.value).toBe('ap-southeast-1');
    expect(channelInput.value).toBe('test_channel');
    expect(streamInput.value).toBe('test_stream');
  });

  it('calls onRegionChange when Region field is edited', () => {
    const onRegionChange = vi.fn();
    render(<ConfigPanel {...defaultProps} onRegionChange={onRegionChange} />);

    const regionInput = screen.getByLabelText('Region');
    fireEvent.change(regionInput, { target: { value: 'us-east-1' } });

    expect(onRegionChange).toHaveBeenCalledWith('us-east-1');
  });

  it('calls onChannelNameChange when Channel Name field is edited', () => {
    const onChannelNameChange = vi.fn();
    render(<ConfigPanel {...defaultProps} onChannelNameChange={onChannelNameChange} />);

    const channelInput = screen.getByLabelText('WebRTC Channel Name');
    fireEvent.change(channelInput, { target: { value: 'new_channel' } });

    expect(onChannelNameChange).toHaveBeenCalledWith('new_channel');
  });

  it('calls onStreamNameChange when Stream Name field is edited', () => {
    const onStreamNameChange = vi.fn();
    render(<ConfigPanel {...defaultProps} onStreamNameChange={onStreamNameChange} />);

    const streamInput = screen.getByLabelText('HLS Stream Name');
    fireEvent.change(streamInput, { target: { value: 'new_stream' } });

    expect(onStreamNameChange).toHaveBeenCalledWith('new_stream');
  });
});
