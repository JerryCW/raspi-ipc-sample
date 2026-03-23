"""Property-based tests for IPC client control message serialization.

Uses Hypothesis to verify round-trip correctness of FrameNotification
serialization/deserialization.
"""

from hypothesis import given, settings, strategies as st

from device.ai.ipc_client import FrameNotification, parse_notification, serialize_notification

# ---------------------------------------------------------------------------
# Strategies
# ---------------------------------------------------------------------------

# shm_name: always starts with "/" followed by 1-50 alphanumeric/underscore chars
shm_name_st = st.from_regex(r"/[a-zA-Z_][a-zA-Z0-9_]{0,49}", fullmatch=True)

# buffer_index: 0 or 1 (ping-pong)
buffer_index_st = st.integers(min_value=0, max_value=1)

# offset: realistic byte offset (header is 64 bytes, buffers up to ~20MB)
offset_st = st.integers(min_value=0, max_value=20 * 1024 * 1024)

# size: frame data size in bytes (up to ~10MB per buffer)
size_st = st.integers(min_value=1, max_value=10 * 1024 * 1024)

# width / height: common video resolutions
width_st = st.integers(min_value=1, max_value=7680)
height_st = st.integers(min_value=1, max_value=4320)

# pixel_format: formats supported by the FrameExporter
pixel_format_st = st.sampled_from(["NV12", "YUY2"])

# timestamp_ms: Unix millisecond timestamp (reasonable range)
timestamp_ms_st = st.integers(min_value=0, max_value=2**53 - 1)

# sequence: frame sequence number
sequence_st = st.integers(min_value=0, max_value=2**53 - 1)

frame_notification_st = st.builds(
    FrameNotification,
    shm_name=shm_name_st,
    buffer_index=buffer_index_st,
    offset=offset_st,
    size=size_st,
    width=width_st,
    height=height_st,
    pixel_format=pixel_format_st,
    timestamp_ms=timestamp_ms_st,
    sequence=sequence_st,
)


# ---------------------------------------------------------------------------
# Property 1: 控制消息序列化往返（Round-trip）
# ---------------------------------------------------------------------------


class TestControlMessageRoundTrip:
    """Property 1: Control message serialization round-trip.

    **Validates: Requirements 1.3**

    For any valid FrameNotification, serializing to JSON and deserializing
    back should produce an equivalent object.
    """

    @given(notif=frame_notification_st)
    @settings(max_examples=200)
    def test_serialize_then_parse_is_identity(self, notif: FrameNotification) -> None:
        """Serializing then parsing a FrameNotification yields the original."""
        json_str = serialize_notification(notif)
        restored = parse_notification(json_str)

        assert restored.shm_name == notif.shm_name
        assert restored.buffer_index == notif.buffer_index
        assert restored.offset == notif.offset
        assert restored.size == notif.size
        assert restored.width == notif.width
        assert restored.height == notif.height
        assert restored.pixel_format == notif.pixel_format
        assert restored.timestamp_ms == notif.timestamp_ms
        assert restored.sequence == notif.sequence

    @given(notif=frame_notification_st)
    @settings(max_examples=200)
    def test_serialized_string_ends_with_newline(self, notif: FrameNotification) -> None:
        """Serialized output is a single JSON line terminated by newline."""
        json_str = serialize_notification(notif)
        assert json_str.endswith("\n")
        # Exactly one newline, at the end
        assert json_str.count("\n") == 1

    @given(notif=frame_notification_st)
    @settings(max_examples=200)
    def test_round_trip_equality(self, notif: FrameNotification) -> None:
        """Double round-trip produces the same result as single round-trip."""
        first = parse_notification(serialize_notification(notif))
        second = parse_notification(serialize_notification(first))
        assert first == second
