"""Current Demo Profile fields shared with the native build."""

import json
from pathlib import Path


PROFILE_FIELDS = frozenset(json.loads(Path(__file__).with_name("profile_fields.json").read_text(encoding="utf-8")))


def validate_profile_fields(profile):
    if not isinstance(profile, dict):
        raise ValueError("Profile must be an object")
    for field in profile:
        if field not in PROFILE_FIELDS:
            raise ValueError(f"Unknown Profile field: '{field}'")
