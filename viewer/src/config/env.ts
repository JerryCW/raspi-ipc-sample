import type { EnvConfig } from '../types';

export const env: EnvConfig = {
  cognito: {
    userPoolId: import.meta.env.VITE_COGNITO_USER_POOL_ID || '',
    clientId: import.meta.env.VITE_COGNITO_CLIENT_ID || '',
    identityPoolId: import.meta.env.VITE_COGNITO_IDENTITY_POOL_ID || '',
    domain: import.meta.env.VITE_COGNITO_DOMAIN || '',
    redirectUri: import.meta.env.VITE_COGNITO_REDIRECT_URI || window.location.origin,
  },
  defaultRegion: import.meta.env.VITE_AWS_REGION || 'ap-southeast-1',
  defaultChannelName: import.meta.env.VITE_DEFAULT_CHANNEL_NAME || 'RaspiEyeAlphaChannel',
  defaultStreamName: import.meta.env.VITE_DEFAULT_STREAM_NAME || 'RaspiEyeAlphaStream',
};
