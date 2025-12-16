# 跨文件符号解析测试用例

本目录包含用于测试 cpp2ls 跨文件符号解析功能的测试文件。

## 文件结构

```
include-test/
├── main.cpp2       # 主文件，包含 utils.h2 和 advanced.h2
├── utils.h2        # 基础工具函数和类型
├── advanced.h2     # 高级功能，包含 types.h2
└── types.h2        # 基础类型定义
```

## 依赖关系图

```
main.cpp2
  ├── includes utils.h2
  │     ├── add()
  │     ├── multiply()
  │     ├── Point (type)
  │     └── make_point()
  │
  └── includes advanced.h2
        ├── includes types.h2
        │     ├── Vector2D (type)
        │     └── Status (type)
        ├── create_vector()
        ├── normalize_vector()
        └── get_status_name()
```

## 测试的LSP功能

### 1. **Go to Definition (跳转到定义)**
在 main.cpp2 中：
- 点击 `add` → 应跳转到 utils.h2:3
- 点击 `multiply` → 应跳转到 utils.h2:7
- 点击 `Point` → 应跳转到 utils.h2:11
- 点击 `create_vector` → 应跳转到 advanced.h2:5
- 点击 `Vector2D` → 应跳转到 types.h2:3
- 点击 `Status::OK` → 应跳转到 types.h2:12

### 2. **Hover (悬停信息)**
悬停在以下符号上应显示类型信息：
- `add` → 显示函数签名 `(x: int, y: int) -> int`
- `Point` → 显示类型定义
- `vec.length()` → 显示方法签名和返回类型
- `Status::OK` → 显示常量值

### 3. **Completion (自动完成)**
在 main.cpp2 中输入时：
- 输入 `ad` → 应建议 `add` (from utils.h2)
- 输入 `cre` → 应建议 `create_vector` (from advanced.h2)
- 输入 `Vec` → 应建议 `Vector2D` (from types.h2)
- 优先级：直接包含的文件 > 全局索引

### 4. **Find References (查找引用)**
- 查找 `add` 的引用 → 应找到 main.cpp2 中的调用
- 查找 `Vector2D` 的引用 → 应找到 advanced.h2 和 main.cpp2 中的使用

### 5. **Document Symbol (文档符号)**
- utils.h2 应列出：add, multiply, Point, make_point
- types.h2 应列出：Vector2D, Status
- advanced.h2 应列出：create_vector, normalize_vector, get_status_name

### 6. **Workspace Symbol (工作区符号搜索)**
- 搜索 "vector" → 应找到 create_vector, normalize_vector, Vector2D
- 搜索 "point" → 应找到 Point, make_point

## 依赖关系跟踪

### 正向依赖 (Dependencies)
- main.cpp2 → [utils.h2, advanced.h2]
- advanced.h2 → [types.h2]
- utils.h2 → []
- types.h2 → []

### 反向依赖 (Reverse Dependencies)
- utils.h2 ← [main.cpp2]
- advanced.h2 ← [main.cpp2]
- types.h2 ← [advanced.h2]

### 级联更新测试
1. 修改 types.h2 中的 Vector2D 定义
2. cpp2ls 应该自动重新索引：
   - advanced.h2（直接依赖）
   - main.cpp2（通过 advanced.h2 间接依赖）
3. 所有相关文件的诊断信息应更新

## 符号解析优先级

cpp2ls 使用以下 3 层查找策略：

1. **本地文件** (通过 cppfront sema)
   - 最快
   - 同一文件内的符号

2. **直接包含的文件**
   - 中等优先级
   - 仅搜索 #include 指令中列出的文件
   - 更精确，避免命名冲突

3. **全局工作区索引**
   - 最低优先级
   - 搜索所有索引的文件
   - 用于未直接包含但在工作区中的符号

## 实现细节

### 关键数据结构
- `m_dependencies`: URI → Set<include_path>
  - 存储每个文件的直接 include
  
- `m_reverse_dependencies`: URI → Set<dependent_URI>
  - 存储哪些文件包含了给定文件
  - 用于级联更新

### 包含路径解析
`resolve_include(include_name, current_file_uri)` 尝试：
1. 相对于当前文件目录
2. 相对于工作区根目录
3. 自动添加 .h2 扩展名

### 修改的文件
- `src/document.h` - 添加 m_includes 成员
- `src/document.cpp` - 增强 get_definition, get_hover, get_completions
- `src/index.h` - 添加依赖图数据结构
- `src/index.cpp` - 实现依赖管理和路径解析
- `src/server.cpp` - 集成依赖更新和级联重新索引

## 运行测试

```bash
# 1. 构建 cpp2ls
cd /Users/arias/Projects/cpp2/cpp2ls
cmake --preset debug
cmake --build --preset "Debug build"

# 2. 在编辑器中打开测试文件
# 使用 LSP 客户端（如 VSCode）连接到 cpp2ls

# 3. 测试跨文件功能
# - 在 main.cpp2 中右键点击 "add"，选择 "Go to Definition"
# - 悬停在 "Vector2D" 上查看类型信息
# - 输入代码触发自动完成
```

## 预期行为

✅ **应该工作：**
- 跳转到直接包含文件中的符号定义
- 显示来自包含文件的悬停信息
- 自动完成建议包含文件中的符号
- 修改 .h2 文件时级联更新依赖文件

❌ **尚未支持：**
- 传递性 include（A includes B includes C，A 不直接 include C）
  - 当前：仅直接包含的文件
  - 未来：将添加传递性解析
- 条件编译（#ifdef 等）
- Include 守卫（#pragma once）

## 调试日志

cpp2ls 在 stderr 输出有用的调试信息：

```
Document opened: file:///path/to/main.cpp2
Re-indexed 1 dependent files
Found 5 references
Returning 42 completion items
```

查看日志以调试跨文件符号解析问题。
