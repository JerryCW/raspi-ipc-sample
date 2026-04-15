# Stage 1: Build frontend
FROM public.ecr.aws/docker/library/node:20 AS build

WORKDIR /app

# Vite env vars — injected at build time via --build-arg
ARG VITE_COGNITO_USER_POOL_ID
ARG VITE_COGNITO_CLIENT_ID
ARG VITE_COGNITO_IDENTITY_POOL_ID
ARG VITE_COGNITO_DOMAIN
ARG VITE_COGNITO_REDIRECT_URI
ARG VITE_AWS_REGION=ap-southeast-1
ARG VITE_DEFAULT_CHANNEL_NAME=RaspiEyeAlphaChannel
ARG VITE_DEFAULT_STREAM_NAME=RaspiEyeAlphaStream

# ARG → ENV so Vite can read them during npm run build
ENV VITE_COGNITO_USER_POOL_ID=$VITE_COGNITO_USER_POOL_ID
ENV VITE_COGNITO_CLIENT_ID=$VITE_COGNITO_CLIENT_ID
ENV VITE_COGNITO_IDENTITY_POOL_ID=$VITE_COGNITO_IDENTITY_POOL_ID
ENV VITE_COGNITO_DOMAIN=$VITE_COGNITO_DOMAIN
ENV VITE_COGNITO_REDIRECT_URI=$VITE_COGNITO_REDIRECT_URI
ENV VITE_AWS_REGION=$VITE_AWS_REGION
ENV VITE_DEFAULT_CHANNEL_NAME=$VITE_DEFAULT_CHANNEL_NAME
ENV VITE_DEFAULT_STREAM_NAME=$VITE_DEFAULT_STREAM_NAME

COPY package*.json ./
RUN npm ci

COPY . .
RUN npm run build

# Stage 2: Production server
FROM public.ecr.aws/docker/library/node:20-alpine

WORKDIR /app

COPY package*.json ./
RUN npm ci --omit=dev

COPY server/ ./server/
COPY --from=build /app/dist ./dist/

ENV PORT=3000
ENV AWS_REGION=ap-southeast-1
ENV KVS_STREAM_NAME=RaspiEyeAlphaStream

EXPOSE 3000

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD wget --no-verbose --tries=1 --spider http://localhost:3000/health || exit 1

USER node

CMD ["node", "server/index.js"]
