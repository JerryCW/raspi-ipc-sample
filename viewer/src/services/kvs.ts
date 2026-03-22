import {
  KinesisVideoClient,
  DescribeSignalingChannelCommand,
  GetSignalingChannelEndpointCommand,
  GetDataEndpointCommand,
} from '@aws-sdk/client-kinesis-video';
import { KinesisVideoSignalingClient, GetIceServerConfigCommand } from '@aws-sdk/client-kinesis-video-signaling';
import {
  KinesisVideoArchivedMediaClient,
  ListFragmentsCommand,
  FragmentSelectorType,
  GetHLSStreamingSessionURLCommand,
} from '@aws-sdk/client-kinesis-video-archived-media';
import type { AWSCredentials, SignalingConfig, Fragment } from '../types';

/**
 * Convert our AWSCredentials to the format expected by AWS SDK v3 clients.
 */
function toSdkCredentials(credentials: AWSCredentials) {
  return {
    accessKeyId: credentials.accessKeyId,
    secretAccessKey: credentials.secretAccessKey,
    sessionToken: credentials.sessionToken,
  };
}

/**
 * Get signaling channel configuration (ARN + WSS/HTTPS endpoints).
 *
 * Calls DescribeSignalingChannel to get the channel ARN, then
 * GetSignalingChannelEndpoint with Protocols ['WSS', 'HTTPS'] and Role 'VIEWER'.
 *
 * Requirements: 3.1
 */
export async function getSignalingChannelConfig(
  channelName: string,
  credentials: AWSCredentials,
  region: string
): Promise<SignalingConfig> {
  const client = new KinesisVideoClient({
    region,
    credentials: toSdkCredentials(credentials),
  });

  // Step 1: Get channel ARN
  const describeResp = await client.send(
    new DescribeSignalingChannelCommand({ ChannelName: channelName })
  );
  const channelArn = describeResp.ChannelInfo?.ChannelARN;
  if (!channelArn) {
    throw new Error(`Signaling channel "${channelName}" not found`);
  }

  // Step 2: Get WSS and HTTPS endpoints for VIEWER role
  const endpointResp = await client.send(
    new GetSignalingChannelEndpointCommand({
      ChannelARN: channelArn,
      SingleMasterChannelEndpointConfiguration: {
        Protocols: ['WSS', 'HTTPS'],
        Role: 'VIEWER',
      },
    })
  );

  const endpoints = endpointResp.ResourceEndpointList ?? [];
  const endpointsByProtocol: Record<string, string> = {};
  for (const ep of endpoints) {
    if (ep.Protocol && ep.ResourceEndpoint) {
      endpointsByProtocol[ep.Protocol] = ep.ResourceEndpoint;
    }
  }

  const wssEndpoint = endpointsByProtocol['WSS'];
  const httpsEndpoint = endpointsByProtocol['HTTPS'];
  if (!wssEndpoint || !httpsEndpoint) {
    throw new Error('Failed to get WSS and HTTPS endpoints for signaling channel');
  }

  return { channelArn, wssEndpoint, httpsEndpoint };
}

/**
 * Get ICE server configuration (STUN + TURN servers).
 *
 * Uses KinesisVideoSignalingClient pointed at the HTTPS endpoint,
 * calls GetIceServerConfig with Service 'TURN', and prepends the
 * regional STUN server.
 *
 * Requirements: 3.5
 */
export async function getIceServerConfig(
  channelArn: string,
  httpsEndpoint: string,
  credentials: AWSCredentials,
  region: string
): Promise<RTCIceServer[]> {
  const signalingClient = new KinesisVideoSignalingClient({
    region,
    endpoint: httpsEndpoint,
    credentials: toSdkCredentials(credentials),
  });

  const iceResp = await signalingClient.send(
    new GetIceServerConfigCommand({
      ChannelARN: channelArn,
      Service: 'TURN',
    })
  );

  const iceServers: RTCIceServer[] = [
    // Regional STUN server
    { urls: `stun:stun.kinesisvideo.${region}.amazonaws.com:443` },
    // TURN servers from KVS
    ...(iceResp.IceServerList ?? []).map((s) => ({
      urls: s.Uris ?? [],
      username: s.Username ?? '',
      credential: s.Password ?? '',
    })),
  ];

  return iceServers;
}

/**
 * List available video fragments within a time range.
 *
 * Uses KinesisVideoArchivedMediaClient to call ListFragments with
 * FragmentSelector (SERVER_TIMESTAMP, TimestampRange).
 *
 * Requirements: 4.2
 */
export async function listFragments(
  streamName: string,
  startTime: Date,
  endTime: Date,
  credentials: AWSCredentials,
  region: string
): Promise<Fragment[]> {
  // First, get the data endpoint for archived media
  const kvsClient = new KinesisVideoClient({
    region,
    credentials: toSdkCredentials(credentials),
  });

  const dataEndpointResp = await kvsClient.send(
    new GetDataEndpointCommand({
      StreamName: streamName,
      APIName: 'LIST_FRAGMENTS',
    })
  );
  const dataEndpoint = dataEndpointResp.DataEndpoint;
  if (!dataEndpoint) {
    throw new Error(`Failed to get data endpoint for stream "${streamName}"`);
  }

  const archivedMediaClient = new KinesisVideoArchivedMediaClient({
    region,
    endpoint: dataEndpoint,
    credentials: toSdkCredentials(credentials),
  });

  // Paginate through all fragments
  const allFragments: Fragment[] = [];
  let nextToken: string | undefined;

  do {
    const listResp = await archivedMediaClient.send(
      new ListFragmentsCommand({
        StreamName: streamName,
        FragmentSelector: {
          FragmentSelectorType: FragmentSelectorType.SERVER_TIMESTAMP,
          TimestampRange: {
            StartTimestamp: startTime,
            EndTimestamp: endTime,
          },
        },
        MaxResults: 1000,
        NextToken: nextToken,
      })
    );

    for (const f of listResp.Fragments ?? []) {
      allFragments.push({
        fragmentNumber: f.FragmentNumber ?? '',
        serverTimestamp: f.ServerTimestamp ? new Date(f.ServerTimestamp) : new Date(0),
        producerTimestamp: f.ProducerTimestamp ? new Date(f.ProducerTimestamp) : new Date(0),
        fragmentLengthMillis: f.FragmentLengthInMilliseconds ?? 0,
      });
    }

    nextToken = listResp.NextToken;
  } while (nextToken);

  return allFragments;
}

/**
 * Get an HLS streaming session URL for on-demand playback.
 *
 * Calls GetDataEndpoint to get the archived media endpoint, then
 * GetHLSStreamingSessionURL with PlaybackMode ON_DEMAND and
 * ContainerFormat FRAGMENTED_MP4.
 *
 * Requirements: 4.3
 */
export async function getHlsStreamingUrl(
  streamName: string,
  startTime: Date,
  endTime: Date,
  credentials: AWSCredentials,
  region: string
): Promise<string> {
  // Step 1: Get data endpoint for HLS
  const kvsClient = new KinesisVideoClient({
    region,
    credentials: toSdkCredentials(credentials),
  });

  const dataEndpointResp = await kvsClient.send(
    new GetDataEndpointCommand({
      StreamName: streamName,
      APIName: 'GET_HLS_STREAMING_SESSION_URL',
    })
  );
  const dataEndpoint = dataEndpointResp.DataEndpoint;
  if (!dataEndpoint) {
    throw new Error(`Failed to get HLS data endpoint for stream "${streamName}"`);
  }

  // Step 2: Get HLS streaming session URL (ON_DEMAND mode)
  const archivedMediaClient = new KinesisVideoArchivedMediaClient({
    region,
    endpoint: dataEndpoint,
    credentials: toSdkCredentials(credentials),
  });

  const hlsResp = await archivedMediaClient.send(
    new GetHLSStreamingSessionURLCommand({
      StreamName: streamName,
      PlaybackMode: 'ON_DEMAND',
      HLSFragmentSelector: {
        FragmentSelectorType: 'SERVER_TIMESTAMP',
        TimestampRange: {
          StartTimestamp: startTime,
          EndTimestamp: endTime,
        },
      },
      ContainerFormat: 'FRAGMENTED_MP4',
      DiscontinuityMode: 'ALWAYS',
      DisplayFragmentTimestamp: 'ALWAYS',
      Expires: 300,
    })
  );

  const hlsUrl = hlsResp.HLSStreamingSessionURL;
  if (!hlsUrl) {
    throw new Error('Failed to get HLS streaming session URL');
  }

  return hlsUrl;
}
