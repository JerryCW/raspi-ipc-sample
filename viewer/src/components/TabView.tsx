import { useState, useCallback } from 'react';
import type { AWSCredentials, Fragment } from '../types';
import { WebRTCPanel } from './WebRTCPanel';
import { HLSPanel } from './HLSPanel';
import { EventsPanel } from './EventsPanel';

type TabId = 'webrtc' | 'hls' | 'events';

interface Tab {
  id: TabId;
  label: string;
}

const TABS: Tab[] = [
  { id: 'webrtc', label: '实时查看' },
  { id: 'hls', label: '录像回放' },
  { id: 'events', label: '活动事件' },
];

interface TabViewProps {
  channelName: string;
  streamName: string;
  credentials: AWSCredentials | null;
  region: string;
  preloadedFragments?: Fragment[];
  idToken: string | null;
}

/**
 * Tab container that switches between WebRTC live view, HLS playback,
 * and activity events.
 *
 * Both WebRTC and HLS panels are always mounted — switching tabs only
 * toggles visibility via CSS. EventsPanel is also always mounted.
 *
 * Validates: Requirements 5.1, 5.2, 5.3, 5.4
 */
export function TabView({
  channelName,
  streamName,
  credentials,
  region,
  preloadedFragments,
  idToken,
}: TabViewProps) {
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
      {/* Segmented Control tab bar */}
      <div className="bg-gray-100 rounded-full p-1 inline-flex w-full sm:w-auto">
        {TABS.map((tab) => (
          <button
            key={tab.id}
            onClick={() => handleTabChange(tab.id)}
            className={`px-4 py-1.5 text-sm font-medium rounded-full transition-all duration-200 flex-1 sm:flex-none ${
              activeTab === tab.id
                ? 'bg-brand-500 text-white shadow-sm'
                : 'text-gray-600 hover:text-gray-900'
            }`}
          >
            {tab.label}
          </button>
        ))}
      </div>

      {/* All panels always mounted, toggle visibility with CSS */}
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
      <div className={activeTab === 'events' ? '' : 'hidden'}>
        <EventsPanel
          idToken={idToken}
          streamName={streamName}
          credentials={credentials}
          region={region}
        />
      </div>
    </div>
  );
}
