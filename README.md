<div align="center">
	<a>
		<img src="https://github.com/user-attachments/assets/13f23b2e-8a9c-4501-ab0b-76c9404dc17c" width="200" height="auto">
	</a>
	<h1>Win Mouse Fix</h1>  
    <p>本项目代码100%由curosr编写, 未经过人工审计!</p>
</div>

<br>

Windows 版 [Mac Mouse Fix](https://github.com/noah-nuebling/mac-mouse-fix), 实现了mac-mouse-fix的几个常用功能.
## 简介
Win Mouse Fix 是一个为 Windows 系统设计的鼠标增强工具, 允许你通过按住鼠标侧键并移动鼠标来执行各种系统操作, 类似于触摸板手势。

### 核心功能

- **任务视图**：按住鼠标侧键 4 + 向上滑动, 打开 Windows 任务视图
- **虚拟桌面切换**：按住鼠标侧键 4 + 左右滑动, 在虚拟桌面之间切换
- **显示桌面**：按住鼠标侧键 4 + 向下滑动, 显示桌面
- **滚动模拟**：按住鼠标侧键 5 + 移动鼠标, 模拟触摸板两指滚动

## 系统要求

- Windows 10 或更高版本
- 支持侧键 (第 4、第 5 按钮) 的鼠标

### 编译要求

- Visual Studio 2017 或更高版本
- nlohmann/json 单文件头

## 快速开始

### 下载预编译版本

1. 前往 [Releases](https://github.com/EEEEhex/win-mouse-fix/releases) 页面
2. 下载最新版本的 `win-mouse-fix.exe`
3. 程序会显示主窗口, 同时在系统托盘显示图标

### 从源码编译

#### 1. 克隆仓库并下载依赖

```bash
git clone https://github.com/EEEEhex/win-mouse-fix
cd win-mouse-fix

# 下载 nlohmann/json 单文件头文件
# Windows PowerShell:
Invoke-WebRequest -Uri "https://github.com/nlohmann/json/releases/latest/download/json.hpp" -OutFile "third_party/json.hpp"
```

#### 2. 使用 Visual Studio 构建
右键编译即可


## 使用说明

### 默认手势配置

| 手势 | 操作 | 说明 |
|------|------|------|
| 按钮 4 + 向上滑动 | 任务视图 | 显示所有打开的窗口和虚拟桌面 |
| 按钮 4 + 向左滑动 | 切换桌面（左） | 切换到左边的虚拟桌面 |
| 按钮 4 + 向右滑动 | 切换桌面（右） | 切换到右边的虚拟桌面 |
| 按钮 4 + 向下滑动 | 显示桌面 | 最小化所有窗口显示桌面 |
| 按钮 5 + 移动 | 滚动模拟 | 模拟触摸板两指滚动 |

> **注意**：按钮 4 通常是鼠标的"后退"按钮, 按钮 5 是"前进"按钮。

### 自定义配置

配置文件位于程序目录下的 `config.json`。你可以编辑此文件来自定义手势。

#### 配置文件结构

```json
{
  "gestures": [
    {
      "triggerButton": "BUTTON_5",
      "gestureType": "SWIPE_UP",
      "actionType": "TASK_VIEW",
      "threshold": 50
    }
  ]
}
```

#### 配置参数说明

- **triggerButton**：触发按钮
  - `BUTTON_4`：侧键 4 (通常是后退键)
  - `BUTTON_5`：侧键 5 (通常是前进键)
  - `BUTTON_MIDDLE`：中键

- **gestureType**：手势类型
  - `SWIPE_UP`：向上滑动
  - `SWIPE_DOWN`：向下滑动
  - `SWIPE_LEFT`：向左滑动
  - `SWIPE_RIGHT`：向右滑动
  - `TWO_FINGER_SCROLL`：滚动模拟

- **actionType**：执行的操作
  - `TASK_VIEW`：任务视图 (Win+Tab)
  - `SHOW_DESKTOP`：显示桌面 (Win+D)
  - `SWITCH_DESKTOP_LEFT`：切换到左边桌面 (Ctrl+Win+Left)
  - `SWITCH_DESKTOP_RIGHT`：切换到右边桌面 (Ctrl+Win+Right)
  - `SCROLL_SIMULATION`：滚动模拟

- **threshold**：触发阈值 (像素)
  - 鼠标移动超过此距离才会触发手势
  - 滚动模拟设为 0

## 项目架构

```
win-mouse-fix/
├── include/                  # 头文件
│   ├── Common.h              # 公共定义和数据结构
│   ├── MouseHook.h           # 鼠标钩子
│   ├── ButtonState.h         # 按钮状态跟踪
│   ├── GestureRecognizer.h   # 手势识别器
│   ├── WindowsActions.h      # Windows 系统操作
│   └── ConfigManager.h       # 配置管理器
├── src/                      # 源文件
│   ├── main.cpp
│   ├── MouseHook.cpp
│   ├── ButtonState.cpp
│   ├── GestureRecognizer.cpp
│   ├── WindowsActions.cpp
│   └── ConfigManager.cpp
├── third_party/
│   └── json.hpp              # json解析
├── config/
│   └── config.json           # 配置文件
└── README.md                 # 本文件
```
## 贡献

欢迎提交 Issue 和 Pull Request！

### 开发建议

1. 使用 C++17 标准
2. 遵循现有的代码风格
3. 为新功能添加注释
4. 测试你的更改

## 鸣谢

- 灵感来源：[Mac Mouse Fix](https://github.com/noah-nuebling/mac-mouse-fix)
- 感谢所有贡献者

## 联系方式

如有问题或建议，请提交 [Issue](https://github.com/EEEEhex/win-mouse-fix/issues).

---

**注意**：本项目仅用于学习和个人使用. 

