// ===== 认证相关 =====

export interface CognitoTokens {
  idToken: string;
  accessToken: string;
  refreshToken: string;
  expiresIn: number;
  tokenType: string;
}

export interface AWSCredentials {
  accessKeyId: string;
  secretAccessKey: string;
  sessionToken: string;
  expiration: Date;
}

export interface AuthState {
  isAuthenticated: boolean;
  isLoading: boolean;
  tokens: CognitoTokens | null;
  credentials: AWSCredentials | null;
  error: string | null;
}

// ===== 连接状态 =====

export type ConnectionStatus =
  | 'idle'
  | 'connecting'
  | 'connected'
  | 'reconnecting'
  | 'error'
  | 'stopped';

// ===== WebRTC 相关 =====

export interface SignalingConfig {
  channelArn: string;
  wssEndpoint: string;
  httpsEndpoint: string;
}

export interface WebRTCStats {
  resolution: string;
  fps: number;
  bitrate: number;
  latency: number;
  packetLoss: number;
  codec: string;
  relayType: 'Direct' | 'STUN' | 'TURN' | null;
  duration: number;
}

// ===== HLS 相关 =====

export interface Fragment {
  fragmentNumber: string;
  serverTimestamp: Date;
  producerTimestamp: Date;
  fragmentLengthMillis: number;
}

export interface HLSStats {
  resolution: string;
  fps: number;
  bitrate: number;
  bufferLength: number;
  droppedFrames: number;
  currentTime: Date | null;
}

// ===== 配置 =====

export interface ViewerConfig {
  region: string;
  channelName: string;
  streamName: string;
}

export interface CognitoConfig {
  userPoolId: string;
  clientId: string;
  identityPoolId: string;
  domain: string;
  redirectUri: string;
}

// ===== 调试日志 =====

export interface LogEntry {
  timestamp: Date;
  message: string;
}

// ===== 环境变量配置 =====

export interface EnvConfig {
  cognito: CognitoConfig;
  defaultRegion: string;
  defaultChannelName: string;
  defaultStreamName: string;
}

// ===== 活动事件相关 =====

export interface ActivityEvent {
  eventId: string;
  startTime: string;          // ISO 8601 UTC
  endTime: string;            // ISO 8601 UTC
  durationSec: number;
  s3Prefix: string;
  thumbnailKey: string;
  snapshotCount: number;
  kvsStreamName: string;
  kvsRegion: string;
  yoloTopClass: string;
  yoloTopConfidence: number;
  verified: 'true' | 'false' | 'failed';
  // SageMaker 回填（verified=true 且识别出鸟时有值）
  species?: string;
  speciesCn?: string;
  family?: string;
  familyCn?: string;
  speciesConfidence?: number;
}
