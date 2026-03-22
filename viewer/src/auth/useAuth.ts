import { useState, useEffect, useCallback, useRef } from 'react';
import type { AWSCredentials, CognitoTokens } from '../types';
import {
  buildLoginUrl,
  buildLogoutUrl,
  exchangeCodeForTokens,
  refreshTokens,
  getAwsCredentials,
  parseAuthCode,
  signInWithPassword,
} from './cognito';

const TOKENS_STORAGE_KEY = 'kvs_viewer_cognito_tokens';
const CREDENTIALS_STORAGE_KEY = 'kvs_viewer_aws_credentials';

/** Minimum refresh lead time in ms (5 minutes). */
const MIN_REFRESH_LEAD_MS = 5 * 60 * 1000;

/**
 * Calculate how many ms before `expiration` we should trigger a refresh.
 * Uses the greater of 10% of remaining time or 5 minutes.
 */
export function computeRefreshDelay(expiration: Date, now: Date = new Date()): number {
  const remaining = expiration.getTime() - now.getTime();
  if (remaining <= 0) return 0;
  const lead = Math.max(remaining * 0.1, MIN_REFRESH_LEAD_MS);
  const delay = remaining - lead;
  return Math.max(delay, 0);
}

function saveTokens(tokens: CognitoTokens): void {
  localStorage.setItem(TOKENS_STORAGE_KEY, JSON.stringify(tokens));
}

function loadTokens(): CognitoTokens | null {
  try {
    const raw = localStorage.getItem(TOKENS_STORAGE_KEY);
    if (!raw) return null;
    return JSON.parse(raw) as CognitoTokens;
  } catch {
    return null;
  }
}

function saveCredentials(credentials: AWSCredentials): void {
  localStorage.setItem(
    CREDENTIALS_STORAGE_KEY,
    JSON.stringify({
      ...credentials,
      expiration: credentials.expiration.toISOString(),
    }),
  );
}

function loadCredentials(): AWSCredentials | null {
  try {
    const raw = localStorage.getItem(CREDENTIALS_STORAGE_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    return {
      ...parsed,
      expiration: new Date(parsed.expiration),
    } as AWSCredentials;
  } catch {
    return null;
  }
}

function clearStorage(): void {
  localStorage.removeItem(TOKENS_STORAGE_KEY);
  localStorage.removeItem(CREDENTIALS_STORAGE_KEY);
}

export function useAuth(): {
  isAuthenticated: boolean;
  isLoading: boolean;
  credentials: AWSCredentials | null;
  login: () => void;
  loginWithPassword: (username: string, password: string) => Promise<void>;
  logout: () => void;
  error: string | null;
} {
  const [isLoading, setIsLoading] = useState(true);
  const [credentials, setCredentials] = useState<AWSCredentials | null>(null);
  const [error, setError] = useState<string | null>(null);

  const refreshTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const tokensRef = useRef<CognitoTokens | null>(null);
  // Guard against double-initialization in StrictMode
  const initRef = useRef(false);

  const isAuthenticated = credentials !== null && credentials.expiration > new Date();

  /** Clear any pending refresh timer. */
  const clearRefreshTimer = useCallback(() => {
    if (refreshTimerRef.current !== null) {
      clearTimeout(refreshTimerRef.current);
      refreshTimerRef.current = null;
    }
  }, []);

  /**
   * Given valid tokens, obtain AWS credentials and schedule the next refresh.
   */
  const obtainCredentials = useCallback(
    async (currentTokens: CognitoTokens): Promise<AWSCredentials | null> => {
      try {
        const creds = await getAwsCredentials(currentTokens.idToken);
        setCredentials(creds);
        saveCredentials(creds);
        setError(null);
        return creds;
      } catch (err) {
        const message = err instanceof Error ? err.message : 'Failed to obtain AWS credentials';
        setError(message);
        return null;
      }
    },
    [],
  );

  /**
   * Refresh tokens using the refresh token, then obtain new AWS credentials.
   */
  const doRefresh = useCallback(
    async (currentTokens: CognitoTokens) => {
      try {
        const newTokens = await refreshTokens(currentTokens.refreshToken);
        tokensRef.current = newTokens;
        saveTokens(newTokens);
        const creds = await obtainCredentials(newTokens);
        if (creds) {
          scheduleRefresh(creds, newTokens);
        }
      } catch (err) {
        const message = err instanceof Error ? err.message : 'Token refresh failed';
        setError(message);
        // Refresh failed — clear state so user can re-login
        clearStorage();
        tokensRef.current = null;
        setCredentials(null);
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [],
  );

  /**
   * Schedule a credential refresh before the given credentials expire.
   */
  const scheduleRefresh = useCallback(
    (creds: AWSCredentials, currentTokens: CognitoTokens) => {
      clearRefreshTimer();
      const delay = computeRefreshDelay(creds.expiration);
      if (delay <= 0) {
        // Already expired or about to — refresh immediately
        void doRefresh(currentTokens);
        return;
      }
      refreshTimerRef.current = setTimeout(() => {
        void doRefresh(currentTokens);
      }, delay);
    },
    [clearRefreshTimer, doRefresh],
  );

  /**
   * Handle the full initialization flow:
   * 1. Check URL for auth code (Cognito callback)
   * 2. Otherwise check localStorage for existing tokens
   */
  useEffect(() => {
    if (initRef.current) return;
    initRef.current = true;

    const init = async () => {
      setIsLoading(true);
      try {
        // 1. Check for authorization code in URL
        const authCode = parseAuthCode(window.location.href);
        if (authCode) {
          // Clean the URL (remove ?code=...)
          window.history.replaceState({}, '', window.location.pathname);

          const newTokens = await exchangeCodeForTokens(authCode);
          tokensRef.current = newTokens;
          saveTokens(newTokens);

          const creds = await obtainCredentials(newTokens);
          if (creds) {
            scheduleRefresh(creds, newTokens);
          }
          return;
        }

        // 2. Check localStorage for existing tokens
        const storedTokens = loadTokens();
        if (!storedTokens) {
          // No tokens — user needs to log in
          return;
        }

        tokensRef.current = storedTokens;

        // Check if we have cached credentials that are still valid
        const storedCreds = loadCredentials();
        if (storedCreds && storedCreds.expiration > new Date()) {
          setCredentials(storedCreds);
          scheduleRefresh(storedCreds, storedTokens);
          return;
        }

        // Credentials expired or missing — try to get new ones
        const creds = await obtainCredentials(storedTokens);
        if (creds) {
          scheduleRefresh(creds, storedTokens);
        }
      } catch (err) {
        const message = err instanceof Error ? err.message : 'Authentication initialization failed';
        setError(message);
      } finally {
        setIsLoading(false);
      }
    };

    void init();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Cleanup refresh timer on unmount
  useEffect(() => {
    return () => {
      clearRefreshTimer();
    };
  }, [clearRefreshTimer]);

  const login = useCallback(() => {
    window.location.href = buildLoginUrl();
  }, []);

  const loginWithPassword = useCallback(async (username: string, password: string) => {
    setIsLoading(true);
    setError(null);
    try {
      const newTokens = await signInWithPassword(username, password);
      tokensRef.current = newTokens;
      saveTokens(newTokens);
      const creds = await obtainCredentials(newTokens);
      if (creds) {
        scheduleRefresh(creds, newTokens);
      }
    } catch (err) {
      const message = err instanceof Error ? err.message : 'Login failed';
      setError(message);
    } finally {
      setIsLoading(false);
    }
  }, [obtainCredentials, scheduleRefresh]);

  const logout = useCallback(() => {
    clearRefreshTimer();
    clearStorage();
    tokensRef.current = null;
    setCredentials(null);
    setError(null);
    window.location.href = buildLogoutUrl();
  }, [clearRefreshTimer]);

  return {
    isAuthenticated,
    isLoading,
    credentials,
    login,
    loginWithPassword,
    logout,
    error,
  };
}
