"""IoT Credential Provider for Python AI processes.

Fetches temporary AWS credentials via the IoT credential endpoint
using mTLS (X.509 device certificate), matching the C++ IoTAuthenticator.

The credential endpoint returns STS temporary credentials that can be
used with boto3 to access S3, DynamoDB, etc.
"""

from __future__ import annotations

import json
import logging
import os
import ssl
import time
import urllib.request
from dataclasses import dataclass
from typing import Optional

import boto3
from botocore.credentials import RefreshableCredentials
from botocore.session import get_session

logger = logging.getLogger(__name__)


@dataclass
class IoTConfig:
    """IoT credential provider configuration, read from [iot] INI section."""
    credential_endpoint: str = ""
    role_alias: str = ""
    thing_name: str = ""
    cert_path: str = ""
    key_path: str = ""
    root_ca_path: str = ""
    region: str = "ap-southeast-1"


def load_iot_config(ini_path: str) -> IoTConfig:
    """Load IoT config from the [iot] section of the INI file."""
    import configparser
    parser = configparser.ConfigParser()
    parser.read(ini_path)

    config = IoTConfig()
    if parser.has_section("iot"):
        config.credential_endpoint = parser.get("iot", "credential_endpoint", fallback="")
        config.role_alias = parser.get("iot", "role_alias", fallback="")
        config.thing_name = parser.get("iot", "thing_name", fallback="")
        config.cert_path = parser.get("iot", "cert_path", fallback="")
        config.key_path = parser.get("iot", "key_path", fallback="")
        config.root_ca_path = parser.get("iot", "root_ca_path", fallback="")

    # Region from [kvs] or env
    config.region = os.environ.get("AWS_DEFAULT_REGION", "ap-southeast-1")
    if parser.has_section("kvs"):
        config.region = parser.get("kvs", "region", fallback=config.region)

    return config


def fetch_iot_credentials(iot_config: IoTConfig) -> dict:
    """Fetch temporary credentials from IoT credential endpoint via mTLS.

    Returns dict with: accessKeyId, secretAccessKey, sessionToken, expiration
    """
    url = (
        f"https://{iot_config.credential_endpoint}"
        f"/role-aliases/{iot_config.role_alias}/credentials"
    )

    # Create SSL context with client certificate (mTLS)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.load_cert_chain(
        certfile=iot_config.cert_path,
        keyfile=iot_config.key_path,
    )
    if iot_config.root_ca_path and os.path.exists(iot_config.root_ca_path):
        ctx.load_verify_locations(cafile=iot_config.root_ca_path)
    else:
        ctx.load_default_certs()

    req = urllib.request.Request(url, headers={
        "x-amzn-iot-thingname": iot_config.thing_name,
    })

    with urllib.request.urlopen(req, context=ctx, timeout=10) as resp:
        data = json.loads(resp.read().decode())

    creds = data["credentials"]
    logger.info(
        "IoT credentials fetched: accessKeyId=%s...%s, expires=%s",
        creds["accessKeyId"][:4], creds["accessKeyId"][-4:],
        creds["expiration"],
    )
    return creds


def create_iot_boto3_session(iot_config: IoTConfig) -> boto3.Session:
    """Create a boto3 Session that auto-refreshes credentials via IoT endpoint.

    Uses botocore RefreshableCredentials so credentials are automatically
    refreshed before expiration.
    """

    def _refresh() -> dict:
        creds = fetch_iot_credentials(iot_config)
        return {
            "access_key": creds["accessKeyId"],
            "secret_key": creds["secretAccessKey"],
            "token": creds["sessionToken"],
            "expiry_time": creds["expiration"],
        }

    # Initial fetch
    initial = _refresh()

    refreshable = RefreshableCredentials.create_from_metadata(
        metadata=initial,
        refresh_using=_refresh,
        method="iot-credential-provider",
    )

    # Create a botocore session and inject the refreshable credentials
    botocore_session = get_session()
    botocore_session._credentials = refreshable
    botocore_session.set_config_variable("region", iot_config.region)

    return boto3.Session(botocore_session=botocore_session, region_name=iot_config.region)
