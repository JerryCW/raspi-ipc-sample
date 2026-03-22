import type { ReactNode } from 'react';

interface LayoutProps {
  children: ReactNode;
  onLogout?: () => void;
}

/**
 * Page layout: header bar + content area.
 *
 * Validates: Requirements 5.1, 10.1
 */
export function Layout({ children, onLogout }: LayoutProps) {
  return (
    <div className="flex min-h-screen flex-col bg-gray-100">
      {/* Header */}
      <header className="flex items-center justify-between bg-white px-6 py-3 shadow-sm">
        <h1 className="text-lg font-semibold text-gray-900">KVS Camera Viewer</h1>
        {onLogout && (
          <button
            onClick={onLogout}
            className="rounded-md px-3 py-1.5 text-sm text-gray-600 hover:bg-gray-100 hover:text-gray-900"
          >
            登出
          </button>
        )}
      </header>

      {/* Content */}
      <main className="mx-auto w-full max-w-5xl flex-1 px-4 py-6">
        {children}
      </main>
    </div>
  );
}
