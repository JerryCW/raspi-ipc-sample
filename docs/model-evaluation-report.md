# 鸟类分类模型评估报告

## 项目背景

树莓派智能摄像头项目，端侧 YOLO 检测到鸟类后裁剪 bbox 截图上传 S3，触发 Lambda 调用 SageMaker 端点进行鸟类物种分类。目标是准确识别重庆地区常见鸟类物种。

## 测试图片

| 图片 | 内容 | 来源 |
|------|------|------|
| `bird-cropped.jpg` | 丝光椋鸟 (Red-billed Starling)，重庆阳台拍摄 | 端侧 YOLO bbox 裁剪 |
| `person-sample.jpg` | 人物照片 | 用于 OOD 检测测试 |

---

## 阶段一：EfficientNetB2（基线模型）

### 模型信息

- 模型：`dennisjooo/Birds-Classifier-EfficientNetB2`
- 架构：EfficientNetB2，固定分类头
- 训练数据：CUB-525 北美鸟类数据集
- 物种数：525 种（全部北美鸟类）
- 模型大小：136MB
- 推理方式：传统分类（logits → softmax → top-3）
- OOD 检测：max_logit < 8.0 → not_a_bird

### 已知问题

- 525 种全部是北美鸟类，无法识别亚洲鸟类（丝光椋鸟、灰椋鸟等不在训练集中）
- 对亚洲鸟类只能给出最接近的北美物种名，结果无意义

### 测试结果

未对 `bird-cropped.jpg` 做详细记录（模型已被替换）。

---

## 阶段二：BioCLIP 零样本推理（当前模型）

### 模型信息

- 模型：`imageomics/bioclip`
- 架构：ViT-B/16 CLIP
- 训练数据：TreeOfLife-10M（1000 万张生物图像，覆盖 45 万物种）
- 模型大小：599MB
- 推理方式：零样本 CLIP（图像嵌入 × 文本嵌入 → 余弦相似度 × 100 → softmax → top-3）
- OOD 检测：max cosine similarity < 0.18 → not_a_bird
- 标签列表：`bird_labels.json`，305 种鸟类 + 20 个负类

### 2.1 初始测试（305 种鸟类，无负类）

配置：305 种鸟类标签，OOD 阈值 0.18

#### bird-cropped.jpg（丝光椋鸟）

| 排名 | 物种 | confidence | similarity |
|------|------|-----------|------------|
| 1 | Eurasian Jay | 0.3251 | 0.2947 |
| 2 | Grey Heron | 0.1882 | 0.2893 |
| 3 | Indian Roller | 0.1409 | 0.2864 |

**结论：识别错误。** 真实物种 Red-billed Starling 未进入 top-3。

#### person-sample.jpg（人物）

| 排名 | 物种 | confidence | similarity |
|------|------|-----------|------------|
| 1 | Great Blue Heron | 0.1800 | 0.1875 |
| 2 | Blue Jay | 0.1331 | 0.1845 |
| 3 | White-cheeked Starling | 0.0878 | 0.1803 |

**结论：OOD 检测失败。** max similarity 0.1875 > 阈值 0.18，人物照片被错误识别为鸟类。

### 2.2 负类拒绝机制（305 种鸟类 + 20 负类）

改动：在 `bird_labels.json` 中新增 `negative_classes` 字段，包含 20 个非鸟类别（person, cat, dog, car 等）。推理时如果 top-1 命中负类，返回 `not_a_bird`。

原理：CLIP 模型同时对比图像与所有文本描述（鸟类 + 非鸟类），如果图片更像 "a photo of a person" 而不是任何鸟类描述，就主动拒绝。

#### person-sample.jpg（人物）— 加入负类后

| 类别 | similarity |
|------|------------|
| NOT_BIRD:person | 0.2340 |
| NOT_BIRD:cat | 0.2154 |
| NOT_BIRD:tree | 0.2104 |
| 最高鸟类 (Great Blue Heron) | 0.1875 |

**结论：OOD 检测成功。** "person" 的 similarity (0.234) 远高于任何鸟类 (0.188)，人物照片被正确拒绝。

#### bird-cropped.jpg（丝光椋鸟）— 加入负类后

| 排名 | 物种 | confidence | similarity |
|------|------|-----------|------------|
| 1 | Eurasian Jay | 0.3251 | 0.2947 |
| 2 | Grey Heron | 0.1882 | 0.2893 |
| 3 | Indian Roller | 0.1409 | 0.2864 |

**结论：鸟类识别不受负类影响，但物种仍然识别错误。**

### 2.3 标签数量对准确率的影响

为了理解为什么 305 种标签下识别错误，做了对比实验：

#### 实验 A：5 种标签（模拟 demo 场景）

标签：White-cheeked Starling, Red-billed Starling, Eurasian Jay, Blue Jay, House Sparrow

| 排名 | 物种 | similarity | confidence |
|------|------|-----------|------------|
| 1 | Eurasian Jay | 0.2947 | 0.9712 |
| 2 | Blue Jay | 0.2551 | 0.0184 |
| 3 | White-cheeked Starling | 0.2493 | 0.0104 |
| 4 | Red-billed Starling | 0.1999 | 0.0001 |

**结论：** 即使只有 5 个标签，Eurasian Jay 仍然排第一。说明 BioCLIP 对这张图的特征提取本身就偏向松鸦。

#### 实验 B：仅椋鸟家族（9 种）

| 排名 | 物种 | similarity | confidence |
|------|------|-----------|------------|
| 1 | White-cheeked Starling | 0.2493 | 0.8716 |
| 2 | European Starling | 0.2294 | 0.1184 |
| 3 | Red-billed Starling | 0.1999 | 0.0062 |

**结论：** 在椋鸟家族内部，White-cheeked Starling 排第一（87%），Red-billed Starling 只有 0.6%。BioCLIP 无法区分这两种近缘物种。

#### 实验 C：加入学名

使用 "Red-billed Starling (Spodiopsar sericeus)" 格式的标签：

| 排名 | 物种 | similarity | confidence |
|------|------|-----------|------------|
| 1 | Eurasian Jay (Garrulus glandarius) | 0.2645 | 0.3393 |
| 2 | Grey Heron (Ardea cinerea) | 0.2626 | 0.2804 |
| 3 | White-cheeked Starling (Spodiopsar cineraceus) | 0.2563 | 0.1501 |
| 4 | Red-billed Starling (Spodiopsar sericeus) | 0.2555 | 0.1373 |

**结论：** 加入学名后 Red-billed 和 White-cheeked 的 similarity 差距缩小（0.2555 vs 0.2563），但仍然无法正确识别。

### 2.4 关键发现

1. **BioCLIP 的 similarity score 是稳定的**：无论标签列表大小如何变化，同一物种的 similarity 不变（Eurasian Jay 始终 ~0.295）。变化的只是 softmax confidence（分母变大，概率被稀释）。

2. **BioCLIP 对丝光椋鸟的特征提取存在偏差**：Red-billed Starling 的 similarity (0.200) 远低于 Eurasian Jay (0.295)，差距 47%。这不是标签数量的问题，而是模型本身的特征表示问题。

3. **负类拒绝机制有效**：通过加入非鸟类别，可以可靠区分"是不是鸟"，不依赖脆弱的阈值。person 的 similarity (0.234) 与最高鸟类 (0.188) 之间有 24% 的差距。

4. **零样本推理的天花板**：BioCLIP 基于 CLIP 对比学习，擅长"广度"（是鸟还是人），但在细粒度物种区分（丝光椋鸟 vs 灰椋鸟 vs 松鸦）上能力有限。

---

## 阶段三：下一步方向（待实施）

### 方案 A：精简标签列表（零成本）

将 305 种鸟类精简到目标地区（重庆/中国西南）常见的 50-80 种。减少不相关物种的干扰，可能提升 top-1 准确率。

预期收益：低。从实验 A 看，即使只有 5 个标签，BioCLIP 仍然把丝光椋鸟识别为松鸦。问题在模型特征提取层面，不在标签数量。

### 方案 B：Fine-tune 预训练模型（推荐）

使用 iNaturalist 鸟类子集（公开数据集，已有数百万张标注图片）对 ConvNeXt-V2 或 EfficientNet-V2 做 fine-tune。

- 训练数据：iNaturalist 2021 鸟类子集（~1500 种，每种数百张）
- 训练方式：SageMaker Training Job + Spot Instance
- 预期成本：几美元（Spot ml.g4dn.xlarge）
- 预期收益：高。有监督训练在细粒度分类上远优于零样本推理。

### 方案 C：两阶段流水线

阶段 1：BioCLIP 负类拒绝（当前已实现）→ 确认是鸟
阶段 2：Fine-tuned 分类模型 → 精确识别物种

可以复用当前 SageMaker Serverless 端点架构，在 Lambda 中串联两次调用。

---

## 变更时间线

| 日期 | 变更 | commit |
|------|------|--------|
| 2026-04-03 | EfficientNetB2 → BioCLIP 零样本推理 | `5db1cfd` |
| 2026-04-03 | 端侧 bbox 裁剪（鸟类检测发送特写而非整帧） | `e0edf1c` |
| 2026-04-03 | 负类拒绝机制（person/cat/dog 等 20 个负类） | `7675655` |

## 当前架构

```
端侧 (Raspberry Pi 5)                    云端 (AWS)
┌─────────────────────┐                  ┌──────────────────────────────┐
│ YOLO11n (Python)    │                  │ S3: bird screenshot          │
│ 检测到鸟 → bbox裁剪 │ ──── S3 ────→   │         ↓                    │
│ 低阈值高召回        │                  │ Lambda: cloud_verifier       │
└─────────────────────┘                  │         ↓                    │
                                         │ SageMaker: BioCLIP           │
                                         │   305 鸟类 + 20 负类         │
                                         │   零样本 CLIP 推理           │
                                         │         ↓                    │
                                         │ DynamoDB: 事件记录           │
                                         └──────────────────────────────┘
```


---

## 附录：Gemini 建议整理

### 模型选择建议

#### 方案 A：通用视觉大模型微调（推荐）

- 模型：`google/vit-base-patch16-224` 或 `microsoft/swin-tiny-patch4-window7-224`
- 理由：ViT 在处理细节（如喙色、羽毛纹理）上表现优异
- 训练方式：SageMaker Training Job，在 iNaturalist 2024 数据集上微调
- 优势：Hugging Face 生态与 SageMaker 原生集成，部署最简单

#### 方案 B：针对性预训练模型

- 模型：Hugging Face 上的 Bird-Species-Classifier 类模型（如 `dennisjooo/Birds-Classifier-EfficientNetB2`）
- 思路：这些模型虽然不包含丝光椋鸟，但已经学到了有用的鸟类特征。只需替换最后一层分类头（Classification Head），用目标物种数据做迁移学习
- 注意：通常基于 Kaggle 525 种鸟类数据集训练，物种覆盖偏北美

### 数据集建议

- 数据来源：iNaturalist Open Data（目前开源界最全的生物数据库）
- 获取方式：通过 Python 脚本调用 iNaturalist API，按学名筛选目标物种图片
  - `Spodiopsar sericeus`（丝光椋鸟）
  - `Sturnus cineraceus`（灰椋鸟）
  - 以及其他重庆常见鸟类
- 数据存储：上传到 S3，SageMaker Training Job 直接从 S3 读取

### BioCLIP 局限性分析（Gemini 原话摘要）

> BioCLIP 本质上是对比学习模型，擅长"广度"（认出这是鸟还是花）和零样本能力，但在面对"丝光椋鸟 vs 灰椋鸟"这种极细粒度的亚洲本地物种区分时，特征辨识度不够。
>
> BioCLIP 像是一个读过百科全书但没下过田野的学者。它知道什么是"椋鸟"，但它没见过成千上万张在重庆雾气里拍到的、躲在黄葛树后面的丝光椋鸟。

### 进阶方案（Gemini 建议，工程量较大）

| 方案 | 核心模型 | 适用场景 | 复杂度 |
|------|----------|----------|--------|
| 跨架构特征融合 | EVA-02 / ConvNeXt-V2 (iNat 预训练) | 追求细粒度精度 | 高 |
| 领域专用 SOTA | TransFG / CS-Win Transformer | 极致的局部注意力 | 很高 |
| 两阶段流水线 | YOLO + Swin Transformer | 解决背景干扰 | 中高 |

注：以上进阶方案适合学术研究或生产级系统，对个人学习项目来说投入产出比偏低。建议先从方案 A（ViT 微调）开始。


---

## 阶段四：候选模型对比分析

基于我们的测试数据和当前学术基准，推荐以下 3 个模型方向。效果优先，不考虑成本。

### 候选模型一览

| 维度 | DINOv2-ViT-L/14 | EVA-02-L/14 | Swin-V2-Large |
|------|-----------------|-------------|---------------|
| 来源 | Meta (Facebook AI) | BAAI (北京智源) | Microsoft |
| HuggingFace ID | `facebook/dinov2-large` | `Yuxin-CV/EVA-02-L-14` | `microsoft/swinv2-large-patch4-window12to24-192to384-22kto1k-ft` |
| 架构 | ViT-L/14, 304M 参数 | ViT-L/14 + MIM 预训练, 304M 参数 | Shifted Window Attention, 197M 参数 |
| 预训练数据 | LVD-142M（1.42 亿张图像，自监督） | Merged-30M + ImageNet-21K（有监督+自监督混合） | ImageNet-22K（有监督） |
| 输入分辨率 | 518×518 | 448×448 | 384×384 |
| CUB-200 基准 (fine-tune) | ~92-93% top-1 | ~93-94% top-1 | ~91-92% top-1 |
| 模型大小 | ~1.2GB | ~1.2GB | ~0.8GB |

### 模型 1：DINOv2-ViT-L/14（推荐首选）

DINOv2 是 Meta 的自监督视觉基础模型，在 1.42 亿张图像上用自蒸馏方法训练，不需要任何标签。

为什么推荐：
- 自监督预训练学到的特征非常通用，对细粒度分类的迁移效果极好
- 在多个基准上，DINOv2 的线性探测（只训练最后一层）就能达到 ~90% 准确率，full fine-tune 可以到 92-93%
- 对局部纹理（羽毛、喙色）的捕捉能力强，因为 ViT-L 的 patch size 14 意味着每个 patch 只有 14×14 像素，能看到很细的细节
- 518×518 的高分辨率输入，比 BioCLIP 的 224×224 多 5 倍像素信息
- HuggingFace 生态完善，SageMaker 部署简单

劣势：
- 模型较大（1.2GB），SageMaker Serverless 需要 6144MB 内存
- 自监督预训练没有见过鸟类标签，fine-tune 时需要足够的标注数据

Fine-tune 后预期效果：
- 在 iNaturalist 鸟类子集上 fine-tune 后，预期 top-1 准确率 90-93%
- 对丝光椋鸟 vs 灰椋鸟这种近缘种，高分辨率输入 + 强特征提取应该能区分喙色（红 vs 黄）和头羽纹理差异
- 需要每个目标物种至少 50-100 张训练图片

### 模型 2：EVA-02-L/14（最高精度潜力）

EVA-02 是北京智源研究院的视觉基础模型，结合了 Masked Image Modeling（MIM）和 CLIP 对比学习的预训练策略。

为什么推荐：
- 在 CUB-200 等细粒度基准上，EVA-02 是目前公开模型中精度最高的之一（~93-94%）
- MIM 预训练让模型学会了"补全缺失部分"的能力，这意味着即使鸟被树叶遮挡一部分，模型也能推断出完整特征
- 结合了 CLIP 的语义理解和 MIM 的视觉重建，两种能力互补
- 对遮挡、光线变化的鲁棒性比纯 ViT 更好

劣势：
- 部署复杂度略高，需要安装 `timm` 库的特定版本
- 社区资源不如 DINOv2 丰富，遇到问题排查难度更大
- 同样需要 6144MB SageMaker 内存

Fine-tune 后预期效果：
- 预期 top-1 准确率 92-95%，是三个候选中精度天花板最高的
- 对遮挡场景（鸟躲在树叶后面）表现最好
- 对丝光椋鸟 vs 灰椋鸟的区分能力应该最强，因为 MIM 预训练让模型特别擅长关注局部细节差异

### 模型 3：Swin-V2-Large（最稳健的选择）

Swin Transformer V2 是微软的层级视觉 Transformer，使用 Shifted Window 注意力机制。

为什么推荐：
- Shifted Window 机制让模型同时关注局部细节和全局上下文，不像纯 ViT 那样容易被背景干扰
- 层级结构（从小 patch 到大 patch）天然适合多尺度特征提取，对不同大小的鸟都能处理
- ImageNet-22K 有监督预训练，已经见过大量自然图像，迁移到鸟类分类的起点更高
- 模型相对较小（0.8GB），SageMaker Serverless 4096MB 就够用
- 微软维护，文档和社区支持最好

劣势：
- 精度天花板略低于 DINOv2 和 EVA-02（~91-92%）
- 384×384 输入分辨率比 DINOv2 的 518×518 低，对极细微的纹理差异（如喙色深浅）可能不够敏感

Fine-tune 后预期效果：
- 预期 top-1 准确率 89-92%
- 对常见物种识别准确，但对极相似的近缘种（丝光椋鸟 vs 灰椋鸟）可能仍有困难
- 训练收敛最快，数据需求最低（每个物种 30-50 张可能就够）

### 综合对比

| 维度 | DINOv2-ViT-L | EVA-02-L | Swin-V2-L |
|------|-------------|----------|-----------|
| 预期 fine-tune 精度 | 90-93% | 92-95% | 89-92% |
| 近缘种区分能力 | 强 | 最强 | 中等 |
| 遮挡鲁棒性 | 中等 | 最好 | 好 |
| 部署难度 | 低 | 中等 | 最低 |
| 训练数据需求 | 中等 (50-100张/种) | 中等 (50-100张/种) | 较低 (30-50张/种) |
| SageMaker 内存 | 6144MB | 6144MB | 4096MB |
| 社区/文档 | 丰富 | 一般 | 最丰富 |
| 学习价值 | 高（自监督学习） | 高（MIM+CLIP 混合） | 中（经典有监督） |

### 建议路线

既然目标是学习，建议按顺序尝试：

1. 先做 DINOv2 — 自监督基础模型 + fine-tune 是当前最主流的范式，学习价值最高，效果也很好
2. 再做 EVA-02 — 对比 MIM 预训练 vs 自监督预训练的效果差异，理解不同预训练策略的影响
3. 最后可选 Swin-V2 — 对比 CNN-like 层级结构 vs 纯 ViT 的差异

三个模型都用同一份 iNaturalist 数据集 fine-tune，横向对比结果，这本身就是一份很有价值的实验报告。
