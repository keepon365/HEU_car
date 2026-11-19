package com.zyar20a.irremote2;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;

import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.UUID;

/**
 * 经典蓝牙 SPP 串口连接（HC-05 / HC-06）。
 *
 * 与红外最大的区别：这是**双向、有连接**的通道。
 *   · 发出去的命令对方会收到（除非断连）
 *   · 断连能被察觉 → 可以让车立即停，而不是等看门狗超时
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

    private BluetoothAdapter adapter;
    private BluetoothSocket socket;
    private OutputStream out;
    private String devLabel = "";

    public BtLink() {
        try { adapter = BluetoothAdapter.getDefaultAdapter(); } catch (Throwable t) { adapter = null; }
    }

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
        close();
        BluetoothSocket s = null;
        try {
            /* 不做 cancelDiscovery：API 31+ 那需要 BLUETOOTH_SCAN 权限，
             * 而我们只连已配对设备、从不主动扫描，省一个权限。 */

            s = d.createRfcommSocketToServiceRecord(SPP_UUID);
            s.connect();

            socket = s;
            out = s.getOutputStream();
            devLabel = nameOf(d) + "  " + d.getAddress();
            return null;
        } catch (Throwable t) {
            try { if (s != null) s.close(); } catch (Throwable ignored) {}
            socket = null;
            out = null;
            String m = t.getMessage();
            return t.getClass().getSimpleName() + (m == null ? "" : ": " + m);
        }
    }

    /**
     * 发一条命令（自动补换行，固件按行解析）。
     *
     * @return false 表示发送失败，连接已被关闭 —— 调用方应当立即让车停
     */
    public boolean send(String cmd) {
        if (out == null) { return false; }
        try {
            out.write((cmd + "\n").getBytes("US-ASCII"));
            out.flush();
            return true;
        } catch (Throwable t) {
            close();
            return false;
        }
    }

    public void close() {
        try { if (out != null) { out.close(); } } catch (Throwable ignored) {}
        out = null;
        try { if (socket != null) { socket.close(); } } catch (Throwable ignored) {}
        socket = null;
        devLabel = "";
    }
}
