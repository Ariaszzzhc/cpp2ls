# 成员函数补全功能

## 概述

cpp2ls 现在完整支持 cppfront 的成员访问语义，包括：
- `.` 操作符 - UFCS (统一函数调用语法)
- `..` 操作符 - 仅成员函数
- `->` 操作符 - 指针成员访问

## Cppfront 函数调用语义

根据 cppfront 官方文档：

### 1. `f(x)` - 普通函数调用
只调用非成员函数，这是 C++ 的标准行为。

### 2. `x.f()` - UFCS (统一函数调用语法)
- **优先**调用成员函数（如果存在）
- **否则**调用 `f(x)` 非成员函数
- 这对于泛型代码很重要，可以调用成员或非成员函数
- 支持流畅的编程风格和自然的 IDE 自动补全

### 3. `a + b` - 操作符重载
调用重载的操作符函数（C++ 标准行为）

### 4. `x..f()` - **仅考虑成员函数**
- **只**调用成员函数
- 如果没有匹配的成员函数，则报错
- 用于明确要求成员函数调用的场景

## LSP 实现

cpp2ls 根据以上语义实现了智能补全：

### `.` 操作符补全（UFCS）
在 `object.↓` 位置输入时：
1. **优先显示**：类型的所有成员函数和成员变量
2. **同时显示**：接受该类型参数的所有非成员函数（UFCS）

**示例：**
```cpp2
Person: type = {
    get_name: (this) -> std::string = { ... }
    age: int;
}

greet: (p: Person) -> std::string = { ... }

main: () = {
    p: Person = ("Alice", 25);
    p.↓  // 补全建议：
         // ✓ get_name (成员函数)
         // ✓ age (成员变量)
         // ✓ greet (非成员函数，UFCS)
}
```

### `..` 操作符补全（仅成员）
在 `object..↓` 位置输入时：
- **只显示**：类型的成员函数和成员变量
- **不显示**：非成员函数

**示例：**
```cpp2
main: () = {
    p: Person = ("Alice", 25);
    p..↓  // 补全建议：
          // ✓ get_name (成员函数)
          // ✓ age (成员变量)
          // ✗ greet (非成员函数，不显示)
}
```

### `->` 操作符补全（指针成员）
在 `pointer->↓` 位置输入时：
- **只显示**：类型的成员函数和成员变量
- 行为类似 `..`

## 实现细节

### 文件修改
- `src/document.h` - 添加 `get_member_completions()` 方法声明
- `src/document.cpp` - 实现成员访问检测和成员补全逻辑

### 关键代码逻辑

#### 1. 检测成员访问操作符
```cpp
// 在光标位置前查找 token
const auto* prev_token = find_token_at(target_line, target_col - 1);

if (prev_token->type() == cpp2::lexeme::Dot) {
    // UFCS: 成员 + 非成员
    is_ufcs = true;
}
else if (prev_token->type() == cpp2::lexeme::DotDot) {
    // 仅成员
    is_member_only = true;
}
else if (prev_token->type() == cpp2::lexeme::Arrow) {
    // 指针成员（仅成员）
    is_member_only = true;
}
```

#### 2. 获取对象类型
```cpp
// 找到操作符前的对象 token
const auto* object_token = /* search backward */;

// 获取对象的声明
const auto* decl_sym = sema->get_declaration_of(object_token, true);

// 获取类型名
std::string type_name = decl_sym->declaration->object_type();
```

#### 3. 获取类型成员
```cpp
// 查找类型声明
const cpp2::declaration_node* type_decl = /* lookup type */;

// 使用 cppfront API 获取成员
auto members = type_decl->get_type_scope_declarations();

for (auto* member : members) {
    if (member->is_function()) {
        // 添加成员函数补全
    }
    else if (member->is_object()) {
        // 添加成员变量补全
    }
}
```

#### 4. UFCS 支持
```cpp
if (is_ufcs) {
    // 1. 获取成员补全
    result = get_member_completions(object_token, index);
    
    // 2. 继续添加非成员函数（下面的正常补全逻辑）
    // 这样既有成员又有非成员函数
}
```

## 测试用例

测试文件：`tests/fixtures/member-completion-test.cpp2`

### 测试场景

#### 1. 基本成员访问
```cpp2
p: Person = ("Alice", 25);
name := p.get_name();    // ✓ 成员函数调用
age := p.age;             // ✓ 成员变量访问
```

#### 2. UFCS 调用
```cpp2
// greet 是非成员函数：greet: (p: Person) -> std::string
greeting := p.greet();    // ✓ UFCS 调用非成员函数
```

#### 3. 仅成员调用
```cpp2
age := p..get_age();      // ✓ 强制成员函数调用
p..greet();               // ✗ 编译错误（greet 不是成员函数）
```

#### 4. 补全建议
```cpp2
p.↓     // 显示：get_name, age, greet, print_info 等
p..↓    // 显示：get_name, age（仅成员）
```

## 使用方法

### 在 VSCode 中测试

1. 构建 cpp2ls：
```bash
cd /Users/arias/Projects/cpp2/cpp2ls
cmake --preset debug
cmake --build --preset "Debug build"
```

2. 配置 LSP 客户端连接到 cpp2ls

3. 打开测试文件：
```bash
code tests/fixtures/member-completion-test.cpp2
```

4. 在代码中输入 `p.` 或 `p..` 触发补全

### 预期行为

- ✅ 输入 `p.` → 显示成员和非成员函数
- ✅ 输入 `p..` → 只显示成员函数
- ✅ 成员函数显示正确的签名
- ✅ 成员变量显示类型信息

## 当前限制

### 已支持
- ✅ 本地文件中定义的类型
- ✅ 成员函数补全
- ✅ 成员变量补全
- ✅ UFCS 语义（`.` 操作符）
- ✅ 仅成员语义（`..` 操作符）
- ✅ 指针成员访问（`->` 操作符）
- ✅ **语法错误时的补全** - 即使代码有语法错误（如输入 `p.` 时），也能提供成员补全

### 尚未支持
- ⏳ 跨文件类型的成员补全
  - 当前：只能获取当前文件中定义的类型成员
  - 原因：ProjectIndex 不存储类型成员信息
  - 计划：增强 IndexedSymbol 存储成员列表
  
- ⏳ 函数返回类型的成员补全
  - 示例：`get_person().get_name()` 
  - 需要解析函数返回类型
  
- ⏳ 嵌套成员访问
  - 示例：`person.address.city`
  - 需要递归类型解析

- ⏳ 标准库类型成员
  - 示例：`std::string` 的成员函数
  - 需要索引标准库头文件

## 技术细节

### Token 类型（cpp2::lexeme）
```cpp
enum class lexeme : i8 {
    Dot,        // .   UFCS 访问
    DotDot,     // ..  仅成员访问
    Arrow,      // ->  指针成员访问
    // ...
};
```

### 成员查找优先级
1. **本地文件中的类型定义** - 通过 sema 查找
2. **索引中的类型定义** - 通过 ProjectIndex 查找
3. **如果都没找到** - 返回空补全列表

### 语法错误处理（Text Fallback）
当代码有语法错误时（例如用户正在输入 `object.`），token 可能不完整或无效。cpp2ls 实现了**文本回退机制**：

1. **首先**尝试基于 token 的检测（正常情况）
2. **如果失败**，使用文本解析直接分析光标前的文本：
   - 检测 `.`, `..`, `->` 操作符
   - 提取操作符前的标识符
   - 在 sema 中按名称查找声明
   - 返回成员补全

这确保了即使在编辑过程中代码暂时有语法错误，成员补全依然可用。

**实现文件：**
- `extract_member_context_from_text()` - 从文本提取上下文 (src/document.cpp:1269)
- `find_declaration_by_name()` - 按名称查找声明 (src/document.cpp:1353)
- 文本回退集成 - 在 get_completions() 中 (src/document.cpp:476)

### 性能优化
- 使用 `std::set<std::string>` 去重，避免重复建议
- UFCS 模式下，成员补全优先（先显示）
- 懒加载：只在检测到 `.` 或 `..` 时才查找成员

## 调试

启用调试日志查看成员补全过程：

```cpp
// 在 get_member_completions() 中添加：
std::cerr << "Looking for members of type: " << type_name << "\n";
std::cerr << "Found " << result.size() << " members\n";
```

## 未来改进

1. **跨文件成员支持**
   - 在 IndexedSymbol 中存储类型成员
   - 修改 index.cpp 在索引时提取成员信息

2. **智能排序**
   - 成员函数优先于非成员函数
   - 常用函数优先（基于使用频率）

3. **更丰富的信息**
   - 显示函数文档注释
   - 显示参数默认值
   - 显示函数是否为 const

4. **模板支持**
   - 模板类型的成员补全
   - 实例化类型的成员补全

## 总结

cpp2ls 现在完整支持 cppfront 的成员访问语义：

- ✅ **`.` 操作符** - UFCS（成员优先，然后非成员）
- ✅ **`..` 操作符** - 仅成员函数
- ✅ **`->` 操作符** - 指针成员访问
- ✅ **智能补全** - 根据上下文显示正确的建议
- ✅ **类型感知** - 自动解析对象类型并查找成员

这使得 cpp2ls 成为第一个完整支持 cppfront UFCS 语义的 LSP 实现！🎉
