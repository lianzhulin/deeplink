# 电子寄存柜（Share Locker）项目方案设计
> 版本：v0.2 | 日期：2026-09-07 | 状态：原型阶段

---

## 1. 项目愿景
**让任何两个接入互联网的实体（人/程序/机器人），无需注册、无需互加好友、无需预建立连接，仅凭一串随机凭证即可随时随地共享一份信息；信息在有效期内可被任意次数查看，到期自动销毁，凭证失效。**

核心语义：**共享优先，时效隔离，零预连接**。

---

## 2. 核心痛点（为什么需要它）
现有信息共享工具存在三个硬伤：
1. **强身份绑定**：微信/QQ需加好友，邮箱需知地址，临时场景下无法快速对接
2. **预连接成本**：需提前交换联系方式、建立会话，对陌生人/临时程序不友好
3. **时效模糊**：要么永久存储（留下痕迹），要么手动删除（容易忘），无自动过期机制

**典型场景**：给陌生测试环境塞临时配置链接、给刚认识的合作伙伴发临时文件路径、让两个微服务实例临时共享密钥片段——不想加好友、不想留痕迹、不想预连接。

---

## 3. 核心设计原则（不可动摇）
| 原则 | 说明 | 对应核心理念 |
|---|---|---|
| **无身份（No Identity）** | 零注册、零登录、零鉴权，服务端不存任何用户标识 | 不需要"谁"存的，只认凭证 |
| **零预连接（Zero Pre-Connection）** | 存取完全独立，无需协商、无需握手 | 两个实体可从未接触过 |
| **唯一凭证（Unique Token）** | 凭证是访问信息的**唯一钥匙**，不可猜、不可枚举 | 拿到凭证=拿到访问权 |
| **时效隔离（TTL Bound）** | 每条信息有明确有效期，到期自动销毁、凭证回收 | 临时共享，不留痕迹 |
| **共享优先（Share-First）** | 默认查看不销毁，支持多次共享；仅 TTL 过期才自动销毁 | 存一次可给多人看 |

---

## 4. 功能架构
### 4.1 核心功能（3 个）
| 功能 | 说明 | 触发方式 |
|---|---|---|
| **寄存（Store）** | 提交任意字符串（后期扩展文件/二进制）→ 系统分配唯一凭证 → 返回凭证+有效期 | `POST /api/store` |
| **查看（View）** | 输入凭证 → 返回原始内容（**不销毁**，可多次查看，直到 TTL 过期） | `GET /api/view?token=xxx` |
| **凭证管理** | 容量硬上限、自动回收过期凭证、零碰撞随机分配 | 服务端内部逻辑 |

### 4.2 辅助功能（原型已实现）
- 双端切换界面：`/store`（寄存）+ `/view`（查看共享），Tab 切换，纯 HTML/CSS/原生 JS，无外部依赖
- 错误码分级：`200` 成功 / `400` 格式错误 / `404` 凭证无效/过期 / `503` 容量满

---

## 5. 技术方案
### 5.1 凭证分配算法（核心）
#### 数据模型
```python
# lockers: 已寄存的信息 (仅服务端内存, 不持久化)
lockers = {
    token: {
        "data": str,         # 寄存内容
        "expires_at": float, # 过期时间戳
        "hits": int          # 查看次数 (可选)
    }
}

# free_pool: 空闲凭证池 = 全集 - lockers.keys() (预先 shuffle, 与 lockers 互补)
free_pool: list[str] = [str(i).zfill(4) for i in range(10000)]
random.shuffle(free_pool)
```

#### 分配逻辑（O(1) 零碰撞）
```python
def allocate_token() -> str | None:
    # 1. 懒清理: 先回收过期项 (顺手做, 无后台线程)
    purge_expired()
    # 2. 容量硬上限: 不超 MAX_SLOTS (全集 10000 的一半: 4999)
    if len(lockers) >= MAX_SLOTS or not free_pool:
        return None
    # 3. Fisher-Yates O(1) 随机抽: 随机位置 swap 到末尾, 再 pop
    idx = random.randrange(len(free_pool))
    idx_last = len(free_pool) - 1
    free_pool[idx], free_pool[idx_last] = free_pool[idx_last], free_pool[idx]
    return free_pool.pop()
```

#### 关键特性
- **零碰撞**：从 free_pool 抽的一定不在 lockers 里，无需重试
- **不可预测**：预先 shuffle + 随机 swap，凭证不连续、不规律
- **自动回收**：过期/删除的凭证 append 回 free_pool，自动复用

### 5.2 存储与时效
- **存储介质**：纯内存（原型阶段），进程重启即清空，无持久化
- **时效控制**：`TTL_SECONDS = 300`（默认 5 分钟，可配置），每条寄存自带 `expires_at` 时间戳
- **过期回收**：懒清理（每次存取操作顺手扫一遍，不启动后台线程）

### 5.3 API 规范
| 方法 | 路径 | 入参 | 出参 | 错误码 |
|---|---|---|---|---|
| POST | `/api/store` | `{"data": "xxx"}` | `{"ok": true, "token": "1234", "expire_time": "15:30:00"}` | 400 内容空/超长 / 503 容量满 |
| GET | `/api/view` | `token=1234` | `{"ok": true, "data": "xxx", "hits": 3}` | 400 格式错 / 404 无效/过期 |

---

## 6. 安全设计（4 层防线）
| 层级 | 设计 | 防御目标 |
|---|---|---|
| 1 | 随机凭证（不连续、不可猜） | 暴力枚举 |
| 2 | 容量硬上限（4999 个有效凭证） | 枚举攻击的收益上限 |
| 3 | TTL 自动过期 | 长期凭证泄露风险 |
| 4 | 无持久化存储 | 服务端数据泄露风险 |

---

## 7. 原型实现现状（对齐共享理念前的调整）
### 7.1 已实现（90% 核心逻辑）
- ✅ 单文件零依赖（`locker.py`，仅 Python 标准库）
- ✅ free_pool 差集分配 + Fisher-Yates O(1) 算法
- ✅ 懒清理过期项（无后台线程）
- ✅ MAX_SLOTS=4999 硬上限（返回 503）
- ✅ 双页 HTML 界面（寄存/查看 Tab 切换）
- ✅ 错误码分级

### 7.2 待调整（对齐共享理念）
| 调整项 | 原实现 | 新实现 | 原因 |
|---|---|---|---|
| 查看语义 | `GET /api/get` **取回即销毁** | `GET /api/view` **查看不销毁** | 共享优先，支持多次查看 |
| 前端命名 | "取回" Tab | "查看共享" Tab | 对齐语义 |
| 查看计数 | 无 | 新增 `hits` 字段 | 让分享者知道查看次数 |

---

## 8. 后续扩展路线（原型之后）
| 阶段 | 内容 | 触发条件 |
|---|---|---|
| **P1 存储持久化** | 用 Redis ZSET + EXPIRE 替换内存 lockers，支持多实例共享 | 单机容量/并发不够 |
| **P2 凭证强化** | 用 `secrets.randbelow` 替换 `random.randrange`（CSPRNG，抗暴力破解） | 有安全合规要求 |
| **P3 数据类型扩展** | 支持文件/二进制/富文本（当前仅字符串，8KB 上限） | 场景需要 |
| **P4 容量扩容** | `TOKEN_DIGITS=5` → 10 万池，MAX_SLOTS=49999 | 用户量上来 |
| **P5 水平扩展** | Redis 做共享存储 + 多实例部署 + 负载均衡 | 高并发场景 |

---

## 9. 附录
### 9.1 原型启动命令
```bash
python3 locker.py              # 默认 8000
python3 locker.py 9000         # 自定义端口
```

### 9.2 核心算法伪代码（完整版）
```python
# 初始化
free_pool = shuffle(range(0, 10000))  # 预先洗好 10000 个四位号
lockers = {}                          # 空柜子
MAX_SLOTS = 4999                      # 硬上限

# 分配凭证
def allocate():
    purge_expired()                   # 顺手清过期
    if len(lockers) >= MAX_SLOTS:     # 满柜
        return None
    idx = random.randrange(len(free_pool))
    free_pool[idx], free_pool[-1] = free_pool[-1], free_pool[idx]
    return free_pool.pop()

# 寄存
def store(data):
    token = allocate()
    if not token: return 503
    lockers[token] = {data, expires_at=now+300, hits=0}
    return 200, token

# 查看 (不销毁)
def view(token):
    purge_expired()
    item = lockers.get(token)
    if not item: return 404
    item["hits"] += 1
    return 200, item.data
```
