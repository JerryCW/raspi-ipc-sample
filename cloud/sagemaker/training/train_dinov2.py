#!/usr/bin/env python3
"""Fine-tune DINOv2-ViT-L/14 for bird species classification.

Uses DINOv2 as a frozen feature extractor with a trainable classification
head. Training data is in ImageFolder format under data/train/ and data/val/.

Usage:
  python cloud/sagemaker/training/train_dinov2.py

Output:
  cloud/sagemaker/training/checkpoints/dinov2-bird-classifier/
"""

import json
import os
import time
from pathlib import Path

import torch
import torch.nn as nn
from torch.utils.data import DataLoader
from torchvision import datasets, transforms
from PIL import Image

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

DATA_DIR = Path("cloud/sagemaker/training/data")
CHECKPOINT_DIR = Path("cloud/sagemaker/training/checkpoints/dinov2-bird-classifier")
DEVICE = torch.device("mps" if torch.backends.mps.is_available() else "cpu")

# DINOv2-L/14 config
DINOV2_MODEL = "dinov2_vitl14"
IMAGE_SIZE = 518  # DINOv2-L native resolution
EMBED_DIM = 1024  # DINOv2-L output dimension

# Training hyperparameters
BATCH_SIZE = 16
NUM_EPOCHS = 30
LEARNING_RATE = 1e-3  # Higher LR since only training the head
WEIGHT_DECAY = 1e-4
LR_WARMUP_EPOCHS = 3
LR_MIN = 1e-6

# Feature extraction strategy
FREEZE_BACKBONE = True  # Phase 1: frozen backbone, train head only


# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------

class DINOv2Classifier(nn.Module):
    """DINOv2 backbone + classification head."""

    def __init__(self, num_classes: int, freeze_backbone: bool = True):
        super().__init__()
        # Load DINOv2-L from torch hub
        self.backbone = torch.hub.load("facebookresearch/dinov2", DINOV2_MODEL)

        if freeze_backbone:
            for param in self.backbone.parameters():
                param.requires_grad = False
            self.backbone.eval()

        # Classification head: LayerNorm → Linear → ReLU → Dropout → Linear
        self.head = nn.Sequential(
            nn.LayerNorm(EMBED_DIM),
            nn.Linear(EMBED_DIM, 512),
            nn.GELU(),
            nn.Dropout(0.3),
            nn.Linear(512, num_classes),
        )

    def forward(self, x):
        if FREEZE_BACKBONE:
            with torch.no_grad():
                features = self.backbone(x)
        else:
            features = self.backbone(x)
        return self.head(features)


# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------

def get_transforms(is_train: bool):
    """Get image transforms for training/validation."""
    if is_train:
        return transforms.Compose([
            transforms.RandomResizedCrop(IMAGE_SIZE, scale=(0.7, 1.0)),
            transforms.RandomHorizontalFlip(),
            transforms.ColorJitter(brightness=0.3, contrast=0.3, saturation=0.3, hue=0.1),
            transforms.RandomRotation(15),
            transforms.ToTensor(),
            transforms.Normalize(
                mean=[0.485, 0.456, 0.406],
                std=[0.229, 0.224, 0.225],
            ),
        ])
    else:
        return transforms.Compose([
            transforms.Resize(IMAGE_SIZE + 32),
            transforms.CenterCrop(IMAGE_SIZE),
            transforms.ToTensor(),
            transforms.Normalize(
                mean=[0.485, 0.456, 0.406],
                std=[0.229, 0.224, 0.225],
            ),
        ])


def load_datasets():
    """Load train and val datasets from ImageFolder."""
    train_dir = DATA_DIR / "train"
    val_dir = DATA_DIR / "val"

    assert train_dir.exists(), f"Train directory not found: {train_dir}"
    assert val_dir.exists(), f"Val directory not found: {val_dir}"

    train_dataset = datasets.ImageFolder(train_dir, transform=get_transforms(True))
    val_dataset = datasets.ImageFolder(val_dir, transform=get_transforms(False))

    print(f"Train: {len(train_dataset)} images, {len(train_dataset.classes)} classes")
    print(f"Val:   {len(val_dataset)} images, {len(val_dataset.classes)} classes")
    print(f"Classes: {train_dataset.classes[:5]}... (showing first 5)")

    return train_dataset, val_dataset


# ---------------------------------------------------------------------------
# Training
# ---------------------------------------------------------------------------

def train_one_epoch(model, loader, criterion, optimizer, device, epoch):
    """Train for one epoch."""
    model.train()
    if FREEZE_BACKBONE:
        model.backbone.eval()  # Keep backbone in eval mode

    total_loss = 0
    correct = 0
    total = 0

    for batch_idx, (images, labels) in enumerate(loader):
        images, labels = images.to(device), labels.to(device)

        optimizer.zero_grad()
        outputs = model(images)
        loss = criterion(outputs, labels)
        loss.backward()
        optimizer.step()

        total_loss += loss.item()
        _, predicted = outputs.max(1)
        total += labels.size(0)
        correct += predicted.eq(labels).sum().item()

        if (batch_idx + 1) % 10 == 0:
            print(
                f"  Epoch {epoch} [{batch_idx+1}/{len(loader)}] "
                f"Loss: {loss.item():.4f} Acc: {100.*correct/total:.1f}%"
            )

    avg_loss = total_loss / len(loader)
    accuracy = 100. * correct / total
    return avg_loss, accuracy


@torch.no_grad()
def evaluate(model, loader, criterion, device):
    """Evaluate on validation set."""
    model.eval()
    total_loss = 0
    correct = 0
    total = 0

    for images, labels in loader:
        images, labels = images.to(device), labels.to(device)
        outputs = model(images)
        loss = criterion(outputs, labels)

        total_loss += loss.item()
        _, predicted = outputs.max(1)
        total += labels.size(0)
        correct += predicted.eq(labels).sum().item()

    avg_loss = total_loss / len(loader)
    accuracy = 100. * correct / total
    return avg_loss, accuracy


def main():
    print(f"Device: {DEVICE}")
    print(f"DINOv2 model: {DINOV2_MODEL}")
    print(f"Freeze backbone: {FREEZE_BACKBONE}")
    print()

    # Load data
    train_dataset, val_dataset = load_datasets()
    num_classes = len(train_dataset.classes)

    train_loader = DataLoader(
        train_dataset, batch_size=BATCH_SIZE, shuffle=True,
        num_workers=4, pin_memory=True, drop_last=True,
    )
    val_loader = DataLoader(
        val_dataset, batch_size=BATCH_SIZE, shuffle=False,
        num_workers=4, pin_memory=True,
    )

    # Build model
    print(f"\nLoading DINOv2-L backbone...")
    model = DINOv2Classifier(num_classes, freeze_backbone=FREEZE_BACKBONE)
    model.to(DEVICE)

    trainable_params = sum(p.numel() for p in model.parameters() if p.requires_grad)
    total_params = sum(p.numel() for p in model.parameters())
    print(f"Trainable params: {trainable_params:,} / {total_params:,} ({100*trainable_params/total_params:.1f}%)")

    # Optimizer & scheduler (only for head parameters)
    optimizer = torch.optim.AdamW(
        model.head.parameters(),
        lr=LEARNING_RATE,
        weight_decay=WEIGHT_DECAY,
    )
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
        optimizer, T_max=NUM_EPOCHS, eta_min=LR_MIN,
    )
    criterion = nn.CrossEntropyLoss(label_smoothing=0.1)

    # Training loop
    best_val_acc = 0
    history = []

    print(f"\nStarting training: {NUM_EPOCHS} epochs, batch_size={BATCH_SIZE}")
    print("=" * 60)

    for epoch in range(1, NUM_EPOCHS + 1):
        start = time.time()

        train_loss, train_acc = train_one_epoch(
            model, train_loader, criterion, optimizer, DEVICE, epoch
        )
        val_loss, val_acc = evaluate(model, val_loader, criterion, DEVICE)
        scheduler.step()

        elapsed = time.time() - start
        lr = optimizer.param_groups[0]["lr"]

        print(
            f"Epoch {epoch}/{NUM_EPOCHS} ({elapsed:.0f}s) — "
            f"Train: loss={train_loss:.4f} acc={train_acc:.1f}% | "
            f"Val: loss={val_loss:.4f} acc={val_acc:.1f}% | "
            f"LR: {lr:.6f}"
        )

        history.append({
            "epoch": epoch,
            "train_loss": round(train_loss, 4),
            "train_acc": round(train_acc, 2),
            "val_loss": round(val_loss, 4),
            "val_acc": round(val_acc, 2),
            "lr": round(lr, 8),
        })

        # Save best model
        if val_acc > best_val_acc:
            best_val_acc = val_acc
            CHECKPOINT_DIR.mkdir(parents=True, exist_ok=True)
            torch.save({
                "epoch": epoch,
                "model_state_dict": model.head.state_dict(),
                "val_acc": val_acc,
                "num_classes": num_classes,
                "class_names": train_dataset.classes,
            }, CHECKPOINT_DIR / "best_head.pth")
            print(f"  ★ New best: {val_acc:.1f}%")

    # Save training history
    with open(CHECKPOINT_DIR / "history.json", "w") as f:
        json.dump(history, f, indent=2)

    # Save class mapping
    with open(CHECKPOINT_DIR / "class_names.json", "w") as f:
        json.dump(train_dataset.classes, f, indent=2)

    print()
    print("=" * 60)
    print(f"Training complete. Best val accuracy: {best_val_acc:.1f}%")
    print(f"Checkpoint: {CHECKPOINT_DIR / 'best_head.pth'}")
    print(f"History: {CHECKPOINT_DIR / 'history.json'}")


if __name__ == "__main__":
    main()
