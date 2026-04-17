import { DynamoDBClient } from '@aws-sdk/client-dynamodb';
import { DynamoDBDocumentClient, QueryCommand } from '@aws-sdk/lib-dynamodb';
import { S3Client, GetObjectCommand } from '@aws-sdk/client-s3';
import { getSignedUrl } from '@aws-sdk/s3-request-presigner';
import {
  KinesisVideoClient,
  GetDataEndpointCommand,
} from '@aws-sdk/client-kinesis-video';
import {
  KinesisVideoArchivedMediaClient,
  GetClipCommand,
  ClipFragmentSelectorType,
} from '@aws-sdk/client-kinesis-video-archived-media';
import jwt from 'jsonwebtoken';
import jwksClient from 'jwks-rsa';
import { spawn } from 'child_process';

// ---------------------------------------------------------------------------
// Configuration helpers
// ---------------------------------------------------------------------------

const region = process.env.COGNITO_REGION || 'ap-southeast-1';
const userPoolId = process.env.COGNITO_USER_POOL_ID || '';
const tableName = process.env.DYNAMODB_TABLE || 'raspi-eye-events';
const s3Bucket = process.env.S3_BUCKET || 'smart-camera-captures';
const deviceId = process.env.DEVICE_ID || 'RaspiEyeAlpha';

const MAX_CLIP_DURATION_SEC = 300;
const CLIP_BUFFER_SEC = 5;
const kvsStreamName = process.env.KVS_STREAM_NAME || '';

const cognitoIssuer = `https://cognito-idp.${region}.amazonaws.com/${userPoolId}`;
const jwksUri = `${cognitoIssuer}/.well-known/jwks.json`;

// ---------------------------------------------------------------------------
// AWS SDK clients (lazy — created once)
// ---------------------------------------------------------------------------

let _docClient;
function getDocClient() {
  if (!_docClient) {
    const ddb = new DynamoDBClient({ region });
    _docClient = DynamoDBDocumentClient.from(ddb);
  }
  return _docClient;
}

let _s3Client;
function getS3Client() {
  if (!_s3Client) {
    _s3Client = new S3Client({ region });
  }
  return _s3Client;
}

// ---------------------------------------------------------------------------
// JWKS key retrieval
// ---------------------------------------------------------------------------

let _jwks;
function getJwksClient() {
  if (!_jwks) {
    _jwks = jwksClient({
      jwksUri,
      cache: true,
      cacheMaxEntries: 5,
      cacheMaxAge: 600000, // 10 min
    });
  }
  return _jwks;
}

function getSigningKey(header) {
  return new Promise((resolve, reject) => {
    getJwksClient().getSigningKey(header.kid, (err, key) => {
      if (err) return reject(err);
      resolve(key.getPublicKey());
    });
  });
}

// ---------------------------------------------------------------------------
// JWT verification middleware
// ---------------------------------------------------------------------------

async function verifyJwt(req, res, next) {
  const authHeader = req.headers.authorization;
  if (!authHeader || !authHeader.startsWith('Bearer ')) {
    return res.status(401).json({ error: 'Missing authorization token' });
  }

  const token = authHeader.slice(7); // strip "Bearer "

  try {
    // Decode header to get kid
    const decoded = jwt.decode(token, { complete: true });
    if (!decoded || !decoded.header) {
      return res.status(401).json({ error: 'Invalid or expired token' });
    }

    const publicKey = await getSigningKey(decoded.header);

    const payload = jwt.verify(token, publicKey, {
      issuer: cognitoIssuer,
      algorithms: ['RS256'],
    });

    req.user = payload;
    next();
  } catch (_err) {
    return res.status(401).json({ error: 'Invalid or expired token' });
  }
}

// ---------------------------------------------------------------------------
// Date validation helpers
// ---------------------------------------------------------------------------

const DATE_REGEX = /^\d{4}-\d{2}-\d{2}$/;

function isValidDateFormat(dateStr) {
  if (!DATE_REGEX.test(dateStr)) return false;
  const d = new Date(`${dateStr}T00:00:00Z`);
  if (isNaN(d.getTime())) return false;
  // Ensure the parsed date matches the input (catches e.g. 2024-02-30)
  const [y, m, day] = dateStr.split('-').map(Number);
  return d.getUTCFullYear() === y && d.getUTCMonth() + 1 === m && d.getUTCDate() === day;
}

function isWithin90Days(dateStr) {
  const target = new Date(`${dateStr}T00:00:00Z`);
  const now = new Date();
  // Set "today" to start-of-day UTC for a clean comparison
  const todayStart = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate()));
  const diffMs = todayStart.getTime() - target.getTime();
  const diffDays = diffMs / (1000 * 60 * 60 * 24);
  return diffDays <= 90;
}

// ---------------------------------------------------------------------------
// GET /api/events?date=YYYY-MM-DD
// ---------------------------------------------------------------------------

async function getEvents(req, res) {
  const { date } = req.query;

  if (!date || !isValidDateFormat(date)) {
    return res.status(400).json({ error: 'Invalid date format. Use YYYY-MM-DD' });
  }

  if (!isWithin90Days(date)) {
    return res.status(400).json({ error: 'Date range exceeds 90 days' });
  }

  const startTimestamp = `${date}T00:00:00Z`;
  const endTimestamp = `${date}T23:59:59Z`;

  try {
    const result = await getDocClient().send(
      new QueryCommand({
        TableName: tableName,
        KeyConditionExpression:
          'device_id = :deviceId AND start_time BETWEEN :start AND :end',
        ExpressionAttributeValues: {
          ':deviceId': deviceId,
          ':start': startTimestamp,
          ':end': endTimestamp,
        },
      })
    );

    const events = (result.Items || []).map((item) => ({
      eventId: item.event_id,
      startTime: item.start_time,
      endTime: item.end_time,
      durationSec: item.duration_sec ?? 0,
      s3Prefix: item.s3_prefix,
      thumbnailKey: item.thumbnail_key,
      snapshotCount: item.snapshot_count ?? 0,
      kvsStreamName: item.kvs_stream_name,
      kvsRegion: item.kvs_region,
      yoloTopClass: item.yolo_top_class,
      yoloTopConfidence: item.yolo_top_confidence ?? 0,
      verified: item.verified ?? 'false',
      species: item.species,
      speciesCn: item.species_cn,
      family: item.family,
      familyCn: item.family_cn,
      speciesConfidence: item.species_confidence,
    }));

    return res.json({ events, count: events.length });
  } catch (_err) {
    console.error('DynamoDB query failed:', _err);
    return res.status(500).json({ error: 'Internal server error' });
  }
}

// ---------------------------------------------------------------------------
// GET /api/events/:sessionId/thumbnail?type=start|end
// ---------------------------------------------------------------------------

async function getThumbnail(req, res) {
  const { eventId } = req.params;

  if (!eventId) {
    return res.status(400).json({ error: 'Missing eventId' });
  }

  try {
    // Query by event_id — filter on device_id partition
    const result = await getDocClient().send(
      new QueryCommand({
        TableName: tableName,
        KeyConditionExpression: 'device_id = :deviceId',
        FilterExpression: 'event_id = :eventId',
        ExpressionAttributeValues: {
          ':deviceId': deviceId,
          ':eventId': eventId,
        },
      })
    );

    const items = result.Items || [];
    if (items.length === 0) {
      return res.status(404).json({ error: 'Event not found' });
    }

    const item = items[0];
    const s3Key = item.thumbnail_key;

    if (!s3Key) {
      return res.status(404).json({ error: 'No thumbnail available' });
    }

    const expiresIn = 900;

    const url = await getSignedUrl(
      getS3Client(),
      new GetObjectCommand({ Bucket: s3Bucket, Key: s3Key }),
      { expiresIn }
    );

    return res.json({ url, expiresIn });
  } catch (_err) {
    console.error('Thumbnail generation failed:', _err);
    return res.status(500).json({ error: 'Failed to generate thumbnail URL' });
  }
}

// ---------------------------------------------------------------------------
// Pure helpers for video clip export
// ---------------------------------------------------------------------------

function computeClipRange(kvsStart, kvsEnd) {
  const clipStart = new Date(kvsStart - CLIP_BUFFER_SEC * 1000);
  let clipEnd = new Date(kvsEnd + CLIP_BUFFER_SEC * 1000);
  if (clipEnd.getTime() - clipStart.getTime() > MAX_CLIP_DURATION_SEC * 1000) {
    clipEnd = new Date(clipStart.getTime() + MAX_CLIP_DURATION_SEC * 1000);
  }
  return { clipStart, clipEnd };
}

function generateClipFilename(detectedClass, kvsStartTimestamp) {
  const dateStr = new Date(kvsStartTimestamp)
    .toISOString()
    .replace(/[:.]/g, '-')
    .slice(0, 19);
  return `${detectedClass}_${dateStr}.mp4`;
}

// ---------------------------------------------------------------------------
// GET /api/events/:sessionId/clip
// ---------------------------------------------------------------------------

async function getVideoClip(req, res) {
  const { eventId } = req.params;

  if (!eventId || typeof eventId !== 'string' || !eventId.trim()) {
    return res.status(400).json({ error: 'Invalid eventId' });
  }

  try {
    const result = await getDocClient().send(
      new QueryCommand({
        TableName: tableName,
        KeyConditionExpression: 'device_id = :deviceId',
        FilterExpression: 'event_id = :eventId',
        ExpressionAttributeValues: {
          ':deviceId': deviceId,
          ':eventId': eventId,
        },
      })
    );

    const items = result.Items || [];
    if (items.length === 0) {
      return res.status(404).json({ error: 'Event not found' });
    }

    const item = items[0];
    const startTimeStr = item.start_time;
    const endTimeStr = item.end_time;
    const streamName = item.kvs_stream_name || kvsStreamName;

    if (!startTimeStr || !endTimeStr) {
      return res.status(404).json({ error: 'Event has no video timestamps' });
    }

    const kvsStart = new Date(startTimeStr).getTime();
    const kvsEnd = new Date(endTimeStr).getTime();
    const { clipStart, clipEnd } = computeClipRange(kvsStart, kvsEnd);

    const kvsClient = new KinesisVideoClient({ region });
    const endpointResp = await kvsClient.send(
      new GetDataEndpointCommand({
        StreamName: streamName,
        APIName: 'GET_CLIP',
      })
    );
    const dataEndpoint = endpointResp.DataEndpoint;
    if (!dataEndpoint) {
      return res.status(500).json({ error: 'Failed to get KVS data endpoint' });
    }

    const archivedClient = new KinesisVideoArchivedMediaClient({
      region,
      endpoint: dataEndpoint,
    });
    const clipResp = await archivedClient.send(
      new GetClipCommand({
        StreamName: streamName,
        ClipFragmentSelector: {
          FragmentSelectorType: ClipFragmentSelectorType.SERVER_TIMESTAMP,
          TimestampRange: {
            StartTimestamp: clipStart,
            EndTimestamp: clipEnd,
          },
        },
      })
    );

    if (!clipResp.Payload) {
      return res.status(404).json({ error: 'No video available for this time range' });
    }

    const filename = generateClipFilename(
      item.yolo_top_class || 'event',
      kvsStart
    );
    res.setHeader('Content-Type', 'video/mp4');
    res.setHeader('Content-Disposition', `attachment; filename="${filename}"`);

    // Remux MKV → MP4 via ffmpeg (no re-encoding, just container conversion)
    const ffmpeg = spawn('ffmpeg', [
      '-i', 'pipe:0',           // read MKV from stdin
      '-c', 'copy',             // no re-encoding
      '-movflags', 'frag_keyframe+empty_moov',  // fragmented MP4 for streaming
      '-f', 'mp4',
      'pipe:1',                 // write MP4 to stdout
    ]);

    const stream = clipResp.Payload;
    stream.pipe(ffmpeg.stdin);
    ffmpeg.stdout.pipe(res);

    ffmpeg.stderr.on('data', () => {
      // suppress ffmpeg stderr (progress info)
    });

    ffmpeg.on('close', (code) => {
      console.log(
        `Video clip exported: eventId=${eventId}, format=mp4, ffmpeg_exit=${code}, range=${clipStart.toISOString()}~${clipEnd.toISOString()}`
      );
    });

    stream.on('error', (err) => {
      console.error(`Stream error during clip export: eventId=${eventId}`, err);
      ffmpeg.kill();
      if (!res.headersSent) {
        res.status(500).json({ error: 'Stream error during video export' });
      }
    });

    ffmpeg.on('error', (err) => {
      console.error(`ffmpeg error: eventId=${eventId}`, err);
      if (!res.headersSent) {
        res.status(500).json({ error: 'Video conversion failed' });
      }
    });
  } catch (err) {
    console.error(`Video clip export failed: eventId=${eventId}`, err);
    return res.status(500).json({ error: 'Failed to export video clip' });
  }
}

// ---------------------------------------------------------------------------
// Exports for testing and registration
// ---------------------------------------------------------------------------

export {
  verifyJwt,
  getEvents,
  getThumbnail,
  getVideoClip,
  isValidDateFormat,
  isWithin90Days,
  computeClipRange,
  generateClipFilename,
};
