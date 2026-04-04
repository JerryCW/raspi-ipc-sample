# 实验 01：DINOv2 Fine-tune 鸟类分类

## 1. 目标与假设

**目标：** 使用 DINOv2-ViT-L/14 作为特征提取器，在 iNaturalist 鸟类数据集上 fine-tune 一个分类头，替代 BioCLIP 零样本推理，提升细粒度鸟类物种识别准确率。

**假设：**
- BioCLIP 零样本推理对 bird-cropped.jpg（丝光椋鸟）的 top-1 是 Eurasian Jay（错误），similarity 0.295
- DINOv2 fine-tune 后应该能正确区分丝光椋鸟和松鸦，因为有监督训练能学到喙色、头羽纹理等细粒度特征
- 预期 top-1 准确率 90-93%（基于 CUB-200 基准）

**基线对比：**
- BioCLIP 零样本：bird-cropped.jpg → Eurasian Jay (conf=0.33, sim=0.295)（错误）
- BioCLIP 零样本：person-sample.jpg → not_a_bird（负类拒绝后正确）

## 2. 环境准备

### 2.1 确认当前环境

```
Python 3.13.5
torch 2.10.0
transformers 5.5.0
datasets 4.8.4 (新安装)
accelerate 1.13.0 (新安装)
scikit-learn 1.8.0 (新安装)
pyinaturalist 0.21.1 (新安装)
```

### 2.2 安装依赖

```bash
.venv/bin/pip install datasets accelerate scikit-learn pyinaturalist
```

## 3. 数据准备

### 3.1 数据策略

**数据来源：** iNaturalist API（research-grade 观测照片）

**目标物种：** 29 种，聚焦于：
- 椋鸟科 5 种（核心区分目标：丝光椋鸟 vs 灰椋鸟等）
- 重庆常见鸟类 20 种
- BioCLIP 容易混淆的物种 3 种（Eurasian Jay, Indian Roller, Blue Jay）
- 不含负类（非鸟类用 OOD 检测处理，不参与分类训练）

**每种下载 150 张：** train 120 张 + val 30 张
**图片预处理：** 中心裁剪为正方形 → resize 到 518×518（DINOv2-L 输入分辨率）

**决策理由：**
- 29 种而不是 305 种：聚焦目标区域常见鸟类，减少训练数据需求和类别间干扰
- 518×518：DINOv2-L 的原生输入分辨率，比 224×224 多 5 倍像素信息
- research-grade：iNaturalist 社区验证过的高质量标注

### 3.2 下载脚本

创建 `cloud/sagemaker/training/download_inat_data.py`，使用 iNaturalist REST API 按学名搜索物种、获取照片 URL、下载并预处理。

### 3.3 开始下载


## 附录 A：Fine-tune 训练基础概念

### 整体思路

DINOv2 已经在 1.42 亿张图片上学会了"看图"的能力（知道什么是纹理、颜色、形状），但它不知道什么是丝光椋鸟。我们要做的是：保留它的"眼睛"（backbone），只教它一个新的"大脑"（classification head）来认鸟。

这就像请一个视力极好但不认识鸟的人，给他看几千张标注好的鸟类照片，教他区分不同物种。

### 核心概念

#### Backbone（骨干网络）= 眼睛

```python
self.backbone = torch.hub.load("facebookresearch/dinov2", "dinov2_vitl14")
```

DINOv2-ViT-L/14，3 亿参数。它把一张 518×518 的图片变成一个 1024 维的数字向量（embedding）。这个向量包含了图片的所有视觉特征。我们冻结它（`freeze_backbone=True`），不改动它的参数。

#### Classification Head（分类头）= 大脑

```python
self.head = nn.Sequential(
    nn.LayerNorm(EMBED_DIM),      # 归一化
    nn.Linear(EMBED_DIM, 512),     # 1024 → 512
    nn.GELU(),                     # 激活函数
    nn.Dropout(0.3),               # 防过拟合
    nn.Linear(512, num_classes),   # 512 → 39种鸟
)
```

接收 backbone 输出的 1024 维向量，输出 39 个数字（每种鸟一个分数）。这是我们唯一训练的部分。

#### Epoch（轮次）

```python
NUM_EPOCHS = 30
```

把所有训练图片看一遍 = 1 个 epoch。30 个 epoch 意味着模型把每张图片看 30 遍。太少学不会，太多会"背答案"（过拟合）。

#### Batch Size（批大小）

```python
BATCH_SIZE = 16
```

每次同时看 16 张图片，算一次平均梯度，更新一次参数。太小训练不稳定，太大内存不够。

#### Learning Rate（学习率）

```python
LEARNING_RATE = 1e-3  # 0.001
```

每次更新参数时"迈多大步"。太大会跳过最优解，太小收敛太慢。因为我们只训练 head（参数少），所以用相对较大的 1e-3。如果 fine-tune 整个 backbone，通常用 1e-5（小 100 倍）。

#### Loss Function（损失函数）

```python
criterion = nn.CrossEntropyLoss(label_smoothing=0.1)
```

衡量模型预测和正确答案之间的差距。模型预测"80% 松鸦、15% 椋鸟、5% 其他"，但正确答案是椋鸟，loss 就会很大。训练的目标就是让 loss 越来越小。

`label_smoothing=0.1` 意思是不要求模型 100% 确定，允许 90% 确定就行，防止过度自信。

#### Data Augmentation（数据增强）

```python
transforms.RandomResizedCrop(518, scale=(0.7, 1.0)),  # 随机裁剪
transforms.RandomHorizontalFlip(),                      # 随机水平翻转
transforms.ColorJitter(brightness=0.3, ...),            # 随机调色
transforms.RandomRotation(15),                          # 随机旋转
```

同一张图片每次看到的都略有不同（裁剪位置、翻转、颜色、角度），相当于人为增加数据量，防止模型"背图片"。

#### Train/Val Split（训练集/验证集）

- Train（120 张/种）：模型学习用的
- Val（30 张/种）：模型没见过的，用来检验真实水平

就像考试：train 是课本习题，val 是模拟考。如果 train 准确率 99% 但 val 只有 60%，说明过拟合了（背答案但不会举一反三）。

### 什么影响训练效果

按重要性排序：

1. **数据质量 > 数据数量 > 模型大小 > 超参数**
   - 标注错误的图片比少几张图片危害大得多
   - 150 张/种对 fine-tune 来说够用（因为 backbone 已经很强）

2. **数据多样性**
   - 同一种鸟需要不同角度、光线、背景的照片
   - 如果 150 张全是正面照，模型看到侧面就不认识了

3. **类别平衡**
   - 每种鸟的图片数量要差不多
   - 如果麻雀 500 张、丝光椋鸟 10 张，模型会偏向猜麻雀

4. **Learning Rate**
   - 影响最大的超参数，通常需要试几个值（1e-2, 1e-3, 1e-4）

5. **是否 fine-tune backbone**
   - Phase 1（当前）：冻结 backbone，只训练 head → 快，但精度有天花板
   - Phase 2（可选）：解冻 backbone，用很小的 LR 微调整个模型 → 慢，但精度更高

训练过程中我们会观察 train_acc 和 val_acc 的变化曲线，如果 val_acc 不再上升甚至下降，就该停了。


## 附录 B：SageMaker Training Job 完整流程

### 概述

SageMaker Training Job 的工作方式：
1. 你把训练数据上传到 S3
2. 你把训练脚本打包
3. SageMaker 启动一台 GPU 机器，从 S3 下载数据，运行你的脚本
4. 训练完成后，SageMaker 把模型文件上传回 S3
5. GPU 机器自动关闭（不再计费）

你不需要管理任何服务器，只需要写训练脚本和一个启动脚本。

### Step 1：上传训练数据到 S3

```bash
# 把本地下载好的鸟类图片打包上传到 S3
# SageMaker 训练时会从这个 S3 路径读取数据
aws s3 sync cloud/sagemaker/training/data/ \
  s3://smart-camera-captures/training-data/birds/ \
  --region ap-southeast-1
```

S3 上的目录结构会是：
```
s3://smart-camera-captures/training-data/birds/
├── train/
│   ├── Red-billed_Starling/
│   │   ├── 0000.jpg
│   │   ├── 0001.jpg
│   │   └── ...
│   ├── White-cheeked_Starling/
│   └── ...
└── val/
    ├── Red-billed_Starling/
    └── ...
```

### Step 2：准备训练脚本

训练脚本需要适配 SageMaker 的约定：
- 训练数据在 `/opt/ml/input/data/training/` 目录
- 模型输出保存到 `/opt/ml/model/` 目录
- 超参数通过环境变量或 JSON 文件传入

### Step 3：启动 Training Job

通过 Python 脚本调用 SageMaker API：
```python
import sagemaker
from sagemaker.pytorch import PyTorch

estimator = PyTorch(
    entry_point="train_sagemaker.py",       # 训练脚本
    source_dir="cloud/sagemaker/training/", # 脚本目录
    role="smart-camera-sagemaker-role",
    instance_count=1,
    instance_type="ml.g4dn.xlarge",         # 1x T4 GPU
    framework_version="2.0.1",
    py_version="py310",
    hyperparameters={
        "epochs": 30,
        "batch_size": 16,
        "learning_rate": 0.001,
    },
    use_spot_instances=True,                # 竞价实例，省 ~70% 费用
    max_wait=7200,                          # 最多等 2 小时
    max_run=3600,                           # 最多跑 1 小时
)

estimator.fit({
    "training": "s3://smart-camera-captures/training-data/birds/"
})
```

### Step 4：监控训练

```bash
# 查看训练日志（实时）
aws logs tail /aws/sagemaker/TrainingJobs --follow

# 或者在 SageMaker Console 查看
# https://ap-southeast-1.console.aws.amazon.com/sagemaker/home?region=ap-southeast-1#/jobs
```

### Step 5：下载训练好的模型

训练完成后，模型自动保存到 S3：
```
s3://sagemaker-ap-southeast-1-823092283330/dinov2-bird-classifier-YYYY-MM-DD/output/model.tar.gz
```

### 费用估算

| 项目 | 费用 |
|------|------|
| ml.g4dn.xlarge On-Demand | $0.74/小时 |
| ml.g4dn.xlarge Spot | ~$0.22/小时 |
| 预计训练时间 | 15-20 分钟 |
| 预计总费用 | $0.05-0.10（Spot） |
| S3 存储（训练数据） | < $0.01/月 |


## 附录 C：数据清洗方案

### 为什么需要清洗

iNaturalist research-grade 图片整体质量不错，但仍有部分低质量图片：
- 远距离群鸟剪影（只有黑色小点，看不到任何特征）
- 鸟占比极小的图片（95% 是建筑物/草地，鸟只有几个像素）
- 多只鸟的群鸟照（不适合单物种分类训练）

这些图片会给模型引入噪声，让模型学到"蓝色天空=某种鸟"或"绿色草地=某种鸟"这样错误的关联。

### 清洗策略

使用 YOLO 检测每张图片中的鸟，过滤条件：
1. **检测不到鸟** → 删除（可能是纯背景、极远距离）
2. **鸟占比 < 5%** → 删除（鸟太小，看不清特征）
3. **鸟数量 > 5** → 删除（群鸟照，不适合单物种训练）

### 清洗脚本

`cloud/sagemaker/training/clean_data.py`

```bash
# 运行清洗（需要 ultralytics）
python cloud/sagemaker/training/clean_data.py
```

输入：`data/`（原始数据）
输出：`data_cleaned/`（清洗后数据）

### 预期效果

- 预计淘汰 10-20% 的低质量图片
- 1000 张/种 → 清洗后约 800-900 张/种
- 清洗后的数据用于 full fine-tune，效果应该比未清洗的更好

### 图片质量分级（参考）

| 质量 | 描述 | 示例 | 处理 |
|------|------|------|------|
| 高 | 近距离特写，特征清晰 | 鸟占 50%+，能看到喙色/羽毛 | 保留 |
| 中 | 中距离，背景杂乱但鸟可辨认 | 鸟占 10-50%，树枝间的鸟 | 保留 |
| 低 | 远距离，鸟很小 | 鸟占 < 5%，草地上的小点 | 删除 |
| 极低 | 纯剪影/看不到鸟 | 天空中的黑点、纯建筑物 | 删除 |


## 附录 D：SageMaker Processing Job 数据清洗

### 什么是 Processing Job

SageMaker Processing Job 和 Training Job 类似，但用途不同：
- Training Job：训练模型
- Processing Job：数据预处理、清洗、特征工程、模型评估

工作方式完全一样：启动实例 → 从 S3 下载数据 → 运行脚本 → 上传结果到 S3 → 关闭实例。

### 清洗流程

```
S3: training-data/birds/raw/          S3: training-data/birds-cleaned/
├── Red-billed_Starling/               ├── train/
│   ├── 0000.jpg                       │   ├── Red-billed_Starling/
│   ├── 0001.jpg                       │   │   ├── 0003.jpg (高质量)
│   └── ... (1500张)                   │   │   └── ...
├── White-cheeked_Starling/            │   └── ...
│   └── ...                            └── val/
└── ...                                    ├── Red-billed_Starling/
                                           │   └── ...
         ↓ YOLO 检测 + 过滤 + 分割 ↓       └── ...
```

### 使用方法

```bash
# Step 1: 确保原始数据已上传到 S3
aws s3 ls s3://smart-camera-captures/training-data/birds/raw/ --region ap-southeast-1

# Step 2: 启动清洗 Processing Job
python cloud/sagemaker/training/launch_cleaning.py

# Step 3: 清洗完成后，用清洗后的数据训练
# 修改 launch_training.py 中的 S3_DATA_URI 为:
# s3://smart-camera-captures/training-data/birds-cleaned
```

### 相关文件

| 文件 | 用途 |
|------|------|
| `clean_sagemaker.py` | SageMaker 上运行的清洗脚本 |
| `launch_cleaning.py` | 本地运行的启动脚本（提交 Processing Job） |
| `clean_data.py` | 本地版清洗脚本（可选，用于本地调试） |

### 费用估算

| 项目 | 费用 |
|------|------|
| ml.g4dn.xlarge | $0.74/小时 |
| 预计处理时间 | 1-2 小时（39种 × 1500张） |
| 预计总费用 | $0.74-1.48 |


## 附录 E：标签层级化（属级别兜底）

### 原理

模型输出 39 种鸟的 confidence 分数。当 top-1 confidence 很低（比如 35%），说明模型不确定。如果 top-1 和 top-2 属于同一个属（比如都是椋鸟属），与其硬猜具体种，不如诚实地输出"椋鸟属"。

### 判断逻辑

```
if top1_confidence > 60%:
    输出具体种（模型很确定）
elif top1 和 top2 属于同一个属:
    输出属名（模型不确定具体种，但确定是哪个属）
else:
    输出 top1（没有更好的选择）
```

### 物种-属映射表

| 英文名 | 中文名 | 拉丁学名 | 属 (Genus) | 属中文名 |
|--------|--------|----------|-----------|---------|
| Red-billed Starling | 丝光椋鸟 | Spodiopsar sericeus | Spodiopsar | 椋鸟属 |
| White-cheeked Starling | 灰椋鸟 | Spodiopsar cineraceus | Spodiopsar | 椋鸟属 |
| Crested Myna | 八哥 | Acridotheres cristatellus | Acridotheres | 八哥属 |
| Common Myna | 家八哥 | Acridotheres tristis | Acridotheres | 八哥属 |
| Black-collared Starling | 黑领椋鸟 | Gracupica nigricollis | Gracupica | 丽椋鸟属 |
| Eurasian Tree Sparrow | 麻雀 | Passer montanus | Passer | 麻雀属 |
| Light-vented Bulbul | 白头鹎 | Pycnonotus sinensis | Pycnonotus | 鹎属 |
| Red-whiskered Bulbul | 红耳鹎 | Pycnonotus jocosus | Pycnonotus | 鹎属 |
| Swinhoe's White-eye | 暗绿绣眼鸟 | Zosterops simplex | Zosterops | 绣眼鸟属 |
| Oriental Magpie-Robin | 鹊鸲 | Copsychus saularis | Copsychus | 鹊鸲属 |
| Spotted Dove | 珠颈斑鸠 | Spilopelia chinensis | Spilopelia | 斑鸠属 |
| Chinese Blackbird | 乌鸫 | Turdus mandarinus | Turdus | 鸫属 |
| Red-billed Blue Magpie | 红嘴蓝鹊 | Urocissa erythroryncha | Urocissa | 蓝鹊属 |
| Chinese Hwamei | 白颊噪鹛 | Pterorhinus sannio | Pterorhinus | 噪鹛属 |
| Japanese Tit | 远东山雀 | Parus minor | Parus | 山雀属 |
| Vinous-throated Parrotbill | 棕头鸦雀 | Sinosuthora webbiana | Sinosuthora | 鸦雀属 |
| Little Grebe | 小䴙䴘 | Tachybaptus ruficollis | Tachybaptus | 䴙䴘属 |
| Blue-fronted Redstart | 蓝额红尾鸲 | Phoenicurus frontalis | Phoenicurus | 红尾鸲属 |
| Eurasian Magpie | 喜鹊 | Pica pica | Pica | 鹊属 |
| Common Kingfisher | 普通翠鸟 | Alcedo atthis | Alcedo | 翠鸟属 |
| Daurian Redstart | 北红尾鸲 | Phoenicurus auroreus | Phoenicurus | 红尾鸲属 |
| Plumbeous Water Redstart | 红尾水鸲 | Phoenicurus fuliginosus | Phoenicurus | 红尾鸲属 |
| Large-billed Crow | 大嘴乌鸦 | Corvus macrorhynchos | Corvus | 鸦属 |
| Black Drongo | 黑卷尾 | Dicrurus macrocercus | Dicrurus | 卷尾属 |
| White-throated Kingfisher | 白胸翡翠 | Halcyon smyrnensis | Halcyon | 翡翠属 |
| Fork-tailed Sunbird | 叉尾太阳鸟 | Aethopyga christinae | Aethopyga | 太阳鸟属 |
| Grey Heron | 苍鹭 | Ardea cinerea | Ardea | 鹭属 |
| Little Egret | 白鹭 | Egretta garzetta | Egretta | 白鹭属 |
| Barn Swallow | 家燕 | Hirundo rustica | Hirundo | 燕属 |
| White Wagtail | 白鹡鸰 | Motacilla alba | Motacilla | 鹡鸰属 |
| Grey Wagtail | 灰鹡鸰 | Motacilla cinerea | Motacilla | 鹡鸰属 |
| Long-tailed Shrike | 棕背伯劳 | Lanius schach | Lanius | 伯劳属 |
| Brown Shrike | 红尾伯劳 | Lanius cristatus | Lanius | 伯劳属 |
| Azure-winged Magpie | 灰喜鹊 | Cyanopica cyanus | Cyanopica | 灰喜鹊属 |
| Oriental Turtle Dove | 山斑鸠 | Streptopelia orientalis | Streptopelia | 斑鸠属 |
| Rock Dove | 原鸽 | Columba livia | Columba | 鸽属 |
| Eurasian Jay | 松鸦 | Garrulus glandarius | Garrulus | 松鸦属 |
| Indian Roller | 棕胸佛法僧 | Coracias benghalensis | Coracias | 佛法僧属 |
| Blue Jay | 冠蓝鸦 | Cyanocitta cristata | Cyanocitta | 冠蓝鸦属 |

### 容易混淆的同属物种对

这些是标签层级化最有价值的场景：

| 属 | 物种 | 区分难点 |
|----|------|---------|
| Spodiopsar（椋鸟属） | 丝光椋鸟 vs 灰椋鸟 | 喙色（红 vs 黄带黑尖）、头羽纹理 |
| Pycnonotus（鹎属） | 白头鹎 vs 红耳鹎 | 头部标记（白斑 vs 红耳斑） |
| Phoenicurus（红尾鸲属） | 北红尾鸲 vs 红尾水鸲 vs 蓝额红尾鸲 | 体色差异较大但姿态相似 |
| Motacilla（鹡鸰属） | 白鹡鸰 vs 灰鹡鸰 | 体色相近，腹部颜色不同 |
| Lanius（伯劳属） | 棕背伯劳 vs 红尾伯劳 | 背部颜色、体型差异 |
| Acridotheres（八哥属） | 八哥 vs 家八哥 | 额头羽簇、眼周裸皮颜色 |

### 实现时机

部署推理时在 inference.py 中实现，不影响训练。


### 清洗速度对比

| 实例 | GPU | 速度 | 1500张/种 | 39种总计 |
|------|-----|------|----------|---------|
| ml.m5.xlarge (CPU) | 无 | 1.26 img/s | ~20 分钟 | ~13 小时 |
| ml.g4dn.xlarge (GPU) | T4 | 30 img/s | ~50 秒 | ~33 分钟 |

GPU 比 CPU 快 24 倍。清洗是一次性任务，GPU 实例多花几毛钱但省 12 小时。


### 清洗结果

| 指标 | 数量 |
|------|------|
| 原始图片 | 58,500 |
| 保留 | 31,542 (53.9%) |
| 移除 - 未检测到鸟 | 6,263 |
| 移除 - 鸟太小 (<8%) | 20,290 |
| 移除 - 群鸟太多 (>3) | 405 |

淘汰主因是"鸟太小"（占淘汰总量的 75%），说明 iNaturalist 上大量图片是远距离拍摄的。

清洗后数据集：
- original 版：train=25,217 val=6,325
- cropped 版：train=25,217 val=6,325
- 平均每种鸟：train≈647 val≈162

S3 路径：`s3://smart-camera-captures/training-data/birds-cleaned/`


## 4. 训练结果（冻结 backbone，150 张/种）

### 训练曲线

| Epoch | Train Loss | Train Acc | Val Loss | Val Acc | LR |
|-------|-----------|-----------|----------|---------|-----|
| 1 | 1.0824 | 89.7% | 1.0041 | 90.8% | 0.000905 |
| 2 | 0.7967 | 98.3% | 0.9743 | 92.1% | 0.000655 |
| 3 | 0.7514 | 99.4% | 0.9738 | 91.9% | 0.000346 |
| 4 | 0.7281 | 99.7% | 0.9253 | 93.2% | 0.000096 |
| 5 | 0.7174 | 99.9% | 0.9220 | **94.0%** | 0.000001 |

最佳 Val Accuracy: 94.0%（Epoch 5）

### 观察

- 第一个 epoch 就到 90.8%，说明 DINOv2 特征质量很高
- Train acc 从 Epoch 2 开始接近 100%，但 Val 仍在缓慢上升，没有严重过拟合
- LR 降到最小值时（Epoch 4-5）Val 反而提升最多，说明小 LR 的精细调整有效
- 94.0% 超出预期（预估 85-92%）


## 4.1 训练结果（实验 1：清洗后原图 300 张/种，冻结 backbone）

### 训练曲线

| Epoch | Train Loss | Train Acc | Val Loss | Val Acc | LR |
|-------|-----------|-----------|----------|---------|-----|
| 1 | 0.8808 | 95.69% | 0.7767 | 98.17% | 0.000905 |
| 2 | 0.7669 | 98.88% | 0.7757 | 98.15% | 0.000655 |
| 3 | 0.7383 | 99.51% | 0.7442 | 98.78% | 0.000346 |
| 4 | 0.7200 | 99.77% | 0.7335 | 98.75% | 0.000096 |
| 5 | 0.7097 | 99.94% | 0.7296 | **98.85%** | 0.000001 |

最佳 Val Accuracy: 98.85%（Epoch 5）。相比基线 94.0% 提升 4.85%，数据清洗 + 更多样本效果显著。

## 4.2 训练结果（实验 2：清洗后裁剪图 300 张/种，冻结 backbone）

### 训练曲线

| Epoch | Train Loss | Train Acc | Val Loss | Val Acc | LR |
|-------|-----------|-----------|----------|---------|-----|
| 1 | 0.9037 | 95.01% | 0.8286 | 96.60% | 0.000905 |
| 2 | 0.7765 | 98.75% | 0.8157 | 96.87% | 0.000655 |
| 3 | 0.7451 | 99.47% | 0.7907 | 97.57% | 0.000346 |
| 4 | 0.7241 | 99.71% | 0.7783 | 97.82% | 0.000096 |
| 5 | 0.7137 | 99.87% | 0.7729 | **97.96%** | 0.000001 |

最佳 Val Accuracy: 97.96%（Epoch 5）。Val acc 略低于 original-300，但实际测试图泛化能力更强（见第 6 节分析）。


## 5. 测试结果

### bird-cropped.jpg（丝光椋鸟）

| 排名 | 物种 | 属 | 置信度 |
|------|------|-----|--------|
| 1 | 丝光椋鸟 | 椋鸟属 | 67.5% ✓ |
| 2 | 白颊噪鹛 | 噪鹛属 | 2.8% |
| 3 | 灰椋鸟 | 椋鸟属 | 2.1% |
| 4 | 红嘴蓝鹊 | 蓝鹊属 | 1.7% |
| 5 | 棕胸佛法僧 | 佛法僧属 | 1.6% |

### bird-sample.jpg（丝光椋鸟原图，未裁剪）

| 排名 | 物种 | 属 | 置信度 |
|------|------|-----|--------|
| 1 | 白头鹎 | 鹎属 | 21.7% ✗ |
| 2 | 乌鸫 | 鸫属 | 8.7% |
| 3 | 丝光椋鸟 | 椋鸟属 | 8.5% |
| 4 | 黑领椋鸟 | 丽椋鸟属 | 5.4% |
| 5 | 红尾伯劳 | 伯劳属 | 4.8% |

原图中鸟占比小、背景干扰大，模型完全误判。丝光椋鸟仅排第 3（8.5%），说明冻结 backbone + 少量未清洗数据不足以处理复杂背景场景。这是实验 1-4 需要解决的核心问题。

### bird-sample2.jpg（乌鸫，近距离特写）

| 排名 | 物种 | 属 | 置信度 |
|------|------|-----|--------|
| 1 | 乌鸫 | 鸫属 | 76.9% ✓ |
| 2 | 家八哥 | 八哥属 | 3.3% |
| 3 | 灰椋鸟 | 椋鸟属 | 2.0% |

近距离特写，鸟占比大，基线模型即可正确识别。（注：此结果使用 original-300 模型测试）

### person-sample.jpg（人物）

→ 非鸟类（top-1 置信度 7.6% < 20% 阈值）✓

### 对比 BioCLIP 零样本

| 测试图片 | BioCLIP 零样本 | DINOv2 基线 | DINOv2 original-300 | DINOv2 cropped-300 |
|----------|---------------|------------|--------------------|--------------------|
| bird-cropped.jpg | Eurasian Jay 33%（错误） | 丝光椋鸟 67.5% ✅ | 丝光椋鸟 65.9% ✅ | 丝光椋鸟 81.2% ✅ |
| bird-sample.jpg | — | 白头鹎 21.7% ❌ | 丝光椋鸟 18.9% ✅ | 丝光椋鸟 68.2% ✅ |
| bird-sample2.jpg | — | — | 乌鸫 76.9% ✅ | 乌鸫 66.8% ✅ |
| person-sample.jpg | not_a_bird ✅ | 非鸟类 ✅ | — | — |

### 结论

DINOv2 冻结 backbone 实验总结：
- 基线（150 张/种未清洗）：val 94.0%，裁剪图正确但原图失败
- 清洗后原图 300 张/种：val 98.85%，原图勉强正确（18.9%）
- 清洗后裁剪图 300 张/种：val 97.96%，原图高置信度正确（68.2%）
- cropped-300 是当前最佳模型，裁剪训练数据显著提升泛化能力
- bird-sample.jpg 作为最终验证标准已通过，等待 full fine-tune 结果看是否能进一步提升


## 6. 对比实验矩阵

### 实验设计

四个训练 Job 同时运行，覆盖数据类型 × 训练模式的组合：

| # | Job 名称 | 数据 | 样本/种 | 训练模式 | 实例 | LR |
|---|---------|------|---------|---------|------|-----|
| 0 | dinov2-bird-classifier (基线) | 未清洗原图 | 150 | 冻结 backbone | g4dn.xlarge | 1e-3 |
| 1 | dinov2-original-300 | 清洗后原图 | 300 | 冻结 backbone | g4dn.xlarge | 1e-3 |
| 2 | dinov2-cropped-300 | 清洗后裁剪图 | 300 | 冻结 backbone | g4dn.2xlarge | 1e-3 |
| 3 | dinov2-full-finetune-500 | 清洗后原图 | 500 | full fine-tune | p3.2xlarge | 1e-5 |
| 4 | dinov2-full-ft-cropped-spot | 清洗后裁剪图 | 500 | full fine-tune | p3.2xlarge (Spot) | 1e-5 |

### 对比维度

- 实验 0 vs 1：数据清洗的效果（未清洗 150 张 vs 清洗后 300 张）
- 实验 1 vs 2：原图 vs bbox 裁剪的效果
- 实验 1 vs 3：冻结 backbone vs full fine-tune 的效果
- 实验 3 vs 4：原图 vs 裁剪图在 full fine-tune 下的效果

### 基线结果（实验 0）

- Val accuracy: 94.0%
- bird-cropped.jpg: 丝光椋鸟 67.5% ✓

### 实验结果汇总

| # | 模型 | Val Acc | bird-cropped | bird-sample | bird-sample2 |
|---|------|---------|-------------|-------------|--------------|
| 0 | 基线：未清洗原图 150张/种，冻结backbone | 94.0% | ✅ 丝光椋鸟 67.5% | ❌ 白头鹎 21.7% | — |
| 1 | original-300：清洗后原图，冻结backbone | 98.85% | ✅ 丝光椋鸟 65.9% | ✅ 丝光椋鸟 18.9% | ✅ 乌鸫 76.9% |
| 2 | cropped-300：清洗后裁剪图，冻结backbone | 97.96% | ✅ 丝光椋鸟 81.2% | ✅ 丝光椋鸟 68.2% | ✅ 乌鸫 66.8% |
| 3 | full-ft-original：清洗后原图，full fine-tune | 训练中 | — | — | — |
| 4 | full-ft-cropped：清洗后裁剪图，full fine-tune | 训练中 | — | — | — |

### 关键发现

1. **数据清洗效果显著**（实验 0→1）：val acc 94.0% → 98.85%，bird-sample.jpg 从完全错误变为正确（虽然置信度仅 18.9%）
2. **裁剪图泛化能力更强**（实验 1 vs 2）：虽然 val acc 略低（98.85% vs 97.96%），但 bird-sample.jpg 置信度 68.2% vs 18.9%，差距巨大。原因是裁剪图迫使模型学习鸟本身特征，而非背景捷径
3. **val acc 不等于实际效果**：original-300 的 val acc 更高，但在真实场景图片上表现远不如 cropped-300
4. **近距离特写两者差距小**：bird-sample2.jpg（乌鸫近距离）两个模型都正确且置信度接近（76.9% vs 66.8%），说明裁剪的优势主要体现在鸟占比小的场景

### 实验 3/4 说明

Full fine-tune 使用 us-east-1 的 p3.2xlarge（V100 16GB），因 DINOv2-ViT-L 全参数训练显存需求大，batch_size 从 8 降为 2，配合 gradient accumulation 4 步（等效 batch=8）。训练中。
