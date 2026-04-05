"""Property-based tests for DetectionConfirmationWindow.

Uses hypothesis to verify correctness properties defined in the design document.
"""

from __future__ import annotations

from hypothesis import given, settings
from hypothesis import strategies as st

from device.ai.confirmation import DetectionConfirmationWindow

# Shared strategies
CLASSES = st.frozensets(st.sampled_from(["person", "cat", "dog", "bird"]))
FRAME_SEQUENCES = st.lists(CLASSES, min_size=1, max_size=50)
WINDOW_SIZES = st.integers(min_value=1, max_value=10)


# Feature: detection-confirmation-window, Property 1: 滑动窗口保留最近 N 帧
# Validates: Requirements 1.1, 1.2
class TestSlidingWindowRetainsLastNFrames:
    """After each push_frame, the internal window length equals
    min(frames_pushed, window_size) and its contents match the last N
    elements of the pushed sequence."""

    @given(frames=FRAME_SEQUENCES, window_size=WINDOW_SIZES)
    @settings(max_examples=200)
    def test_window_length_and_contents(
        self,
        frames: list[frozenset[str]],
        window_size: int,
    ) -> None:
        cw = DetectionConfirmationWindow(window_size=window_size, min_count=1)

        for i, frame in enumerate(frames):
            cw.push_frame(set(frame))
            pushed = i + 1

            # Length invariant
            expected_len = min(pushed, window_size)
            assert len(cw._window) == expected_len

            # Contents invariant — window matches tail of sequence
            expected_tail = [set(f) for f in frames[max(0, pushed - window_size) : pushed]]
            actual = list(cw._window)
            assert actual == expected_tail


# Feature: detection-confirmation-window, Property 2: M-of-N 确认等价性
# Validates: Requirements 2.1, 2.2
class TestMofNConfirmationEquivalence:
    """push_frame returns a confirmed set containing a class if and only if
    that class appears in >= min_count frames within the current window."""

    @given(
        frames=FRAME_SEQUENCES,
        window_size=WINDOW_SIZES,
        min_count=st.integers(min_value=1, max_value=10),
        target=st.sampled_from(["person", "cat", "dog", "bird"]),
    )
    @settings(max_examples=200)
    def test_confirmation_iff_count_ge_min(
        self,
        frames: list[frozenset[str]],
        window_size: int,
        min_count: int,
        target: str,
    ) -> None:
        # min_count is clamped to window_size internally
        cw = DetectionConfirmationWindow(window_size=window_size, min_count=min_count)
        effective_min = min(min_count, window_size)

        for i, frame in enumerate(frames):
            confirmed = cw.push_frame(set(frame))
            pushed = i + 1

            # Manually compute the current window tail
            start = max(0, pushed - window_size)
            window_tail = frames[start:pushed]

            # Count how many frames in the window contain the target
            target_count = sum(1 for f in window_tail if target in f)

            if target_count >= effective_min:
                assert target in confirmed, (
                    f"Expected '{target}' in confirmed (count={target_count}, "
                    f"min={effective_min}), got {confirmed}"
                )
            else:
                assert target not in confirmed, (
                    f"Expected '{target}' NOT in confirmed (count={target_count}, "
                    f"min={effective_min}), got {confirmed}"
                )

# Feature: detection-confirmation-window, Property 3: 类别独立性
# Validates: Requirements 2.3
class TestClassIndependence:
    """Removing all occurrences of a specific class X from the frame
    sequence must not change the confirmation status of any other class.

    This verifies that each class's confirmation is computed independently.
    """

    @given(
        frames=st.lists(
            st.frozensets(st.sampled_from(["person", "cat", "dog", "bird"])),
            min_size=1,
            max_size=50,
        ),
        window_size=st.integers(min_value=1, max_value=10),
        min_count=st.integers(min_value=1, max_value=10),
        removed_class=st.sampled_from(["person", "cat", "dog", "bird"]),
    )
    @settings(max_examples=200)
    def test_removing_class_does_not_affect_others(
        self,
        frames: list[frozenset[str]],
        window_size: int,
        min_count: int,
        removed_class: str,
    ) -> None:
        # Run original sequence
        cw_original = DetectionConfirmationWindow(
            window_size=window_size, min_count=min_count
        )
        original_results: list[set[str]] = []
        for frame in frames:
            confirmed = cw_original.push_frame(set(frame))
            original_results.append(confirmed)

        # Build filtered sequence: remove `removed_class` from every frame
        filtered_frames = [f - {removed_class} for f in frames]

        # Run filtered sequence
        cw_filtered = DetectionConfirmationWindow(
            window_size=window_size, min_count=min_count
        )
        for i, frame in enumerate(filtered_frames):
            confirmed = cw_filtered.push_frame(set(frame))

            # The confirmed set (minus removed_class) from the original run
            # must equal the confirmed set from the filtered run
            expected = original_results[i] - {removed_class}
            assert confirmed == expected, (
                f"Frame {i}: after removing '{removed_class}', "
                f"expected confirmed={expected}, got {confirmed}"
            )


# Feature: detection-confirmation-window, Property 4: N=1, M=1 等效于直通
# Validates: Requirements 4.3
class TestPassthroughWhenN1M1:
    """When window_size=1 and min_count=1, push_frame must return exactly
    the input set — the confirmation window acts as a no-op pass-through."""

    @given(classes=CLASSES)
    @settings(max_examples=200)
    def test_push_frame_returns_input(self, classes: frozenset[str]) -> None:
        """**Validates: Requirements 4.3**"""
        cw = DetectionConfirmationWindow(window_size=1, min_count=1)
        result = cw.push_frame(set(classes))
        assert result == set(classes)


# Feature: detection-confirmation-window, Property 5: 参数钳位
# Validates: Requirements 4.5
class TestParameterClamping:
    """When min_count > window_size, the effective min_count must be
    clamped to window_size after initialization."""

    @given(data=st.data())
    @settings(max_examples=200)
    def test_min_count_clamped_to_window_size(self, data: st.DataObject) -> None:
        """**Validates: Requirements 4.5**"""
        window_size = data.draw(st.integers(min_value=1, max_value=10), label="window_size")
        min_count = data.draw(
            st.integers(min_value=window_size + 1, max_value=20), label="min_count"
        )

        cw = DetectionConfirmationWindow(window_size=window_size, min_count=min_count)
        assert cw.min_count == window_size, (
            f"Expected min_count to be clamped to {window_size}, "
            f"got {cw.min_count} (original min_count={min_count})"
        )
