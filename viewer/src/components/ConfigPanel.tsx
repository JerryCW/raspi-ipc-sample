/**
 * Configuration panel for Region, WebRTC Channel Name, and HLS Stream Name.
 *
 * Validates: Requirements 5.6, 5.7
 */

interface ConfigPanelProps {
  region: string;
  channelName: string;
  streamName: string;
  onRegionChange: (value: string) => void;
  onChannelNameChange: (value: string) => void;
  onStreamNameChange: (value: string) => void;
}

export function ConfigPanel({
  region,
  channelName,
  streamName,
  onRegionChange,
  onChannelNameChange,
  onStreamNameChange,
}: ConfigPanelProps) {
  return (
    <div className="flex flex-wrap items-end gap-4 rounded-lg border border-gray-200 bg-white p-4">
      <div className="flex flex-col gap-1">
        <label htmlFor="cfg-region" className="text-xs font-medium text-gray-500">
          Region
        </label>
        <input
          id="cfg-region"
          type="text"
          value={region}
          onChange={(e) => onRegionChange(e.target.value)}
          className="rounded-md border border-gray-300 px-3 py-1.5 text-sm focus:border-blue-500 focus:outline-none focus:ring-1 focus:ring-blue-500"
        />
      </div>
      <div className="flex flex-col gap-1">
        <label htmlFor="cfg-channel" className="text-xs font-medium text-gray-500">
          WebRTC Channel Name
        </label>
        <input
          id="cfg-channel"
          type="text"
          value={channelName}
          onChange={(e) => onChannelNameChange(e.target.value)}
          className="rounded-md border border-gray-300 px-3 py-1.5 text-sm focus:border-blue-500 focus:outline-none focus:ring-1 focus:ring-blue-500"
        />
      </div>
      <div className="flex flex-col gap-1">
        <label htmlFor="cfg-stream" className="text-xs font-medium text-gray-500">
          HLS Stream Name
        </label>
        <input
          id="cfg-stream"
          type="text"
          value={streamName}
          onChange={(e) => onStreamNameChange(e.target.value)}
          className="rounded-md border border-gray-300 px-3 py-1.5 text-sm focus:border-blue-500 focus:outline-none focus:ring-1 focus:ring-blue-500"
        />
      </div>
    </div>
  );
}
