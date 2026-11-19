#requires -Version 5.1
# 波特率可通过 -Baud 改（HC-05 出厂 9600，被 AT 改过的话这里跟上）
param([int]$Baud = 9600)
<#
==========================================================================
  小车调参台  ——  通过蓝牙串口实时改参数，不烧录、不写 EEPROM
--------------------------------------------------------------------------
  为什么要有它：
    调直线时每次改 GAIN_R 都要重新烧录（插线 + 编译 + 上传 30 秒），
    而车走不走直全靠肉眼比对，一轮下来只能试一两个值。
    这个工具把"改参数 → 跑一段 → 看结果"压到两秒一轮。

  ★ 它不会写 EEPROM：
    固件里只有 `W` 命令会存盘，本工具**从不发送 W**。
    串口改的参数只活在 RAM 里，小车一断电就回到源码默认值 / 上次存档值。
    （所以：调的满意了，还是要想办法把值写回 #define 再烧一次固化。）

  ★ 调参时必须拔掉 USB 线：
    HC-05 和 USB 转串口都接在 D0/D1 上，两个 TX 同时驱动会打架。
    电脑只用蓝牙连，不要插 USB。

  ★ 电脑这边要先配对 HC-05：
    设置 → 蓝牙和设备 → 添加设备 → 选 HC-05 → PIN 1234 或 0000
    配对后系统会生成一个「出站」COM 口，一般描述里带「蓝牙」或「Bluetooth」。
==========================================================================
#>

$ErrorActionPreference = 'Continue'
# 不强制把控制台改成 UTF-8：Windows 中文环境的默认代码页（936）本来就能正常
# 显示中文，硬切 65001 反而可能因为字体/旧版 conhost 出方块。脚本源码是
# UTF-8 with BOM，PowerShell 自己能正确解析，与控制台编码无关。
try { $Host.UI.RawUI.WindowTitle = '小车调参台 · 蓝牙串口' } catch { }

# ------------------------------------------------------------------ 全局状态
$script:Sp        = $null
$script:Buf       = ''
$script:RxLine    = New-Object 'System.Collections.Generic.List[byte]'
$script:CurParam  = ''
$script:Step      = 2
$script:StopAt    = 0          # ticks，到点自动发 S
$script:Quiet     = $false
$script:Running   = $false
$script:Jobs      = New-Object System.Collections.ArrayList
$script:Hist      = New-Object System.Collections.ArrayList   # 参数变更历史，方便回看"哪组最直"
$script:JobWait   = 0          # ticks，扫参时两组之间的静默期
$script:LogPath   = Join-Path $PSScriptRoot ("串口日志_{0:yyyyMMdd_HHmmss}.txt" -f (Get-Date))

# 车端回读值（来自 [CAL] 行）
$script:CarL = 0; $script:CarR = 0; $script:CarS = 0
$script:CarFwd = 0; $script:CarTurn = 0

# ------------------------------------------------------------------ 终端输出
function Clear-Line {
    $w = 120
    try { $w = [Math]::Max(40, [Console]::BufferWidth - 1) } catch { }
    try { [Console]::Write("`r" + (' ' * $w) + "`r") } catch { }
}

function Show-Prompt {
    Clear-Line
    try { [Console]::Write('> ' + $script:Buf) } catch { }
}

function Say([string]$text, [string]$color = 'Gray') {
    $line = '[' + (Get-Date).ToString('HH:mm:ss') + '] ' + $text
    try { Add-Content -LiteralPath $script:LogPath -Value $line -Encoding UTF8 } catch { }
    Clear-Line
    Write-Host $line -ForegroundColor $color
    Show-Prompt
}

function Say-Head([string]$text) {
    Clear-Line
    Write-Host ''
    Write-Host ('  ' + $text) -ForegroundColor Magenta
    Write-Host ('  ' + ('=' * 56)) -ForegroundColor DarkGray
    Show-Prompt
}

# ------------------------------------------------------------------ 串口
function Send-Cmd([string]$cmd) {
    if ($null -eq $script:Sp -or -not $script:Sp.IsOpen) {
        Say '✗ 串口没打开' 'Red'
        return
    }
    try {
        $script:Sp.Write($cmd + "`n")

        # 日志文件里记下每一条真正发出去的命令 —— 这就是"电脑端的参数记录"
        try { Add-Content -LiteralPath $script:LogPath -Value ('    TX> ' + $cmd) -Encoding UTF8 } catch { }

        if ($cmd -match '^([LRS])(\d+)$') {
            [void]$script:Hist.Add([pscustomobject]@{
                T = (Get-Date).ToString('HH:mm:ss')
                K = $matches[1]
                V = [int]$matches[2]
            })
            if ($script:Hist.Count -gt 300) { $script:Hist.RemoveAt(0) }
        }
    } catch {
        Say ('✗ 发送失败：' + $_.Exception.Message) 'Red'
        Close-Port
    }
}

function Close-Port {
    if ($null -eq $script:Sp) { return }
    try { if ($script:Sp.IsOpen) { $script:Sp.Write("S`n") } } catch { }
    try { if ($script:Sp.IsOpen) { $script:Sp.Close() } } catch { }
    try { $script:Sp.Dispose() } catch { }
    $script:Sp = $null
}

function Pump-Rx {
    if ($null -eq $script:Sp -or -not $script:Sp.IsOpen) { return }

    $n = 0
    try { $n = $script:Sp.BytesToRead } catch { return }
    if ($n -le 0) { return }

    $bytes = New-Object 'byte[]' $n
    $got = 0
    try { $got = $script:Sp.Read($bytes, 0, $n) } catch { return }
    if ($got -le 0) { return }

    for ($i = 0; $i -lt $got; $i++) { [void]$script:RxLine.Add($bytes[$i]) }

    # 按 0x0A 切行，逐字节累积 —— 中文被拆包也不会乱码
    while ($true) {
        $idx = -1
        for ($i = 0; $i -lt $script:RxLine.Count; $i++) {
            if ($script:RxLine[$i] -eq 10) { $idx = $i; break }
        }
        if ($idx -lt 0) { break }

        $seg = $script:RxLine.GetRange(0, $idx).ToArray()
        $script:RxLine.RemoveRange(0, $idx + 1)

        $line = [System.Text.Encoding]::UTF8.GetString($seg)
        $line = $line.TrimEnd([char]13)
        if ($line.Length -gt 0) { On-CarLine $line }
    }

    if ($script:RxLine.Count -gt 4000) { $script:RxLine.Clear() }   # 防跑飞
}

function On-CarLine([string]$line) {
    # 机器可读的参数行：解析后用来做"车端现值"显示和 +/- 调参基准
    if ($line -match '^\[CAL\]\s*L=(\d+)\s+R=(\d+)\s+S=(\d+)\s+FWD=(\d+)\s+TURN=(\d+)') {
        $script:CarL    = [int]$matches[1]
        $script:CarR    = [int]$matches[2]
        $script:CarS    = [int]$matches[3]
        $script:CarFwd  = [int]$matches[4]
        $script:CarTurn = [int]$matches[5]

        if ($script:Quiet) {
            Say ('[参数] 左={0}  右={1}  速度={2}‰  前进duty={3}  转弯duty={4}' -f `
                    $script:CarL, $script:CarR, $script:CarS, $script:CarFwd, $script:CarTurn) 'Green'
        } else {
            Say $line 'Green'
        }
        return
    }

    if ($script:Quiet -and $line -notmatch '动作|转向|前进|后退|停|错误') { return }

    $c = 'Gray'
    if ($line -match '^\[动作\]') { $c = 'Cyan' }
    elseif ($line -match '转向|前进 duty|扫描') { $c = 'Cyan' }
    elseif ($line -match '✗|错误|失败|用法') { $c = 'DarkYellow' }
    Say $line $c
}

# ------------------------------------------------------------------ 选端口
function Select-Port {
    $names = @()
    try { $names = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object) } catch { }

    if ($names.Count -eq 0) {
        Say '没有找到任何串口。先把 HC-05 配对：设置 → 蓝牙和设备 → 添加设备' 'Red'
        return $null
    }

    # 拿设备描述，方便认出哪个是蓝牙口
    $desc = @{}
    try {
        $q = Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction Stop |
             Where-Object { $_.Name -match '\(COM\d+\)' }
        foreach ($d in $q) {
            if ($d.Name -match '\((COM\d+)\)') {
                $desc[$matches[1]] = $d.Name
            }
        }
    } catch { }

    Clear-Line
    Write-Host ''
    Write-Host '  可用串口：' -ForegroundColor Cyan
    $i = 1
    foreach ($n in $names) {
        $label = $n
        if ($desc.ContainsKey($n)) { $label = $n + '   ' + $desc[$n] }
        Write-Host ('    [{0}] {1}' -f $i, $label)
        $i++
    }

    $bt = @($names | Where-Object { $desc[$_] -match '蓝牙|Bluetooth' })
    if ($bt.Count -gt 0) {
        Write-Host ('  ★ 蓝牙口应该是 ' + ($bt -join '、') + '，选它') -ForegroundColor Yellow
    } else {
        Write-Host '  （没识别到蓝牙口。若列出了 COM 口但不知道是哪个，去「设备管理器 → 端口」看）' -ForegroundColor DarkYellow
    }

    Write-Host ''
    $ans = Read-Host '  输入序号或 COM 号（直接回车 = 第 1 个，q = 退出）'
    if ([string]::IsNullOrWhiteSpace($ans)) { return $names[0] }
    if ($ans.Trim() -eq 'q') { return $null }

    if ($ans -match '^\d+$') {
        $idx = [int]$ans
        if ($idx -ge 1 -and $idx -le $names.Count) { return $names[$idx - 1] }
        Say '序号超出范围' 'Red'
        return $null
    }

    $ans = $ans.Trim().ToUpper()
    if ($ans -notmatch '^COM\d+$') { $ans = 'COM' + $ans }
    return $ans
}

function Open-Port([string]$name) {
    try {
        $sp = New-Object System.IO.Ports.SerialPort
        $sp.PortName  = $name
        $sp.BaudRate  = $Baud
        $sp.Parity    = [System.IO.Ports.Parity]::None
        $sp.DataBits  = 8
        $sp.StopBits  = [System.IO.Ports.StopBits]::One
        $sp.Handshake = [System.IO.Ports.Handshake]::None
        $sp.ReadTimeout  = 300
        $sp.WriteTimeout = 800
        $sp.DtrEnable = $false       # 不动 DTR/RTS：不会把板子复位
        $sp.RtsEnable = $false
        $sp.Open()
        try { $sp.DiscardInBuffer() } catch { }
        $script:Sp = $sp
        return $true
    } catch {
        Say ('✗ 打不开 ' + $name + '：' + $_.Exception.Message) 'Red'
        Say '  常见原因：被串口助手/Arduino IDE 占着；HC-05 没连上；端口号不对' 'DarkYellow'
        return $false
    }
}

# ------------------------------------------------------------------ 帮助
function Show-Help {
    Say-Head '命令一览'
    Write-Host @'
  运动（发一次就走，直到再发停车）
    f        前进            b        后退
    a        左转            d        右转
    s        停车

  直线测试
    run      前进 2 秒后自动停
    run 3    前进 3 秒后自动停

  调参（改完立刻生效，断电即失效，不写 EEPROM）
    L256     左轮增益      R222   右轮增益（压右轮治左偏）
    S1100    全局速度 ‰    T85    转弯占空比
    A78      微调占空比    U290   自动模式转向角速度(deg/s)
    cal      读回车端当前参数（发 ?）

  快速微调（推荐：跑一次、加一点、再跑一次）
    use R    把 R 设为"当前参数"
    +        当前参数 +步长 并立刻发送
    -        当前参数 -步长
    step 5   改步长（默认 2）
    use L / use S 同理

  自动扫参（一次跑完一组，你只管看哪次最直）
    sweep R 212 232 2 3
              ↑参数 ↑起 ↑止 ↑步长 ↑每个值跑几秒
    依次：设 R212 跑 3 秒 → 停 2 秒 → 设 R214 跑 3 秒 ……

  其他
    hist     看最近改过的参数（配合日志回顾哪组最直）
    quiet    只显示参数行和动作行（自动模式刷屏时用）
    cls      清屏
    help     本帮助
    quit     退出（会先发停车）
'@ -ForegroundColor Gray
    Write-Host '  ★ 不要发 W（存盘）和 D（恢复默认）—— 前者会写 EEPROM，后者会擦掉存档' -ForegroundColor Yellow
    Write-Host '  ★ 调参时把 USB 线拔掉，只走蓝牙' -ForegroundColor Yellow
    Show-Prompt
}

# ------------------------------------------------------------------ 命令
function Get-CurVal {
    switch ($script:CurParam) {
        'R' { return $script:CarR }
        'L' { return $script:CarL }
        'S' { return $script:CarS }
        default { return $null }
    }
}

function Set-CurVal([int]$v) {
    switch ($script:CurParam) {
        'R' { $script:CarR = $v }
        'L' { $script:CarL = $v }
        'S' { $script:CarS = $v }
    }
}

function Invoke-Line([string]$raw) {
    $t  = $raw.Trim()
    $lo = $t.ToLower()

    if ($t.Length -eq 0) { return }

    # ---- 本地命令 ----
    if ($lo -eq 'help' -or $lo -eq 'h') { Show-Help; return }
    if ($lo -eq 'cls')  { Clear-Host; return }
    if ($lo -eq 'cal')  { Send-Cmd '?'; return }

    if ($lo -eq 'hist') {
        if ($script:Hist.Count -eq 0) { Say '还没有改过参数' 'DarkYellow'; return }
        Say-Head '参数变更历史（最新在最后）'
        $start = [Math]::Max(0, $script:Hist.Count - 20)
        for ($i = $start; $i -lt $script:Hist.Count; $i++) {
            $h = $script:Hist[$i]
            Write-Host ('    ' + $h.T + '    ' + $h.K + ' = ' + $h.V) -ForegroundColor Gray
        }
        Show-Prompt
        return
    }

    if ($lo -eq 'quiet') {
        $script:Quiet = -not $script:Quiet
        if ($script:Quiet) { Say '安静模式：只显示参数行和动作行' 'Yellow' }
        else               { Say '完整模式：车端输出全部显示' 'Yellow' }
        return
    }

    if ($lo -eq 'quit' -or $lo -eq 'exit') {
        Say '退出：先发停车，再关串口' 'Yellow'
        Send-Cmd 'S'
        Start-Sleep -Milliseconds 150
        $script:Running = $false
        return
    }

    # ---- 直线测试 ----
    if ($lo -match '^run(?:\s+(\d+(?:\.\d+)?))?$') {
        $sec = 2.0
        if ($matches[1]) { $sec = [double]$matches[1] }
        Send-Cmd 'F'
        $script:StopAt = [DateTime]::Now.AddSeconds($sec).Ticks
        Say ('直线测试 ' + $sec + ' 秒 —— 松手不用管，到点自动停') 'Yellow'
        return
    }

    # ---- 快速微调 ----
    if ($lo -match '^use\s+([lr]|s)$') {
        $script:CurParam = $matches[1].ToUpper()
        $v = Get-CurVal
        if ($null -eq $v) {
            Say ('车端还没回读 ' + $script:CurParam + ' 的值，先发 cal') 'DarkYellow'
            $script:CurParam = ''
            return
        }
        Say ('当前参数 ' + $script:CurParam + ' = ' + $v + '，用 + / - 调，步长 ' + $script:Step) 'Yellow'
        return
    }

    if ($lo -match '^step\s+(\d+)$') {
        $script:Step = [Math]::Max(1, [int]$matches[1])
        Say ('步长 = ' + $script:Step) 'Yellow'
        return
    }

    if ($t -eq '+' -or $t -eq '-') {
        if ($script:CurParam -eq '') { Say '先 use R / use L / use S 指定要调哪个' 'DarkYellow'; return }
        $base = Get-CurVal
        if ($null -eq $base) { Say '还没有车端回读值，先发 cal' 'DarkYellow'; return }

        if ($t -eq '+') { $new = $base + $script:Step } else { $new = $base - $script:Step }

        # 边界与固件里的一致，越界直接拦下，免得车端静默忽略
        $ok = $true
        if ($script:CurParam -eq 'R' -or $script:CurParam -eq 'L') {
            if ($new -lt 150 -or $new -gt 400) { $ok = $false }
        } elseif ($script:CurParam -eq 'S') {
            if ($new -lt 600 -or $new -gt 1400) { $ok = $false }
        }
        if (-not $ok) { Say ('已到范围边界（' + $base + '）') 'DarkYellow'; return }

        # 先乐观更新本地值：连按 + 时不会因为等回读而重复发同一个数
        Set-CurVal $new
        Send-Cmd ($script:CurParam + $new)
        Say ('→ ' + $script:CurParam + $new + '   跑一下看直不直（run 2）') 'Cyan'
        return
    }

    # ---- 自动扫参 ----
    if ($lo -match '^sweep\s+([lr])\s+(\d+)\s+(\d+)\s+(\d+)(?:\s+(\d+(?:\.\d+)?))?$') {
        $p    = $matches[1].ToUpper()
        $from = [int]$matches[2]
        $to   = [int]$matches[3]
        $st   = [Math]::Max(1, [int]$matches[4])
        $sec  = 3.0
        if ($matches[5]) { $sec = [double]$matches[5] }

        if ($from -gt $to) { $tmp = $from; $from = $to; $to = $tmp }
        if ($from -lt 150 -or $to -gt 400) {
            Say '增益范围是 150~400，超出会被车端忽略' 'Red'
            return
        }

        $script:Jobs.Clear()
        $list = @()
        for ($v = $from; $v -le $to; $v += $st) { $list += $v }

        $k = 0
        foreach ($v in $list) {
            $k++
            [void]$script:Jobs.Add([pscustomobject]@{
                Cmd     = ($p + $v)
                Desc    = ($p + $v)
                Sec     = $sec
                Index   = $k
                Total   = $list.Count
            })
        }

        $script:JobWait = 0
        Say ('扫参开始：' + $p + ' ' + $from + '→' + $to + ' 步长 ' + $st + '，每个值跑 ' + $sec + ' 秒，共 ' + $list.Count + ' 组') 'Magenta'
        Say '每组开始会打一条分隔线，记住哪一次走得最直' 'Yellow'
        return
    }

    # ---- 单字符运动 ----
    if ($t -eq 'f') { Send-Cmd 'F'; Say '前进' 'Cyan'; return }
    if ($t -eq 'b') { Send-Cmd 'B'; Say '后退' 'Cyan'; return }
    if ($t -eq 'a') { Send-Cmd 'L'; Say '左转' 'Cyan'; return }
    if ($t -eq 'd') { Send-Cmd 'R'; Say '右转' 'Cyan'; return }
    if ($t -eq 's') { Send-Cmd 'S'; Say '停车' 'Cyan'; return }

    # ---- 其余原样透传给固件 ----
    Send-Cmd $t
}

# ------------------------------------------------------------------ 主循环
function Main {
    Clear-Host
    Write-Host ''
    Write-Host '  小车调参台  v1.0' -ForegroundColor Green
    Write-Host '  蓝牙实时调参：不烧录、不写 EEPROM' -ForegroundColor DarkGray
    Write-Host ''

    $port = Select-Port
    if (-not $port) { Write-Host '  已取消' -ForegroundColor DarkYellow; return }
    if (-not (Open-Port $port)) { return }

    Say ('已打开 ' + $port + ' @ ' + $Baud) 'Green'
    Start-Sleep -Milliseconds 250
    Send-Cmd '?'
    Start-Sleep -Milliseconds 450
    Pump-Rx

    if ($script:CarL -eq 0) {
        Say ('没读到参数行。检查：HC-05 是否已连上、波特率是否 ' + $Baud + '、USB 线是否拔了') 'DarkYellow'
    } else {
        Say ('车端当前：左=' + $script:CarL + '  右=' + $script:CarR + '  速度=' + $script:CarS + '‰') 'Green'
        Say '（这是真正生效的值。若和源码默认不一样，说明 EEPROM 里有旧存档）' 'DarkGray'
    }

    Say '输入 help 看命令；f 前进 / s 停车 / run 2 直线测试' 'Yellow'
    Say ('本次日志同时写到：' + $script:LogPath) 'DarkGray'

    $script:Running = $true
    while ($script:Running) {
        Pump-Rx

        # 到点停车
        if ($script:StopAt -gt 0 -and [DateTime]::Now.Ticks -ge $script:StopAt) {
            $script:StopAt = 0
            Send-Cmd 'S'
            Say '（到时，已停车）' 'Yellow'
            if ($script:Jobs.Count -gt 0) {
                $script:JobWait = [DateTime]::Now.AddSeconds(2).Ticks
                Say '（2 秒后测下一组）' 'DarkGray'
            } elseif ($script:Jobs.Count -eq 0 -and $script:JobWait -eq 0) {
                # 普通 run 结束
            }
        }

        # 扫参：上一组停够 2 秒后启动下一组
        if ($script:Jobs.Count -gt 0 -and $script:StopAt -eq 0 -and `
            ($script:JobWait -eq 0 -or [DateTime]::Now.Ticks -ge $script:JobWait)) {
            $script:JobWait = 0
            $job = $script:Jobs[0]
            $script:Jobs.RemoveAt(0)

            Say-Head ('第 {0}/{1} 组：{2}，跑 {3} 秒' -f $job.Index, $job.Total, $job.Desc, $job.Sec)
            Send-Cmd $job.Cmd
            Start-Sleep -Milliseconds 150
            Send-Cmd 'F'
            $script:StopAt = [DateTime]::Now.AddSeconds($job.Sec).Ticks
        }

        # 键盘（KeyAvailable 在个别终端会抛异常，包一层免得整个循环退出）
        $hasKey = $false
        try { $hasKey = [Console]::KeyAvailable } catch { }

        if ($hasKey) {
            $k = [Console]::ReadKey($true)

            if ($k.Key -eq [ConsoleKey]::Enter) {
                $line = $script:Buf
                $script:Buf = ''
                Clear-Line
                if ($line.Trim().Length -gt 0) {
                    Say ('> ' + $line) 'DarkGray'
                    Invoke-Line $line
                } else {
                    Show-Prompt
                }
            }
            elseif ($k.Key -eq [ConsoleKey]::Backspace) {
                if ($script:Buf.Length -gt 0) {
                    $script:Buf = $script:Buf.Substring(0, $script:Buf.Length - 1)
                    Show-Prompt
                }
            }
            elseif ($k.Key -eq [ConsoleKey]::Escape) {
                $script:Buf = ''
                Show-Prompt
            }
            elseif ([int]$k.KeyChar -ge 32) {
                $script:Buf += $k.KeyChar
                try { [Console]::Write($k.KeyChar) } catch { }
            }
        }

        Start-Sleep -Milliseconds 40
    }

    Close-Port
    Say ('已关闭。日志在 ' + $script:LogPath) 'DarkGray'
}

# 只有「直接运行」才进主流程；dot-source（. .\tune.ps1）时只加载函数，方便测解析逻辑
if ($MyInvocation.InvocationName -ne '.') { Main }
