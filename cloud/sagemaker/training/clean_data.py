#!/usr/bin/env python3
"""数据清洗脚本：过滤低质量训练图片。

使用 YOLO 检测每张图片中的鸟，过滤掉：
1. 检测不到鸟的图片（可能是纯背景、远距离剪影）
2. 鸟占比太小的图片（< 5% 面积，鸟太远看不清特征）
3. 多只鸟的群鸟照（不适合单物种分类训练）

清洗后的图片保存到 data_cleaned/ 目录，保持原有的 train/val 结构。

Usage:
  python cloud/sagemaker/training/clean_data.py

需要安装: pip install ultralytics
"""

import os
import shutil
from pathlib import Path

from ultralytics import YOLO
from PIL import Image
from tqdm import tqdm

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------

INPUT_DIR = Path("cloud/sagemaker/training/data/raw")
OUTPUT_DIR = Path("cloud/sagemaker/training/data_cleaned")

# YOLO 模型（用 yolo11l 做检测，大模型减少误判）
YOLO_MODEL = "yolo11l.pt"

# 过滤条件
MIN_BIRD_AREA_RATIO = 0.08   # 鸟占图片面积的最小比例（8%）
MAX_BIRD_COUNT = 3            # 最多允许几只鸟（超过的群鸟照过滤掉）
MIN_CONFIDENCE = 0.3          # YOLO 检测置信度阈值
BIRD_CLASS_ID = 14            # COCO 数据集中 bird 的类别 ID


def analyze_image(model, image_path: str) -> dict:
    """用 YOLO 分析一张图片，返回鸟的检测信息。"""
    results = model(image_path, verbose=False, conf=MIN_CONFIDENCE)

    if not results or len(results) == 0:
        return {"bird_count": 0, "max_area_ratio": 0.0, "has_bird": False}

    result = results[0]
    img_h, img_w = result.orig_shape
    img_area = img_h * img_w

    bird_count = 0
    max_area_ratio = 0.0

    for box in result.boxes:
        cls_id = int(box.cls[0].item())
        if cls_id == BIRD_CLASS_ID:
            bird_count += 1
            # 计算 bbox 面积占比
            x1, y1, x2, y2 = box.xyxy[0].tolist()
            bbox_area = (x2 - x1) * (y2 - y1)
            area_ratio = bbox_area / img_area
            max_area_ratio = max(max_area_ratio, area_ratio)

    return {
        "bird_count": bird_count,
        "max_area_ratio": max_area_ratio,
        "has_bird": bird_count > 0,
    }


def should_keep(info: dict) -> tuple[bool, str]:
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
    print("数据清洗：过滤低质量训练图片")
    print("=" * 60)
    print(f"输入: {INPUT_DIR}")
    print(f"输出: {OUTPUT_DIR}")
    print(f"过滤条件:")
    print(f"  最小鸟占比: {MIN_BIRD_AREA_RATIO:.0%}")
    print(f"  最大鸟数量: {MAX_BIRD_COUNT}")
    print(f"  最小检测置信度: {MIN_CONFIDENCE}")
    print()

    # 加载 YOLO 模型
    print("加载 YOLO 模型...")
    model = YOLO(YOLO_MODEL)

    # 统计
    stats = {
        "total": 0,
        "kept": 0,
        "removed_no_bird": 0,
        "removed_too_small": 0,
        "removed_too_many": 0,
    }
    species_stats = {}

    # 遍历所有物种目录（raw/ 下直接是物种文件夹）
    for species_dir in sorted(INPUT_DIR.iterdir()):
        if not species_dir.is_dir():
            continue

        species_name = species_dir.name
        images = sorted(species_dir.glob("*.jpg"))

        if not images:
            continue

        kept = 0
        removed = 0
        kept_files = []

        pbar = tqdm(images, desc=f"  {species_name}", unit="img")
        for img_path in pbar:
            stats["total"] += 1

            # YOLO 分析
            info = analyze_image(model, str(img_path))
            keep, reason = should_keep(info)

            if keep:
                kept_files.append(img_path)
                stats["kept"] += 1
                kept += 1
            else:
                if "no_bird" in reason:
                    stats["removed_no_bird"] += 1
                elif "too_small" in reason:
                    stats["removed_too_small"] += 1
                elif "too_many" in reason:
                    stats["removed_too_many"] += 1
                removed += 1

            pbar.set_postfix(kept=kept, removed=removed)

        # 按 80/20 分 train/val
        import random
        random.seed(42)  # 固定随机种子，保证可复现
        random.shuffle(kept_files)
        split_idx = int(len(kept_files) * 0.8)
        train_files = kept_files[:split_idx]
        val_files = kept_files[split_idx:]

        for f in train_files:
            out_path = OUTPUT_DIR / "train" / species_name / f.name
            out_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out_path)

        for f in val_files:
            out_path = OUTPUT_DIR / "val" / species_name / f.name
            out_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, out_path)

        species_stats[species_name] = {
            "total": len(images),
            "kept": kept,
            "removed": removed,
            "train": len(train_files),
            "val": len(val_files),
            "keep_rate": f"{kept/len(images)*100:.0f}%" if images else "N/A",
        }

    # 打印汇总
    print()
    print("=" * 60)
    print("清洗结果汇总")
    print("=" * 60)
    print(f"总图片数: {stats['total']}")
    print(f"保留: {stats['kept']} ({stats['kept']/stats['total']*100:.1f}%)")
    print(f"移除 - 未检测到鸟: {stats['removed_no_bird']}")
    print(f"移除 - 鸟太小: {stats['removed_too_small']}")
    print(f"移除 - 群鸟太多: {stats['removed_too_many']}")
    print(f"\n清洗后数据: {OUTPUT_DIR}")

    # 打印每个物种的保留率
    print(f"\n各物种保留率:")
    for name, s in sorted(species_stats.items()):
        print(f"  {name:40s} {s['kept']:4d}/{s['total']:4d} ({s['keep_rate']}) → train:{s['train']} val:{s['val']}")


if __name__ == "__main__":
    main()
