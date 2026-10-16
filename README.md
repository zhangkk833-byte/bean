# Bean · UE5 体素绘画小游戏

在三维网格上堆方块画画的小游戏，基于 **Unreal Engine 5.8**。第一人称漫游，鼠标指着格子放方块；也可以直接导入一张图片，让程序把它压缩成体素提示层，再一格一格盖出来。

## 功能

- **网格绘制**：左键放置、右键移除；按住不放可以连续扫过一整片格子。
- **48 色调色板**：颜色匹配在 CIE L\*a\*b\* 空间里做最近色查找（sRGB 不感知均匀，直接在 RGB 里比会把深红和棕色弄混）。
- **图片导入**：选图 → 选提示层大小（32 / 64 / 128 / 256）→ 生成提示层预览。提示层可以调透明度、显示 / 隐藏，满意了点「应用」落成方块，不满意点「取消」。
- **图片导出**：点「导出图片」会先弹出一个全屏预览，直接看到当前拼豆按这个比例缩放后的样子，并显示成品尺寸（比如「每个豆 8 像素 · 成品 240 × 180 像素」）。这时候可以：
  - 点 4 / 8 / 16 / 32 像素换比例，预览图立刻重画（会让成品超过 4096 像素的比例置灰）；
  - 点「保存图片」，选保存位置（默认存到「图片」文件夹，文件名带时间戳）写出 PNG；
  - 点「返回继续拼豆」直接退回拼豆，什么都不写。

  预览期间不会往硬盘写任何文件。导出的是当前画面里所有豆子的紧贴边界，不是整块 256×256 底板，所以小作品也是一张小图；未「应用」的提示层也会一起导出（提示层没被豆子盖住的地方）。保存成功后屏幕正上方会弹出一条短提示，几秒后自动消失。
- **漫游与飞行**：走路 + 飞行两种移动方式，飞行时可以垂直升降。
- **存档**：F5 保存当前画面。

> 图片导入和导出都依赖 Windows 原生文件对话框，**只有 Windows 平台可用**；其他平台会打印一条警告并跳过。

## 操作

| 按键 | 作用 |
| --- | --- |
| `W` / `S` | 前进 / 后退 |
| `A` / `D` | 左移 / 右移 |
| 鼠标 | 转视角 |
| 左键 | 放置方块 |
| 右键 | 移除方块 |
| 滚轮 | 切换当前颜色 |
| `F` | 切换飞行模式 |
| `空格` / `左Ctrl` | 飞行时上升 / 下降 |
| `Q` | 切换光标模式（指针模式 ↔ 锁定模式） |
| `E` | 显示 / 隐藏 UI |
| `F5` | 保存 |
| `Alt+Enter` / `F11` | 切换全屏 |

右上角另有「退出全屏」「退出」按钮，屏幕右侧是图片导入相关按钮（导入图片 / 应用 / 取消 / 透明度 / 显示隐藏提示层）和底部的「导出图片」按钮。导出预览打开时是模态的：鼠标会被拿回、走动和放置都会暂停，直到你点「保存图片」或「返回继续拼豆」。

按键绑定没有走 `.ini`，而是代码里用 Enhanced Input 现建的（`Source/bean/BeanPlayerController.cpp` 的 `BindEnhancedInput()`），改键直接改那里。

## 构建

需要 UE 5.8，且工程放在引擎能识别的目录下。

```
# 编译编辑器目标
"D:\UE_5.8\Engine\Build\BatchFiles\Build.bat" beanEditor Win64 Development -Project="<工程路径>\bean.uproject" -WaitMutex

# 打 Windows Shipping 包（产物在 Saved/StagedBuilds/Windows）
"D:\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun -project="<工程路径>\bean.uproject" -noP4 -platform=Win64 -clientconfig=Shipping -serverconfig=Shipping -cook -stage -pak -build -map=/Game/Bean -nocompileeditor -utf8output
```

## 目录结构

```
bean.uproject            工程描述文件
Config/                  工程配置（含 BeanPalette.json 调色板数据）
Content/                 关卡与材质
Source/bean/             C++ 源码
  BeanPlayerController   输入、放置/移除、图片导入流程、提示层大小选择面板、导出预览与保存流程
  BeanGrid               网格数据、提示层预览与应用、导出边界与 PNG 生成
  BeanPalette            调色板与 Lab 最近色匹配
  BeanHUDWidget          HUD 按钮、状态条与导出预览面板
  BeanCharacter          角色与移动
  BeanAssets             运行时生成的资源（含导出用的位图绘制）
  BeanGameMode           游戏模式
```

`Binaries/`、`Intermediate/`、`Saved/`、`DerivedDataCache/`、`Packaged/` 都是构建产物，已在 `.gitignore` 里排除。
