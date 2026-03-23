import { DynamoDBClient } from '@aws-sdk/client-dynamodb';
import { DynamoDBDocumentClient, QueryCommand } from '@aws-sdk/lib-dynamodb';
import { S3Client, GetObjectCommand } from '@aws-sdk/client-s3';
import { getSignedUrl } from '@aws-sdk/s3-request-presigner';
import jwt from 'jsonwebtoken';
import jwksClient from 'jwks-rsa';

// ---------------------------------------------------------------------------
// Configuration helpers
// ---------------------------------------------------------------------------

const region = process.env.COGNITO_REGION || 'ap-southeast-1';
const userPoolId = process.env.COGNITO_USER_POOL_ID || '';
const tableName = process.env.DYNAMODB_TABLE || 'smart-camera-events';
const s3Bucket = process.env.S3_BUCKET || 'smart-camera-captures';
const deviceId = process.env.DEVICE_ID || '';

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

  const startTimestamp = `${date}T00:00:00.000Z`;
  const endTimestamp = `${date}T23:59:59.999Z`;

  try {
    const result = await getDocClient().send(
      new QueryCommand({
        TableName: tableName,
        KeyConditionExpression:
          'device_id = :deviceId AND event_timestamp BETWEEN :start AND :end',
        ExpressionAttributeValues: {
          ':deviceId': deviceId,
          ':start': startTimestamp,
          ':end': endTimestamp,
        },
      })
    );

    const events = (result.Items || []).map((item) => ({
      sessionId: item.session_id,
      eventTimestamp: item.event_timestamp,
      detectedClass: item.detected_class,
      maxConfidence: item.max_confidence,
      durationSeconds: item.duration_seconds,
      kvsStartTimestamp: item.kvs_start_timestamp,
      kvsEndTimestamp: item.kvs_end_timestamp,
      detectionCount: item.detection_count,
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
  const { sessionId } = req.params;
  const type = req.query.type || 'start';

  if (type !== 'start' && type !== 'end') {
    return res.status(400).json({ error: 'Invalid type. Use start or end' });
  }

  try {
    // Query by session_id — we need a scan/filter since session_id is not a key
    // Use a GSI in production; for now filter on device_id partition
    const result = await getDocClient().send(
      new QueryCommand({
        TableName: tableName,
        KeyConditionExpression: 'device_id = :deviceId',
        FilterExpression: 'session_id = :sessionId',
        ExpressionAttributeValues: {
          ':deviceId': deviceId,
          ':sessionId': sessionId,
        },
      })
    );

    const items = result.Items || [];
    if (items.length === 0) {
      return res.status(404).json({ error: 'Event not found' });
    }

    const item = items[0];
    const s3Key = type === 'start' ? item.s3_start_jpeg_path : item.s3_end_jpeg_path;

    if (!s3Key) {
      return res.status(404).json({ error: 'Event not found' });
    }

    const expiresIn = 900; // 15 minutes in seconds

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
// Exports for testing and registration
// ---------------------------------------------------------------------------

export {
  verifyJwt,
  getEvents,
  getThumbnail,
  isValidDateFormat,
  isWithin90Days,
};
