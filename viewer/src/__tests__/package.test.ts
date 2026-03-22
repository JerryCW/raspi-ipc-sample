import { describe, it, expect } from 'vitest';
import { readFileSync } from 'fs';
import { resolve } from 'path';

describe('package.json scripts', () => {
  const pkg = JSON.parse(
    readFileSync(resolve(__dirname, '../../package.json'), 'utf-8')
  );

  const requiredScripts = ['dev', 'build', 'preview', 'lint'] as const;

  for (const script of requiredScripts) {
    it(`defines the "${script}" script`, () => {
      expect(pkg.scripts).toHaveProperty(script);
      expect(typeof pkg.scripts[script]).toBe('string');
      expect(pkg.scripts[script].length).toBeGreaterThan(0);
    });
  }
});
