import type { ReactNode } from 'react';

interface LayoutProps {
  children: ReactNode;
  onLogout?: () => void;
}

/**
 * Page layout: header bar + content area.
 *
 * Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 9.8
 */
export function Layout({ children, onLogout }: LayoutProps) {
  return (
    <div className="flex min-h-screen flex-col bg-surface">
      {/* Header */}
      <header className="sticky top-0 z-50 flex items-center justify-between border-b border-black/10 bg-white/70 px-6 py-3 backdrop-blur-xl">
        <h1 className="text-lg font-semibold text-brand-600">RaspiEye</h1>
        {onLogout && (
          <>
            {/* Desktop: text button */}
            <button
              onClick={onLogout}
              className="hidden rounded-xl px-3 py-1.5 text-sm text-gray-600 hover:bg-brand-50 hover:text-brand-700 sm:inline-flex"
            >
              登出
            </button>
            {/* Mobile: icon button */}
            <button
              onClick={onLogout}
              className="inline-flex items-center justify-center rounded-xl p-2 text-gray-600 hover:bg-brand-50 hover:text-brand-700 sm:hidden"
              aria-label="登出"
            >
              <svg xmlns="http://www.w3.org/2000/svg" className="h-5 w-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4" />
                <polyline points="16 17 21 12 16 7" />
                <line x1="21" y1="12" x2="9" y2="12" />
              </svg>
            </button>
          </>
        )}
      </header>

      {/* Content */}
      <main className="mx-auto w-full max-w-3xl flex-1 px-4 py-6">
        {children}
      </main>
    </div>
  );
}
