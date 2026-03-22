import { describe, it, expect } from 'vitest';
import * as fc from 'fast-check';
import {
  hmacSha256,
  sha256,
  getSignatureKey,
  createPresignedUrlWithDatetime,
} from '../services/sigv4';
import type { AWSCredentials } from '../types';

describe('sigv4', () => {
  describe('sha256', () => {
    it('returns correct hash for empty string', async () => {
      const result = await sha256('');
      expect(result).toBe(
        'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
      );
    });

    it('returns correct hash for known input', async () => {
      const result = await sha256('hello');
      expect(result).toBe(
        '2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824'
      );
    });
  });

  describe('hmacSha256', () => {
    it('produces consistent output for same inputs', async () => {
      const key = new TextEncoder().encode('testkey');
      const r1 = await hmacSha256(key, 'data');
      const r2 = await hmacSha256(key, 'data');
      expect(new Uint8Array(r1)).toEqual(new Uint8Array(r2));
    });

    it('produces different output for different data', async () => {
      const key = new TextEncoder().encode('testkey');
      const r1 = await hmacSha256(key, 'data1');
      const r2 = await hmacSha256(key, 'data2');
      expect(new Uint8Array(r1)).not.toEqual(new Uint8Array(r2));
    });
  });

  describe('getSignatureKey', () => {
    it('produces a 32-byte signing key', async () => {
      const key = await getSignatureKey(
        'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY',
        '20250101',
        'us-east-1',
        'kinesisvideo'
      );
      expect(new Uint8Array(key).length).toBe(32);
    });

    it('is deterministic for same inputs', async () => {
      const args = ['secret', '20250101', 'ap-southeast-1', 'kinesisvideo'] as const;
      const k1 = await getSignatureKey(...args);
      const k2 = await getSignatureKey(...args);
      expect(new Uint8Array(k1)).toEqual(new Uint8Array(k2));
    });
  });

  describe('createPresignedUrlWithDatetime', () => {
    const credentials: AWSCredentials = {
      accessKeyId: 'AKIAIOSFODNN7EXAMPLE',
      secretAccessKey: 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY',
      sessionToken: 'FwoGZXIvYXdzEBYaDH7example',
      expiration: new Date('2025-12-31T23:59:59Z'),
    };

    const endpoint = 'wss://v-1234abcd.kinesisvideo.us-east-1.amazonaws.com';
    const host = 'v-1234abcd.kinesisvideo.us-east-1.amazonaws.com';
    const channelArn = 'arn:aws:kinesisvideo:us-east-1:123456789012:channel/test/1234567890';
    const clientId = 'ABCD1234';
    const region = 'us-east-1';
    const datetime = '20250101T120000Z';
    const date = '20250101';

    it('returns a URL starting with the endpoint', async () => {
      const url = await createPresignedUrlWithDatetime(
        endpoint, host, channelArn, clientId, credentials, region, datetime, date
      );
      expect(url.startsWith(endpoint + '?')).toBe(true);
    });

    it('includes all required SigV4 query parameters', async () => {
      const url = await createPresignedUrlWithDatetime(
        endpoint, host, channelArn, clientId, credentials, region, datetime, date
      );
      const params = new URL(url).searchParams;
      expect(params.get('X-Amz-Algorithm')).toBe('AWS4-HMAC-SHA256');
      expect(params.get('X-Amz-ChannelARN')).toBe(channelArn);
      expect(params.get('X-Amz-ClientId')).toBe(clientId);
      expect(params.get('X-Amz-Credential')).toContain(credentials.accessKeyId);
      expect(params.get('X-Amz-Date')).toBe(datetime);
      expect(params.get('X-Amz-Expires')).toBe('300');
      expect(params.get('X-Amz-SignedHeaders')).toBe('host');
      expect(params.get('X-Amz-Signature')).toBeTruthy();
      expect(params.get('X-Amz-Signature')!.length).toBe(64); // 256-bit hex
    });

    it('includes X-Amz-Security-Token when sessionToken is present', async () => {
      const url = await createPresignedUrlWithDatetime(
        endpoint, host, channelArn, clientId, credentials, region, datetime, date
      );
      const params = new URL(url).searchParams;
      expect(params.get('X-Amz-Security-Token')).toBe(credentials.sessionToken);
    });

    it('omits X-Amz-Security-Token when sessionToken is empty', async () => {
      const noTokenCreds: AWSCredentials = {
        ...credentials,
        sessionToken: '',
      };
      const url = await createPresignedUrlWithDatetime(
        endpoint, host, channelArn, clientId, noTokenCreds, region, datetime, date
      );
      const params = new URL(url).searchParams;
      expect(params.has('X-Amz-Security-Token')).toBe(false);
    });

    it('is deterministic — same inputs produce same output', async () => {
      const args = [endpoint, host, channelArn, clientId, credentials, region, datetime, date] as const;
      const url1 = await createPresignedUrlWithDatetime(...args);
      const url2 = await createPresignedUrlWithDatetime(...args);
      expect(url1).toBe(url2);
    });

    it('produces different signatures for different credentials', async () => {
      const otherCreds: AWSCredentials = {
        ...credentials,
        secretAccessKey: 'DifferentSecretKeyForTesting12345678',
      };
      const url1 = await createPresignedUrlWithDatetime(
        endpoint, host, channelArn, clientId, credentials, region, datetime, date
      );
      const url2 = await createPresignedUrlWithDatetime(
        endpoint, host, channelArn, clientId, otherCreds, region, datetime, date
      );
      const sig1 = new URL(url1).searchParams.get('X-Amz-Signature');
      const sig2 = new URL(url2).searchParams.get('X-Amz-Signature');
      expect(sig1).not.toBe(sig2);
    });

    it('credential scope contains correct date/region/service', async () => {
      const url = await createPresignedUrlWithDatetime(
        endpoint, host, channelArn, clientId, credentials, region, datetime, date
      );
      const params = new URL(url).searchParams;
      const cred = params.get('X-Amz-Credential')!;
      expect(cred).toBe(`${credentials.accessKeyId}/${date}/${region}/kinesisvideo/aws4_request`);
    });
  });

  // Feature: viewer-rewrite, Property 1: SigV4 签名确定性
  // **Validates: Requirements 3.2**
  describe('Property 1: SigV4 签名确定性', () => {
    // Arbitrary for non-empty alphanumeric strings (valid AWS key-like values)
    const arbAlphaNum = (minLen = 1, maxLen = 64) =>
      fc.stringMatching(new RegExp(`^[A-Za-z0-9]{${minLen},${maxLen}}$`));

    // Arbitrary for AWS credentials
    const arbCredentials = fc.record({
      accessKeyId: arbAlphaNum(16, 32),
      secretAccessKey: arbAlphaNum(16, 64),
      sessionToken: arbAlphaNum(16, 128),
      expiration: fc.date({ min: new Date('2025-01-01'), max: new Date('2030-12-31') }),
    });

    // Arbitrary for WSS endpoint URL
    const arbEndpoint = arbAlphaNum(4, 20).map(
      (id) => `wss://${id}.kinesisvideo.us-east-1.amazonaws.com`
    );

    // Arbitrary for channel ARN
    const arbChannelArn = fc.tuple(arbAlphaNum(4, 16), arbAlphaNum(4, 12)).map(
      ([name, id]) => `arn:aws:kinesisvideo:us-east-1:123456789012:channel/${name}/${id}`
    );

    // Arbitrary for clientId
    const arbClientId = arbAlphaNum(4, 32);

    // Arbitrary for region
    const arbRegion = fc.constantFrom('us-east-1', 'us-west-2', 'ap-southeast-1', 'eu-west-1', 'ap-northeast-1');

    // Arbitrary for SigV4 datetime (YYYYMMDDTHHMMSSZ format)
    const arbDatetime = fc.tuple(
      fc.integer({ min: 2020, max: 2030 }),
      fc.integer({ min: 1, max: 12 }),
      fc.integer({ min: 1, max: 28 }),
      fc.integer({ min: 0, max: 23 }),
      fc.integer({ min: 0, max: 59 }),
      fc.integer({ min: 0, max: 59 }),
    ).map(([y, m, d, h, mi, s]) => {
      const pad = (n: number, len = 2) => String(n).padStart(len, '0');
      const datetime = `${pad(y, 4)}${pad(m)}${pad(d)}T${pad(h)}${pad(mi)}${pad(s)}Z`;
      const date = `${pad(y, 4)}${pad(m)}${pad(d)}`;
      return { datetime, date };
    });

    it('calling createPresignedUrlWithDatetime twice with same inputs produces identical URLs', async () => {
      await fc.assert(
        fc.asyncProperty(
          arbEndpoint,
          arbChannelArn,
          arbClientId,
          arbCredentials,
          arbRegion,
          arbDatetime,
          async (endpoint, channelArn, clientId, credentials, region, { datetime, date }) => {
            const host = new URL(endpoint).host;

            const url1 = await createPresignedUrlWithDatetime(
              endpoint, host, channelArn, clientId, credentials, region, datetime, date
            );
            const url2 = await createPresignedUrlWithDatetime(
              endpoint, host, channelArn, clientId, credentials, region, datetime, date
            );

            expect(url1).toBe(url2);
          }
        ),
        { numRuns: 100 }
      );
    });
  });

  // Feature: viewer-rewrite, Property 2: SigV4 签名 URL 结构完整性
  // **Validates: Requirements 3.2**
  describe('Property 2: SigV4 签名 URL 结构完整性', () => {
    const arbAlphaNum = (minLen = 1, maxLen = 64) =>
      fc.stringMatching(new RegExp(`^[A-Za-z0-9]{${minLen},${maxLen}}$`));

    const arbEndpoint = arbAlphaNum(4, 20).map(
      (id) => `wss://${id}.kinesisvideo.us-east-1.amazonaws.com`
    );

    const arbChannelArn = fc.tuple(arbAlphaNum(4, 16), arbAlphaNum(4, 12)).map(
      ([name, id]) => `arn:aws:kinesisvideo:us-east-1:123456789012:channel/${name}/${id}`
    );

    const arbClientId = arbAlphaNum(4, 32);

    const arbRegion = fc.constantFrom('us-east-1', 'us-west-2', 'ap-southeast-1', 'eu-west-1', 'ap-northeast-1');

    const arbDatetime = fc.tuple(
      fc.integer({ min: 2020, max: 2030 }),
      fc.integer({ min: 1, max: 12 }),
      fc.integer({ min: 1, max: 28 }),
      fc.integer({ min: 0, max: 23 }),
      fc.integer({ min: 0, max: 59 }),
      fc.integer({ min: 0, max: 59 }),
    ).map(([y, m, d, h, mi, s]) => {
      const pad = (n: number, len = 2) => String(n).padStart(len, '0');
      const datetime = `${pad(y, 4)}${pad(m)}${pad(d)}T${pad(h)}${pad(mi)}${pad(s)}Z`;
      const date = `${pad(y, 4)}${pad(m)}${pad(d)}`;
      return { datetime, date };
    });

    // Credentials with non-empty sessionToken
    const arbCredentialsWithToken = fc.record({
      accessKeyId: arbAlphaNum(16, 32),
      secretAccessKey: arbAlphaNum(16, 64),
      sessionToken: arbAlphaNum(16, 128),
      expiration: fc.date({ min: new Date('2025-01-01'), max: new Date('2030-12-31') }),
    });

    // Credentials with empty sessionToken
    const arbCredentialsWithoutToken = fc.record({
      accessKeyId: arbAlphaNum(16, 32),
      secretAccessKey: arbAlphaNum(16, 64),
      sessionToken: fc.constant(''),
      expiration: fc.date({ min: new Date('2025-01-01'), max: new Date('2030-12-31') }),
    });

    const requiredParams = [
      'X-Amz-Algorithm',
      'X-Amz-Credential',
      'X-Amz-Date',
      'X-Amz-Expires',
      'X-Amz-SignedHeaders',
      'X-Amz-Signature',
    ];

    it('presigned URL contains all required SigV4 query parameters for any valid inputs', async () => {
      await fc.assert(
        fc.asyncProperty(
          arbEndpoint,
          arbChannelArn,
          arbClientId,
          arbCredentialsWithToken,
          arbRegion,
          arbDatetime,
          async (endpoint, channelArn, clientId, credentials, region, { datetime, date }) => {
            const host = new URL(endpoint).host;
            const url = await createPresignedUrlWithDatetime(
              endpoint, host, channelArn, clientId, credentials, region, datetime, date
            );
            const params = new URL(url).searchParams;

            for (const param of requiredParams) {
              expect(params.has(param), `missing required param: ${param}`).toBe(true);
              expect(params.get(param), `empty required param: ${param}`).toBeTruthy();
            }

            // Signature must be 64-char hex (256-bit HMAC-SHA256)
            expect(params.get('X-Amz-Signature')!).toMatch(/^[a-f0-9]{64}$/);
          }
        ),
        { numRuns: 100 }
      );
    });

    it('presigned URL includes X-Amz-Security-Token when sessionToken is non-empty', async () => {
      await fc.assert(
        fc.asyncProperty(
          arbEndpoint,
          arbChannelArn,
          arbClientId,
          arbCredentialsWithToken,
          arbRegion,
          arbDatetime,
          async (endpoint, channelArn, clientId, credentials, region, { datetime, date }) => {
            const host = new URL(endpoint).host;
            const url = await createPresignedUrlWithDatetime(
              endpoint, host, channelArn, clientId, credentials, region, datetime, date
            );
            const params = new URL(url).searchParams;

            expect(params.has('X-Amz-Security-Token')).toBe(true);
            expect(params.get('X-Amz-Security-Token')).toBe(credentials.sessionToken);
          }
        ),
        { numRuns: 100 }
      );
    });

    it('presigned URL omits X-Amz-Security-Token when sessionToken is empty', async () => {
      await fc.assert(
        fc.asyncProperty(
          arbEndpoint,
          arbChannelArn,
          arbClientId,
          arbCredentialsWithoutToken,
          arbRegion,
          arbDatetime,
          async (endpoint, channelArn, clientId, credentials, region, { datetime, date }) => {
            const host = new URL(endpoint).host;
            const url = await createPresignedUrlWithDatetime(
              endpoint, host, channelArn, clientId, credentials, region, datetime, date
            );
            const params = new URL(url).searchParams;

            expect(params.has('X-Amz-Security-Token')).toBe(false);
          }
        ),
        { numRuns: 100 }
      );
    });
  });
});
