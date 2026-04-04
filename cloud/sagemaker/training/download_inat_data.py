#!/usr/bin/env python3
"""Download bird species images from iNaturalist for DINOv2 fine-tuning.

Uses the iNaturalist API to download research-grade observation photos
for target bird species. Images are saved in ImageFolder format:
  data/train/<species_name>/001.jpg
  data/val/<species_name>/001.jpg

Usage:
  python cloud/sagemaker/training/download_inat_data.py
"""

import json
import os
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import requests
from PIL import Image
from io import BytesIO
from tqdm import tqdm

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Target species for fine-tuning.
# Focused on: Chongqing/China common birds + confusable species pairs
TARGET_SPECIES = {
    # --- 椋鸟科 (Sturnidae) — 核心区分目标 ---
    "Red-billed Starling": "Spodiopsar sericeus",
    "White-cheeked Starling": "Spodiopsar cineraceus",
    "Crested Myna": "Acridotheres cristatellus",
    "Common Myna": "Acridotheres tristis",
    "Black-collared Starling": "Gracupica nigricollis",

    # --- 重庆常见鸟类（用户确认的 20 种核心清单）---
    "Eurasian Tree Sparrow": "Passer montanus",
    "Light-vented Bulbul": "Pycnonotus sinensis",
    "Red-whiskered Bulbul": "Pycnonotus jocosus",
    "Swinhoe's White-eye": "Zosterops simplex",          # 暗绿绣眼鸟
    "Oriental Magpie-Robin": "Copsychus saularis",        # 鹊鸲
    "Spotted Dove": "Spilopelia chinensis",                # 珠颈斑鸠
    "Chinese Blackbird": "Turdus mandarinus",              # 乌鸫
    "Red-billed Blue Magpie": "Urocissa erythroryncha",   # 红嘴蓝鹊
    "Chinese Hwamei": "Pterorhinus sannio",                # 白颊噪鹛
    "Japanese Tit": "Parus minor",                         # 远东山雀
    "Vinous-throated Parrotbill": "Sinosuthora webbiana",  # 棕头鸦雀
    "Little Grebe": "Tachybaptus ruficollis",              # 小䴙䴘
    "Blue-fronted Redstart": "Phoenicurus frontalis",      # 蓝额红尾鸲
    "Eurasian Magpie": "Pica pica",                        # 喜鹊
    "Common Kingfisher": "Alcedo atthis",                  # 普通翠鸟
    "Daurian Redstart": "Phoenicurus auroreus",            # 北红尾鸲
    "Plumbeous Water Redstart": "Phoenicurus fuliginosus", # 红尾水鸲

    # --- 重庆其他常见鸟类 ---
    "Large-billed Crow": "Corvus macrorhynchos",
    "Black Drongo": "Dicrurus macrocercus",
    "White-throated Kingfisher": "Halcyon smyrnensis",
    "Fork-tailed Sunbird": "Aethopyga christinae",
    "Grey Heron": "Ardea cinerea",
    "Little Egret": "Egretta garzetta",
    "Barn Swallow": "Hirundo rustica",
    "White Wagtail": "Motacilla alba",
    "Grey Wagtail": "Motacilla cinerea",
    "Long-tailed Shrike": "Lanius schach",
    "Brown Shrike": "Lanius cristatus",
    "Azure-winged Magpie": "Cyanopica cyanus",
    "Oriental Turtle Dove": "Streptopelia orientalis",
    "Rock Dove": "Columba livia",

    # --- BioCLIP 容易混淆的物种 ---
    "Eurasian Jay": "Garrulus glandarius",
    "Indian Roller": "Coracias benghalensis",
    "Blue Jay": "Cyanocitta cristata",
}

IMAGES_PER_SPECIES = 1500  # 下载数量（清洗后再分 train/val）
TRAIN_RATIO = 0.8
OUTPUT_DIR = Path("cloud/sagemaker/training/data/raw")
IMAGE_SIZE = 518  # DINOv2-L 输入分辨率

# iNaturalist API
INAT_API_BASE = "https://api.inaturalist.org/v1"


def search_taxon_id(scientific_name: str) -> int | None:
    """Search iNaturalist for a taxon ID by scientific name."""
    resp = requests.get(
        f"{INAT_API_BASE}/taxa",
        params={"q": scientific_name, "rank": "species", "per_page": 5},
    )
    resp.raise_for_status()
    results = resp.json().get("results", [])
    for r in results:
        if r.get("name", "").lower() == scientific_name.lower():
            return r["id"]
    # Fallback: return first result if close match
    if results:
        return results[0]["id"]
    return None


def get_observation_photos(taxon_id: int, total: int = 1000) -> list[str]:
    """Get photo URLs for research-grade observations of a taxon.
    
    Handles pagination since iNaturalist API returns max 200 per request.
    """
    photo_urls = []
    page = 1
    per_page = 200  # iNaturalist API max

    while len(photo_urls) < total:
        resp = requests.get(
            f"{INAT_API_BASE}/observations",
            params={
                "taxon_id": taxon_id,
                "quality_grade": "research",
                "photos": "true",
                "per_page": per_page,
                "page": page,
                "order": "desc",
                "order_by": "votes",
            },
        )
        resp.raise_for_status()
        observations = resp.json().get("results", [])

        if not observations:
            break  # No more results

        for obs in observations:
            for photo in obs.get("photos", []):
                url = photo.get("url", "")
                if url:
                    url = url.replace("/square.", "/medium.")
                    photo_urls.append(url)
                if len(photo_urls) >= total:
                    break
            if len(photo_urls) >= total:
                break

        page += 1
        time.sleep(0.5)  # Rate limit between pages

    return photo_urls[:total]


def download_and_resize(url: str, save_path: Path, size: int = 518) -> bool:
    """Download an image, resize to square, and save as JPEG."""
    try:
        resp = requests.get(url, timeout=15)
        resp.raise_for_status()
        img = Image.open(BytesIO(resp.content)).convert("RGB")

        # Center crop to square, then resize
        w, h = img.size
        side = min(w, h)
        left = (w - side) // 2
        top = (h - side) // 2
        img = img.crop((left, top, left + side, top + side))
        img = img.resize((size, size), Image.LANCZOS)

        save_path.parent.mkdir(parents=True, exist_ok=True)
        img.save(save_path, "JPEG", quality=90)
        return True
    except Exception as e:
        print(f"    Failed to download {url}: {e}")
        return False


def main():
    print(f"Target species: {len(TARGET_SPECIES)}")
    print(f"Images per species: {IMAGES_PER_SPECIES}")
    print(f"Output: {OUTPUT_DIR}")
    print()

    stats = {"total": 0, "success": 0, "failed": 0, "species_ok": 0, "species_fail": 0}

    for common_name, scientific_name in TARGET_SPECIES.items():
        print(f"[{common_name}] ({scientific_name})")

        # 1. Find taxon ID
        taxon_id = search_taxon_id(scientific_name)
        if not taxon_id:
            print(f"  ✗ Taxon not found, skipping")
            stats["species_fail"] += 1
            continue
        print(f"  Taxon ID: {taxon_id}")

        # 2. Get photo URLs
        photo_urls = get_observation_photos(taxon_id, total=IMAGES_PER_SPECIES)
        print(f"  Found {len(photo_urls)} photos")

        if len(photo_urls) < 10:
            print(f"  ✗ Too few photos (<10), skipping")
            stats["species_fail"] += 1
            continue

        # 3. Download images (all to raw/, no train/val split)
        folder_name = common_name.replace(" ", "_").replace("'", "")

        # Build task list: (url, save_path) pairs
        tasks = []
        skipped = 0
        for i, url in enumerate(photo_urls):
            save_path = OUTPUT_DIR / folder_name / f"{i:04d}.jpg"
            if save_path.exists():
                skipped += 1
            else:
                tasks.append((url, save_path))

        if not tasks:
            print(f"  ✓ {len(photo_urls)}/{len(photo_urls)} images (all cached)")
            stats["species_ok"] += 1
            print()
            continue

        downloaded = skipped
        failed = 0
        pbar = tqdm(total=len(photo_urls), desc=f"  Downloading", unit="img", initial=skipped)

        with ThreadPoolExecutor(max_workers=4) as executor:
            futures = {
                executor.submit(download_and_resize, url, path, IMAGE_SIZE): (url, path)
                for url, path in tasks
            }
            for future in as_completed(futures):
                if future.result():
                    downloaded += 1
                    stats["success"] += 1
                else:
                    failed += 1
                    stats["failed"] += 1
                stats["total"] += 1
                pbar.update(1)
                pbar.set_postfix(ok=downloaded, fail=failed)

        pbar.close()
        print(f"  ✓ {downloaded}/{len(photo_urls)} images (skipped {skipped}, failed {failed})")
        stats["species_ok"] += 1
        print()

    # Summary
    print("=" * 50)
    print(f"Species OK: {stats['species_ok']}/{len(TARGET_SPECIES)}")
    print(f"Species failed: {stats['species_fail']}")
    print(f"Images downloaded: {stats['success']}")
    print(f"Images failed: {stats['failed']}")
    print(f"Output directory: {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
