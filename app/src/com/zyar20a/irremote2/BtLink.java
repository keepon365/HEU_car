package com.zyar20a.irremote2;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;
import android.os.SystemClock;

import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.UUID;

/**
 * 经典蓝牙 SPP 串口连接（HC-05 / HC-06）。
 *
 * ★ v3.1 起是**双向**的：除了发命令，还常驻一条读线程收车端回话。
 *   这不是为了好玩 —— 蓝牙链路会「假活」：
 *     · HC-05 掉电/供电跌落复位后，Android 这边的 socket 仍然 isConnected()==true
 *     · 之后 write() 可能不报错（写进内核缓冲）或干脆阻塞不返回
 *   结果就是「显示已连接、点按钮没反应」。只有收到对端数据才算真活，
 *   所以固件对 '!' 回 "#OK"，App 拿它当心跳。
 *
 * ★ 写超时守护：BluetoothSocket 没有 API 能设写超时，而 write() 卡住时
 *   整条发送线程永久挂死（连 doConnect 都排不进去）。这里用一条守护线程
 *   盯着：一次 write 超过 2 秒没返回，就主动 close() —— close 会让阻塞的
 *   write 抛异常退出，链路随即进入重连流程。
 *
 * ★ 三个必须注意的地方：
 *   1. connect() 会阻塞几秒，**只能在子线程调用**
 *   2. Android 12+ 需要 BLUETOOTH_CONNECT 运行时权限，没授权会抛 SecurityException
 *   3. HC-05 默认波特率 9600，与固件 Serial 一致；如果模块被改过，收不到就是波特率不对
 */
public class BtLink
{
    /** 串口服务的标准 UUID，所有 SPP 模块（HC-05/06）都用这个 */
    private static final UUID SPP_UUID =
            UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");

    /** 一次 write 卡多久算死。正常一帧 2 字节在 9600 波特率下 <2ms。 */
    private static final long WRITE_TIMEOUT_MS = 2000;

    /** 守护线程巡检周期 */
    private static final long WATCH_PERIOD_MS  = 250;

    /** 收数据回调。**都在后台线程调用**，实现方自己 post 到主线程。 */
    public interface Listener {
        /** 收到完整一行（已去掉 \r\n） */
        void onLine(String line);
        /** 链路已断（读线程结束 / 写失败），参数是原因 */
        void onLost(String why);
    }

    private BluetoothAdapter adapter;
    private BluetoothSocket socket;
    private OutputStream out;
    private InputStream in;
    private String devLabel = "";

    /** 最近一次连上的设备，给「自动重连」用 */
    private volatile BluetoothDevice lastDev;

    /** 最近一次收到车端字节的时刻（elapsedRealtime）。0 = 从没收到过 */
    private volatile long lastRxMs;

    /** 正在写的起始时刻；0 = 当前没有写操作在途 */
    private volatile long writeStartMs;

    private volatile boolean running;
    private Thread reader;
    private Thread watch;
    private volatile Listener listener;

    public BtLink() {
        try { adapter = BluetoothAdapter.getDefaultAdapter(); } catch (Throwable t) { adapter = null; }
    }

    public void setListener(Listener l) { listener = l; }

    public boolean available() {
        return adapter != null;
    }

    /** 蓝牙是否已打开。没有权限时也可能抛异常，所以包一层。 */
    public boolean enabled() {
        try { return adapter != null && adapter.isEnabled(); } catch (Throwable t) { return false; }
    }

    public boolean connected() {
        try { return socket != null && socket.isConnected() && out != null; } catch (Throwable t) { return false; }
    }

    /** 已连接设备的名字 + 地址，用于界面显示 */
    public String deviceLabel() {
        return devLabel;
    }

    /** 上次连过的设备（断开后仍保留），用于自动重连 */
    public BluetoothDevice lastDevice() {
        return lastDev;
    }

    /**
     * 距上次收到车端数据过了多少毫秒。
     * 从没收到过返回 Long.MAX_VALUE（调用方据此判断「连上但没回话」）。
     */
    public long msSinceRx() {
        long t = lastRxMs;
        if (t == 0) { return Long.MAX_VALUE; }
        return SystemClock.elapsedRealtime() - t;
    }

    /** 已配对设备列表。没有权限时返回空表而不是崩。 */
    public List<BluetoothDevice> paired() {
        List<BluetoothDevice> list = new ArrayList<BluetoothDevice>();
        if (adapter == null) { return list; }
        try {
            Set<BluetoothDevice> set = adapter.getBondedDevices();
            if (set != null) { list.addAll(set); }
        } catch (Throwable ignored) {
        }
        return list;
    }

    /** 已配对设备的名字，取不到时用地址代替 */
    public static String nameOf(BluetoothDevice d) {
        try {
            String n = d.getName();
            if (n != null && n.length() > 0) { return n; }
        } catch (Throwable ignored) {
        }
        return d.getAddress();
    }

    /**
     * 连接。**必须在子线程调用**（RFCOMM 建连要几秒）。
     *
     * @return null 表示成功；否则是错误描述
     */
    public String connect(BluetoothDevice d) {
        close(null);
        BluetoothSocket s = null;
        try {
            /* 不做 cancelDiscovery：API 31+ 那需要 BLUETOOTH_SCAN 权限，
             * 而我们只连已配对设备、从不主动扫描，省一个权限。 */

            s = d.createRfcommSocketToServiceRecord(SPP_UUID);
            s.connect();

            socket = s;
            out = s.getOutputStream();
            in  = s.getInputStream();
            devLabel = nameOf(d) + "  " + d.getAddress();
            lastDev  = d;

            /* 建连成功先记一次「刚收到」，给心跳一个宽容期，
             * 否则连着的那一刻就会被判成"3 秒无响应"。 */
            lastRxMs = SystemClock.elapsedRealtime();
            running  = true;

            startReader();
            startWatch();
            return null;
        } catch (Throwable t) {
            try { if (s != null) s.close(); } catch (Throwable ignored) {}
            socket = null;
            out = null;
            in = null;
            String m = t.getMessage();
            return t.getClass().getSimpleName() + (m == null ? "" : ": " + m);
        }
    }

    /** 常驻读线程：收到字节就刷新 lastRxMs，按行回调 */
    private void startReader() {
        final InputStream is = in;
        if (is == null) { return; }
        reader = new Thread(new Runnable() {
            @Override public void run() {
                final byte[] buf = new byte[128];
                final StringBuilder line = new StringBuilder();
                String why = "车端断开";
                try {
                    while (running) {
                        int n = is.read(buf);
                        if (n < 0) { break; }
                        if (n == 0) { continue; }
                        lastRxMs = SystemClock.elapsedRealtime();
                        for (int i = 0; i < n; i++) {
                            char c = (char) (buf[i] & 0xFF);
                            if (c == '\n') {
                                String s = line.toString().trim();
                                line.setLength(0);
                                if (s.length() > 0) {
                                    Listener l = listener;
                                    if (l != null) { try { l.onLine(s); } catch (Throwable ignored) {} }
                                }
                            } else if (c != '\r') {
                                if (line.length() < 200) { line.append(c); }
                            }
                        }
                    }
                } catch (Throwable t) {
                    why = "读异常 " + t.getClass().getSimpleName();
                }
                /* 只有「本来连着」才报断开，主动 close 不算 */
                boolean stillOurs = running;
                running = false;
                if (stillOurs) {
                    Listener l = listener;
                    if (l != null) { try { l.onLost(why); } catch (Throwable ignored) {} }
                }
            }
        }, "bt-rx");
        reader.setDaemon(true);
        reader.start();
    }

    /** 守护线程：把卡死的 write 撬开 */
    private void startWatch() {
        watch = new Thread(new Runnable() {
            @Override public void run() {
                while (running) {
                    try { Thread.sleep(WATCH_PERIOD_MS); } catch (InterruptedException e) { return; }
                    long st = writeStartMs;
                    if (st != 0 && SystemClock.elapsedRealtime() - st > WRITE_TIMEOUT_MS) {
                        /* write 卡死：关 socket 让它抛异常退出。
                         * 不在这里发 onLost —— 卡住的那一头（send 的 catch）会调 close，
                         * 这里只负责撬锁。 */
                        writeStartMs = 0;
                        close(null);
                        return;
                    }
                }
            }
        }, "bt-watch");
        watch.setDaemon(true);
        watch.start();
    }

    /**
     * 发一条命令（自动补换行，固件按行解析）。
     *
     * @return false 表示发送失败，连接已被关闭 —— 调用方应当提示重连
     */
    public boolean send(String cmd) {
        final OutputStream o = out;
        if (o == null) { return false; }
        writeStartMs = SystemClock.elapsedRealtime();
        try {
            o.write((cmd + "\n").getBytes("US-ASCII"));
            o.flush();
            return true;
        } catch (Throwable t) {
            close("写失败 " + t.getClass().getSimpleName());
            return false;
        } finally {
            writeStartMs = 0;
        }
    }

    public void close() { close(null); }

    /**
     * 关闭连接。
     * @param why 非空时对外报「链路已断」；主动断开（换设备/退出）传 null
     */
    public void close(String why) {
        boolean wasRunning = running;
        running = false;
        writeStartMs = 0;

        try { if (in  != null) { in.close();  } } catch (Throwable ignored) {}
        try { if (out != null) { out.close(); } } catch (Throwable ignored) {}
        in = null;
        out = null;
        try { if (socket != null) { socket.close(); } } catch (Throwable ignored) {}
        socket = null;
        devLabel = "";
        lastRxMs = 0;

        if (why != null && wasRunning) {
            Listener l = listener;
            if (l != null) { try { l.onLost(why); } catch (Throwable ignored) {} }
        }
    }
}
