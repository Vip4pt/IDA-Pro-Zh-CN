# IDA_zh_CN —— IDA Professional 9.5 界面汉化补丁

运行时界面汉化,两层配合:
1. **钩子层(zh_hook.dll)**: inline hook Qt6 文本入口 —— 绘制层
   (`QPainter::drawText`、`QFontMetrics::horizontalAdvance`)、源头层
   (`QLabel/QAbstractButton/QAction::setText`、`setToolTip`、`QWidget::setWindowTitle`)
   与富文本层(`QTextDocument::setHtml`(标签感知)/`setPlainText`),另含
   uxtheme `DrawThemeText[Ex]`。按词典精确匹配替换,未命中原样放行。
2. **遍历层(ida_zh_CN.py)**: 启动后用 PySide6 遍历全部窗口/菜单树
   (QApplication.allWidgets),已显示但先于钩子创建的界面(如 Quick Start)
   就地翻译; 未翻译文本枚举到 `ui_inventory.tsv` 供补词典。

不修改 IDA 任何文件,不触碰许可逻辑;卸载 = 删除本插件。

## 文件布局

```
plugins\
├── ida_zh_CN.py        加载器(IDAPython 插件,必须在 plugins 根目录)
└── ida_zh_CN\
    ├── zh_hook.dll     钩子 DLL(x64, MinHook, 18 个钩子点)
    ├── zh_CN.tsv       词典:UTF-8,每行 "原文<TAB>译文",'#' 开头为注释
    ├── captured.tsv    钩子采集到的未翻译界面字符串(自动追加)
    ├── ui_inventory.tsv 全量界面文本清单(PySide6 遍历导出)
    ├── zh_hook.log     安装/诊断日志
    └── src\            源码与构建脚本(zh_hook.c、MinHook、build_vs.bat、字典分片)
```

## 使用

- 启动 IDA 即生效(词典约 930 条:主菜单与多数子菜单/右键菜单/窗口标题/常用按钮/
  Quick Start/反编译器上下文)。
- **Edit > Plugins > IDA_zh_CN** = 重载词典并重新遍历(改完 zh_CN.tsv 点它即可)。
- Python 控制台高级控制:
  ```python
  p = idaapi.find_plugin("IDA_zh_CN", True)
  p.run(0)   # 0=重载词典+遍历 1=统计 2=关采集 3=开采集 4=仅遍历
  ```
- 补词典流程:正常使用 IDA → 未命中的界面文本自动进 `captured.tsv`,
  全量可翻文本在 `ui_inventory.tsv`(格式 `类型|路径<TAB>文本`)→ 人工翻译后
  把 "原文\t译文" 追加到 `zh_CN.tsv` → Edit > Plugins > IDA_zh_CN 重载。
- 注意助记符:IDA 菜单的 `&` 位置不固定(实际是 `Deb&ugger`、`Searc&h`),
  词典键以 captured.tsv / ui_inventory.tsv 里的原文为准。

## 安装 / 卸载

- 安装:把 `ida_zh_CN.py` 和 `ida_zh_CN\` 复制到 `%APPDATA%\Hex-Rays\IDA Pro\plugins\`。
- 卸载:删除上述两项。

## 重新构建

需 VS 2022(x64);MinHook 源码已内置于 `src\MinHook\`(MIT)。

```bat
cd plugins\ida_zh_CN\src
build_vs.bat
```

产物为 `..\zh_hook.dll`。

## 已知限制

- 只翻译词典精确命中的文本;动态拼接的句子不翻(可自行加整句词条)。
- 反汇编/反编译正文刻意不翻。
- IDA 升级(尤其 Beta)若更换 Qt 版本,导出符号名可能变化:钩子解析失败会
  降级为"不汉化"并记录在 zh_hook.log,不会崩溃。
- QLineEdit/QTextEdit 内的编辑文本不经绘制钩子,不翻(也不应翻)。
