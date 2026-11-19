# ===========================================================================
#  扫描窗口.ps1  —— 直接读写 COM9，按键实时送到板子
#  ---------------------------------------------------------------------------
#  为什么不用 arduino-cli monitor：它的键盘转发依赖终端状态，经常按了没反应。
#  这里用 .NET 的串口对象，自己读、自己写，按键一定是通的。
#
#  特性：
#    - 打开串口时不动 DTR/RTS，不会复位板子（不会出现乱码）
#    - 断线自动重连，不会整个窗口退出
#    - 按键立刻发出去，不用回车
#    - ESC 退出
# ===========================================================================

$ErrorActionPreference = 'Continue'

$PortName = 'COM9'
$Baud     = 115200

# 让中文能正常显示
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

function Open-Port {
    $p = New-Object System.IO.Ports.SerialPort $PortName, $Baud, `
            ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
    $p.ReadTimeout  = 200
    $p.WriteTimeout = 500
    $p.DtrEnable    = $false      # 不触发板子复位，避免开头乱码
    $p.RtsEnable    = $false
    $p.Open()
    return $p
}

$sp = $null
$lastTry = [datetime]::MinValue
$keyOk = $true          # 键盘可用性探测，被重定向时自动关掉

while ($true) {

    # ---- 连接 ----
    if ($null -eq $sp) {
        try {
            $sp = Open-Port
            Write-Host ""
            Write-Host ("[connected {0} @ {1}]" -f $PortName, $Baud) -ForegroundColor Green
        }
        catch {
            if ($null -ne $sp) { try { $sp.Dispose() } catch {} ; $sp = $null }
            if (((Get-Date) - $lastTry).TotalSeconds -ge 2) {
                $lastTry = Get-Date
                Write-Host ("[waiting for {0} ...]" -f $PortName) -ForegroundColor DarkYellow
            }
            Start-Sleep -Milliseconds 300
            continue
        }
    }

    # ---- 读 ----
    try {
        $chunk = $sp.ReadExisting()
        if ($chunk.Length -gt 0) {
            [Console]::Out.Write($chunk)
            [Console]::Out.Flush()
        }
    }
    catch {
        Write-Host ""
        Write-Host "[link lost - reconnecting]" -ForegroundColor Yellow
        try { $sp.Close() }    catch {}
        try { $sp.Dispose() }  catch {}
        $sp = $null
        Start-Sleep -Milliseconds 400
        continue
    }

    # ---- 写（按键立刻发出，不用回车）----
    # 只有在真实控制台里才能检测按键；被重定向时跳过，不要报错刷屏
    if ($keyOk) {
        try {
            $hasKey = [Console]::KeyAvailable
        }
        catch {
            $keyOk = $false
            $hasKey = $false
        }

        if ($hasKey) {
            $k = [Console]::ReadKey($true)
            if ($k.Key -eq [ConsoleKey]::Escape) { break }

            if ($k.KeyChar -ne [char]0) {
                try {
                    $sp.Write([string]$k.KeyChar)
                    # 回显一下，让你确认按键真的发出去了
                    Write-Host ("<{0}>" -f $k.KeyChar) -ForegroundColor Cyan -NoNewline
                }
                catch {}
            }
        }
    }

    Start-Sleep -Milliseconds 20
}

if ($null -ne $sp) {
    try { $sp.Close() } catch {}
}
Write-Host ""
Write-Host "[port closed]" -ForegroundColor DarkGray
