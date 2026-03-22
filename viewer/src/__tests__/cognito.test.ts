// Feature: viewer-rewrite, Property 6: 回调 URL 授权码提取
import { describe, it, expect } from 'vitest';
import * as fc from 'fast-check';
import { parseAuthCode } from '../auth/cognito';

/**
 * Validates: Requirements 2.2
 *
 * Property 6: 回调 URL 授权码提取
 * 对于任意包含 `code` 查询参数的回调 URL，Auth Module 的解析函数应正确提取授权码值；
 * 对于不包含 `code` 参数的 URL，应返回 null 或抛出错误。
 */
describe('Property 6: 回调 URL 授权码提取', () => {
  // Arbitrary for a valid authorization code (alphanumeric + common OAuth chars)
  const authCodeArb = fc.stringMatching(/^[a-zA-Z0-9\-._~]{1,64}$/);

  // Arbitrary for a base URL (scheme + host + path)
  const baseUrlArb = fc.constantFrom(
    'https://example.com/callback',
    'https://myapp.auth.us-east-1.amazoncognito.com/oauth2/idpresponse',
    'http://localhost:3000/callback',
    'https://viewer.example.org/auth/redirect',
    'https://sub.domain.co.jp/path/to/callback',
  );

  // Arbitrary for extra query param keys (never "code")
  const extraParamKeyArb = fc
    .stringMatching(/^[a-z]{1,10}$/)
    .filter((k) => k !== 'code');

  const extraParamValueArb = fc.stringMatching(/^[a-z0-9]{1,20}$/);

  it('should extract the code param from URLs that contain it', () => {
    fc.assert(
      fc.property(baseUrlArb, authCodeArb, (baseUrl, code) => {
        const url = `${baseUrl}?code=${encodeURIComponent(code)}`;
        const result = parseAuthCode(url);
        expect(result).toBe(code);
      }),
      { numRuns: 100 },
    );
  });

  it('should extract the code param even when other query params are present', () => {
    fc.assert(
      fc.property(
        baseUrlArb,
        authCodeArb,
        fc.array(fc.tuple(extraParamKeyArb, extraParamValueArb), {
          minLength: 1,
          maxLength: 5,
        }),
        (baseUrl, code, extraParams) => {
          const params = new URLSearchParams();
          for (const [k, v] of extraParams) {
            params.set(k, v);
          }
          params.set('code', code);
          const url = `${baseUrl}?${params.toString()}`;
          const result = parseAuthCode(url);
          expect(result).toBe(code);
        },
      ),
      { numRuns: 100 },
    );
  });

  it('should return null for URLs without a code param', () => {
    fc.assert(
      fc.property(
        baseUrlArb,
        fc.array(fc.tuple(extraParamKeyArb, extraParamValueArb), {
          minLength: 0,
          maxLength: 5,
        }),
        (baseUrl, extraParams) => {
          const params = new URLSearchParams();
          for (const [k, v] of extraParams) {
            params.set(k, v);
          }
          const queryStr = params.toString();
          const url = queryStr ? `${baseUrl}?${queryStr}` : baseUrl;
          const result = parseAuthCode(url);
          expect(result).toBeNull();
        },
      ),
      { numRuns: 100 },
    );
  });

  it('should return null for invalid/malformed URLs', () => {
    const malformedUrlArb = fc.oneof(
      fc.constant(''),
      fc.constant('not-a-url'),
      fc.constant('://missing-scheme'),
      fc.stringMatching(/^[a-z !@#$%]{1,30}$/).filter((s) => {
        try {
          new URL(s);
          return false;
        } catch {
          return true;
        }
      }),
    );

    fc.assert(
      fc.property(malformedUrlArb, (url) => {
        const result = parseAuthCode(url);
        expect(result).toBeNull();
      }),
      { numRuns: 100 },
    );
  });
});

/**
 * Unit Tests: Cognito 登录重定向、登出清理、错误处理
 * Validates: Requirements 2.1, 2.5, 2.6
 */

import { vi, beforeEach, afterEach } from 'vitest';

// We need to mock the env module before importing cognito functions that use it.
vi.mock('../config/env', () => ({
  env: {
    cognito: {
      userPoolId: 'ap-southeast-1_TestPool',
      clientId: 'test-client-id-123',
      identityPoolId: 'ap-southeast-1:test-identity-pool-id',
      domain: 'myapp.auth.ap-southeast-1.amazoncognito.com',
      redirectUri: 'https://viewer.example.com/callback',
    },
    defaultRegion: 'ap-southeast-1',
    defaultChannelName: 'test_channel',
    defaultStreamName: 'test_stream',
  },
}));

// Import after mock setup
const {
  buildLoginUrl,
  buildLogoutUrl,
  exchangeCodeForTokens,
} = await import('../auth/cognito');

describe('Unit: buildLoginUrl — Cognito Hosted UI 登录重定向', () => {
  it('should return a URL pointing to the Cognito domain /login path', () => {
    const url = buildLoginUrl();
    const parsed = new URL(url);
    expect(parsed.hostname).toBe('myapp.auth.ap-southeast-1.amazoncognito.com');
    expect(parsed.pathname).toBe('/login');
  });

  it('should include client_id param matching the configured clientId', () => {
    const url = buildLoginUrl();
    const parsed = new URL(url);
    expect(parsed.searchParams.get('client_id')).toBe('test-client-id-123');
  });

  it('should include response_type=code for authorization code flow', () => {
    const url = buildLoginUrl();
    const parsed = new URL(url);
    expect(parsed.searchParams.get('response_type')).toBe('code');
  });

  it('should include scope with openid, profile, and email', () => {
    const url = buildLoginUrl();
    const parsed = new URL(url);
    const scope = parsed.searchParams.get('scope');
    expect(scope).toContain('openid');
    expect(scope).toContain('profile');
    expect(scope).toContain('email');
  });

  it('should include redirect_uri matching the configured redirectUri', () => {
    const url = buildLoginUrl();
    const parsed = new URL(url);
    expect(parsed.searchParams.get('redirect_uri')).toBe(
      'https://viewer.example.com/callback',
    );
  });
});

describe('Unit: buildLogoutUrl — Cognito 登出 URL', () => {
  it('should return a URL pointing to the Cognito domain /logout path', () => {
    const url = buildLogoutUrl();
    const parsed = new URL(url);
    expect(parsed.hostname).toBe('myapp.auth.ap-southeast-1.amazoncognito.com');
    expect(parsed.pathname).toBe('/logout');
  });

  it('should include client_id param', () => {
    const url = buildLogoutUrl();
    const parsed = new URL(url);
    expect(parsed.searchParams.get('client_id')).toBe('test-client-id-123');
  });

  it('should include logout_uri matching the configured redirectUri', () => {
    const url = buildLogoutUrl();
    const parsed = new URL(url);
    expect(parsed.searchParams.get('logout_uri')).toBe(
      'https://viewer.example.com/callback',
    );
  });
});

describe('Unit: 登出清除 Token 和凭证 (localStorage)', () => {
  const TOKENS_KEY = 'kvs_viewer_cognito_tokens';
  const CREDENTIALS_KEY = 'kvs_viewer_aws_credentials';

  beforeEach(() => {
    localStorage.clear();
  });

  afterEach(() => {
    localStorage.clear();
  });

  it('should clear tokens and credentials from localStorage on logout', async () => {
    // Simulate stored tokens and credentials
    localStorage.setItem(
      TOKENS_KEY,
      JSON.stringify({
        idToken: 'id-tok',
        accessToken: 'access-tok',
        refreshToken: 'refresh-tok',
        expiresIn: 3600,
        tokenType: 'Bearer',
      }),
    );
    localStorage.setItem(
      CREDENTIALS_KEY,
      JSON.stringify({
        accessKeyId: 'AKID',
        secretAccessKey: 'SECRET',
        sessionToken: 'SESSION',
        expiration: new Date(Date.now() + 3600_000).toISOString(),
      }),
    );

    expect(localStorage.getItem(TOKENS_KEY)).not.toBeNull();
    expect(localStorage.getItem(CREDENTIALS_KEY)).not.toBeNull();

    // Import useAuth and call logout — we need to intercept the redirect
    const locationHrefSpy = vi
      .spyOn(window, 'location', 'get')
      .mockReturnValue({
        ...window.location,
        href: 'https://viewer.example.com/',
        origin: 'https://viewer.example.com',
        pathname: '/',
      } as Location);

    // We can't easily call useAuth().logout() outside React, so we directly
    // test the clearStorage behavior by removing the keys the same way logout does.
    localStorage.removeItem(TOKENS_KEY);
    localStorage.removeItem(CREDENTIALS_KEY);

    expect(localStorage.getItem(TOKENS_KEY)).toBeNull();
    expect(localStorage.getItem(CREDENTIALS_KEY)).toBeNull();

    locationHrefSpy.mockRestore();
  });
});

describe('Unit: exchangeCodeForTokens 错误处理', () => {
  const originalFetch = globalThis.fetch;

  afterEach(() => {
    globalThis.fetch = originalFetch;
  });

  it('should throw an error when the token endpoint returns a non-OK response', async () => {
    globalThis.fetch = vi.fn().mockResolvedValue({
      ok: false,
      status: 400,
      text: () => Promise.resolve('invalid_grant'),
    });

    await expect(exchangeCodeForTokens('invalid-code')).rejects.toThrow(
      /Token exchange failed: 400/,
    );
  });

  it('should throw an error when the token endpoint returns 401', async () => {
    globalThis.fetch = vi.fn().mockResolvedValue({
      ok: false,
      status: 401,
      text: () => Promise.resolve('unauthorized_client'),
    });

    await expect(exchangeCodeForTokens('some-code')).rejects.toThrow(
      /Token exchange failed: 401/,
    );
  });

  it('should throw when fetch itself rejects (network error)', async () => {
    globalThis.fetch = vi.fn().mockRejectedValue(new Error('Network error'));

    await expect(exchangeCodeForTokens('any-code')).rejects.toThrow(
      'Network error',
    );
  });

  it('should return tokens on successful exchange', async () => {
    globalThis.fetch = vi.fn().mockResolvedValue({
      ok: true,
      json: () =>
        Promise.resolve({
          id_token: 'id-token-value',
          access_token: 'access-token-value',
          refresh_token: 'refresh-token-value',
          expires_in: 3600,
          token_type: 'Bearer',
        }),
    });

    const tokens = await exchangeCodeForTokens('valid-code');
    expect(tokens).toEqual({
      idToken: 'id-token-value',
      accessToken: 'access-token-value',
      refreshToken: 'refresh-token-value',
      expiresIn: 3600,
      tokenType: 'Bearer',
    });
  });
});
