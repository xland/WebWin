# 右键菜单 API（`menu` 域）

`Menu` 是**类型**（`window.Menu`），不是实例。菜单要显式创建：

```ts
const menu = await Menu.create([
  { label: 'Item1', type: 'radio' },
  { label: 'Item2', type: 'radio' },
  { label: 'Item3', type: 'radio', checked: true },
  { type: 'separator' },
  {
    label: 'View',
    submenu: [
      { label: 'Checkbox', type: 'checkbox' },
      { label: 'Normal',   id: 'abcdefg', onclick: () => {} }
    ]
  }
]);
```

`Menu.create(...)` 是 async：菜单是 C++ 里的 Win32 对象，得等它建好才拿得到 id。

## 菜单项

```ts
interface MenuItem {
  type?:     'normal' | 'radio' | 'checkbox' | 'separator'; // 缺省 normal
  label?:    string;                                          // separator 不需要
  id?:       string;                                          // 不填自动生成；同一菜单内不能重复
  checked?:  boolean;                                         // radio / checkbox 初始勾选
  disabled?: boolean;
  submenu?:  MenuItem[];                                      // 递归；带 submenu 的项不接受 onclick
  onclick?:  (info: { checked?: boolean }) => void;           // radio / checkbox 才有 checked
}
```

### 类型行为

- `normal`：点 → `onclick`
- `radio`：**连续**的 radio 是同一组，组内互斥；用 `separator` 或其它类型断开
- `checkbox`：每次点击在勾选 / 不勾选之间切换
- `separator`：分隔线（也会断开 radio 组）

## 生命周期

| 方法 | 行为 |
| --- | --- |
| `Menu.create(items)` | 建菜单（async） |
| `menu.popup()` | 在鼠标处弹 |
| `menu.popup({ x, y })` | 在屏幕坐标 (x, y) 处弹 |
| `menu.destroy()` | 销毁菜单；**忘调就泄漏** |

弹菜单期间再来一次 `popup()` → 返回错误"已有菜单在显示"。

## 挂到窗口

```ts
await win.setContextMenu(menu);   // 挂到当前窗口
await win.setContextMenu(null);   // 解绑（回到浏览器默认的右键菜单）
```

挂上后在该窗口的**所有文档**里点右键都弹这个菜单。

## 路由

菜单项点击**只推给创建这个菜单的那个页面**（顶级文档或某个 iframe）。
A 页面建的菜单，点哪都把回调送回 A；即使挂到 X 窗口，也只送回 A。

## 错误处理

同其他域：catch 里 e 可能是字符串。

## 尚未支持

- 动态改菜单（改 label / checked / disabled）—— 见 `arch/01-api.md` 的 T007
- 多级子菜单：允许，但实际用得不多，没有专门测试
