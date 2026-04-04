#!/usr/bin/env python3
"""数据清洗脚本（SageMaker Processing Job 版本）

SageMaker Processing Job 约定：
- 输入数据：/opt/ml/processing/input/（从 S3 自动下载）
- 输出数据：/opt/ml/processing/output/（处理完自动上传回 S3）

流程：
1. 从 /opt/ml/processing/input/ 读取原始图片（raw/<species>/*.jpg）
2. 用 YOLO 检测每张图中的鸟，过滤低质量图片
3. 清洗后的图片按 80/20 随机分成 train/val
4. 输出到 /opt/ml/processing/output/train/ 和 /opt/ml/processing/output/val/

Usage (SageMaker 自动调用):
  python clean_sagemaker.py
"""

import os
import random
import shutil
import subprocess
import sys
from pathlib import Path

from tqdm import tqdm

# ---------------------------------------------------------------------------
# SageMaker Processing 环境变量
# ---------------------------------------------------------------------------

# SageMaker Processing 会设置这些路径
# 本地调试时用默认值
INPUT_DIR = Path(os.environ.get("SM_INPUT_DIR", "/opt/ml/processing/input"))
OUTPUT_DIR = Path(os.environ.get("SM_OUTPUT_DIR", "/opt/ml/processing/output"))

# ---------------------------------------------------------------------------
# 清洗配置
# ---------------------------------------------------------------------------

YOLO_MODEL = "yolo11l.pt"          # 大模型，减少误判
MIN_BIRD_AREA_RATIO = 0.08         # 鸟占图片面积最小比例（8%）
MAX_BIRD_COUNT = 3                  # 最多允许几只鸟
MIN_CONFIDENCE = 0.3                # YOLO 检测置信度阈值
BIRD_CLASS_ID = 14                  # COCO 数据集中 bird 的类别 ID
TRAIN_RATIO = 0.8                   # train/val 分割比例
RANDOM_SEED = 42                    # 随机种子（保证可复现）


def install_dependencies():
    """安装 YOLO 依赖（Processing Job 的容器可能没有预装）"""
    print("安装依赖...")
    subprocess.check_call([sys.executable, "-m", "pip", "install",
                          "ultralytics", "Pillow", "tqdm", "-q"])
    print("依赖安装完成")


def analyze_image(model, image_path: str) -> dict:
    """用 YOLO 分析一张图片，返回鸟的检测信息和最大鸟的 bbox。"""
    results = model(image_path, verbose=False, conf=MIN_CONFIDENCE)

    if not results or len(results) == 0:
        return {"bird_count": 0, "max_area_ratio": 0.0, "has_bird": False, "best_bbox": None}

    result = results[0]
    img_h, img_w = result.orig_shape
    img_area = img_h * img_w

    bird_count = 0
    max_area_ratio = 0.0
    best_bbox = None  # 面积最大的鸟的 bbox

    for box in result.boxes:
        cls_id = int(box.cls[0].item())
        if cls_id == BIRD_CLASS_ID:
            bird_count += 1
            x1, y1, x2, y2 = box.xyxy[0].tolist()
            bbox_area = (x2 - x1) * (y2 - y1)
            area_ratio = bbox_area / img_area
            if area_ratio > max_area_ratio:
                max_area_ratio = area_ratio
                best_bbox = (int(x1), int(y1), int(x2), int(y2))

    return {
        "bird_count": bird_count,
        "max_area_ratio": max_area_ratio,
        "has_bird": bird_count > 0,
        "best_bbox": best_bbox,
        "img_size": (img_w, img_h),
    }


def crop_bird_bbox(image_path: str, bbox: tuple, img_size: tuple, padding: float = 0.15) -> "Image":
    """裁剪鸟的 bbox 区域，带 padding。

    Args:
        image_path: 图片路径
        bbox: (x1, y1, x2, y2)
        img_size: (width, height)
        padding: bbox 外扩比例（15%）
    """
    from PIL import Image

    img = Image.open(image_path).convert("RGB")
    img_w, img_h = img_size
    x1, y1, x2, y2 = bbox

    # 加 padding
    bw, bh = x2 - x1, y2 - y1
    pad_x = int(bw * padding)
    pad_y = int(bh * padding)
    x1 = max(0, x1 - pad_x)
    y1 = max(0, y1 - pad_y)
    x2 = min(img_w, x2 + pad_x)
    y2 = min(img_h, y2 + pad_y)

    return img.crop((x1, y1, x2, y2))


def should_keep(info: dict) -> tuple:
    """判断是否保留这张图片。返回 (keep, reason)。"""
    if not info["has_bird"]:
        return False, "no_bird_detected"
    if info["max_area_ratio"] < MIN_BIRD_AREA_RATIO:
        return False, f"bird_too_small ({info['max_area_ratio']:.1%})"
    if info["bird_count"] > MAX_BIRD_COUNT:
        return False, f"too_many_birds ({info['bird_count']})"
    return True, "ok"


def main():
    print("=" * 60)
    print("数据清洗 — SageMaker Processing Job")
    print("=" * 60)
    print(f"输入: {INPUT_DIR}")
    print(f"输出: {OUTPUT_DIR}")
    print(f"  原图版: {OUTPUT_DIR}/original/train/ + val/")
    print(f"  裁剪版: {OUTPUT_DIR}/cropped/train/ + val/")
    print(f"过滤条件:")
    print(f"  最小鸟占比: {MIN_BIRD_AREA_RATIO:.0%}")
    print(f"  最大鸟数量: {MAX_BIRD_COUNT}")
    print(f"  最小检测置信度: {MIN_CONFIDENCE}")
    print(f"  Train/Val 比例: {TRAIN_RATIO}/{1-TRAIN_RATIO}")
    print()

    # 安装依赖
    install_dependencies()

    # 加载 YOLO 模型
    from ultralytics import YOLO
    print(f"\n加载 YOLO 模型: {YOLO_MODEL}")
    model = YOLO(YOLO_MODEL)

    # 统计
    stats = {
        "total": 0, "kept": 0,
        "removed_no_bird": 0, "removed_too_small": 0, "removed_too_many": 0,
    }

    # 查找输入目录中的物种文件夹
    # 支持两种结构：input/<species>/ 或 input/raw/<species>/
    raw_dir = INPUT_DIR / "raw"
    if raw_dir.exists():
        species_parent = raw_dir
    else:
        species_parent = INPUT_DIR

    species_dirs = sorted([d for d in species_parent.iterdir() if d.is_dir()])
    print(f"找到 {len(species_dirs)} 个物种目录")

    for species_dir in species_dirs:
        species_name = species_dir.name
        images = sorted(species_dir.glob("*.jpg"))

        if not images:
            continue

        kept_files = []
        kept_infos = []  # 保存每张图的检测信息（用于 bbox 裁剪）
        removed = 0

        pbar = tqdm(images, desc=f"  {species_name}", unit="img")
        for img_path in pbar:
            stats["total"] += 1

            info = analyze_image(model, str(img_path))
            keep, reason = should_keep(info)

            if keep:
                kept_files.append(img_path)
                kept_infos.append(info)
                stats["kept"] += 1
            else:
                if "no_bird" in reason:
                    stats["removed_no_bird"] += 1
                elif "too_small" in reason:
                    stats["removed_too_small"] += 1
                elif "too_many" in reason:
                    stats["removed_too_many"] += 1
                removed += 1

            pbar.set_postfix(kept=len(kept_files), removed=removed)

        # 按 80/20 随机分 train/val（对 files 和 infos 同步打乱）
        random.seed(RANDOM_SEED)
        combined = list(zip(kept_files, kept_infos))
        random.shuffle(combined)
        split_idx = int(len(combined) * TRAIN_RATIO)
        train_items = combined[:split_idx]
        val_items = combined[split_idx:]

        # 保存两份数据：original（原图）和 cropped（bbox 裁剪）
        for split_name, items in [("train", train_items), ("val", val_items)]:
            for f, info in items:
                # 原图版
                orig_path = OUTPUT_DIR / "original" / split_name / species_name / f.name
                orig_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, orig_path)

                # bbox 裁剪版
                if info["best_bbox"]:
                    crop_path = OUTPUT_DIR / "cropped" / split_name / species_name / f.name
                    crop_path.parent.mkdir(parents=True, exist_ok=True)
                    cropped = crop_bird_bbox(str(f), info["best_bbox"], info["img_size"])
                    cropped.save(str(crop_path), "JPEG", quality=90)

        print(f"  {species_name}: {len(kept_files)}/{len(images)} kept "
              f"→ train:{len(train_items)} val:{len(val_items)}")

    # 汇总
    print()
    print("=" * 60)
    print("清洗结果汇总")
    print("=" * 60)
    print(f"总图片数: {stats['total']}")
    print(f"保留: {stats['kept']} ({stats['kept']/max(stats['total'],1)*100:.1f}%)")
    print(f"移除 - 未检测到鸟: {stats['removed_no_bird']}")
    print(f"移除 - 鸟太小: {stats['removed_too_small']}")
    print(f"移除 - 群鸟太多: {stats['removed_too_many']}")

    # 统计输出
    for variant in ["original", "cropped"]:
        vdir = OUTPUT_DIR / variant
        train_count = sum(1 for _ in (vdir / "train").rglob("*.jpg")) if (vdir / "train").exists() else 0
        val_count = sum(1 for _ in (vdir / "val").rglob("*.jpg")) if (vdir / "val").exists() else 0
        print(f"\n{variant}: train={train_count} val={val_count}")


if __name__ == "__main__":
    main()
