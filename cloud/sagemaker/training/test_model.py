#!/usr/bin/env python3
"""测试训练好的 DINOv2 模型，用 bird-cropped.jpg 验证。"""

import json
import os
import sys

import torch
import torch.nn as nn
from torchvision import transforms
from PIL import Image

# DINOv2 配置（必须和训练时一致）
DINOV2_MODEL = "dinov2_vitl14"
IMAGE_SIZE = 518
EMBED_DIM = 1024

CHECKPOINT_DIR = "cloud/sagemaker/training/checkpoints"

# 中文名 + 属级映射
SPECIES_INFO = {
    "Azure-winged_Magpie": ("灰喜鹊", "灰喜鹊属"),
    "Barn_Swallow": ("家燕", "燕属"),
    "Black-collared_Starling": ("黑领椋鸟", "丽椋鸟属"),
    "Black_Drongo": ("黑卷尾", "卷尾属"),
    "Blue-fronted_Redstart": ("蓝额红尾鸲", "红尾鸲属"),
    "Blue_Jay": ("冠蓝鸦", "冠蓝鸦属"),
    "Brown_Shrike": ("红尾伯劳", "伯劳属"),
    "Chinese_Blackbird": ("乌鸫", "鸫属"),
    "Chinese_Hwamei": ("白颊噪鹛", "噪鹛属"),
    "Common_Kingfisher": ("普通翠鸟", "翠鸟属"),
    "Common_Myna": ("家八哥", "八哥属"),
    "Crested_Myna": ("八哥", "八哥属"),
    "Daurian_Redstart": ("北红尾鸲", "红尾鸲属"),
    "Eurasian_Jay": ("松鸦", "松鸦属"),
    "Eurasian_Magpie": ("喜鹊", "鹊属"),
    "Eurasian_Tree_Sparrow": ("麻雀", "麻雀属"),
    "Fork-tailed_Sunbird": ("叉尾太阳鸟", "太阳鸟属"),
    "Grey_Heron": ("苍鹭", "鹭属"),
    "Grey_Wagtail": ("灰鹡鸰", "鹡鸰属"),
    "Indian_Roller": ("棕胸佛法僧", "佛法僧属"),
    "Japanese_Tit": ("远东山雀", "山雀属"),
    "Large-billed_Crow": ("大嘴乌鸦", "鸦属"),
    "Light-vented_Bulbul": ("白头鹎", "鹎属"),
    "Little_Egret": ("白鹭", "白鹭属"),
    "Little_Grebe": ("小䴙䴘", "䴙䴘属"),
    "Long-tailed_Shrike": ("棕背伯劳", "伯劳属"),
    "Oriental_Magpie-Robin": ("鹊鸲", "鹊鸲属"),
    "Oriental_Turtle_Dove": ("山斑鸠", "斑鸠属"),
    "Plumbeous_Water_Redstart": ("红尾水鸲", "红尾鸲属"),
    "Red-billed_Blue_Magpie": ("红嘴蓝鹊", "蓝鹊属"),
    "Red-billed_Starling": ("丝光椋鸟", "椋鸟属"),
    "Red-whiskered_Bulbul": ("红耳鹎", "鹎属"),
    "Rock_Dove": ("原鸽", "鸽属"),
    "Spotted_Dove": ("珠颈斑鸠", "斑鸠属"),
    "Swinhoes_White-eye": ("暗绿绣眼鸟", "绣眼鸟属"),
    "Vinous-throated_Parrotbill": ("棕头鸦雀", "鸦雀属"),
    "White-cheeked_Starling": ("灰椋鸟", "椋鸟属"),
    "White-throated_Kingfisher": ("白胸翡翠", "翡翠属"),
    "White_Wagtail": ("白鹡鸰", "鹡鸰属"),
}


class DINOv2Classifier(nn.Module):
    def __init__(self, num_classes, freeze_backbone=True):
        super().__init__()
        self.backbone = torch.hub.load("facebookresearch/dinov2", DINOV2_MODEL)
        self.freeze_backbone = freeze_backbone
        if freeze_backbone:
            for param in self.backbone.parameters():
                param.requires_grad = False
            self.backbone.eval()
        self.head = nn.Sequential(
            nn.LayerNorm(EMBED_DIM),
            nn.Linear(EMBED_DIM, 512),
            nn.GELU(),
            nn.Dropout(0.3),
            nn.Linear(512, num_classes),
        )

    def forward(self, x):
        with torch.no_grad():
            features = self.backbone(x)
        return self.head(features)


def main():
    # 加载类别名
    with open(os.path.join(CHECKPOINT_DIR, "class_names.json")) as f:
        class_names = json.load(f)
    num_classes = len(class_names)
    print(f"Classes: {num_classes}")

    # 加载模型
    print("Loading DINOv2 model...")
    model = DINOv2Classifier(num_classes)
    checkpoint = torch.load(
        os.path.join(CHECKPOINT_DIR, "model.pth"),
        map_location="cpu",
        weights_only=False,
    )
    model.load_state_dict(checkpoint["model_state_dict"])
    model.eval()
    print(f"Best val accuracy: {checkpoint['best_val_acc']:.1f}%")

    # 图片预处理（和训练时的 val transform 一致）
    preprocess = transforms.Compose([
        transforms.Resize(IMAGE_SIZE + 32),
        transforms.CenterCrop(IMAGE_SIZE),
        transforms.ToTensor(),
        transforms.Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]),
    ])

    # 测试图片
    test_images = ["bird-cropped.jpg", "bird-sample.jpg", "bird-sample2.jpg", "person-sample.jpg"]

    for img_path in test_images:
        if not os.path.exists(img_path):
            print(f"\n{img_path}: 文件不存在，跳过")
            continue

        img = Image.open(img_path).convert("RGB")
        tensor = preprocess(img).unsqueeze(0)

        with torch.no_grad():
            logits = model(tensor)
            probs = torch.nn.functional.softmax(logits, dim=1)

        top5_probs, top5_indices = torch.topk(probs, k=min(5, num_classes), dim=1)

        top1_conf = top5_probs[0][0].item()
        CONFIDENCE_THRESHOLD = 0.20  # 低于 20% 判为 not_a_bird

        print(f"\n{img_path}:")
        if top1_conf < CONFIDENCE_THRESHOLD:
            print(f"  → 非鸟类 (top-1 置信度 {top1_conf*100:.1f}% < {CONFIDENCE_THRESHOLD*100:.0f}% 阈值)")
        else:
            for prob, idx in zip(top5_probs[0], top5_indices[0]):
                en_name = class_names[idx.item()]
                cn_name, genus = SPECIES_INFO.get(en_name, (en_name, "未知属"))
                print(f"  {cn_name}（{genus}）{' ' * max(0, 16-len(cn_name)-len(genus))} {prob.item()*100:.1f}%")


if __name__ == "__main__":
    main()
