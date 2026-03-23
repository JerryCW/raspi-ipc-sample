"""Property-based tests for DetectorConfig environment variable overrides.

Uses Hypothesis to verify that environment variables with prefix AI_SUMMARY_
always take precedence over INI file values when loading configuration.

Feature: ai-video-summary, Property 22: 环境变量覆盖配置
"""

from __future__ import annotations

import os
import tempfile
from dataclasses import fields

from hypothesis import given, settings, strategies as st

from device.ai.config import DetectorConfig


# ---------------------------------------------------------------------------
# Strategies — generate random but valid config values per field type
# ---------------------------------------------------------------------------

# String fields: non-empty printable strings without newlines or '=' (INI-safe)
# Also exclude '%' which triggers configparser interpolation syntax
_ini_safe_str = st.text(
    alphabet=st.characters(
        whitelist_categories=("L", "N", "P", "S"),
        blacklist_characters="\n\r=;#[]%",
    ),
    min_size=1,
    max_size=30,
).filter(lambda s: s.strip() == s and len(s.strip()) > 0)

# Positive float for numeric float fields
_pos_float = st.floats(min_value=0.01, max_value=9999.0, allow_nan=False, allow_infinity=False)

# Positive int for numeric int fields
_pos_int = st.integers(min_value=1, max_value=9999)

# Comma-separated list of class names for detect_classes
_class_list = st.lists(
    st.sampled_from(["person", "cat", "dog", "bird", "car", "truck"]),
    min_size=1,
    max_size=5,
    unique=True,
)


def _strategy_for_field(f) -> st.SearchStrategy:
    """Return a Hypothesis strategy that produces valid values for a field."""
    hint = f.type if isinstance(f.type, str) else getattr(f.type, "__name__", str(f.type))
    if hint == "float":
        return _pos_float
    if hint == "int":
        return _pos_int
    if hint == "List[str]":
        return _class_list
    # str
    return _ini_safe_str


# Build a composite strategy that picks one field at random and generates
# distinct INI and env-var values for it.
_ALL_FIELDS = fields(DetectorConfig)


@st.composite
def field_with_ini_and_env(draw):
    """Draw a random field name, an INI value, and a *different* env-var value."""
    f = draw(st.sampled_from(_ALL_FIELDS))
    strat = _strategy_for_field(f)
    ini_val = draw(strat)
    env_val = draw(strat.filter(lambda v: v != ini_val))
    return f, ini_val, env_val


def _to_ini_str(value, type_hint: str) -> str:
    """Serialize a Python value to its INI string representation."""
    if type_hint == "List[str]":
        return ",".join(value)
    return str(value)


def _coerced_value(raw_str: str, type_hint: str):
    """Replicate the coercion logic so we know the expected final value."""
    if type_hint == "float":
        return float(raw_str)
    if type_hint == "int":
        return int(raw_str)
    if type_hint == "List[str]":
        return [s.strip() for s in raw_str.split(",") if s.strip()]
    return raw_str


# ---------------------------------------------------------------------------
# Property 22: 环境变量覆盖配置
# ---------------------------------------------------------------------------


class TestEnvVarOverridesConfig:
    """Property 22: Environment variable overrides INI config.

    Feature: ai-video-summary, Property 22: 环境变量覆盖配置

    **Validates: Requirements 8.5**

    For any [ai_summary] config parameter, if the corresponding env var
    (AI_SUMMARY_{FIELD_NAME_UPPER}) is set, the final value equals the
    env var value, not the INI file value.
    """

    @given(data=field_with_ini_and_env())
    @settings(max_examples=200)
    def test_env_var_wins_over_ini_value(self, data):
        """When both INI and env var are present, env var value is used."""
        f, ini_val, env_val = data
        hint = f.type if isinstance(f.type, str) else getattr(f.type, "__name__", str(f.type))

        ini_str = _to_ini_str(ini_val, hint)
        env_str = _to_ini_str(env_val, hint)
        env_key = f"AI_SUMMARY_{f.name.upper()}"

        # Write a temp INI file with the ini_val
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".ini", delete=False
        ) as tmp:
            tmp.write("[ai_summary]\n")
            tmp.write(f"{f.name} = {ini_str}\n")
            tmp_path = tmp.name

        old_env = os.environ.get(env_key)
        try:
            os.environ[env_key] = env_str
            config = DetectorConfig.from_ini(tmp_path)
            actual = getattr(config, f.name)
            expected = _coerced_value(env_str, hint)
            assert actual == expected, (
                f"Field '{f.name}': expected env value {expected!r}, "
                f"got {actual!r} (ini={ini_str!r}, env={env_str!r})"
            )
        finally:
            # Restore original env state
            if old_env is None:
                os.environ.pop(env_key, None)
            else:
                os.environ[env_key] = old_env
            os.unlink(tmp_path)
