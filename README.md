# 红警3 纯呈现层零 OOS 战争迷雾清除引擎 (ra3fog / RedQueen)

基于 Direct3D 9 运行时 Hook（`d3d9.dll` 劫持）的红警3本地全功能战争迷雾清除方案。

## 核心特性
- **零联机 OOS（Out of Sync）**：不修改任何 Simulation 游戏同步状态，完全保持战网联机 100% 同步。
- **纯 Presentation/D3D9 呈现层旁路**：劫持 `IDirect3DDevice9` 虚表与 Shroud 纹理状态。
- **独立热键**：`F9`（迷雾开关）、`F10`（强放超武）、`F8`（隐形）、`F12`（全图建造）。

## 项目结构
- `src/d3d9proxy.cpp`：D3D9 导出函数劫持与 Device Hook 核心入口。
- `src/ra3fog_engine.cpp`：Shroud 纹理旁路与 3D 地形迷雾消除引擎。
- `src/ra3hud.cpp`：HUD / 小地图单位点与 UI 目标提示。
- `src/ra3macro.cpp`：热键控制与宏逻辑。
- `src/ra3fog.h` / `src/ra3ipc.h`：核心定义与 IPC 头文件。
- `build.ps1.bat`：一键 MSVC 自动化编译脚本。

## 编译方法
直接运行 `build.ps1.bat`，生成产物位于 `build/d3d9.dll`。放入游戏主程序目录（`Data/`）即可生效。
