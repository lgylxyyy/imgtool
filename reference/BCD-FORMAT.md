# BCD（Boot Configuration Data）格式规格

> 来源：`/workspace/images/Windows 10 UEFI.qcow2` 的 ESP
> `\EFI\Microsoft\Boot\BCD`（28,672 字节）与 `\EFI\Microsoft\Recovery\BCD`（12,288 字节）
> 本文件由实际解析得出，用于指导 C 写入器实现。

## 一、容器：Windows 注册表 hive（REGF）

```
偏移 0  : 'regf' 签名
偏移 4  : 主次序号（u32 LE）
偏移 8  : 次序号（u32 LE）
```
两者相等 → 干净关闭。**本项目使用 `libhivex` 读写，避免手写 hive 校验。**

## 二、目录树

```
NewStoreRoot/                    ← 根名固定（不是 "BCD"）
├── {Description}
└── {Objects}/
    ├── {9dea862c-5cdd-4e70-acc1-f32b344d4795}/   Windows Boot Manager
    │   └── Elements/
    │       ├── 11000001  Description        (bin,88B)
    │       ├── 12000002  Device             (str) \EFI\Microsoft\Boot\bootmgfw.efi
    │       ├── 12000004  OSDevice           (str) "Windows Boot Manager"
    │       ├── 12000005  Locale             (str) zh-CN
    │       ├── 14000006  Inherits           (multi) {7ea2e1ac-…}
    │       ├── 23000003  DisplayMessageOverride (str) {d14b9cb4-…}
    │       ├── 23000006  …                  (str) {d14b9cb3-…}
    │       ├── 24000001  DisplayOrder       (multi) {d14b9cb4-…}
    │       ├── 24000010  ToolsDisplayOrder  (multi) {b2721d73-…}
    │       └── 25000004  ★RecoverySequence  (bin,8B) 0300000000000000
    └── {d14b9cb4-…}/  Windows OS Loader
        ├── 11000001  Description      (bin,88B)
        ├── 12000002  Device           (str) \Windows\system32\winload.efi
        ├── 12000004  OSDevice         (str) "Windows 11"  ← 显示名
        ├── 12000005  Locale           (str) zh-CN
        ├── 14000006  Inherits         (multi) {6efb52bf-…}
        ├── 16000060  DetectHAL        (bin,1B) 01
        ├── 17000077  …                (bin,8B) 7500001500000000
        ├── 21000001  OSDevice(对象)   (bin,88B)
        ├── 22000002  SystemRoot       (str) \Windows
        ├── 23000003  ResumeObject    (str) {d14b9cb3-…}
        ├── 25000020  …                (bin,8B) 0000000000000000
        └── 250000c2  …                (bin,8B) 0100000000000000
    {d14b9cb3-…}/  Windows Resume (winresume.efi)
```

## 三·〇、★ 元素 ID 的编码规律（权威）

> 来源：Geoff Chappell《BCD Elements》
> geoffchappell.com/notes/windows/boot/bcd/elements.htm
> （bcdedit /? + BCDSRV.DLL 类型库反查，2026-10 经 Tavily 检索确认）

元素 ID 是个 dword，按**位段**解读：

| 掩码 | 含义 | 取值 |
|---|---|---|
| `0xF0000000` | **class** | 1=library 2=application 3=device 4=template |
| `0x0F000000` | **format** | 1=device 2=string 3=GUID 4=GUID list<br>5=integer 6=boolean 7=integer list |

**据此可直接读出任意元素的类型**，例如：
```
14000008 → class=1(lib)  format=4(GUID list)   → RecoverySequence
24000001 → class=2(app)  format=4(GUID list)   → DisplayOrder
25000004 → class=2(app)  format=5(integer)     → Timeout(秒) ★非RecoverySequence
22000002 → class=2(app)  format=2(string)      → SystemRoot
23000003 → class=2(app)  format=3(单个GUID)    → ResumeObject
16000009 → class=1(lib)  format=6(boolean)     → recoveryenabled
```

## 三·一、已确认元素一览

| ID | 类型 | 含义 | bcdedit 名 |
|---|---|---|---|
| 11000001 | device(二进制88B) | Description | description |
| 12000002 | string | Device（文件路径） | device |
| 12000004 | string | OSDevice / 显示名 | osdevice / description |
| 12000005 | string | Locale | locale |
| 14000006 | GUID list | Inherits | inherit |
| **14000008** | **GUID list** | **★ RecoverySequence** | **recoverysequence** |
| 1500004b | integer | — | — |
| 16000009 | boolean | 自动恢复开关 | recoveryenabled |
| 16000060 | boolean | DetectHAL | detecthal |
| 21000001 | device | OSLoader OSDevice | — |
| 22000002 | string | SystemRoot | systemroot |
| 23000003 | 单 GUID | ResumeObject | resumeobject |
| 24000001 | GUID list | DisplayOrder | displayorder |
| 24000010 | GUID list | ToolsDisplayOrder | toolsdisplayorder |
| **25000004** | **integer** | **Timeout(秒)** | timeout |
| 250000e0 | integer | bootstatuspolicy | bootstatuspolicy |
| 32000004 | string | RAMDisk SDI 路径 | ramdisksdipath |

## 三、元素 ID 命名规律

```
11xxxxxx  BcdLibraryObject_*      （对象级描述）
12xxxxxx  BcdLibraryObject_Device*
14xxxxxx  Inherits / inheritance 相关
16xxxxxx  BcdOSLoaderObject_*     （加载器开关）
17xxxxxx  BcdOSLoaderObject_* 数值
21xxxxxx  BcdOSLoaderObject_Device*
22xxxxxx  BcdOSLoaderObject_SystemRoot 等
23xxxxxx  引用类（resume / displaymessage）
24xxxxxx  列表类（DisplayOrder / ToolsDisplayOrder）
25xxxxxx  开关/引用（RecoverySequence 等）
```

**已确认的元素**：
| ID | 含义 | 类型 |
|---|---|---|
| 11000001 | Description（含显示名、语言） | bin 88B |
| 12000002 | Device（文件路径或分区对象） | str |
| 12000004 | OSDevice / 显示名 | str |
| 12000005 | Locale | str "zh-CN" |
| 14000006 | Inherits | multi-str |
| 16000060 | DetectHAL | bin 1B |
| 21000001 | OSLoader OSDevice（对象形式） | bin 88B |
| 22000002 | SystemRoot | str `\Windows` |
| 23000003 | ResumeObject 引用 | str GUID |
| 24000001 | DisplayOrder | multi-str GUID 列表 |
| **25000004** | **RecoverySequence** | bin 8B |

## 四、★ 可移植性关键结论

1. **`12000002`/`12000004` 存的是文件路径**（如 `\Windows\system32\winload.efi`），
   **不是分区 GUID** → 设备由引导器按"同卷/扫描"解析，**跨盘可用**。
2. bootmgr 的 device 是 `\EFI\Microsoft\Boot\bootmgfw.efi`（相对 ESP 自身）→ 天然可移植。
3. **对象间用 GUID 互引**（Inherits/Resume/DisplayOrder/RecoverySequence）。

## 五、实现路线（C）

```
① 用 libhivex 打开参考 BCD（read=true）
② 修改需要的项（显示名 / RecoverySequence / 新增恢复对象）
③ commit() 落盘
④ 写入 ESP 的 \EFI\Microsoft\Boot\BCD
```
> 不手写 hive 格式 —— libhivex 已处理校验、cell 分配、重平衡。

## 六、许可证

`libhivex`：**LGPL-2.1+**（动态链接即可）。已在 THIRD_PARTY.md 登记。
