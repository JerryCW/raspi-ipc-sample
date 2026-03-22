import { describe, it, expect } from 'vitest';
import * as fc from 'fast-check';
import { formatTimestamp } from '../hooks/useDebugLog';

/**
 * Feature: viewer-rewrite, Property 14: 日志时间戳格式
 *
 * For any Date object, the log formatting function's timestamp output
 * should match the HH:MM:SS format (regex: ^\d{2}:\d{2}:\d{2}$),
 * with hours in 00-23, minutes in 00-59, and seconds in 00-59.
 *
 * **Validates: Requirements 7.1**
 */

const HH_MM_SS_REGEX = /^\d{2}:\d{2}:\d{2}$/;

// Arbitrary for any valid Date
const arbDate = fc.date({
  min: new Date('1970-01-01T00:00:00Z'),
  max: new Date('2099-12-31T23:59:59Z'),
});

describe('Property 14: 日志时间戳格式', () => {
  it('formatTimestamp output matches HH:MM:SS format for any Date', () => {
    fc.assert(
      fc.property(arbDate, (date) => {
        const result = formatTimestamp(date);
        expect(result).toMatch(HH_MM_SS_REGEX);
      }),
      { numRuns: 100 },
    );
  });

  it('hours are in 00-23, minutes in 00-59, seconds in 00-59', () => {
    fc.assert(
      fc.property(arbDate, (date) => {
        const result = formatTimestamp(date);
        const [hh, mm, ss] = result.split(':').map(Number);

        expect(hh).toBeGreaterThanOrEqual(0);
        expect(hh).toBeLessThanOrEqual(23);
        expect(mm).toBeGreaterThanOrEqual(0);
        expect(mm).toBeLessThanOrEqual(59);
        expect(ss).toBeGreaterThanOrEqual(0);
        expect(ss).toBeLessThanOrEqual(59);
      }),
      { numRuns: 100 },
    );
  });

  it('is deterministic — same Date always produces the same timestamp', () => {
    fc.assert(
      fc.property(arbDate, (date) => {
        const result1 = formatTimestamp(date);
        const result2 = formatTimestamp(date);
        expect(result1).toBe(result2);
      }),
      { numRuns: 100 },
    );
  });

  it('correctly reflects the Date components (hours, minutes, seconds)', () => {
    fc.assert(
      fc.property(arbDate, (date) => {
        const result = formatTimestamp(date);
        const [hh, mm, ss] = result.split(':').map(Number);

        expect(hh).toBe(date.getHours());
        expect(mm).toBe(date.getMinutes());
        expect(ss).toBe(date.getSeconds());
      }),
      { numRuns: 100 },
    );
  });
});
