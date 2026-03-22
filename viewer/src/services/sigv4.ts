import type { AWSCredentials } from '../types';

/**
 * HMAC-SHA256 using Web Crypto API.
 * Returns raw ArrayBuffer suitable for chaining in signing key derivation.
 */
export async function hmacSha256(
  key: ArrayBuffer | Uint8Array,
  data: string
): Promise<ArrayBuffer> {
  const cryptoKey = await crypto.subtle.importKey(
    'raw',
    key,
    { name: 'HMAC', hash: 'SHA-256' },
    false,
    ['sign']
  );
  return crypto.subtle.sign('HMAC', cryptoKey, new TextEncoder().encode(data));
}

/**
 * SHA-256 hash returning a lowercase hex string.
 */
export async function sha256(data: string): Promise<string> {
  const hashBuffer = await crypto.subtle.digest(
    'SHA-256',
    new TextEncoder().encode(data)
  );
  return arrayBufferToHex(hashBuffer);
}

/**
 * Derive the SigV4 signing key via HMAC-SHA256 chain:
 *   AWS4 + secretKey → dateStamp → region → service → "aws4_request"
 */
export async function getSignatureKey(
  secretKey: string,
  dateStamp: string,
  region: string,
  service: string
): Promise<ArrayBuffer> {
  const kDate = await hmacSha256(
    new TextEncoder().encode(`AWS4${secretKey}`),
    dateStamp
  );
  const kRegion = await hmacSha256(kDate, region);
  const kService = await hmacSha256(kRegion, service);
  return hmacSha256(kService, 'aws4_request');
}

/**
 * Convert an ArrayBuffer to a lowercase hex string.
 */
function arrayBufferToHex(buffer: ArrayBuffer): string {
  return Array.from(new Uint8Array(buffer))
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
}

/**
 * Create a SigV4 presigned URL for KVS WebSocket endpoint.
 *
 * Ported from raspi-camera-viewer-origin/src/server.js `createPresignedUrl`,
 * using Web Crypto API instead of Node.js crypto.
 */
export async function createPresignedUrl(
  endpoint: string,
  channelArn: string,
  clientId: string,
  credentials: AWSCredentials,
  region: string
): Promise<string> {
  const url = new URL(endpoint);
  const host = url.host;
  const datetime = new Date().toISOString().replace(/[:-]|\.\d{3}/g, '');
  const date = datetime.slice(0, 8);

  return createPresignedUrlWithDatetime(
    endpoint,
    host,
    channelArn,
    clientId,
    credentials,
    region,
    datetime,
    date
  );
}

/**
 * Internal implementation with explicit datetime params for testability.
 * Exported for testing — allows deterministic timestamp injection.
 */
export async function createPresignedUrlWithDatetime(
  endpoint: string,
  host: string,
  channelArn: string,
  clientId: string,
  credentials: AWSCredentials,
  region: string,
  datetime: string,
  date: string
): Promise<string> {
  const service = 'kinesisvideo';

  // Build query params
  const queryParams: Record<string, string> = {
    'X-Amz-Algorithm': 'AWS4-HMAC-SHA256',
    'X-Amz-ChannelARN': channelArn,
    'X-Amz-ClientId': clientId,
    'X-Amz-Credential': `${credentials.accessKeyId}/${date}/${region}/${service}/aws4_request`,
    'X-Amz-Date': datetime,
    'X-Amz-Expires': '300',
    'X-Amz-SignedHeaders': 'host',
  };

  if (credentials.sessionToken) {
    queryParams['X-Amz-Security-Token'] = credentials.sessionToken;
  }

  // Sort alphabetically and build canonical query string
  const sortedKeys = Object.keys(queryParams).sort();
  const canonicalQueryString = sortedKeys
    .map((k) => `${encodeURIComponent(k)}=${encodeURIComponent(queryParams[k])}`)
    .join('&');

  // Canonical request
  const canonicalRequest = [
    'GET',
    '/',
    canonicalQueryString,
    `host:${host}`,
    '',
    'host',
    'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855', // SHA-256 of empty string
  ].join('\n');

  // String to sign
  const canonicalRequestHash = await sha256(canonicalRequest);
  const stringToSign = [
    'AWS4-HMAC-SHA256',
    datetime,
    `${date}/${region}/${service}/aws4_request`,
    canonicalRequestHash,
  ].join('\n');

  // Signing key and signature
  const signingKey = await getSignatureKey(
    credentials.secretAccessKey,
    date,
    region,
    service
  );
  const signatureBuffer = await hmacSha256(signingKey, stringToSign);
  const signature = arrayBufferToHex(signatureBuffer);

  return `${endpoint}?${canonicalQueryString}&X-Amz-Signature=${signature}`;
}
