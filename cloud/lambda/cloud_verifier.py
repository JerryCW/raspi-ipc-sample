"""Cloud Verifier Lambda — S3 event.json → SageMaker 验证 → DynamoDB 写入。

触发条件：event.json 上传到 S3 时触发。
S3 路径格式：{device_id}/{YYYY-MM-DD}/{event_id}/event.json
截图格式：{device_id}/{YYYY-MM-DD}/{event_id}/{YYYYMMDD}_{HHMMSS}_{NNN}.jpg

DynamoDB 表：raspi-eye-events (PK=device_id, SK=start_time)
"""

from __future__ import annotations

import json
import logging
import os
import time
import urllib.parse
from datetime import datetime, timezone
from decimal import Decimal
from typing import Any

import boto3
from botocore.exceptions import ClientError

logger = logging.getLogger()
logger.setLevel(logging.INFO)

# 环境变量
SAGEMAKER_ENDPOINT_NAME = os.environ.get("SAGEMAKER_ENDPOINT_NAME", "bird-classifier-endpoint")
DYNAMODB_TABLE = os.environ.get("DYNAMODB_TABLE", "raspi-eye-events")

# SageMaker top-1 置信度阈值
BIRD_CONFIDENCE_THRESHOLD = 0.5

# 加载鸟类中英文映射表
_SPECIES_MAP = {}
_species_map_path = os.path.join(os.path.dirname(__file__), "bird_species_map.json")
if os.path.exists(_species_map_path):
    with open(_species_map_path, "r") as _f:
        _SPECIES_MAP = json.load(_f)


def lambda_handler(event: dict, context: Any) -> dict:
    """S3 Event Notification 入口。"""
    start_time = time.time()

    s3_client = boto3.client("s3")
    sagemaker_client = boto3.client("sagemaker-runtime")
    dynamodb = boto3.resource("dynamodb")
    table = dynamodb.Table(DYNAMODB_TABLE)

    for record in event.get("Records", []):
        s3_info = record.get("s3", {})
        bucket = s3_info.get("bucket", {}).get("name", "")
        raw_key = s3_info.get("object", {}).get("key", "")
        key = urllib.parse.unquote_plus(raw_key)

        if not key.endswith("/event.json"):
            logger.info("跳过非 event.json: %s", key)
            continue

        # 解析路径：{device_id}/{date}/{event_id}/event.json
        parts = key.split("/")
        if len(parts) < 4:
            logger.warning("路径格式不符: %s", key)
            continue

        device_id = parts[0]
        event_id = parts[2]

        logger.info("处理事件: bucket=%s key=%s device_id=%s event_id=%s",
                     bucket, key, device_id, event_id)

        try:
            _process_event(
                s3_client, sagemaker_client, table,
                bucket, key, device_id, event_id, start_time,
            )
        except Exception:
            logger.exception("处理 %s 时发生未捕获异常", key)

    return {"statusCode": 200, "body": "OK"}


def _process_event(
    s3_client: Any,
    sagemaker_client: Any,
    table: Any,
    bucket: str,
    metadata_key: str,
    device_id: str,
    event_id: str,
    lambda_start: float,
) -> None:
    """核心处理流程。"""
    # 1. 下载 event.json
    metadata = _download_json(s3_client, bucket, metadata_key)

    # 2. 解析 YOLO 检测结果
    detections = metadata.get("detections_summary", {})
    yolo_top_class, yolo_top_confidence = _extract_top_detection(detections)

    # 3. 获取截图列表
    s3_prefix = metadata_key.rsplit("/", 1)[0] + "/"
    snapshot_filenames = metadata.get("snapshots", [])
    if snapshot_filenames:
        screenshot_keys = [s3_prefix + fname for fname in snapshot_filenames]
    else:
        screenshot_keys = _list_screenshots(s3_client, bucket, s3_prefix)

    # 4. thumbnail_key = 第一张截图的完整 S3 key
    thumbnail_key = screenshot_keys[0] if screenshot_keys else ""

    # 5. 计算 duration_sec
    start_time_str = metadata.get("start_time", "")
    end_time_str = metadata.get("end_time", "")
    duration_sec = _calc_duration(start_time_str, end_time_str)

    logger.info(
        "事件: event_id=%s yolo_top=%s(%.4f) screenshots=%d duration=%ds",
        event_id, yolo_top_class, yolo_top_confidence,
        len(screenshot_keys), duration_sec,
    )

    # 6. SageMaker 验证
    verified = False
    species_info: dict = {}

    if screenshot_keys:
        result = _verify_bird(s3_client, sagemaker_client, bucket, screenshot_keys)
        verified = result.get("verified", False)
        species_info = result
    else:
        logger.info("无截图，跳过 SageMaker 验证")

    # 7. 写 DynamoDB
    _write_dynamodb(
        table, metadata, device_id, event_id,
        s3_prefix, thumbnail_key, duration_sec,
        yolo_top_class, yolo_top_confidence, detections,
        verified, species_info,
    )

    # 8. 写 verified.json 到 S3
    _write_verified_json(
        s3_client, bucket, s3_prefix, metadata,
        verified, species_info, yolo_top_class,
    )

    elapsed_ms = int((time.time() - lambda_start) * 1000)
    logger.info(
        "完成: event_id=%s verified=%s species=%s elapsed=%dms",
        event_id, verified,
        species_info.get("species", "N/A"), elapsed_ms,
    )


def _verify_bird(
    s3_client: Any,
    sagemaker_client: Any,
    bucket: str,
    screenshot_keys: list[str],
) -> dict:
    """调用 SageMaker 验证鸟类，early-stop 策略。

    返回 {"verified": bool, "species": str, "species_confidence": float,
           "species_cn": str, "family": str, "family_cn": str}
    """
    for i, s3_key in enumerate(screenshot_keys):
        try:
            response = s3_client.get_object(Bucket=bucket, Key=s3_key)
            image_bytes = response["Body"].read()
        except ClientError as exc:
            logger.warning("下载截图失败 %s: %s", s3_key, exc)
            continue

        try:
            predictions = _invoke_sagemaker(sagemaker_client, image_bytes)
        except Exception as exc:
            logger.error("SageMaker 调用失败 %s: %s", s3_key, exc)
            # SageMaker 故障时不标记 verified，保持 false
            return {"verified": False}

        if not predictions:
            continue

        top1 = predictions[0]
        species = top1.get("species", "unknown")
        confidence = top1.get("confidence", 0.0)

        logger.info("SageMaker %d/%d: species=%s confidence=%.4f",
                     i + 1, len(screenshot_keys), species, confidence)

        if confidence >= BIRD_CONFIDENCE_THRESHOLD:
            # 查映射表
            info = _SPECIES_MAP.get(species, {})
            return {
                "verified": True,
                "species": species.replace("_", " "),
                "species_confidence": round(confidence, 4),
                "species_cn": info.get("species_cn", ""),
                "family": info.get("family", ""),
                "family_cn": info.get("family_cn", ""),
            }

    return {"verified": False}


def _extract_top_detection(detections: dict) -> tuple[str, float]:
    """从 detections_summary 中提取置信度最高的类别。"""
    if not detections:
        return "unknown", 0.0
    top_class = max(detections, key=lambda c: detections[c].get("max_confidence", 0))
    top_conf = detections[top_class].get("max_confidence", 0)
    return top_class, top_conf


def _calc_duration(start_str: str, end_str: str) -> int:
    """计算 ISO 8601 时间差（秒），解析失败返回 0。"""
    try:
        start = datetime.fromisoformat(start_str.replace("Z", "+00:00"))
        end = datetime.fromisoformat(end_str.replace("Z", "+00:00"))
        return max(0, int((end - start).total_seconds()))
    except (ValueError, TypeError):
        return 0


def _list_screenshots(s3_client: Any, bucket: str, prefix: str) -> list[str]:
    """列出 S3 prefix 下所有 .jpg 文件。"""
    keys = []
    try:
        response = s3_client.list_objects_v2(Bucket=bucket, Prefix=prefix)
        for obj in response.get("Contents", []):
            if obj["Key"].endswith(".jpg"):
                keys.append(obj["Key"])
    except ClientError as exc:
        logger.warning("列出截图失败 prefix=%s: %s", prefix, exc)
    return sorted(keys)


def _download_json(s3_client: Any, bucket: str, key: str) -> dict:
    """下载并解析 JSON 文件。"""
    response = s3_client.get_object(Bucket=bucket, Key=key)
    return json.loads(response["Body"].read().decode("utf-8"))


def _invoke_sagemaker(sagemaker_client: Any, image_bytes: bytes) -> list[dict]:
    """调用 SageMaker 端点。"""
    response = sagemaker_client.invoke_endpoint(
        EndpointName=SAGEMAKER_ENDPOINT_NAME,
        ContentType="application/x-image",
        Accept="application/json",
        Body=image_bytes,
    )
    result = json.loads(response["Body"].read().decode("utf-8"))
    return result.get("predictions", [])


def _write_dynamodb(
    table: Any,
    metadata: dict,
    device_id: str,
    event_id: str,
    s3_prefix: str,
    thumbnail_key: str,
    duration_sec: int,
    yolo_top_class: str,
    yolo_top_confidence: float,
    detections: dict,
    verified: bool,
    species_info: dict,
) -> None:
    """写入 DynamoDB raspi-eye-events 表。"""
    start_time_str = metadata.get("start_time", "")
    if not start_time_str:
        start_time_str = datetime.now(timezone.utc).isoformat()

    item: dict[str, Any] = {
        # Keys
        "device_id": device_id,
        "start_time": start_time_str,
        # 事件基本信息
        "event_id": event_id,
        "end_time": metadata.get("end_time", ""),
        "duration_sec": duration_sec,
        "s3_prefix": s3_prefix,
        "thumbnail_key": thumbnail_key,
        "snapshot_count": metadata.get("frame_count", 0),
        # KVS 回放
        "kvs_stream_name": metadata.get("kvs_stream_name", ""),
        "kvs_region": metadata.get("kvs_region", ""),
        # YOLO 检测
        "yolo_top_class": yolo_top_class,
        "yolo_top_confidence": _dec(yolo_top_confidence),
        "yolo_detections": _detections_to_dynamo(detections),
        # 验证状态
        "verified": verified,
    }

    # SageMaker 回填字段（仅 verified=True 时有值）
    if verified and species_info:
        for field in ("species", "species_cn", "family", "family_cn"):
            val = species_info.get(field, "")
            if val:
                item[field] = val
        if species_info.get("species_confidence") is not None:
            item["species_confidence"] = _dec(species_info["species_confidence"])

    # 清理空字符串
    item = {k: v for k, v in item.items() if v != "" and v is not None}

    try:
        table.put_item(
            Item=item,
            ConditionExpression="attribute_not_exists(device_id) AND attribute_not_exists(start_time)",
        )
        logger.info("DynamoDB 写入: device_id=%s start_time=%s", device_id, start_time_str)
    except ClientError as exc:
        if exc.response["Error"]["Code"] == "ConditionalCheckFailedException":
            logger.info("幂等跳过: %s %s", device_id, start_time_str)
        else:
            raise


def _write_verified_json(
    s3_client: Any,
    bucket: str,
    s3_prefix: str,
    metadata: dict,
    verified: bool,
    species_info: dict,
    yolo_top_class: str,
) -> None:
    """在 S3 同目录写入 verified.json（不覆盖 event.json，避免重触发）。"""
    result = dict(metadata)
    result["verified"] = verified
    result["yolo_top_class"] = yolo_top_class

    if verified and species_info:
        for field in ("species", "species_cn", "family", "family_cn", "species_confidence"):
            val = species_info.get(field)
            if val is not None:
                result[field] = val

    verified_key = s3_prefix + "verified.json"
    s3_client.put_object(
        Bucket=bucket,
        Key=verified_key,
        Body=json.dumps(result, ensure_ascii=False, indent=2).encode("utf-8"),
        ContentType="application/json",
    )
    logger.info("S3 写入: %s", verified_key)


def _dec(value: Any) -> Any:
    """float → Decimal（DynamoDB 兼容）。"""
    if isinstance(value, float):
        return Decimal(str(value))
    return value


def _detections_to_dynamo(detections: dict) -> dict:
    """将 detections_summary 中的 float 转为 Decimal。"""
    if not detections:
        return {}
    return json.loads(
        json.dumps(detections),
        parse_float=lambda x: Decimal(x),
    )
