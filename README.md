# RainingKeys

Ballance版**KeyViewer**/**RainingKeys**。

**当前只适配BML+ 0.3.12及以上版本！！！**

---

## 功能

### 1. 按键方块

在屏幕上显示按键，按下时方块**颜色、大小变化**。默认显示上下左右四键。

### 2. 雨键

按下按键，从方块边缘生成雨带，直观显示按键最近一段时间持续的状态。

### 3. 按键计数

每个按键可以开启计数，并长期保存。可以用命令 **/rkc** 一键清空所有按键。

### 4. KPS / Total 

默认关闭，可以去Main里面打开。

- **KPS**：实时按键速度（Keys Per Second）。
- **Total**：按键计数总数，只计入当前显示的按键计数。

---

## 设置选项

### 1. Main

| 选项 | 说明 |
|---|---|
| **Enabled** | 总开关。 |
| **OnlyInLevel** | 只在关卡内显示。 |
| **OffsetX**、**OffsetY** | 全局坐标。 |
| **Scale** | 全局缩放。 |
| **KeyNums** | 打开的按键数量。**老BML更改完需要重启游戏以新增设置选项！** |
| **EnableKPS**、**EnableTotal** | 打开KPS/Total。**老BML更改完需要重启游戏以新增设置选项！** |
| **EnableDebugLog** | 打开调试日志，一般不用打开。 |

### 2. 一般Key

#### 按键设置

| 选项 | 说明 |
|---|---|
| **Key** | 绑定的按键。 |
| **Label** | 显示的文字,**留空**表示自动用按键名。 |
| **LabelSize** | 文字大小。 |
| **LabelColor** | 文字颜色。 |
| **EnableRaining** | 是否开启雨键。 |
| **X**、**Y** | 按键相对位置偏移。 |
| **W**、**H** | 按键宽度与高度。 |
| **BgColor** | 背景颜色（不按的颜色）。 |
| **PressedColor** | 按下去的颜色。 |
| **PressedScale** | 按下去之后的大小。 |

#### 雨键设置

| 选项 | 说明 |
|---|---|
| **RainDir** | 雨带方向，可选`Up` / `Down` / `Left` / `Right`。 |
| **RainOffsetX**、**RainOffsetY** | 雨带相对位置偏移。 |
| **RainSpeed** | 雨带流动速度。 |
| **RainLength** | 雨带长度限制。 |
| **RainWidth** | 雨带宽度，0代表和按键宽度一致。 |
| **RainColor** | 雨带颜色。 |

#### 按键计数设置

| 选项 | 说明 |
|---|---|
| **EnableCount** | 打开按键计数。 |
| **CountSize** | 按键计数文字大小。 |
| **CountColor** | 按键计数文字颜色。 |

### 3. KPS设置

| 选项 | 说明 |
|---|---|
| **X**、**Y**、**W**、**H**、**BgColor**、**Label**、**LabelSize** | 同一般Key的按键设置。 |
| **LabelColor** | 同一般Key的按键设置。 |
| **ValueSize**、**ValueColor** | 同一般Key的按键计数设置。 |
| **KpsRefresh** | KPS刷新间隔 **（单位ms）**。 |

### 4. Total设置

| 选项 | 说明 |
|---|---|
| **X**、**Y**、**W**、**H**、**BgColor**、**Label**、**LabelSize** | 同一般Key的按键设置。 |
| **LabelColor** | 同一般Key的按键设置。 |
| **ValueSize**、**ValueColor** | 同一般Key的按键计数设置。 |
