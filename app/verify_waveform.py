# -*- coding: utf-8 -*-
"""
波形校验：把「新版编码器」和「旧版 APK 的编码器」逐元素对比，
再用 Arduino-IRremote 的 decodeNEC 逻辑把波形解回来，看车端拿到的是什么码。

旧版编码器 = 反汇编 com.zyar20a.irremote.NecSender$Companion.encode(long)
    out[0]=9000 out[1]=4500
    for (bit = 31; bit >= 0; bit--) { out[i++]=560; out[i++] = bit?1690:560 }
    out[66] = 最后一个 space
"""
CODES = {
    "前进": 0x00FF629D,
    "后退": 0x00FFA857,
    "左转": 0x00FF22DD,
    "右转": 0x00FFC23D,
    "停车": 0x00FF02FD,
    "左旋": 0x00FFE01F,
    "右旋": 0x00FF906F,
}


def old_encode(code):
    """旧版 APK 的 encode：高位先发，67 个元素"""
    out = [9000, 4500]
    space = 560
    for bit in range(31, -1, -1):
        out.append(560)
        space = 1690 if (code >> bit) & 1 else 560
        out.append(space)
    out.append(space)          # out[66]
    return out


def new_encode(code):
    """新版 MainActivity.necFrame：必须和 old_encode 一模一样"""
    p = [0] * 67
    k = 0
    p[k] = 9000; k += 1
    p[k] = 4500; k += 1
    space = 560
    for bit in range(31, -1, -1):
        p[k] = 560; k += 1
        space = 1690 if (code >> bit) & 1 else 560
        p[k] = space; k += 1
    p[66] = space
    return p


def wrong_encode(code):
    """上一版（按 LSB 先发）的编码器，用来复现故障"""
    p = [9000, 4500]
    for i in range(32):
        p.append(560)
        p.append(1690 if (code >> i) & 1 else 560)
    return p


def decode_nec(pattern):
    """Arduino-IRremote v2 的 decodeNEC：data = (data << 1) | bit"""
    data = 0
    i = 2
    for _ in range(32):
        # pattern[i] 是 mark，pattern[i+1] 是 space
        if i + 1 >= len(pattern):
            break
        space = pattern[i + 1]
        bit = 1 if space > (560 + 1690) // 2 else 0
        data = ((data << 1) | bit) & 0xFFFFFFFF
        i += 2
    return data


def rev32(v):
    r = 0
    for i in range(32):
        if (v >> i) & 1:
            r |= 1 << (31 - i)
    return r


if __name__ == "__main__":
    ok = True
    print("%-6s %-10s %-10s %-10s %-10s" % ("按键", "目标码", "新版解码", "旧版解码", "旧版=新版"))
    for name, code in CODES.items():
        o = old_encode(code)
        n = new_encode(code)
        same = (o == n)
        d_old = decode_nec(o)
        d_new = decode_nec(n)
        print("%-6s 0x%08X 0x%08X 0x%08X %s"
              % (name, code, d_new, d_old, "是" if same else "否"))
        if not same or d_old != code or d_new != code:
            ok = False

    print()
    print("故障复现（上一版按 LSB 先发）：")
    for name, code in CODES.items():
        w = wrong_encode(code)
        d = decode_nec(w)
        print("  %s 发 0x%08X → 车端解出 0x%08X（反转值 0x%08X）%s"
              % (name, code, d, rev32(code),
                 "  命中" if d == code else "  ✗ 不在 KEY_TABLE 里，车不响应"))

    print()
    print("结论：" + ("新版与旧版波形逐元素一致，且都能解回正确键码" if ok else "★ 不一致，别打包★"))

