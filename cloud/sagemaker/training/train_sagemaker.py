#!/usr/bin/env python3
"""DINOv2 fine-tune 训练脚本（SageMaker 版本）

=== 这个脚本做什么？===
把 DINOv2（一个已经学会"看图"的大模型）改造成鸟类分类器。
DINOv2 是"眼睛"（backbone），我们在它上面接一个"大脑"（classification head），
然后用鸟类照片教这个"大脑"认鸟。

=== SageMaker 怎么运行这个脚本？===
SageMaker 会：
1. 启动一台带 GPU 的云服务器（ml.g4dn.xlarge）
2. 安装 PyTorch 等依赖（用预置的 Docker 镜像）
3. 从 S3 下载训练数据到 /opt/ml/input/data/training/
4. 运行这个脚本：python train_sagemaker.py --epochs 30 --batch-size 16 --lr 0.001
5. 脚本把训练好的模型保存到 /opt/ml/model/
6. SageMaker 自动把 /opt/ml/model/ 打包上传回 S3
7. 关闭服务器（停止计费）

整个过程你只需要运行 launch_training.py，其他全自动。

=== 超参数说明 ===
--epochs:           训练轮数，每轮把所有图片看一遍（默认 30）
--batch-size:       每次同时处理的图片数（默认 16）
--lr:               学习率，控制每次参数更新的步长（默认 0.001）
--weight-decay:     权重衰减，防止参数过大导致过拟合（默认 0.0001）
--freeze-backbone:  是否冻结 DINOv2 backbone（1=冻结只训练 head，0=全部训练）
"""

import argparse
import json
import os
import time
from pathlib import Path

import torch
import torch.nn as nn
from torch.utils.data import DataLoader
from torchvision import datasets, transforms


# ---------------------------------------------------------------------------
# SageMaker 环境变量
# ---------------------------------------------------------------------------
# SageMaker 启动训练时会自动设置这些环境变量。
# 本地调试时这些环境变量不存在，所以用默认值兜底。

# SM_CHANNEL_TRAINING: 训练数据目录
#   SageMaker 上 = /opt/ml/input/data/training/（S3 数据自动下载到这里）
#   本地调试时 = cloud/sagemaker/training/data
SM_CHANNEL_TRAINING = os.environ.get("SM_CHANNEL_TRAINING", "cloud/sagemaker/training/data")

# SM_MODEL_DIR: 模型输出目录
#   SageMaker 上 = /opt/ml/model/（训练完自动打包上传到 S3）
#   本地调试时 = cloud/sagemaker/training/checkpoints/dinov2-sagemaker
SM_MODEL_DIR = os.environ.get("SM_MODEL_DIR", "cloud/sagemaker/training/checkpoints/dinov2-sagemaker")

# SM_CHECKPOINT_DIR: checkpoint 目录（实时同步到 S3，Spot 中断也不丢）
#   SageMaker 上 = /opt/ml/checkpoints/
#   本地调试时 = cloud/sagemaker/training/checkpoints/dinov2-sagemaker
SM_CHECKPOINT_DIR = os.environ.get("SM_CHECKPOINT_DIR", "cloud/sagemaker/training/checkpoints/dinov2-sagemaker")

# SM_CHECKPOINT_DIR: checkpoint 目录
#   SageMaker 上 = /opt/ml/checkpoints/（实时同步到 S3，Spot 中断也不丢）
#   本地调试时 = cloud/sagemaker/training/checkpoints/dinov2-sagemaker
SM_CHECKPOINT_DIR = os.environ.get("SM_CHECKPOINT_DIR", "cloud/sagemaker/training/checkpoints/dinov2-sagemaker")

# SM_NUM_GPUS: GPU 数量（ml.g4dn.xlarge 有 1 块 T4）
SM_NUM_GPUS = int(os.environ.get("SM_NUM_GPUS", "0"))

# --- DINOv2 模型配置 ---
# dinov2_vitl14 = DINOv2 Large 版本，patch size 14
# 输入 518×518 图片 → 输出 1024 维特征向量
DINOV2_MODEL = "dinov2_vitl14"
IMAGE_SIZE = 518   # DINOv2-L 的原生输入分辨率（比常见的 224 大很多，能看到更多细节）
EMBED_DIM = 1024   # DINOv2-L 输出的特征向量维度


# ---------------------------------------------------------------------------
# 模型定义
# ---------------------------------------------------------------------------

class DINOv2Classifier(nn.Module):
    """DINOv2 backbone（眼睛）+ 分类头（大脑）

    工作流程：
    1. 图片 (518×518) → DINOv2 backbone → 1024 维特征向量
       （这一步提取图片的视觉特征：颜色、纹理、形状等）
    2. 1024 维向量 → 分类头 → 39 个分数（每种鸟一个）
       （这一步判断这些特征最像哪种鸟）
    3. 取分数最高的那种鸟作为预测结果
    """

    def __init__(self, num_classes: int, freeze_backbone: bool = True):
        super().__init__()

        # --- 加载 DINOv2 backbone ---
        # torch.hub.load 会从 GitHub 下载模型代码和权重（约 1.2GB）
        # SageMaker 实例有网络连接，可以直接下载
        # 首次下载后会缓存，后续不需要重新下载
        print(f"Loading DINOv2 backbone: {DINOV2_MODEL}")
        self.backbone = torch.hub.load("facebookresearch/dinov2", DINOV2_MODEL)
        self.freeze_backbone = freeze_backbone

        if freeze_backbone:
            # 冻结 backbone：不更新 DINOv2 的 3 亿参数
            # 好处：训练快（只训练 head 的 ~55 万参数），不容易过拟合
            # 坏处：精度有天花板（backbone 的特征表示是固定的）
            for param in self.backbone.parameters():
                param.requires_grad = False  # 告诉 PyTorch 不要计算这些参数的梯度
            self.backbone.eval()  # 关闭 Dropout 和 BatchNorm 的训练行为
            print("Backbone frozen (only training classification head)")

        # --- 分类头 ---
        # 这是我们唯一训练的部分（冻结 backbone 时）
        self.head = nn.Sequential(
            # LayerNorm: 归一化输入，让训练更稳定
            nn.LayerNorm(EMBED_DIM),

            # Linear: 全连接层，1024 → 512（降维，提取关键特征）
            nn.Linear(EMBED_DIM, 512),

            # GELU: 激活函数（给网络引入非线性，否则多层 Linear 等于一层）
            nn.GELU(),

            # Dropout: 训练时随机丢弃 30% 的神经元
            # 防止过拟合：强迫网络不依赖任何单个特征
            nn.Dropout(0.3),

            # Linear: 全连接层，512 → num_classes（输出每种鸟的分数）
            nn.Linear(512, num_classes),
        )

        # 打印参数统计
        trainable = sum(p.numel() for p in self.parameters() if p.requires_grad)
        total = sum(p.numel() for p in self.parameters())
        print(f"Parameters: {trainable:,} trainable / {total:,} total ({100*trainable/total:.2f}%)")

    def forward(self, x):
        """前向传播：图片 → 特征 → 分类分数

        Args:
            x: 图片张量，shape = (batch_size, 3, 518, 518)
               3 = RGB 三个颜色通道

        Returns:
            分类分数，shape = (batch_size, num_classes)
            每个值代表"这张图是第 i 种鸟"的可能性（未归一化的 logit）
        """
        if self.freeze_backbone:
            # torch.no_grad(): 不计算梯度，节省内存和计算时间
            with torch.no_grad():
                features = self.backbone(x)  # (batch, 1024)
        else:
            features = self.backbone(x)
        return self.head(features)  # (batch, num_classes)


# ---------------------------------------------------------------------------
# 数据加载
# ---------------------------------------------------------------------------

def get_transforms(is_train: bool):
    """获取图片预处理流水线。

    训练时：随机裁剪、翻转、调色、旋转（数据增强，防止过拟合）
    验证时：固定 resize + 中心裁剪（保证评估结果可复现）

    最后都要 ToTensor（PIL Image → PyTorch Tensor）和 Normalize（标准化到 ImageNet 均值/方差）。
    Normalize 的值 [0.485, 0.456, 0.406] 是 ImageNet 数据集的 RGB 均值，
    DINOv2 在 ImageNet 上预训练，所以我们要用同样的标准化。
    """
    if is_train:
        return transforms.Compose([
            # 随机裁剪到 518×518，裁剪范围是原图的 70%-100%
            # 效果：每次看到的鸟在图片中的位置和大小都不同
            transforms.RandomResizedCrop(IMAGE_SIZE, scale=(0.7, 1.0)),

            # 50% 概率水平翻转
            # 效果：鸟朝左和朝右都能认
            transforms.RandomHorizontalFlip(),

            # 随机调整亮度、对比度、饱和度、色调
            # 效果：不同光线条件下都能认
            transforms.ColorJitter(brightness=0.3, contrast=0.3, saturation=0.3, hue=0.1),

            # 随机旋转 ±15 度
            # 效果：鸟歪着头也能认
            transforms.RandomRotation(15),

            # PIL Image → PyTorch Tensor，像素值从 [0, 255] 变成 [0.0, 1.0]
            transforms.ToTensor(),

            # 标准化：减去均值，除以标准差（匹配 DINOv2 预训练时的数据分布）
            transforms.Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]),
        ])
    else:
        return transforms.Compose([
            # 验证集：固定操作，不做随机增强
            transforms.Resize(IMAGE_SIZE + 32),    # 先放大一点
            transforms.CenterCrop(IMAGE_SIZE),      # 再中心裁剪到 518×518
            transforms.ToTensor(),
            transforms.Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]),
        ])


def load_data(data_dir: str, batch_size: int, max_samples_per_class: int = 0):
    """加载 ImageFolder 格式的训练和验证数据。

    ImageFolder 是 PyTorch 的标准数据格式：
      data_dir/
        train/
          Red-billed_Starling/    ← 文件夹名 = 类别名
            0000.jpg              ← 这种鸟的第 1 张图
            0001.jpg
            ...
          White-cheeked_Starling/
            0000.jpg
            ...
        val/
          Red-billed_Starling/
            ...

    PyTorch 会自动：
    - 扫描所有子文件夹，每个文件夹 = 一个类别
    - 按文件夹名字母排序，分配类别编号（0, 1, 2, ...）
    - 加载图片时自动应用 transforms

    Args:
        max_samples_per_class: 每种鸟最多用多少张训练图片（0=不限制）
    """
    train_dir = os.path.join(data_dir, "train")
    val_dir = os.path.join(data_dir, "val")

    print(f"Loading training data from: {train_dir}")
    print(f"Loading validation data from: {val_dir}")

    train_dataset = datasets.ImageFolder(train_dir, transform=get_transforms(True))
    val_dataset = datasets.ImageFolder(val_dir, transform=get_transforms(False))

    # 限制每种鸟的训练样本数
    if max_samples_per_class > 0:
        from collections import defaultdict
        class_counts = defaultdict(int)
        filtered_indices = []
        for idx, (_, label) in enumerate(train_dataset.samples):
            if class_counts[label] < max_samples_per_class:
                filtered_indices.append(idx)
                class_counts[label] += 1

        original_size = len(train_dataset)
        train_dataset = torch.utils.data.Subset(train_dataset, filtered_indices)
        print(f"Max samples per class: {max_samples_per_class} "
              f"(filtered {original_size} → {len(train_dataset)})")

    num_classes = len(datasets.ImageFolder(train_dir).classes)
    class_names = datasets.ImageFolder(train_dir).classes
    print(f"Found {len(train_dataset)} training images")
    print(f"Found {len(val_dataset)} validation images")
    print(f"Number of classes: {num_classes}")
    print(f"Classes: {class_names}")

    # DataLoader: 负责把数据分成 batch，送给模型
    train_loader = DataLoader(
        train_dataset,
        batch_size=batch_size,  # 每次取 16 张图
        shuffle=True,           # 每个 epoch 打乱顺序（防止模型记住顺序）
        num_workers=4,          # 4 个子进程并行加载图片（加速）
        pin_memory=True,        # 预先把数据放到 GPU 可访问的内存（加速 GPU 传输）
        drop_last=True,         # 丢弃最后不足 batch_size 的数据（保持 batch 大小一致）
    )
    val_loader = DataLoader(
        val_dataset,
        batch_size=batch_size,
        shuffle=False,          # 验证集不需要打乱
        num_workers=4,
        pin_memory=True,
    )

    return train_loader, val_loader, class_names, num_classes


# ---------------------------------------------------------------------------
# 训练循环
# ---------------------------------------------------------------------------

def train_one_epoch(model, loader, criterion, optimizer, device, epoch, grad_accum_steps=1):
    """训练一个 epoch（把所有训练图片看一遍）。

    支持梯度累积：实际 batch_size 较小以节省显存，
    累积 grad_accum_steps 步梯度后再更新参数，等效大 batch。
    """
    model.train()
    if hasattr(model, 'freeze_backbone') and model.freeze_backbone:
        model.backbone.eval()

    total_loss = 0
    correct = 0
    total = 0

    optimizer.zero_grad()

    for batch_idx, (images, labels) in enumerate(loader):
        images, labels = images.to(device), labels.to(device)

        outputs = model(images)
        loss = criterion(outputs, labels)
        loss = loss / grad_accum_steps  # 梯度累积时需要平均 loss

        loss.backward()

        # 每 grad_accum_steps 步更新一次参数
        if (batch_idx + 1) % grad_accum_steps == 0 or (batch_idx + 1) == len(loader):
            optimizer.step()
            optimizer.zero_grad()

        total_loss += loss.item() * grad_accum_steps  # 还原真实 loss 用于统计
        _, predicted = outputs.max(1)
        total += labels.size(0)
        correct += predicted.eq(labels).sum().item()

        if (batch_idx + 1) % (10 * grad_accum_steps) == 0:
            print(f"  Epoch {epoch} [{batch_idx+1}/{len(loader)}] "
                  f"Loss: {loss.item()*grad_accum_steps:.4f} Acc: {100.*correct/total:.1f}%")

    avg_loss = total_loss / len(loader)
    accuracy = 100. * correct / total
    return avg_loss, accuracy


@torch.no_grad()  # 验证时不需要计算梯度（节省内存和时间）
def evaluate(model, loader, criterion, device):
    """在验证集上评估模型。

    和训练的区别：
    - 不计算梯度（@torch.no_grad）
    - 不更新参数
    - 不做数据增强（验证集用固定的 resize + center crop）
    - model.eval() 关闭 Dropout

    验证准确率才是模型真实水平的体现。
    """
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


# ---------------------------------------------------------------------------
# 主函数
# ---------------------------------------------------------------------------

def main():
    # --- 解析超参数 ---
    # SageMaker 把 hyperparameters 转成命令行参数传入
    # 例如 {"epochs": 30, "batch-size": 16} → --epochs 30 --batch-size 16
    parser = argparse.ArgumentParser()
    parser.add_argument("--epochs", type=int, default=30,
                        help="训练轮数（每轮看一遍所有图片）")
    parser.add_argument("--batch-size", type=int, default=16,
                        help="每次处理的图片数")
    parser.add_argument("--lr", type=float, default=1e-3,
                        help="学习率（参数更新步长）")
    parser.add_argument("--weight-decay", type=float, default=1e-4,
                        help="权重衰减（L2 正则化，防止过拟合）")
    parser.add_argument("--freeze-backbone", type=int, default=1,
                        help="1=冻结 backbone 只训练 head，0=全部训练")
    parser.add_argument("--max-samples-per-class", type=int, default=0,
                        help="每种鸟最多用多少张训练图片（0=不限制，用全部）")
    parser.add_argument("--grad-accum-steps", type=int, default=1,
                        help="梯度累积步数（等效 batch = batch-size × grad-accum-steps）")
    args = parser.parse_args()

    print("=" * 60)
    print("DINOv2 Bird Classifier — SageMaker Training Job")
    print("=" * 60)
    print(f"Hyperparameters:")
    print(f"  epochs:          {args.epochs}")
    print(f"  batch_size:      {args.batch_size}")
    print(f"  learning_rate:   {args.lr}")
    print(f"  weight_decay:    {args.weight_decay}")
    print(f"  freeze_backbone: {bool(args.freeze_backbone)}")
    print(f"Environment:")
    print(f"  data_dir:   {SM_CHANNEL_TRAINING}")
    print(f"  model_dir:  {SM_MODEL_DIR}")
    print(f"  num_gpus:   {SM_NUM_GPUS}")
    print()

    # --- 选择计算设备 ---
    # SageMaker ml.g4dn.xlarge 有 1 块 NVIDIA T4 GPU
    # torch.cuda.is_available() 在有 GPU 时返回 True
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Device: {device}")
    if torch.cuda.is_available():
        print(f"GPU: {torch.cuda.get_device_name(0)}")
        print(f"GPU Memory: {torch.cuda.get_device_properties(0).total_memory / 1e9:.1f} GB")

    # --- 加载数据 ---
    train_loader, val_loader, class_names, num_classes = load_data(
        SM_CHANNEL_TRAINING, args.batch_size, args.max_samples_per_class
    )

    # --- 构建模型 ---
    model = DINOv2Classifier(num_classes, freeze_backbone=bool(args.freeze_backbone))
    model.to(device)  # 把模型搬到 GPU 上

    # --- 优化器 ---
    # AdamW: 目前最常用的优化器，自适应学习率 + 权重衰减
    if args.freeze_backbone:
        # 冻结 backbone 时：只优化 head 的参数
        params = model.head.parameters()
    else:
        # 解冻 backbone 时：backbone 用小 LR（0.00001），head 用大 LR（0.001）
        # 因为 backbone 已经训练好了，只需要微调，步子不能太大
        params = [
            {"params": model.backbone.parameters(), "lr": args.lr * 0.01},
            {"params": model.head.parameters(), "lr": args.lr},
        ]

    optimizer = torch.optim.AdamW(params, lr=args.lr, weight_decay=args.weight_decay)

    # --- 学习率调度器 ---
    # CosineAnnealing: 学习率从 lr 逐渐降低到 eta_min，像余弦曲线一样平滑下降
    # 前期大步快走，后期小步精调
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
        optimizer, T_max=args.epochs, eta_min=1e-6
    )

    # --- 损失函数 ---
    # CrossEntropyLoss: 分类任务的标准损失函数
    # label_smoothing=0.1: 不要求模型 100% 确定，允许 90% 就行
    #   防止模型过度自信（对训练集 99% 确定但对新图片不确定）
    criterion = nn.CrossEntropyLoss(label_smoothing=0.1)

    # --- 训练循环 ---
    best_val_acc = 0  # 记录最佳验证准确率
    history = []      # 记录每个 epoch 的指标
    start_epoch = 1   # 默认从第 1 个 epoch 开始

    # --- 尝试从 checkpoint 恢复（Spot 中断后重启时用）---
    checkpoint_path = os.path.join(SM_CHECKPOINT_DIR, "checkpoint.pth")
    if os.path.exists(checkpoint_path):
        print(f"发现 checkpoint，恢复训练...")
        ckpt = torch.load(checkpoint_path, map_location=device, weights_only=False)
        model.load_state_dict(ckpt["model_state_dict"])
        optimizer.load_state_dict(ckpt["optimizer_state_dict"])
        scheduler.load_state_dict(ckpt["scheduler_state_dict"])
        start_epoch = ckpt["current_epoch"] + 1
        best_val_acc = ckpt["best_val_acc"]
        history = ckpt.get("history", [])
        print(f"从 Epoch {start_epoch} 恢复，之前最佳 Val: {best_val_acc:.1f}%")

    print(f"\nStarting training...")
    print("=" * 60)

    for epoch in range(start_epoch, args.epochs + 1):
        start = time.time()

        # 训练一个 epoch
        train_loss, train_acc = train_one_epoch(
            model, train_loader, criterion, optimizer, device, epoch, args.grad_accum_steps
        )

        # 在验证集上评估
        val_loss, val_acc = evaluate(model, val_loader, criterion, device)

        # 更新学习率
        scheduler.step()

        elapsed = time.time() - start
        lr = optimizer.param_groups[0]["lr"]

        # 打印本轮结果（这些日志会出现在 CloudWatch 中）
        print(f"Epoch {epoch}/{args.epochs} ({elapsed:.0f}s) — "
              f"Train: loss={train_loss:.4f} acc={train_acc:.1f}% | "
              f"Val: loss={val_loss:.4f} acc={val_acc:.1f}% | "
              f"LR: {lr:.6f}")

        history.append({
            "epoch": epoch,
            "train_loss": round(train_loss, 4),
            "train_acc": round(train_acc, 2),
            "val_loss": round(val_loss, 4),
            "val_acc": round(val_acc, 2),
            "lr": round(lr, 8),
        })

        # 记录最佳模型，每个 epoch 都保存 checkpoint
        if val_acc > best_val_acc:
            best_val_acc = val_acc
            print(f"  ★ New best: {val_acc:.1f}%")

        # --- 每个 epoch 结束后保存 checkpoint 到 /opt/ml/checkpoints/ ---
        # SageMaker 会实时同步这个目录到 S3，即使 Spot 中断也不丢
        os.makedirs(SM_CHECKPOINT_DIR, exist_ok=True)
        checkpoint_path = os.path.join(SM_CHECKPOINT_DIR, "checkpoint.pth")
        torch.save({
            "model_state_dict": model.state_dict(),
            "head_state_dict": model.head.state_dict(),
            "optimizer_state_dict": optimizer.state_dict(),
            "scheduler_state_dict": scheduler.state_dict(),
            "num_classes": num_classes,
            "class_names": class_names,
            "best_val_acc": best_val_acc,
            "current_epoch": epoch,
            "val_acc": val_acc,
            "history": history,
            "hyperparameters": vars(args),
        }, checkpoint_path)
        print(f"  Checkpoint saved (epoch {epoch}, val_acc={val_acc:.1f}%)")

    # -----------------------------------------------------------------------
    # 保存模型到 SM_MODEL_DIR
    # SageMaker 会自动把这个目录打包成 model.tar.gz 上传到 S3
    # -----------------------------------------------------------------------
    os.makedirs(SM_MODEL_DIR, exist_ok=True)

    # 保存模型权重
    # 包含 backbone + head 的完整权重，方便后续部署
    model_path = os.path.join(SM_MODEL_DIR, "model.pth")
    torch.save({
        "model_state_dict": model.state_dict(),       # 完整模型权重
        "head_state_dict": model.head.state_dict(),    # 只有 head 的权重（备用）
        "num_classes": num_classes,                     # 类别数（39）
        "class_names": class_names,                     # 类别名列表
        "best_val_acc": best_val_acc,                   # 最佳验证准确率
        "hyperparameters": vars(args),                  # 训练超参数（可复现）
    }, model_path)
    print(f"\nModel saved to: {model_path}")

    # 保存类别名映射（部署推理时需要：编号 → 鸟名）
    class_names_path = os.path.join(SM_MODEL_DIR, "class_names.json")
    with open(class_names_path, "w") as f:
        json.dump(class_names, f, indent=2)

    # 保存训练历史（用于画 loss/accuracy 曲线，分析训练过程）
    history_path = os.path.join(SM_MODEL_DIR, "history.json")
    with open(history_path, "w") as f:
        json.dump(history, f, indent=2)

    print(f"Class names saved to: {class_names_path}")
    print(f"Training history saved to: {history_path}")
    print()
    print("=" * 60)
    print(f"Training complete!")
    print(f"  Best validation accuracy: {best_val_acc:.1f}%")
    print(f"  Model output: {SM_MODEL_DIR}")
    print("=" * 60)


if __name__ == "__main__":
    main()
