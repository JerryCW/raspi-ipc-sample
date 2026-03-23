import { useState, useEffect, useRef } from 'react';
import { useAuth } from '../auth/useAuth';
import { env } from '../config/env';
import { Layout } from '../components/Layout';
import { ConfigPanel } from '../components/ConfigPanel';
import { TabView } from '../components/TabView';
import { listFragments } from '../services/kvs';
import { getDayStartUTC8 } from '../components/Timeline';
import type { Fragment } from '../types';

/**
 * Main viewer page composing Layout + ConfigPanel + TabView.
 * Preloads today's fragments as soon as credentials are available.
 */
export function ViewerPage() {
  const { credentials, tokens, logout } = useAuth();

  const [region, setRegion] = useState(env.defaultRegion);
  const [channelName, setChannelName] = useState(env.defaultChannelName);
  const [streamName, setStreamName] = useState(env.defaultStreamName);
  const [preloadedFragments, setPreloadedFragments] = useState<Fragment[]>([]);

  // Preload today's fragments once credentials are available
  const preloadedRef = useRef(false);
  useEffect(() => {
    if (credentials && !preloadedRef.current) {
      preloadedRef.current = true;
      const dayStart = getDayStartUTC8(new Date());
      const dayEnd = new Date(dayStart.getTime() + 24 * 60 * 60 * 1000);
      listFragments(streamName, dayStart, dayEnd, credentials, region)
        .then((frags) => setPreloadedFragments(frags))
        .catch(() => {});
    }
  }, [credentials, streamName, region]);

  return (
    <Layout onLogout={logout}>
      <div className="flex flex-col gap-6">
        <ConfigPanel
          region={region}
          channelName={channelName}
          streamName={streamName}
          onRegionChange={setRegion}
          onChannelNameChange={setChannelName}
          onStreamNameChange={setStreamName}
        />
        <TabView
          channelName={channelName}
          streamName={streamName}
          credentials={credentials}
          region={region}
          preloadedFragments={preloadedFragments}
          idToken={tokens?.idToken ?? null}
        />
      </div>
    </Layout>
  );
}
