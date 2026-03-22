import { describe, it, expect } from 'vitest';
import * as fc from 'fast-check';
import { clampTimeRange, mapFragmentsToSegments } from '../components/Timeline';
import type { Fragment } from '../types';

const MIN_RANGE_MS = 60 * 1000; // 1 minute
const MAX_RANGE_MS = 7 * 24 * 60 * 60 * 1000; // 7 days

// ===== Arbitraries =====

/** Arbitrary for a timestamp in a reasonable range (2020–2030). */
const arbTimestampMs = fc.integer({
  min: new Date('2020-01-01').getTime(),
  max: new Date('2030-12-31').getTime(),
});

/** Arbitrary for a positive duration in ms (1ms to 30 days). */
const arbDurationMs = fc.integer({ min: 1, max: 30 * 24 * 60 * 60 * 1000 });

/**
 * Arbitrary for a time range: { start, end } where start < end.
 * Duration can be anything from 1ms to 30 days.
 */
const arbTimeRange = fc
  .tuple(arbTimestampMs, arbDurationMs)
  .map(([start, dur]) => ({ start, end: start + dur }));

/**
 * Arbitrary for a valid time range that fits within [MIN_RANGE_MS, MAX_RANGE_MS].
 */
const arbValidRange = fc
  .tuple(
    arbTimestampMs,
    fc.integer({ min: MIN_RANGE_MS, max: MAX_RANGE_MS }),
  )
  .map(([start, dur]) => ({ start, end: start + dur }));

/**
 * Arbitrary for a Fragment within a given time range.
 * The fragment starts somewhere in [rangeStart, rangeEnd) and has a positive length.
 */
function arbFragmentInRange(rangeStart: number, rangeEnd: number): fc.Arbitrary<Fragment> {
  const span = rangeEnd - rangeStart;
  return fc
    .tuple(
      fc.integer({ min: 0, max: Math.max(0, span - 1) }),
      fc.integer({ min: 1, max: Math.max(1, Math.min(span, 60_000)) }),
      fc.stringMatching(/^[0-9]{6,20}$/),
    )
    .map(([offset, length, fragNum]) => {
      const ts = new Date(rangeStart + offset);
      return {
        fragmentNumber: fragNum,
        serverTimestamp: ts,
        producerTimestamp: ts,
        fragmentLengthMillis: length,
      };
    });
}

// Feature: viewer-rewrite, Property 8: 时间轴缩放范围约束
// **Validates: Requirements 4.6**
describe('Property 8: 时间轴缩放范围约束', () => {
  it('clamped range duration is always between 1 minute and 7 days for any input range', () => {
    fc.assert(
      fc.property(arbTimeRange, ({ start, end }) => {
        const clamped = clampTimeRange(start, end);
        const duration = clamped.end - clamped.start;

        // Duration must be within [MIN_RANGE_MS, MAX_RANGE_MS]
        expect(duration).toBeGreaterThanOrEqual(MIN_RANGE_MS);
        expect(duration).toBeLessThanOrEqual(MAX_RANGE_MS);
      }),
      { numRuns: 100 },
    );
  });

  it('ranges already within bounds are returned unchanged', () => {
    fc.assert(
      fc.property(arbValidRange, ({ start, end }) => {
        const clamped = clampTimeRange(start, end);
        expect(clamped.start).toBe(start);
        expect(clamped.end).toBe(end);
      }),
      { numRuns: 100 },
    );
  });

  it('ranges smaller than 1 minute are expanded to exactly 1 minute', () => {
    const arbTooSmall = fc
      .tuple(arbTimestampMs, fc.integer({ min: 0, max: MIN_RANGE_MS - 1 }))
      .map(([start, dur]) => ({ start, end: start + dur }));

    fc.assert(
      fc.property(arbTooSmall, ({ start, end }) => {
        const clamped = clampTimeRange(start, end);
        const duration = clamped.end - clamped.start;
        expect(duration).toBe(MIN_RANGE_MS);
      }),
      { numRuns: 100 },
    );
  });

  it('ranges larger than 7 days are shrunk to exactly 7 days', () => {
    const arbTooLarge = fc
      .tuple(arbTimestampMs, fc.integer({ min: MAX_RANGE_MS + 1, max: MAX_RANGE_MS * 2 }))
      .map(([start, dur]) => ({ start, end: start + dur }));

    fc.assert(
      fc.property(arbTooLarge, ({ start, end }) => {
        const clamped = clampTimeRange(start, end);
        const duration = clamped.end - clamped.start;
        expect(duration).toBe(MAX_RANGE_MS);
      }),
      { numRuns: 100 },
    );
  });

  it('clamped range is centered on the midpoint of the original range', () => {
    fc.assert(
      fc.property(arbTimeRange, ({ start, end }) => {
        const clamped = clampTimeRange(start, end);
        const originalMid = (start + end) / 2;
        const clampedMid = (clamped.start + clamped.end) / 2;

        // Midpoints should be equal (within rounding tolerance)
        expect(Math.abs(clampedMid - originalMid)).toBeLessThanOrEqual(1);
      }),
      { numRuns: 100 },
    );
  });
});


// Feature: viewer-rewrite, Property 9: Fragment 到时间段的映射
// **Validates: Requirements 4.7**
describe('Property 9: Fragment 到时间段的映射', () => {
  it('every segment is correctly classified: hasRecording iff at least one fragment overlaps it', () => {
    fc.assert(
      fc.property(
        arbValidRange.chain(({ start, end }) =>
          fc
            .array(arbFragmentInRange(start, end), { minLength: 0, maxLength: 20 })
            .map((frags) => ({ rangeStart: start, rangeEnd: end, fragments: frags })),
        ),
        ({ rangeStart, rangeEnd, fragments }) => {
          const segments = mapFragmentsToSegments(fragments, rangeStart, rangeEnd);

          for (const seg of segments) {
            const overlaps = fragments.some((f) => {
              const fStart = f.producerTimestamp.getTime();
              const fEnd = fStart + f.fragmentLengthMillis;
              // Fragment overlaps segment if they share any time
              return fStart < seg.end && fEnd > seg.start;
            });

            if (seg.hasRecording) {
              expect(overlaps).toBe(true);
            } else {
              expect(overlaps).toBe(false);
            }
          }
        },
      ),
      { numRuns: 100 },
    );
  });

  it('all fragments are covered by at least one hasRecording segment', () => {
    fc.assert(
      fc.property(
        arbValidRange.chain(({ start, end }) =>
          fc
            .array(arbFragmentInRange(start, end), { minLength: 1, maxLength: 20 })
            .map((frags) => ({ rangeStart: start, rangeEnd: end, fragments: frags })),
        ),
        ({ rangeStart, rangeEnd, fragments }) => {
          const segments = mapFragmentsToSegments(fragments, rangeStart, rangeEnd);
          const recordingSegments = segments.filter((s) => s.hasRecording);

          for (const f of fragments) {
            const fStart = Math.max(f.producerTimestamp.getTime(), rangeStart);
            const fEnd = Math.min(
              f.producerTimestamp.getTime() + f.fragmentLengthMillis,
              rangeEnd,
            );
            if (fStart >= fEnd) continue; // Fragment outside range, skip

            // The fragment's clipped interval must be fully covered by recording segments
            const covered = recordingSegments.some(
              (s) => s.start <= fStart && s.end >= fEnd,
            );
            expect(covered).toBe(true);
          }
        },
      ),
      { numRuns: 100 },
    );
  });

  it('segments fully cover the range with no gaps or overlaps', () => {
    fc.assert(
      fc.property(
        arbValidRange.chain(({ start, end }) =>
          fc
            .array(arbFragmentInRange(start, end), { minLength: 0, maxLength: 20 })
            .map((frags) => ({ rangeStart: start, rangeEnd: end, fragments: frags })),
        ),
        ({ rangeStart, rangeEnd, fragments }) => {
          const segments = mapFragmentsToSegments(fragments, rangeStart, rangeEnd);

          // Segments should cover the entire range
          if (segments.length > 0) {
            expect(segments[0].start).toBe(rangeStart);
            expect(segments[segments.length - 1].end).toBe(rangeEnd);
          }

          // Adjacent segments should be contiguous (no gaps, no overlaps)
          for (let i = 1; i < segments.length; i++) {
            expect(segments[i].start).toBe(segments[i - 1].end);
          }
        },
      ),
      { numRuns: 100 },
    );
  });

  it('with no fragments, the entire range is a single no-recording segment', () => {
    fc.assert(
      fc.property(arbValidRange, ({ start, end }) => {
        const segments = mapFragmentsToSegments([], start, end);
        expect(segments).toHaveLength(1);
        expect(segments[0]).toEqual({
          start,
          end,
          hasRecording: false,
        });
      }),
      { numRuns: 100 },
    );
  });
});
