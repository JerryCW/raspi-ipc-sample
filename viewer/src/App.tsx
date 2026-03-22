import { useState } from 'react';
import { Routes, Route, Navigate } from 'react-router-dom';
import { useAuth } from './auth/useAuth';
import { ViewerPage } from './pages/ViewerPage';

function LoginForm({
  onLogin,
  isLoading,
  error,
}: {
  onLogin: (username: string, password: string) => Promise<void>;
  isLoading: boolean;
  error: string | null;
}) {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!username || !password) return;
    await onLogin(username, password);
  };

  return (
    <div className="flex min-h-screen items-center justify-center bg-gradient-to-br from-gray-100 to-blue-50">
      <div className="w-full max-w-sm rounded-xl bg-white p-8 shadow-lg">
        <div className="mb-6 text-center">
          <h1 className="text-2xl font-bold text-gray-900">RaspiEye</h1>
          <p className="mt-1 text-sm text-gray-500">登录以查看摄像头视频</p>
        </div>

        <form onSubmit={handleSubmit} className="flex flex-col gap-4">
          <div>
            <label htmlFor="username" className="mb-1 block text-sm font-medium text-gray-700">
              用户名
            </label>
            <input
              id="username"
              type="text"
              value={username}
              onChange={(e) => setUsername(e.target.value)}
              placeholder="admin"
              autoComplete="username"
              required
              className="w-full rounded-lg border border-gray-300 px-3 py-2 text-sm focus:border-blue-500 focus:outline-none focus:ring-1 focus:ring-blue-500"
            />
          </div>

          <div>
            <label htmlFor="password" className="mb-1 block text-sm font-medium text-gray-700">
              密码
            </label>
            <input
              id="password"
              type="password"
              value={password}
              onChange={(e) => setPassword(e.target.value)}
              placeholder="••••••••"
              autoComplete="current-password"
              required
              className="w-full rounded-lg border border-gray-300 px-3 py-2 text-sm focus:border-blue-500 focus:outline-none focus:ring-1 focus:ring-blue-500"
            />
          </div>

          {error && (
            <p className="rounded-md bg-red-50 px-3 py-2 text-sm text-red-600">{error}</p>
          )}

          <button
            type="submit"
            disabled={isLoading || !username || !password}
            className="mt-2 rounded-lg bg-blue-600 px-4 py-2.5 text-sm font-medium text-white hover:bg-blue-700 disabled:cursor-not-allowed disabled:opacity-50"
          >
            {isLoading ? '登录中...' : '登录'}
          </button>
        </form>
      </div>
    </div>
  );
}

function AppContent() {
  const { isAuthenticated, isLoading, loginWithPassword, error } = useAuth();

  if (isLoading) {
    return (
      <div className="flex min-h-screen items-center justify-center bg-gray-100">
        <div className="text-center">
          <div className="mx-auto mb-4 h-8 w-8 animate-spin rounded-full border-4 border-blue-600 border-t-transparent" />
          <p className="text-sm text-gray-600">正在验证身份...</p>
        </div>
      </div>
    );
  }

  if (!isAuthenticated) {
    return <LoginForm onLogin={loginWithPassword} isLoading={isLoading} error={error} />;
  }

  return <ViewerPage />;
}

export default function App() {
  return (
    <Routes>
      <Route path="/" element={<AppContent />} />
      <Route path="*" element={<Navigate to="/" replace />} />
    </Routes>
  );
}
