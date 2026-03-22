// Feature: viewer-rewrite, Property 7: 凭证刷新调度
import { describe, it, expect } from 'vitest';
import * as fc from 'fast-check';
import { computeRefreshDelay } from '../auth/useAuth';

/**
 * Validates: Requirements 2.4
 *
 * Property 7: 凭证刷新调度
 * 对于任意具有过期时间的 AWS 临时凭证，刷新定时器应在凭证过期时间之前触发
 * （提前量应为过期时长的一定比例，如提前 5 分钟或剩余时长的 10%）。
 */

const FIVE_MINUTES_MS = 5 * 60 * 1000;

// Arbitrary: positive remaining time in ms (1ms to 24 hours)
const remainingMsArb = fc.integer({ min: 1, max: 24 * 60 * 60 * 1000 });

// Helper to build now + expiration from a remaining time
function makeDates(remainingMs: number): { now: Date; expiration: Date } {
  const now = new Date(1_700_000_000_000); // fixed base
  const expiration = new Date(now.getTime() + remainingMs);
  return { now, expiration };
}

describe('Property 7: 凭证刷新调度', () => {
  it('delay is always less than remaining time (refresh happens before expiration)', () => {
    fc.assert(
      fc.property(remainingMsArb, (remainingMs) => {
        const { now, expiration } = makeDates(remainingMs);
        const delay = computeRefreshDelay(expiration, now);
        expect(delay).toBeLessThan(remainingMs);
      }),
      { numRuns: 100 },
    );
  });

  it('delay is always >= 0', () => {
    fc.assert(
      fc.property(remainingMsArb, (remainingMs) => {
        const { now, expiration } = makeDates(remainingMs);
        const delay = computeRefreshDelay(expiration, now);
        expect(delay).toBeGreaterThanOrEqual(0);
      }),
      { numRuns: 100 },
    );
  });

  it('when remaining > 50 min, lead is 10% of remaining (delay = 90% of remaining)', () => {
    const longRemainingArb = fc.integer({
      min: 50 * 60 * 1000 + 1,
      max: 24 * 60 * 60 * 1000,
    });

    fc.assert(
      fc.property(longRemainingArb, (remainingMs) => {
        const { now, expiration } = makeDates(remainingMs);
        const delay = computeRefreshDelay(expiration, now);
        // 10% of remaining > 5 min, so lead = 10% → delay = remaining - remaining*0.1
        const expectedDelay = remainingMs - remainingMs * 0.1;
        expect(delay).toBe(expectedDelay);
      }),
      { numRuns: 100 },
    );
  });

  it('when remaining <= 50 min, lead is 5 minutes (delay = remaining - 5min)', () => {
    // remaining * 0.1 <= 5min when remaining <= 50min
    const shortRemainingArb = fc.integer({
      min: FIVE_MINUTES_MS + 1, // must be > 5min so delay > 0
      max: 50 * 60 * 1000,
    });

    fc.assert(
      fc.property(shortRemainingArb, (remainingMs) => {
        const { now, expiration } = makeDates(remainingMs);
        const delay = computeRefreshDelay(expiration, now);
        expect(delay).toBe(remainingMs - FIVE_MINUTES_MS);
      }),
      { numRuns: 100 },
    );
  });

  it('when already expired (remaining <= 0), delay is 0', () => {
    const expiredOffsetArb = fc.integer({ min: 0, max: 24 * 60 * 60 * 1000 });

    fc.assert(
      fc.property(expiredOffsetArb, (offsetMs) => {
        const now = new Date(1_700_000_000_000);
        const expiration = new Date(now.getTime() - offsetMs); // in the past or exactly now
        const delay = computeRefreshDelay(expiration, now);
        expect(delay).toBe(0);
      }),
      { numRuns: 100 },
    );
  });
});
