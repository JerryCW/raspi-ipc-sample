import {
  CognitoIdentityClient,
  GetCredentialsForIdentityCommand,
  GetIdCommand,
} from '@aws-sdk/client-cognito-identity';
import { env } from '../config/env';
import type { AWSCredentials, CognitoTokens } from '../types';

const { cognito } = env;

/**
 * Build the Cognito Hosted UI login URL.
 * Redirects the user to Cognito for OAuth2 authorization code flow.
 */
export function buildLoginUrl(): string {
  const params = new URLSearchParams({
    client_id: cognito.clientId,
    response_type: 'code',
    scope: 'openid profile email',
    redirect_uri: cognito.redirectUri,
  });
  return `https://${cognito.domain}/login?${params.toString()}`;
}

/**
 * Build the Cognito Hosted UI logout URL.
 * Redirects the user to Cognito logout endpoint, then back to the app.
 */
export function buildLogoutUrl(): string {
  const params = new URLSearchParams({
    client_id: cognito.clientId,
    logout_uri: cognito.redirectUri,
  });
  return `https://${cognito.domain}/logout?${params.toString()}`;
}

/**
 * Parse the authorization code from a callback URL's query string.
 * Returns the code value, or null if not present.
 */
export function parseAuthCode(url: string): string | null {
  try {
    const parsed = new URL(url);
    return parsed.searchParams.get('code');
  } catch {
    return null;
  }
}

/**
 * Exchange an authorization code for Cognito tokens via the token endpoint.
 */
export async function exchangeCodeForTokens(
  code: string,
): Promise<CognitoTokens> {
  const tokenEndpoint = `https://${cognito.domain}/oauth2/token`;

  const body = new URLSearchParams({
    grant_type: 'authorization_code',
    client_id: cognito.clientId,
    code,
    redirect_uri: cognito.redirectUri,
  });

  const response = await fetch(tokenEndpoint, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: body.toString(),
  });

  if (!response.ok) {
    const errorText = await response.text();
    throw new Error(`Token exchange failed: ${response.status} ${errorText}`);
  }

  const data = await response.json();

  return {
    idToken: data.id_token,
    accessToken: data.access_token,
    refreshToken: data.refresh_token,
    expiresIn: data.expires_in,
    tokenType: data.token_type,
  };
}

/**
 * Refresh tokens using a refresh token via the Cognito token endpoint.
 */
export async function refreshTokens(
  refreshToken: string,
): Promise<CognitoTokens> {
  const tokenEndpoint = `https://${cognito.domain}/oauth2/token`;

  const body = new URLSearchParams({
    grant_type: 'refresh_token',
    client_id: cognito.clientId,
    refresh_token: refreshToken,
  });

  const response = await fetch(tokenEndpoint, {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: body.toString(),
  });

  if (!response.ok) {
    const errorText = await response.text();
    throw new Error(`Token refresh failed: ${response.status} ${errorText}`);
  }

  const data = await response.json();

  return {
    idToken: data.id_token,
    accessToken: data.access_token,
    refreshToken: data.refresh_token ?? refreshToken,
    expiresIn: data.expires_in,
    tokenType: data.token_type,
  };
}

/**
 * Exchange a Cognito ID token for AWS temporary credentials via Identity Pool.
 * Uses GetId + GetCredentialsForIdentity from the Cognito Identity service.
 */
export async function getAwsCredentials(
  idToken: string,
): Promise<AWSCredentials> {
  const region = env.cognito.userPoolId.split('_')[0];
  const client = new CognitoIdentityClient({ region });

  const providerName = `cognito-idp.${region}.amazonaws.com/${cognito.userPoolId}`;
  const logins = { [providerName]: idToken };

  // Step 1: Get identity ID
  const getIdResponse = await client.send(
    new GetIdCommand({
      IdentityPoolId: cognito.identityPoolId,
      Logins: logins,
    }),
  );

  if (!getIdResponse.IdentityId) {
    throw new Error('Failed to obtain Cognito identity ID');
  }

  // Step 2: Get credentials for the identity
  const credentialsResponse = await client.send(
    new GetCredentialsForIdentityCommand({
      IdentityId: getIdResponse.IdentityId,
      Logins: logins,
    }),
  );

  const creds = credentialsResponse.Credentials;
  if (!creds?.AccessKeyId || !creds.SecretKey || !creds.SessionToken) {
    throw new Error('Incomplete credentials returned from Cognito Identity');
  }

  return {
    accessKeyId: creds.AccessKeyId,
    secretAccessKey: creds.SecretKey,
    sessionToken: creds.SessionToken,
    expiration: creds.Expiration ?? new Date(Date.now() + 3600 * 1000),
  };
}

/**
 * Sign in directly with username/password using Cognito USER_PASSWORD_AUTH flow.
 * Returns tokens without redirecting to Hosted UI.
 */
export async function signInWithPassword(
  username: string,
  password: string,
): Promise<CognitoTokens> {
  const region = cognito.userPoolId.split('_')[0];
  const endpoint = `https://cognito-idp.${region}.amazonaws.com/`;

  const response = await fetch(endpoint, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/x-amz-json-1.1',
      'X-Amz-Target': 'AWSCognitoIdentityProviderService.InitiateAuth',
    },
    body: JSON.stringify({
      AuthFlow: 'USER_PASSWORD_AUTH',
      ClientId: cognito.clientId,
      AuthParameters: {
        USERNAME: username,
        PASSWORD: password,
      },
    }),
  });

  if (!response.ok) {
    const errorData = await response.json();
    throw new Error(errorData.message || `Authentication failed: ${response.status}`);
  }

  const data = await response.json();
  const result = data.AuthenticationResult;

  if (!result) {
    throw new Error('Authentication challenge required — not supported in this flow');
  }

  return {
    idToken: result.IdToken,
    accessToken: result.AccessToken,
    refreshToken: result.RefreshToken,
    expiresIn: result.ExpiresIn,
    tokenType: result.TokenType,
  };
}
