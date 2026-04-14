"""Cloud Verifier Lambda — S3 Event Notification → SageMaker verification → DynamoDB write.

触发条件：event.json 上传到 S3 时触发。
S3 路径格式：{device_id}/{YYYY-MM-DD}/{event_id}/event.json
截图格式：{device_id}/{YYYY-MM-DD}/{event_id}/{YYYYMMDD}_{HHMMSS}_{NNN}.jpg

验证流程：
  - 下载 event.json → 列出同目录下所有 .jpg → 调用 SageMaker 鸟类分类
  - 非鸟类（person/cat/dog）直接标记 verified
  - 结果写入 DynamoDB，更新 S3 verified.json
"""

from __future__ import annotations

import json
import logging
import os
import time
import urllib.parse
from datetime import datetime, timezone
from typing import Any

import boto3
from botocore.exceptions import ClientError

logger = logging.getLogger()
logger.setLevel(logging.INFO)

# 环境变量
SAGEMAKER_ENDPOINT_NAME = os.environ.get("SAGEMAKER_ENDPOINT_NAME", "bird-classifier-endpoint")
DYNAMODB_TABLE = os.environ.get("DYNAMODB_TABLE", "smart-camera-events")

# SageMaker top-1 置信度阈值
BIRD_CONFIDENCE_THRESHOLD = 0.5

# 不需要 SageMaker 验证的类别，直接标记 verified
DIRECT_VERIFY_CLASSES = {"person", "cat", "dog"}


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

        # 只处理 event.json
        if not key.endswith("/event.json"):
            logger.info("跳过非 event.json 对象: %s", key)
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
            result = _process_event(
                s3_client, sagemaker_client, table,
                bucket, key, device_id, event_id, start_time,
            )
        except Exception:
            logger.exception("处理 %s 时发生未捕获异常", key)
            result = {"status": "error", "event_id": event_id}

    return {"statusCode": 200, "body": "OK"}


def _process_event(
    s3_client: Any,
    sagemaker_client: Any,
    table: Any,
    bucket: str,
    metadata_key: str,
    device_id: str,
    event_id: str,
    start_time: float,
) -> dict:
    """核心处理：下载 event.json → 列出截图 → 验证 → 写 DynamoDB → 更新 S3。"""
    # 1. 下载 event.json
    metadata = _download_metadata(s3_client, bucket, metadata_key)

    # 防重触发：如果已有终态 verification_status，跳过
    existing_status = metadata.get("verification_status")
    if existing_status and existing_status not in ("pending", None):
        logger.info("已验证（重触发），跳过: %s (status=%s)", event_id, existing_status)
        return {"status": "skipped", "event_id": event_id, "reason": "already_verified"}

    # 2. 解析 detections_summary，提取主要检测类别
    detections = metadata.get("detections_summary", {})
    if detections:
        # 取置信度最高的类别作为 primary_class
        primary_class = max(detections, key=lambda c: detections[c].get("max_confidence", 0))
        max_confidence = detections[primary_class].get("max_confidence", 0)
    else:
        primary_class = "unknown"
        max_confidence = 0

    # 3. 列出同目录下的所有 .jpg 截图
    s3_prefix = metadata_key.rsplit("/", 1)[0] + "/"
    screenshots = _list_screenshots(s3_client, bucket, s3_prefix)

    logger.info(
        "事件元数据: event_id=%s primary_class=%s max_confidence=%.4f screenshots=%d",
        event_id, primary_class, max_confidence, len(screenshots),
    )

    # 4. 验证策略
    if screenshots:
        verification = verify_bird(
            s3_client, sagemaker_client, bucket, s3_prefix, screenshots, metadata,
        )
        if verification["verification_status"] == "verified" and verification.get("bird_species"):
            logger.info(
                "SageMaker 识别为鸟类 (%s)，YOLO primary_class=%s",
                verification["bird_species"], primary_class,
            )
            primary_class = "bird"
        elif verification["verification_status"] == "rejected":
            # SageMaker 认为不是鸟 → 使用 YOLO 的分类结果
            verification = {
                "verification_status": "verified",
                "pipeline_stage": "cloud_verified",
                "verification_fallback": False,
            }
            logger.info("SageMaker 拒绝鸟类，使用 YOLO class=%s", primary_class)
    else:
        verification = {
            "verification_status": "verified",
            "pipeline_stage": "cloud_verified",
            "verification_fallback": False,
        }
        logger.info("无截图，直接验证 class=%s", primary_class)

    elapsed_ms = int((time.time() - start_time) * 1000)
    verification_status = verification["verification_status"]

    # 5. 写 DynamoDB（仅 verified）
    if verification_status == "verified":
        _write_dynamodb(
            table, metadata, verification, device_id, event_id,
            primary_class, max_confidence, s3_prefix,
        )
    else:
        logger.info("事件被拒绝，跳过 DynamoDB: event_id=%s", event_id)

    # 6. 更新 S3 verified.json
    _update_s3_metadata(s3_client, bucket, s3_prefix, metadata, verification, primary_class)

    logger.info(
        "验证完成: event_id=%s status=%s elapsed_ms=%d primary_class=%s "
        "bird_species=%s species_confidence=%s",
        event_id, verification_status, elapsed_ms, primary_class,
        verification.get("bird_species", "N/A"),
        verification.get("species_confidence", "N/A"),
    )

    return {"status": verification_status, "event_id": event_id, "elapsed_ms": elapsed_ms}


def _list_screenshots(s3_client: Any, bucket: str, prefix: str) -> list[str]:
    """列出 S3 prefix 下所有 .jpg 文件的 key。"""
    keys = []
    try:
        response = s3_client.list_objects_v2(Bucket=bucket, Prefix=prefix)
        for obj in response.get("Contents", []):
            if obj["Key"].endswith(".jpg"):
                keys.append(obj["Key"])
    except ClientError as exc:
        logger.warning("列出截图失败 prefix=%s: %s", prefix, exc)
    return sorted(keys)


def verify_bird(
    s3_client: Any,
    sagemaker_client: Any,
    bucket: str,
    s3_prefix: str,
    screenshot_keys: list[str],
    metadata: dict,
) -> dict:
    """通过 SageMaker 端点验证鸟类，支持 early-stop。

    逐张下载截图调用 SageMaker，top-1 置信度 >= 阈值即停止。
    SageMaker 超时/失败时降级为 verified + fallback 标记。
    """
    if not screenshot_keys:
        logger.warning("无截图用于鸟类验证，拒绝")
        return {
            "verification_status": "rejected",
            "pipeline_stage": "cloud_rejected",
            "verification_fallback": False,
        }

    for i, s3_key in enumerate(screenshot_keys):
        try:
            logger.info("下载截图 %d/%d: %s", i + 1, len(screenshot_keys), s3_key)
            response = s3_client.get_object(Bucket=bucket, Key=s3_key)
            image_bytes = response["Body"].read()
        except ClientError as exc:
            logger.warning("下载截图失败 %s: %s", s3_key, exc)
            continue

        try:
            predictions = _invoke_sagemaker(sagemaker_client, image_bytes)
        except Exception as exc:
            logger.error(
                "SageMaker 调用失败 %s: %s — 降级为 verified", s3_key, exc,
            )
            return {
                "verification_status": "verified",
                "pipeline_stage": "cloud_verified",
                "verification_fallback": True,
                "bird_species": None,
                "species_confidence": None,
            }

        if not predictions:
            logger.warning("SageMaker 返回空结果: %s", s3_key)
            continue

        top1 = predictions[0]
        top1_species = top1.get("species", "unknown")
        top1_confidence = top1.get("confidence", 0.0)

        logger.info(
            "SageMaker 结果 %d: species=%s confidence=%.4f",
            i + 1, top1_species, top1_confidence,
        )

        if top1_confidence >= BIRD_CONFIDENCE_THRESHOLD:
            logger.info(
                "鸟类验证通过 (early stop %d/%d): %s (%.4f)",
                i + 1, len(screenshot_keys), top1_species, top1_confidence,
            )
            return {
                "verification_status": "verified",
                "pipeline_stage": "cloud_verified",
                "verification_fallback": False,
                "bird_species": top1_species,
                "species_confidence": round(top1_confidence, 4),
            }

    logger.info("所有 %d 张截图低于阈值，拒绝", len(screenshot_keys))
    return {
        "verification_status": "rejected",
        "pipeline_stage": "cloud_rejected",
        "verification_fallback": False,
    }


def _download_metadata(s3_client: Any, bucket: str, key: str) -> dict:
    """下载并解析 event.json。"""
    response = s3_client.get_object(Bucket=bucket, Key=key)
    body = response["Body"].read().decode("utf-8")
    return json.loads(body)


def _invoke_sagemaker(sagemaker_client: Any, image_bytes: bytes) -> list[dict]:
    """调用 SageMaker 端点，返回预测结果列表。"""
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
    verification: dict,
    device_id: str,
    event_id: str,
    primary_class: str,
    max_confidence: float,
    s3_prefix: str,
) -> None:
    """写入 DynamoDB，使用条件表达式防重复。

    PK = device_id, SK = event_timestamp (ISO 8601)。
    """
    start_time_str = metadata.get("start_time", "")
    end_time_str = metadata.get("end_time", "")

    # event_timestamp 用 start_time 作为排序键
    event_timestamp = start_time_str if start_time_str else datetime.now(timezone.utc).isoformat()

    # 从 detections_summary 提取所有检测类别
    detections = metadata.get("detections_summary", {})
    detected_classes = list(detections.keys()) if detections else [primary_class]

    # TTL: 30 天
    expiry_ttl = int(time.time()) + 30 * 24 * 3600

    item = {
        "device_id": device_id,
        "event_timestamp": event_timestamp,
        "event_id": event_id,
        "primary_class": primary_class,
        "detected_classes": detected_classes,
        "max_confidence": _to_decimal(max_confidence),
        "start_time": start_time_str,
        "end_time": end_time_str,
        "frame_count": metadata.get("frame_count", 0),
        "status": metadata.get("status", "unknown"),
        "s3_prefix": s3_prefix,
        "expiry_ttl": expiry_ttl,
        # 验证结果
        "verification_status": verification.get("verification_status", "verified"),
        "pipeline_stage": verification.get("pipeline_stage", "cloud_verified"),
    }

    # 鸟类特有字段
    if verification.get("bird_species"):
        item["bird_species"] = verification["bird_species"]
    if verification.get("species_confidence") is not None:
        item["species_confidence"] = _to_decimal(verification["species_confidence"])
    if verification.get("verification_fallback"):
        item["verification_fallback"] = True

    # detections_summary 原样存入（方便查询）
    if detections:
        item["detections_summary"] = json.loads(json.dumps(detections), parse_float=lambda x: _to_decimal(float(x)))

    try:
        table.put_item(
            Item=item,
            ConditionExpression="attribute_not_exists(device_id) AND attribute_not_exists(event_timestamp)",
        )
        logger.info("DynamoDB 写入成功: device_id=%s event_timestamp=%s", device_id, event_timestamp)
    except ClientError as exc:
        if exc.response["Error"]["Code"] == "ConditionalCheckFailedException":
            logger.info("幂等跳过: 事件已存在 device_id=%s event_timestamp=%s", device_id, event_timestamp)
        else:
            raise


def _update_s3_metadata(
    s3_client: Any,
    bucket: str,
    s3_prefix: str,
    metadata: dict,
    verification: dict,
    primary_class: str,
) -> None:
    """在 S3 同目录下写入 verified.json（不覆盖 event.json，避免重触发）。"""
    metadata["pipeline_stage"] = verification.get("pipeline_stage", "cloud_verified")
    metadata["verification_status"] = verification.get("verification_status", "verified")
    metadata["primary_class"] = primary_class

    if verification.get("bird_species"):
        metadata["bird_species"] = verification["bird_species"]
    if verification.get("species_confidence") is not None:
        metadata["species_confidence"] = verification["species_confidence"]
    if verification.get("verification_fallback"):
        metadata["verification_fallback"] = True

    verified_key = s3_prefix + "verified.json"

    s3_client.put_object(
        Bucket=bucket,
        Key=verified_key,
        Body=json.dumps(metadata, ensure_ascii=False, indent=2).encode("utf-8"),
        ContentType="application/json",
    )
    logger.info("S3 验证结果已写入: %s", verified_key)


def _to_decimal(value: Any) -> Any:
    """float → Decimal，DynamoDB 兼容。"""
    from decimal import Decimal

    if isinstance(value, float):
        return Decimal(str(value))
    if isinstance(value, int):
        return value
    return value
