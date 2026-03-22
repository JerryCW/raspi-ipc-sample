import { useState, useCallback } from 'react';
import type { AWSCredentials, Fragment } from '../types';
import { WebRTCPanel } from './WebRTCPanel';
import { HLSPanel } from './HLSPanel';

type TabId = 'webrtc' | 'hls';

interface Tab {
  id: TabId;
  label: string;
}

const TABS: Tab[] = [
  { id: 'webrtc', label: '实时查看' },
  { id: 'hls', label: '录像回放' },
];

interface TabViewProps {
  channelName: string;
  streamName: string;
  credentials: AWSCredentials | null;
  region: string;
  preloadedFragments?: Fragment[];
}

/**
 * Tab container that switches between WebRTC live view and HLS playback.
 *
 * Both panels are always mounted — switching tabs only toggles visibility via CSS.
 * This preserves connection state (WebRTC/HLS keep playing in background).
 *
 * Validates: Requirements 5.1, 5.2, 5.3
 */
export function TabView({ channelName, streamName, credentials, region, preloadedFragments }: TabViewProps) {
  const [activeTab, setActiveTab] = useState<TabId>('webrtc');

  const handleTabChange = useCallback(
    (tab: TabId) => {
      if (tab === activeTab) return;
      setActiveTab(tab);
    },
    [activeTab],
  );

  return (
    <div className="flex flex-col gap-4">
      {/* Tab bar */}
      <div className="flex border-b border-gray-200">
        {TABS.map((tab) => (
          <button
            key={tab.id}
            onClick={() => handleTabChange(tab.id)}
            className={`px-4 py-2 text-sm font-medium transition-colors ${
              activeTab === tab.id
                ? 'border-b-2 border-blue-600 text-blue-600'
                : 'text-gray-500 hover:text-gray-700'
            }`}
          >
            {tab.label}
          </button>
        ))}
      </div>

      {/* Both panels always mounted, toggle visibility with CSS */}
      <div className={activeTab === 'webrtc' ? '' : 'hidden'}>
        <WebRTCPanel
          channelName={channelName}
          credentials={credentials}
          region={region}
        />
      </div>
      <div className={activeTab === 'hls' ? '' : 'hidden'}>
        <HLSPanel
          streamName={streamName}
          credentials={credentials}
          region={region}
          preloadedFragments={preloadedFragments}
        />
      </div>
    </div>
  );
}
